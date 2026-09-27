#include "pg/sim/ForEach.h"

#include "pg/sim/GeometryGraph.h"

#include <algorithm>
#include <deque>
#include <map>
#include <mutex>
#include <set>

namespace pg::sim {

namespace {

/// The iteration's number, how many there are and its value, on the piece.
void mark(Geometry& g, int iteration, int count) {
    g.detail().create("iteration", AttrType::Int).write<int32_t>()[0] = iteration;
    g.detail().create("numiterations", AttrType::Int).write<int32_t>()[0] = count;
}

void markValue(Geometry& g, int32_t value) { g.detail().create("value", AttrType::Int).write<int32_t>()[0] = value; }

void markValue(Geometry& g, const std::string& value) {
    AttributeArray& a = g.detail().create("value", AttrType::String);
    const int32_t id = a.internString(value);
    a.write<int32_t>()[0] = id;
}

/// `whole` with only the primitives (or points) `keep` marks.
std::shared_ptr<Geometry> keepOnly(const GeometryPtr& whole, const std::vector<uint8_t>& keep, bool prims) {
    auto g = std::make_shared<Geometry>(*whole);
    if (prims) g->deletePrimitives(keep, true);
    else g->deletePoints(keep);
    return g;
}

}  // namespace

std::vector<GeometryPtr> forEachPieces(const GeometryPtr& whole, ForEachMethod method, const std::string& attribute,
                                       int count, size_t limit, std::string& error) {
    std::vector<GeometryPtr> out;
    error.clear();
    if (!whole) return out;
    auto enough = [&] { return limit > 0 && out.size() >= limit; };
    switch (method) {
        case ForEachMethod::Count:
        case ForEachMethod::Feedback: {
            const int n = std::max(count, 0);
            for (int i = 0; i < n && !enough(); ++i) {
                auto g = std::make_shared<Geometry>(*whole);
                mark(*g, i, n);
                markValue(*g, i);
                out.push_back(g);
                if (method == ForEachMethod::Feedback) break;  // the End feeds each the one before
            }
            return out;
        }
        case ForEachMethod::Primitives:
        case ForEachMethod::Points: {
            const bool prims = method == ForEachMethod::Primitives;
            const size_t n = prims ? whole->primitiveCount() : whole->pointCount();
            std::vector<uint8_t> keep(n, 0);
            for (size_t i = 0; i < n && !enough(); ++i) {
                keep[i] = 1;
                auto g = keepOnly(whole, keep, prims);
                keep[i] = 0;
                mark(*g, static_cast<int>(i), static_cast<int>(n));
                markValue(*g, static_cast<int32_t>(i));
                out.push_back(g);
            }
            return out;
        }
        case ForEachMethod::Pieces: break;
    }
    // By an attribute: the primitives' if they have it, else the points'.
    bool prims = true;
    const AttributeArray* a = whole->primitives().find(attribute);
    if (!a) {
        a = whole->points().find(attribute);
        prims = false;
    }
    if (!a || (a->type() != AttrType::Int && a->type() != AttrType::String)) {
        error = a ? "'" + attribute + "' is neither an integer nor a string: it cannot say which piece is which"
                  : "no attribute '" + attribute + "' on the primitives or the points -- a Connectivity node gives one";
        return out;
    }
    const auto ids = a->read<int32_t>();
    const bool text = a->type() == AttrType::String;
    // The pieces in the order of their values: numbers, or strings as they sort.
    std::map<int32_t, std::vector<uint32_t>> byNumber;
    std::map<std::string, std::vector<uint32_t>> byText;
    for (size_t i = 0; i < ids.size(); ++i) {
        if (text) byText[a->stringValue(ids[i])].push_back(static_cast<uint32_t>(i));
        else byNumber[ids[i]].push_back(static_cast<uint32_t>(i));
    }
    const int n = static_cast<int>(text ? byText.size() : byNumber.size());
    std::vector<uint8_t> keep(ids.size(), 0);
    int iteration = 0;
    auto take = [&](const std::vector<uint32_t>& members) {
        for (const uint32_t m : members) keep[m] = 1;
        auto g = keepOnly(whole, keep, prims);
        for (const uint32_t m : members) keep[m] = 0;
        mark(*g, iteration++, n);
        return g;
    };
    if (text) {
        for (const auto& [value, members] : byText) {
            if (enough()) break;
            auto g = take(members);
            markValue(*g, value);
            out.push_back(g);
        }
    } else {
        for (const auto& [value, members] : byNumber) {
            if (enough()) break;
            auto g = take(members);
            markValue(*g, value);
            out.push_back(g);
        }
    }
    return out;
}

namespace {

ForEachMethod methodOf(int m) { return static_cast<ForEachMethod>(std::clamp(m, 0, 4)); }

/// Cooked alone, the first piece: what the body's nodes show while edited.
class ForEachBeginNode : public pg::Node {
public:
    explicit ForEachBeginNode(std::string name) : pg::Node("foreachbegin", std::move(name)) {
        setInputCount(1);
        params_.setInt("method", 0);
        params_.setString("attribute", "class");
        params_.setInt("count", 4);
    }

    GeometryPtr cookNode(const CookContext& ctx, std::span<const GeometryPtr> in) override {
        std::lock_guard<std::mutex> lock(mu_);
        error_.clear();
        if (in.empty() || !in[0]) return std::make_shared<Geometry>();
        const auto pieces = forEachPieces(in[0], methodOf(params_.evalInt("method", ctx, 0)), params_.getString("attribute"),
                                          params_.evalInt("count", ctx, 4), 1, error_);
        return pieces.empty() ? std::make_shared<Geometry>() : pieces.front();
    }

    std::string cookError() const override {
        std::lock_guard<std::mutex> lock(mu_);
        return error_;
    }

private:
    mutable std::mutex mu_;
    std::string error_;
};

/// The loop: the body cooked a piece at a time, what each made put together
/// -- or, for Feedback, each fed what the last made.
class ForEachEndNode : public pg::Node {
public:
    explicit ForEachEndNode(std::string name) : pg::Node("foreachend", std::move(name)) {
        setInputCount(1);
        params_.setInt("method", 0);
        params_.setString("attribute", "class");
        params_.setInt("count", 4);
        params_.setString("problem", "");  // why there is no loop, as the network says
        params_.setString("begin", "");    // the network's: which Begin it closes
    }

    void setBody(std::shared_ptr<const Network> body, int output) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            body_ = std::move(body);
            output_ = output;
            graph_ = std::make_unique<GeometryGraph>();
            varies_ = false;
            if (body_) {
                // Whether the body changes with the frame by itself.
                GeometryGraph probe;
                probe.sync(*body_);
                const pg::Node* out = probe.coreNode(output_);
                varies_ = out && probe.engine().isTimeDependent(*out);
            }
        }
        bumpVersion();
    }

    bool isTimeDependentSelf() const override {
        std::lock_guard<std::mutex> lock(mu_);
        return varies_ || pg::Node::isTimeDependentSelf();
    }

    GeometryPtr cookNode(const CookContext& ctx, std::span<const GeometryPtr> in) override {
        std::lock_guard<std::mutex> lock(mu_);
        error_ = params_.getString("problem");
        if (!error_.empty() || !body_ || in.empty() || !in[0]) return std::make_shared<Geometry>();
        const ForEachMethod method = methodOf(params_.evalInt("method", ctx, 0));
        const int count = params_.evalInt("count", ctx, 4);
        std::vector<GeometryPtr> pieces = forEachPieces(in[0], method, params_.getString("attribute"), count, 0, error_);
        if (!error_.empty()) return std::make_shared<Geometry>();
        const double fps = ctx.fps > 0.0 ? ctx.fps : 30.0;
        auto run = [&](const GeometryPtr& piece, int iteration) -> GeometryPtr {
            graph_->setInputs({piece});
            graph_->sync(*body_);
            GeometryPtr made = graph_->cook(output_, ctx.frame, static_cast<float>(1.0 / fps), ctx.interrupt);
            for (const sim::Node& n : body_->nodes()) {
                const std::string e = graph_->error(n.id);
                if (!e.empty() && error_.empty()) error_ = "iteration " + std::to_string(iteration) + ", " + n.name + ": " + e;
            }
            return made ? made : std::make_shared<Geometry>();
        };
        auto clean = [](std::shared_ptr<Geometry> g) {
            for (const char* name : {"iteration", "numiterations", "value"}) g->detail().erase(name);
            return g;
        };
        if (method == ForEachMethod::Feedback) {
            const int n = std::max(count, 0);
            GeometryPtr last = in[0];
            for (int i = 0; i < n; ++i) {
                if (ctx.interrupted()) return nullptr;
                auto piece = std::make_shared<Geometry>(*last);
                mark(*piece, i, n);
                markValue(*piece, i);
                last = run(piece, i);
            }
            return clean(std::make_shared<Geometry>(*last));
        }
        auto merged = std::make_shared<Geometry>();
        for (size_t i = 0; i < pieces.size(); ++i) {
            if (ctx.interrupted()) return nullptr;
            merged->append(*run(pieces[i], static_cast<int>(i)));
        }
        return clean(merged);
    }

    std::string cookError() const override {
        std::lock_guard<std::mutex> lock(mu_);
        return error_;
    }

private:
    mutable std::mutex mu_;
    std::shared_ptr<const Network> body_;
    int output_ = 0;
    std::unique_ptr<GeometryGraph> graph_;
    bool varies_ = false;
    std::string error_;
};

}  // namespace

void registerForEachNodes() {
    static const bool once = [] {
        auto& r = NodeRegistry::instance();
        r.add("foreachbegin", [](const std::string& n) { return std::make_unique<ForEachBeginNode>(n); });
        r.add("foreachend", [](const std::string& n) { return std::make_unique<ForEachEndNode>(n); });
        return true;
    }();
    (void)once;
}

void setForEachBody(pg::Node& node, std::shared_ptr<const Network> body, int output) {
    if (auto* end = dynamic_cast<ForEachEndNode*>(&node)) end->setBody(std::move(body), output);
}

int forEachBegin(const Network& net, int end, std::string& error) {
    error.clear();
    const std::string named = net.text(end, "begin");
    if (!named.empty()) {
        const Node* n = net.named(named);
        if (!n || n->type != "foreach_begin") {
            error = "no For-Each Begin called '" + named + "'";
            return 0;
        }
        return n->id;
    }
    // The nearest upstream: breadth first, the links in their order.
    std::deque<int> todo;
    std::set<int> seen;
    for (const Link& l : net.linksInto(end, "geometry")) todo.push_back(l.from);
    while (!todo.empty()) {
        const int id = todo.front();
        todo.pop_front();
        if (!seen.insert(id).second) continue;
        const Node* n = net.node(id);
        if (!n) continue;
        if (n->type == "foreach_begin") return id;
        for (const Link& l : net.links()) {
            if (l.to == id) todo.push_back(l.from);
        }
    }
    error = "no For-Each Begin upstream: link one in before the nodes the loop runs";
    return 0;
}

bool forEachBody(const Network& net, int begin, int end, Network& body, int& output, std::string& error) {
    error.clear();
    const std::vector<Link> into = net.linksInto(end, "geometry");
    if (into.empty()) {
        error = "nothing comes into it: the loop's last node goes into it";
        return false;
    }
    output = into.front().from;
    // Everything upstream of what comes in -- but the Begin, and what only feeds it.
    std::set<int> keep;
    std::deque<int> todo = {output};
    bool reaches = false;
    while (!todo.empty()) {
        const int id = todo.front();
        todo.pop_front();
        if (id == begin) {
            reaches = true;
            continue;
        }
        if (!keep.insert(id).second) continue;
        for (const Link& l : net.links()) {
            if (l.to == id) todo.push_back(l.from);
        }
    }
    if (!reaches) {
        error = "its For-Each Begin is not upstream of what comes into it";
        return false;
    }
    body = net;
    body.setAsset({});
    for (const Node& n : net.nodes()) {
        if (!keep.count(n.id) && n.id != begin) body.remove(n.id);
    }
    // The piece comes in where the Begin was.
    const Node* b = net.node(begin);
    const int input = body.add("asset_input", b ? b->x : 0.0f, b ? b->y : 0.0f);
    for (const Link& l : net.links()) {
        if (l.from == begin && keep.count(l.to)) body.connect(input, "geometry", l.to, l.input);
    }
    body.remove(begin);
    return true;
}

}  // namespace pg::sim
