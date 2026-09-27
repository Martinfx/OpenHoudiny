#include "pg/sim/Asset.h"

#include "pg/sim/GeometryGraph.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

namespace pg::sim {

namespace fs = std::filesystem;

struct EmbeddedExample {
    const char* name;
    const char* text;
};
/// The assets in examples/assets, compiled in by CMake (generated/SimAssets.cpp).
const std::vector<EmbeddedExample>& embeddedAssets();

namespace {

bool isStaticType(std::string_view name) {
    for (const NodeType& t : nodeTypes()) {
        if (name == t.name) return true;
    }
    return false;
}

}  // namespace

// --- the library ---------------------------------------------------------------------------------

AssetLibrary& AssetLibrary::instance() {
    static AssetLibrary library;
    return library;
}

bool AssetLibrary::add(const Network& net, const std::string& file, std::string& error) {
    auto def = build(net, file, error);
    return def && insert(std::move(def), false, error);
}

bool AssetLibrary::addIfNewer(const Network& net, std::string& error) {
    std::string file;
    if (const auto old = find(net.asset().name)) {
        if (old->version >= net.asset().version) return true;
        file = old->file;
    }
    auto def = build(net, file, error);
    return def && insert(std::move(def), true, error);
}

std::shared_ptr<AssetDef> AssetLibrary::build(const Network& net, const std::string& file, std::string& error) {
    const AssetInfo& info = net.asset();
    if (info.name.empty()) {
        error = "not an asset: it has no name (File > Save as Asset gives it one)";
        return nullptr;
    }
    if (!isValidName(info.name)) {
        error = "'" + info.name + "' is not a name for an asset: letters, digits and _";
        return nullptr;
    }
    if (isStaticType(info.name)) {
        error = "'" + info.name + "' is the name of a node the program has: an asset needs another";
        return nullptr;
    }
    auto def = std::make_shared<AssetDef>();
    def->name = info.name;
    def->version = info.version;
    def->file = file;
    auto copy = std::make_shared<Network>(net);
    def->output = copy->displayed();
    if (!def->output) {
        error = info.name + ": no node has the display flag -- the displayed node is what the asset gives";
        return nullptr;
    }
    // The inputs: Asset Input nodes, by their index.
    for (const Node& n : copy->nodes()) {
        if (n.type != "asset_input") continue;
        const auto index = static_cast<size_t>(std::clamp(static_cast<int>(copy->param(n.id, "index")[0]), 0, 3));
        if (def->inputs.size() <= index) def->inputs.resize(index + 1, 0);
        def->inputs[index] = n.id;
    }
    // The type of its instances: its inputs, a geometry out, the promoted parameters.
    NodeType& t = def->type;
    t.name = internText(info.name);
    t.label = internText(info.label.empty() ? info.name : info.label);
    t.category = "Assets";
    t.help = internText(info.help.empty() ? "A digital asset: " + (info.label.empty() ? info.name : info.label) +
                                                ", version " + std::to_string(info.version) + "."
                                          : info.help);
    for (size_t i = 0; i < def->inputs.size(); ++i) {
        t.inputs.push_back({i == 0 ? "geometry" : internText("input" + std::to_string(i)),
                            i == 0 ? "Geometry" : internText("Input " + std::to_string(i)), PinType::Geometry});
    }
    t.outputs.push_back({"geometry", "Geometry", PinType::Geometry});
    for (const Promotion& p : info.promoted) {
        const Node* n = copy->named(p.node);
        const ParamDef* d = n ? copy->paramDef(n->id, p.param) : nullptr;
        if (!d) {
            error = info.name + ": the promoted " + p.node + "." + p.param + " is not there";
            return nullptr;
        }
        ParamDef pd = *d;
        pd.name = internText(p.name);
        pd.label = internText(p.label.empty() ? std::string(d->label) : p.label);
        pd.section = t.label;
        pd.value = copy->param(n->id, p.param);
        if (isText(d->kind)) pd.text = internText(copy->text(n->id, p.param));
        t.params.push_back(std::move(pd));
    }
    t.version = info.version;
    t.bypassable = true;
    t.core = "asset";
    def->net = std::move(copy);
    return def;
}

bool AssetLibrary::insert(std::shared_ptr<AssetDef> def, bool onlyNewer, std::string& error) {
    std::lock_guard<std::mutex> lock(mu_);
    const auto it = defs_.find(def->name);
    if (onlyNewer && it != defs_.end() && it->second->version >= def->version) return true;
    // Checked here, under the lock: of the definitions a cycle would take,
    // the last one added would have been refused.
    std::vector<std::string> seen;
    if (holdsLocked(*def->net, def->name, seen)) {
        error = def->name + ": it has itself inside" +
                (seen.empty() ? std::string() : " (through " + seen.back() + ")") +
                " -- an asset cannot hold an instance of itself";
        return false;
    }
    def->revision = revision_.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (it != defs_.end()) {
        retired_.push_back(it->second);
        it->second = std::move(def);
    } else {
        defs_.emplace(def->name, std::move(def));
    }
    return true;
}

bool AssetLibrary::holdsLocked(const Network& net, std::string_view name, std::vector<std::string>& seen) const {
    for (const Node& n : net.nodes()) {
        if (n.type == name) return true;
    }
    for (const Node& n : net.nodes()) {
        if (std::find(seen.begin(), seen.end(), n.type) != seen.end()) continue;
        const auto it = defs_.find(n.type);
        if (it == defs_.end()) continue;
        seen.push_back(n.type);
        if (holdsLocked(*it->second->net, name, seen)) return true;
    }
    return false;
}

std::shared_ptr<const AssetDef> AssetLibrary::find(std::string_view name) const {
    std::lock_guard<std::mutex> lock(mu_);
    const auto it = defs_.find(name);
    return it == defs_.end() ? nullptr : it->second;
}

bool AssetLibrary::varies(std::string_view name) const {
    const auto def = find(name);
    if (!def) return false;
    const uint64_t now = revision();
    const uint64_t memo = def->variesMemo.load(std::memory_order_acquire);
    if (memo >> 1 == now) return (memo & 1) != 0;
    // Asked of a graph of its network; the assets inside answer the same way
    // (no lock is held, and none holds itself).
    GeometryGraph probe;
    probe.sync(*def->net);
    const pg::Node* out = probe.coreNode(def->output);
    const bool v = out && probe.engine().isTimeDependent(*out);
    def->variesMemo.store(now << 1 | (v ? 1u : 0u), std::memory_order_release);
    return v;
}

std::vector<std::shared_ptr<const AssetDef>> AssetLibrary::all() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<std::shared_ptr<const AssetDef>> out;
    for (const auto& [name, def] : defs_) out.push_back(def);
    return out;
}

int AssetLibrary::loadFolder(const std::string& folder, std::vector<std::string>& errors) {
    std::error_code ec;
    if (!fs::is_directory(folder, ec)) return 0;
    std::vector<fs::path> files;
    for (const auto& entry : fs::directory_iterator(folder, ec)) {
        if (entry.path().extension() == ".pgasset") files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    int count = 0;
    for (const fs::path& p : files) {
        std::ifstream in(p, std::ios::binary);
        std::stringstream text;
        text << in.rdbuf();
        Network net;
        std::string error;
        if (!Network::load(text.str(), net, error) || !add(net, p.string(), error)) {
            errors.push_back(p.string() + ": " + error);
            continue;
        }
        ++count;
    }
    return count;
}

void AssetLibrary::loadDefaults(std::vector<std::string>* errors) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (defaults_) return;
        defaults_ = true;
    }
    std::vector<std::string> sink;
    std::vector<std::string>& out = errors ? *errors : sink;
    for (const EmbeddedExample& e : embeddedAssets()) {
        Network net;
        std::string error;
        if (!Network::load(e.text, net, error) || !add(net, {}, error)) out.push_back(std::string(e.name) + ": " + error);
    }
    for (const std::string& folder : folders()) loadFolder(folder, out);
}

std::vector<std::string> AssetLibrary::folders() {
    std::vector<std::string> out;
    if (const char* env = std::getenv("PROTOTYPE_ASSETS"); env && *env) {
        std::string list = env;
        size_t start = 0;
        while (start <= list.size()) {
            const size_t colon = std::min(list.find(':', start), list.size());
            if (colon > start) out.push_back(list.substr(start, colon - start));
            start = colon + 1;
        }
    }
    out.push_back(userFolder());
    return out;
}

std::string AssetLibrary::userFolder() {
    if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg) return (fs::path(xdg) / "prototype" / "assets").string();
    if (const char* home = std::getenv("HOME"); home && *home) {
        return (fs::path(home) / ".local" / "share" / "prototype" / "assets").string();
    }
    return (fs::temp_directory_path() / "prototype-assets").string();
}

std::vector<const NodeType*> assetTypes() {
    std::vector<const NodeType*> out;
    for (const auto& def : AssetLibrary::instance().all()) out.push_back(&def->type);
    std::sort(out.begin(), out.end(), [](const NodeType* a, const NodeType* b) { return std::string(a->label) < b->label; });
    return out;
}

int collapseToAsset(Network& net, const std::vector<int>& ids, const AssetInfo& info, const std::string& file,
                    Network* def, std::string& error) {
    if (ids.empty()) {
        error = "no node chosen to make an asset of";
        return 0;
    }
    const std::set<int> chosen(ids.begin(), ids.end());
    for (const int id : chosen) {
        const Node* n = net.node(id);
        const NodeType* t = n ? findNodeType(n->type) : nullptr;
        if (!n || !t) {
            error = "no node " + std::to_string(id);
            return 0;
        }
        if (!t->core) {
            error = n->name + " is not a geometry node: only geometry nodes go into an asset";
            return 0;
        }
        if (n->type == "asset_input") {
            error = n->name + " is what comes into the asset it is in: it stays there";
            return 0;
        }
    }
    // What comes in -- a node's output outside feeding one inside -- and
    // what goes on.
    std::vector<std::pair<int, std::string>> sources;
    std::vector<Link> in, out;
    for (const Link& l : net.links()) {
        const bool from = chosen.count(l.from) != 0, to = chosen.count(l.to) != 0;
        if (to && !from) {
            in.push_back(l);
            const std::pair<int, std::string> s{l.from, l.output};
            if (std::find(sources.begin(), sources.end(), s) == sources.end()) sources.push_back(s);
        }
        if (from && !to) out.push_back(l);
    }
    if (sources.size() > 4) {
        error = std::to_string(sources.size()) + " geometries come into them: an asset takes four at most";
        return 0;
    }
    int output = 0;
    for (const Link& l : out) {
        if (output && output != l.from) {
            error = "the geometry of more than one of them goes on: an asset gives one";
            return 0;
        }
        output = l.from;
    }
    if (!output && chosen.count(net.displayed())) output = net.displayed();
    if (!output) {
        // The one whose geometry goes into none of the others.
        std::vector<int> ends;
        for (const int id : chosen) {
            const bool feeds = std::any_of(net.links().begin(), net.links().end(),
                                           [&](const Link& l) { return l.from == id && chosen.count(l.to); });
            if (!feeds) ends.push_back(id);
        }
        if (ends.size() != 1) {
            error = "which of them gives the asset's geometry? Put the display flag on it";
            return 0;
        }
        output = ends.front();
    }

    // The definition: the network without the rest, what comes in from
    // Asset Input nodes to the left.
    Network d = net;
    for (const Node& n : net.nodes()) {
        if (!chosen.count(n.id)) d.remove(n.id);
    }
    AssetInfo asset = info;
    asset.promoted.clear();
    d.setAsset(asset);
    d.setDisplay(output);
    float left = 1e30f, top = 1e30f;
    for (const int id : chosen) {
        left = std::min(left, net.node(id)->x);
        top = std::min(top, net.node(id)->y);
    }
    for (size_t i = 0; i < sources.size(); ++i) {
        const int input = d.add("asset_input", left - 280.0f, top + 90.0f * static_cast<float>(i));
        d.setParam(input, "index", ParamValue{static_cast<float>(i), 0.0f, 0.0f});
        for (const Link& l : in) {
            if (l.from == sources[i].first && l.output == sources[i].second) d.connect(input, "geometry", l.to, l.input);
        }
    }
    if (!AssetLibrary::instance().add(d, file, error)) return 0;

    // In their place, an instance.
    float x = 0.0f, y = 0.0f;
    for (const int id : chosen) {
        x += net.node(id)->x;
        y += net.node(id)->y;
    }
    x /= static_cast<float>(chosen.size());
    y /= static_cast<float>(chosen.size());
    const bool displayed = chosen.count(net.displayed()) != 0;
    for (const int id : chosen) net.remove(id);
    const int instance = net.add(info.name, std::round(x), std::round(y));
    if (!instance) {
        error = "the library has no asset " + info.name;
        return 0;
    }
    for (size_t i = 0; i < sources.size(); ++i) {
        net.connect(sources[i].first, sources[i].second, instance, i == 0 ? "geometry" : "input" + std::to_string(i));
    }
    for (const Link& l : out) net.connect(instance, "geometry", l.to, l.input);
    if (displayed) net.setDisplay(instance);
    if (def) *def = std::move(d);
    return instance;
}

bool writeAsset(const Network& def, const std::string& path, std::string& error) {
    std::error_code ec;
    const fs::path file(path);
    if (file.has_parent_path()) fs::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::binary);
    if (!out || !(out << def.save())) {
        error = path + ": cannot write it";
        return false;
    }
    return true;
}

// --- the nodes of the core -----------------------------------------------------------------------

namespace {

/// What comes into an asset: the instance's input `index`, which the
/// instance hands over before its inside cooks. Outside an asset, nothing.
class AssetInputNode : public pg::Node {
public:
    explicit AssetInputNode(std::string name) : pg::Node("asset_input", std::move(name)) {
        setInputCount(0);
        params_.setInt("index", 0);
    }

    void setGeometry(GeometryPtr g) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (g == geo_) return;
            geo_ = std::move(g);
        }
        bumpVersion();
    }

    GeometryPtr cookNode(const CookContext&, std::span<const GeometryPtr>) override {
        std::lock_guard<std::mutex> lock(mu_);
        return geo_ ? geo_ : std::make_shared<Geometry>();
    }

private:
    std::mutex mu_;
    GeometryPtr geo_;
};

/// An instance of an asset: the definition's network, cooked with the
/// instance's parameters and inputs.
class AssetNode : public pg::Node {
public:
    explicit AssetNode(std::string name) : pg::Node("asset", std::move(name)) {
        setInputCount(4);
        params_.setString("asset", "");
        params_.setInt("definition", 0);
    }

    bool isTimeDependentSelf() const override {
        if (pg::Node::isTimeDependentSelf()) return true;
        return AssetLibrary::instance().varies(params_.getString("asset"));
    }

    GeometryPtr cookNode(const CookContext& ctx, std::span<const GeometryPtr> in) override {
        std::lock_guard<std::mutex> lock(mu_);
        error_.clear();
        const std::string name = params_.getString("asset");
        const auto def = AssetLibrary::instance().find(name);
        if (!def) {
            error_ = "no asset '" + name + "' in the library";
            return std::make_shared<Geometry>();
        }
        if (def != def_) {
            def_ = def;
            inner_ = std::make_unique<Network>(*def->net);
            graph_ = std::make_unique<GeometryGraph>();
            // What is promoted, the instance drives: keys and expressions inside give way.
            for (const Promotion& p : def->net->asset().promoted) {
                const sim::Node* n = inner_->named(p.node);
                const ParamDef* d = n ? inner_->paramDef(n->id, p.param) : nullptr;
                if (!d) continue;
                inner_->clearKeys(n->id, p.param);
                for (const std::string& ch : Network::channels(*d)) inner_->setExpression(n->id, ch, "");
            }
        }
        for (const Promotion& p : def->net->asset().promoted) {
            const sim::Node* n = inner_->named(p.node);
            const ParamDef* d = n ? inner_->paramDef(n->id, p.param) : nullptr;
            if (!d) continue;
            switch (d->kind) {
                case ParamKind::File:
                case ParamKind::Text:
                case ParamKind::Code: inner_->setText(n->id, p.param, params_.getString(p.name)); break;
                case ParamKind::Float: inner_->setParam(n->id, p.param, {params_.evalFloat(p.name, ctx), 0.0f, 0.0f}); break;
                case ParamKind::Int:
                case ParamKind::Choice:
                    inner_->setParam(n->id, p.param, {static_cast<float>(params_.evalInt(p.name, ctx)), 0.0f, 0.0f});
                    break;
                case ParamKind::Toggle:
                    inner_->setParam(n->id, p.param, {params_.evalBool(p.name, ctx) ? 1.0f : 0.0f, 0.0f, 0.0f});
                    break;
                case ParamKind::Vector:
                case ParamKind::Color: {
                    const Vec3 v = params_.evalVec3(p.name, ctx);
                    inner_->setParam(n->id, p.param, {v.x, v.y, v.z});
                    break;
                }
            }
        }
        graph_->setInputs(std::vector<GeometryPtr>(in.begin(), in.end()));
        graph_->sync(*inner_, def->file.empty() ? std::string() : fs::path(def->file).parent_path().string());
        const double fps = ctx.fps > 0.0 ? ctx.fps : 30.0;
        GeometryPtr out = graph_->cook(def->output, ctx.frame, static_cast<float>(1.0 / fps), ctx.interrupt);
        if (ctx.interrupted()) return nullptr;
        // What went wrong inside, node by node.
        for (const sim::Node& n : inner_->nodes()) {
            const std::string e = graph_->error(n.id);
            if (!e.empty()) error_ += (error_.empty() ? "" : "\n") + name + "/" + n.name + ": " + e;
        }
        return out ? out : std::make_shared<Geometry>();
    }

    std::string cookError() const override {
        std::lock_guard<std::mutex> lock(mu_);
        return error_;
    }

private:
    mutable std::mutex mu_;
    std::shared_ptr<const AssetDef> def_;
    std::unique_ptr<Network> inner_;
    std::unique_ptr<GeometryGraph> graph_;
    std::string error_;
};

}  // namespace

void registerAssetNodes() {
    static const bool once = [] {
        auto& r = NodeRegistry::instance();
        r.add("asset", [](const std::string& n) { return std::make_unique<AssetNode>(n); });
        r.add("asset_input", [](const std::string& n) { return std::make_unique<AssetInputNode>(n); });
        return true;
    }();
    (void)once;
}

void setAssetInput(pg::Node& node, GeometryPtr geometry) {
    if (auto* input = dynamic_cast<AssetInputNode*>(&node)) input->setGeometry(std::move(geometry));
}

}  // namespace pg::sim
