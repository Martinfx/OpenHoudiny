#include "pg/sim/GeometryGraph.h"

#include "pg/sim/Asset.h"
#include "pg/sim/ForEach.h"

#include "pg/sim/Network.h"
#include "pg/sim/WaterMesh.h"

#include <algorithm>
#include <filesystem>
#include <tuple>
#include <vector>

namespace pg::sim {

namespace fs = std::filesystem;

void FrameNode::setFrame(int number, std::shared_ptr<const Frame> frame) {
    // Cooked before from another frame -- simulated again, or not yet then:
    // what was made of any frame is out of date.
    const auto it = seen_.find(number);
    const bool same = it == seen_.end() || (it->second.any ? frame && it->second.frame.lock() == frame : !frame);
    if (!same) {
        seen_.clear();
        bumpVersion();
    }
    seen_[number] = {frame, frame != nullptr};
    frame_ = std::move(frame);
}

namespace {

/// The particles of the water: points with their velocity v and foam.
class LiquidPointsNode : public FrameNode {
public:
    explicit LiquidPointsNode(std::string name) : FrameNode("liquid_points", std::move(name)) { setInputCount(0); }

    GeometryPtr cookNode(const CookContext&, std::span<const GeometryPtr>) override {
        auto geo = std::make_shared<Geometry>();
        if (!frame_) return geo;
        const WaterFrame& w = frame_->water;
        const size_t n = w.positions.size();
        geo->addPoints(n);
        std::copy(w.positions.begin(), w.positions.end(), geo->positionsForWrite().begin());
        auto v = geo->points().create("v", AttrType::Vec3).write<Vec3>();
        auto foam = geo->points().create("foam", AttrType::Float).write<float>();
        for (size_t i = 0; i < n && 3 * i + 2 < w.velocities.size(); ++i) {
            v[i] = Vec3(fromHalf(w.velocities[3 * i]), fromHalf(w.velocities[3 * i + 1]), fromHalf(w.velocities[3 * i + 2]));
            foam[i] = i < w.whiteness.size() ? static_cast<float>(w.whiteness[i]) / 255.0f : 0.0f;
        }
        // Each particle's number, the same from frame to frame.
        if (w.ids.size() == n) {
            auto id = geo->points().create("id", AttrType::Int).write<int32_t>();
            for (size_t i = 0; i < n; ++i) id[i] = static_cast<int32_t>(w.ids[i]);
        }
        return geo;
    }
};

/// The water's surface: a closed mesh round it with N, v and foam, the
/// rain's ripples on it (WaterMesh.h).
class LiquidSurfaceNode : public FrameNode {
public:
    explicit LiquidSurfaceNode(std::string name) : FrameNode("liquid_surface", std::move(name)) {
        setInputCount(0);
        params_.setBool("ripples", true);
    }

    GeometryPtr cookNode(const CookContext&, std::span<const GeometryPtr>) override {
        if (!frame_) return std::make_shared<Geometry>();
        return waterMesh(frame_->water, params_.getBool("ripples", true) ? &frame_->rain : nullptr);
    }
};

/// The drops of the rain -- and the droplets of its splashes -- as points
/// with their velocity v; droplets marked by droplet 1.
class RainPointsNode : public FrameNode {
public:
    explicit RainPointsNode(std::string name) : FrameNode("rain_points", std::move(name)) {
        setInputCount(0);
        params_.setBool("droplets", true);
    }

    GeometryPtr cookNode(const CookContext&, std::span<const GeometryPtr>) override {
        auto geo = std::make_shared<Geometry>();
        if (!frame_) return geo;
        const RainFrame& r = frame_->rain;
        const bool droplets = params_.getBool("droplets", true);
        const size_t drops = r.dropCount(), splashes = droplets ? r.dropletCount() : 0;
        geo->addPoints(drops + splashes);
        auto P = geo->positionsForWrite();
        auto v = geo->points().create("v", AttrType::Vec3).write<Vec3>();
        auto mark = geo->points().create("droplet", AttrType::Int).write<int32_t>();
        // Each one's number, the same from frame to frame -- the droplets'
        // from 2^30 on, not to meet the drops'.
        const bool numbered = r.dropIds.size() == r.dropCount() && r.dropletIds.size() == r.dropletCount();
        std::span<int32_t> id;
        if (numbered) id = geo->points().create("id", AttrType::Int).write<int32_t>();
        auto fill = [&](const std::vector<float>& from, const std::vector<uint32_t>& ids, size_t count, size_t at,
                        int32_t kind) {
            for (size_t i = 0; i < count; ++i) {
                const float* p = from.data() + 6 * i;
                P[at + i] = Vec3(p[0], p[1], p[2]);
                v[at + i] = Vec3(p[3], p[4], p[5]);
                mark[at + i] = kind;
                if (numbered) id[at + i] = static_cast<int32_t>(kind ? (ids[i] | (1u << 30)) : ids[i]);
            }
        };
        fill(r.drops, r.dropIds, drops, 0, 0);
        fill(r.droplets, r.dropletIds, splashes, drops, 1);
        return geo;
    }
};

/// The gas as volumes -- density (smoke), temperature and flame -- on the
/// solver's grid.
class GasVolumeNode : public FrameNode {
public:
    explicit GasVolumeNode(std::string name) : FrameNode("gas_volume", std::move(name)) { setInputCount(0); }

    GeometryPtr cookNode(const CookContext&, std::span<const GeometryPtr>) override {
        auto geo = std::make_shared<Geometry>();
        if (!frame_ || frame_->fields.empty()) return geo;
        const Domain& d = frame_->domain;
        const size_t n = d.cellCount();
        if (frame_->fields.size() < 3 * n) return geo;
        const char* names[3] = {"density", "temperature", "flame"};
        for (int channel = 0; channel < 3; ++channel) {
            std::vector<float> values(n);
            for (size_t c = 0; c < n; ++c) values[c] = fromHalf(frame_->fields[3 * c + static_cast<size_t>(channel)]);
            geo->addVolume(Volume::make(names[channel], d.origin(), d.voxel, d.cells[0], d.cells[1], d.cells[2],
                                        std::move(values)));
        }
        return geo;
    }
};

/// The pieces of the rigid bodies where they are at the frame, with the
/// velocity v of each point; the grit and the bars, when asked for -- or the
/// glue between them, as a network (output 1).
class RbdPiecesNode : public FrameNode {
public:
    explicit RbdPiecesNode(std::string name) : FrameNode("rbd_pieces", std::move(name)) {
        setInputCount(0);
        params_.setBool("grit", false);
        params_.setBool("rebar", false);
        params_.setInt("output", 0);  // 0 pieces, 1 constraints
    }

    GeometryPtr cookNode(const CookContext&, std::span<const GeometryPtr>) override {
        if (!frame_ || frame_->rigid.empty()) return std::make_shared<Geometry>();
        if (params_.getInt("output", 0) == 1) return rigidNetwork(frame_->rigid);
        std::shared_ptr<Geometry> geo = posedPieces(frame_->rigid);
        if (params_.getBool("rebar", false)) geo->append(*rebarBars(frame_->rigid));
        if (params_.getBool("grit", false)) appendGrit(*geo, frame_->rigid);
        return geo;
    }
};

/// The glue between the pieces as a network (rigidNetwork): a point at each
/// body's middle, a line for each joint, its strength a share of the RBD
/// Solver's Glue.
class RbdConstraintsNode : public pg::Node {
public:
    explicit RbdConstraintsNode(std::string name) : pg::Node("rbd_constraints", std::move(name)) {
        setInputCount(1);
        params_.setString("attribute", "piece");
    }

    GeometryPtr cookNode(const CookContext&, std::span<const GeometryPtr> in) override {
        if (in.empty() || !in[0] || in[0]->primitiveCount() == 0) return std::make_shared<Geometry>();
        const std::string attribute = params_.getString("attribute", "piece");
        const auto layout = rigidLayout(*in[0], attribute);
        return rigidNetwork(*rigidGlue(*in[0], *layout, attribute), attribute);
    }
};

/// "12345:1690000000": what says a file changed -- its size and the time it
/// last did; empty for a file that is not there.
std::string stampOf(const std::string& path) {
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    if (ec) return {};
    const auto time = fs::last_write_time(path, ec);
    if (ec) return {};
    // The count is the library's: __int128 in libc++, which to_string does not take.
    return std::to_string(size) + ":" + std::to_string(static_cast<long long>(time.time_since_epoch().count()));
}

}  // namespace

void registerSimGeometryNodes() {
    registerAssetNodes();
    registerForEachNodes();
    static const bool once = [] {
        auto& r = NodeRegistry::instance();
        r.add("liquid_points", [](const std::string& n) { return std::make_unique<LiquidPointsNode>(n); });
        r.add("liquid_surface", [](const std::string& n) { return std::make_unique<LiquidSurfaceNode>(n); });
        r.add("rain_points", [](const std::string& n) { return std::make_unique<RainPointsNode>(n); });
        r.add("gas_volume", [](const std::string& n) { return std::make_unique<GasVolumeNode>(n); });
        r.add("rbd_pieces", [](const std::string& n) { return std::make_unique<RbdPiecesNode>(n); });
        r.add("rbd_constraints", [](const std::string& n) { return std::make_unique<RbdConstraintsNode>(n); });
        return true;
    }();
    (void)once;
}

GeometryGraph::GeometryGraph() {
    registerSimGeometryNodes();
    registerAssetNodes();
    registerForEachNodes();
}

GeometryGraph::~GeometryGraph() = default;

void GeometryGraph::sync(const Network& net, const std::string& folder) {
    const uint64_t library = AssetLibrary::instance().revision();
    if (&net == synced_ && net.revision() == revision_ && folder == folder_ && library == library_) {
        // The network is the same; a file it reads may not be.
        for (auto& [id, m] : nodes_) {
            if (m.file.empty()) continue;
            m.node->editParams([&](ParamSet& p) { return p.setString("stamp", stampOf(m.file)); });
        }
        return;
    }
    synced_ = &net;
    revision_ = net.revision();
    folder_ = folder;
    library_ = library;

    // Gone, or of another type now: out of the graph.
    for (auto it = nodes_.begin(); it != nodes_.end();) {
        const Node* n = net.node(it->first);
        if (n && n->type == it->second.type) {
            ++it;
            continue;
        }
        graph_.remove(it->second.node->name());
        it = nodes_.erase(it);
    }
    // New: made.
    for (const Node& n : net.nodes()) {
        const NodeType* t = findNodeType(n.type);
        if (!t || !t->core || nodes_.count(n.id)) continue;
        pg::Node* made = graph_.create(t->core, "n" + std::to_string(n.id));
        if (made) {
            Mirror m;
            m.node = made;
            m.type = n.type;
            nodes_[n.id] = std::move(m);
        }
    }

    // The parameters, set -- only those that changed dirty anything. One
    // that changes with the frame -- keys, or an expression of $F or of a
    // parameter that changes -- is bound as an expression of the core,
    // evaluated on a copy of the network as it is now.
    std::shared_ptr<const Network> snapshot;
    struct Body {
        pg::Node* node;
        std::shared_ptr<const Network> net;
        int output;
    };
    std::vector<Body> bodies;  // loops whose body changed: handed over after
    for (auto& [id, m] : nodes_) {
        const Node& n = *net.node(id);
        // The type's parameters and those the node's snippet asks for.
        const std::vector<const ParamDef*> defs = net.params(id);
        m.bypass = n.bypass;
        m.file.clear();
        std::vector<std::string> varying;
        for (const ParamDef* d : defs) {
            if (!isText(d->kind) && net.varies(id, d->name)) varying.push_back(d->name);
        }
        // An expression may read another node, which may have changed.
        const bool reads = !varying.empty() && !n.exprs.empty();
        const bool rebind = varying != m.varying || n.keys != m.keys || n.exprs != m.exprs ||
                            (reads && net.revision() != m.revision);
        if (rebind && !varying.empty() && !snapshot) snapshot = std::make_shared<const Network>(net);
        m.node->editParams([&](ParamSet& p) {
            bool changed = false;
            if (rebind) {
                changed = true;
                for (const ParamDef* dp : defs) {
                    const ParamDef& d = *dp;
                    const std::string name = d.name;
                    p.setExpression(name, {});
                    for (const char* c : {".x", ".y", ".z"}) p.setExpression(name + c, {});
                    if (std::find(varying.begin(), varying.end(), name) == varying.end()) continue;
                    const bool vector = d.kind == ParamKind::Vector || d.kind == ParamKind::Color;
                    if (!vector) {
                        p.setExpression(name, [snapshot, id = id, name](const CookContext& ctx) {
                            return static_cast<double>(snapshot->valueAt(id, name, static_cast<float>(ctx.frame))[0]);
                        });
                        continue;
                    }
                    for (size_t c = 0; c < 3; ++c) {
                        p.setExpression(name + (c == 0 ? ".x" : c == 1 ? ".y" : ".z"),
                                        [snapshot, id = id, name, c](const CookContext& ctx) {
                                            return static_cast<double>(snapshot->valueAt(id, name, static_cast<float>(ctx.frame))[c]);
                                        });
                    }
                }
                m.varying = varying;
                m.keys = n.keys;
                m.exprs = n.exprs;
                m.revision = net.revision();
            }
            for (const ParamDef* dp : defs) {
                const ParamDef& d = *dp;
                // An expression that does not change is a value: frame 1's, as any.
                const ParamValue v = net.hasExpression(id, d.name) ? net.valueAt(id, d.name, 1.0f) : net.param(id, d.name);
                const std::string name = d.name;
                switch (d.kind) {
                    case ParamKind::Float: changed |= p.setFloat(name, v[0]); break;
                    case ParamKind::Int:
                    case ParamKind::Choice: changed |= p.setInt(name, static_cast<int>(v[0])); break;
                    case ParamKind::Toggle: changed |= p.setBool(name, v[0] != 0.0f); break;
                    case ParamKind::Vector:
                    case ParamKind::Color: changed |= p.setVec3(name, Vec3(v[0], v[1], v[2])); break;
                    case ParamKind::Text:
                    case ParamKind::Code: changed |= p.setString(name, net.text(id, d.name)); break;
                    case ParamKind::File: {
                        std::string path = net.text(id, d.name);
                        if (!path.empty() && !folder.empty() && fs::path(path).is_relative()) {
                            path = (fs::path(folder) / path).lexically_normal().string();
                        }
                        changed |= p.setString(name, path);
                        changed |= p.setString("stamp", path.empty() ? std::string() : stampOf(path));
                        m.file = path;
                        break;
                    }
                }
            }
            // An asset's instance: which asset, and which definition of it.
            if (const NodeType* t = findNodeType(n.type); t && t->core && std::string_view(t->core) == "asset") {
                changed |= p.setString("asset", n.type);
                const auto def = AssetLibrary::instance().find(n.type);
                changed |= p.setInt("definition", def ? static_cast<int>(def->revision) : 0);
            }
            // A loop's end: how its Begin cuts, and the nodes between them.
            if (n.type == "foreach_end") {
                std::string why;
                const int begin = forEachBegin(net, id, why);
                auto body = std::make_shared<Network>();
                int output = 0;
                if (!begin || !forEachBody(net, begin, id, *body, output, why)) body.reset();
                changed |= p.setString("problem", why);
                if (begin) {
                    changed |= p.setInt("method", static_cast<int>(net.param(begin, "method")[0]));
                    changed |= p.setString("attribute", net.text(begin, "attribute"));
                    changed |= p.setInt("count", static_cast<int>(net.param(begin, "count")[0]));
                }
                std::string text = body ? body->save() : std::string();
                if (text != m.body) {
                    m.body = std::move(text);
                    bodies.push_back({m.node, std::move(body), output});
                }
            }
            return changed;
        });
    }
    for (Body& b : bodies) setForEachBody(*b.node, std::move(b.net), b.output);

    // The wiring: each geometry input in the order of its pins, and of the
    // links into a pin that takes several. First what goes in, for every
    // node -- a bypassed one passes it on -- then the links, around them.
    std::vector<std::tuple<int, size_t, int>> wiring;  // node, input, from
    for (auto& [id, m] : nodes_) {
        m.input = 0;
        const NodeType& t = *findNodeType(net.node(id)->type);
        std::vector<int> sources;
        bool geometryPins = false;
        for (const PinDef& pin : t.inputs) {
            if (pin.type != PinType::Geometry) continue;
            geometryPins = true;
            const std::vector<Link> in = net.linksInto(id, pin.name);
            if (pin.many) {
                for (const Link& l : in) sources.push_back(l.from);
            } else {
                sources.push_back(in.empty() ? 0 : in.front().from);
            }
            if (m.input == 0 && !in.empty()) m.input = in.front().from;
        }
        if (net.node(id)->type == "foreach_end") {
            // The loop cuts what comes into its Begin; the body is its own.
            std::string why;
            const int begin = forEachBegin(net, id, why);
            const std::vector<Link> in = begin ? net.linksInto(begin, "geometry") : std::vector<Link>();
            sources = {in.empty() ? 0 : in.front().from};
        }
        const size_t count = std::max<size_t>(sources.size(), geometryPins ? 1 : 0);
        if (m.node->inputCount() != count) m.node->setInputCount(count);
        for (size_t i = 0; i < count; ++i) wiring.emplace_back(id, i, i < sources.size() ? sources[i] : 0);
    }
    // Unwired first, then wired: turning A -> B into B -> A must not look
    // like a loop halfway through.
    std::vector<std::tuple<pg::Node*, size_t, pg::Node*>> changes;
    for (const auto& [id, index, from] : wiring) {
        const auto src = nodes_.find(resolve(from));
        pg::Node* source = src != nodes_.end() ? src->second.node : nullptr;
        pg::Node* node = nodes_.at(id).node;
        if (node->input(index) != source) changes.emplace_back(node, index, source);
    }
    for (const auto& [node, index, source] : changes) node->setInput(index, nullptr);
    for (const auto& [node, index, source] : changes) {
        if (source) node->setInput(index, source);
    }
}

int GeometryGraph::resolve(int id) const {
    // A bypassed node passes on what comes into it -- and so on up.
    for (int hops = 0; hops < 10000; ++hops) {
        const auto it = nodes_.find(id);
        if (it == nodes_.end()) return 0;
        if (!it->second.bypass) return id;
        id = it->second.input;
    }
    return 0;
}

GeometryPtr GeometryGraph::cook(int id, int frame, float timeStep) { return cook(id, frame, timeStep, nullptr); }

GeometryPtr GeometryGraph::cook(int id, int frame, float timeStep, const std::atomic<bool>* interrupt) {
    if (!nodes_.count(id)) return nullptr;
    const int shown = resolve(id);
    const auto it = nodes_.find(shown);
    if (it == nodes_.end()) return std::make_shared<Geometry>();  // bypassed, and nothing comes in
    // The simulation's frame into the nodes that read it; what comes into
    // the asset into its inputs.
    std::shared_ptr<const Frame> f = frames_ ? frames_(frame) : nullptr;
    for (auto& [nid, m] : nodes_) {
        if (auto* reads = dynamic_cast<FrameNode*>(m.node)) reads->setFrame(frame, f);
        if (m.type == "asset_input") {
            const auto index = static_cast<size_t>(std::clamp(m.node->params().getInt("index", 0), 0, 3));
            setAssetInput(*m.node, index < inputs_.size() ? inputs_[index] : nullptr);
        }
    }
    const double dt = std::max(timeStep, 1e-6f);
    return engine_.cook(*it->second.node, CookContext{static_cast<double>(frame) * dt, frame, 1.0 / dt, interrupt});
}

std::string GeometryGraph::error(int id) const {
    const auto it = nodes_.find(id);
    return it == nodes_.end() ? std::string() : it->second.node->cookError();
}

std::string GeometryGraph::warning(int id) const {
    const auto it = nodes_.find(id);
    return it == nodes_.end() ? std::string() : it->second.node->cookWarning();
}

std::string GeometryGraph::log(int id) const {
    const auto it = nodes_.find(id);
    return it == nodes_.end() ? std::string() : it->second.node->cookLog();
}

}  // namespace pg::sim
