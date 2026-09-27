#include "pg/nodes/Expression.h"
#include "pg/nodes/Nodes.h"

#include <mutex>

namespace pg {
namespace {

/// Runs a snippet of the per-element language over every point.
///
/// The node reports itself time dependent when the snippet reads @Time or
/// @Frame -- derived from the parsed program, not from a checkbox the user has
/// to remember to tick. Getting this wrong in either direction is a classic
/// source of "why is my cache not invalidating" bugs.
class PointWrangleNode : public Node {
public:
    explicit PointWrangleNode(std::string name) : Node("pointwrangle", std::move(name)) {
        setInputCount(1);
        params_.setString("snippet", "");
    }

    bool isTimeDependentSelf() const override {
        if (Node::isTimeDependentSelf()) return true;
        const auto* prog = program();
        if (!prog) return false;
        for (const auto& s : prog->slotNames()) {
            if (s == "Time" || s == "Frame") return true;
        }
        return false;
    }

    GeometryPtr cookNode(const CookContext& ctx, std::span<const GeometryPtr> in) override {
        auto geo = editableCopy(in.empty() ? nullptr : in[0]);
        const expr::Program* prog = program();
        if (!prog) return geo;  // parse error: pass through, error is readable below

        std::string error;
        const bool ok = prog->run(*geo, ctx, error);
        std::lock_guard<std::mutex> lk(mu_);
        runError_ = ok ? std::string() : error;
        return geo;
    }

    /// Empty when the snippet parsed and last ran cleanly.
    std::string cookError() const override {
        program();  // a snippet set since the last cook: parsed now, so its error shows
        std::lock_guard<std::mutex> lk(mu_);
        return !parseError_.empty() ? parseError_ : runError_;
    }

private:
    /// Parses on demand and re-parses only when the snippet text changes.
    const expr::Program* program() const {
        const std::string snippet = params_.getString("snippet", "");
        std::lock_guard<std::mutex> lk(mu_);
        if (snippet != cachedSource_) {
            cachedSource_ = snippet;
            parseError_.clear();
            runError_.clear();
            compiled_ = snippet.empty() ? nullptr : expr::Program::parse(snippet, parseError_);
        }
        return compiled_.get();
    }

    mutable std::mutex mu_;
    mutable std::string cachedSource_ = "\x01unset";  // never a valid snippet
    mutable std::unique_ptr<expr::Program> compiled_;
    mutable std::string parseError_, runError_;
};

}  // namespace

void registerWrangleNodes() {
    NodeRegistry::instance().add("pointwrangle", [](const std::string& n) {
        return std::make_unique<PointWrangleNode>(n);
    });
}

/// Reads back the parse/run error of a `pointwrangle` node, if any.
std::string wrangleError(const Node& node) {
    const auto* w = dynamic_cast<const PointWrangleNode*>(&node);
    return w ? w->cookError() : std::string();
}

}  // namespace pg
