#include "pg/lang/Lang.h"
#include "pg/nodes/Nodes.h"

#include <mutex>

namespace pg {
namespace {

/// What a wrangle's ch() and $F read: its own parameters -- the ones its
/// snippet asks for by name -- and the frame being cooked.
class WrangleHost : public lang::TimeHost {
public:
    WrangleHost(const ParamSet& params, const CookContext& ctx) : TimeHost(ctx), params_(params) {}

    bool channel(std::string_view path, int component, double& out, std::string& error) const override {
        const std::string name(path);
        if (name.find('/') != std::string::npos) {
            error = "a parameter of another node: ch(\"" + name + "\") is for parameter expressions";
            return false;
        }
        const ParamValue* v = params_.value(name);
        if (!v && !params_.hasExpression(name) && !params_.hasExpression(name + ".x")) {
            error = "no parameter '" + name + "'";
            return false;
        }
        if (v && std::holds_alternative<Vec3>(*v)) {
            out = params_.evalVec3(name, ctx_)[std::clamp(component, 0, 2)];
        } else if (v && std::holds_alternative<int>(*v)) {
            out = params_.evalInt(name, ctx_);
        } else if (v && std::holds_alternative<bool>(*v)) {
            out = params_.evalBool(name, ctx_) ? 1.0 : 0.0;
        } else {
            out = params_.evalFloat(name, ctx_);
        }
        return true;
    }

    bool channelText(std::string_view path, std::string& out, std::string& error) const override {
        const ParamValue* v = params_.value(std::string(path));
        if (!v || !std::holds_alternative<std::string>(*v)) {
            error = "no text parameter '" + std::string(path) + "'";
            return false;
        }
        out = std::get<std::string>(*v);
        return true;
    }

private:
    const ParamSet& params_;
};

/// Runs a snippet of the wrangle language over every point, primitive or
/// vertex -- or once over the whole geometry -- of what comes into its
/// first input, reading the other three as it likes.
///
/// It reports itself time dependent when the snippet reads @Time, @Frame,
/// $F or $T -- derived from the parsed program, not from a checkbox the user
/// has to remember to tick -- or when a parameter it reads is animated.
class AttribWrangleNode : public Node {
public:
    AttribWrangleNode(std::string type, std::string name) : Node(std::move(type), std::move(name)) {
        setInputCount(4);
        params_.setString("snippet", "");
        params_.setInt("runover", 0);
        params_.setString("group", "");
    }

    bool isTimeDependentSelf() const override {
        if (Node::isTimeDependentSelf()) return true;
        const auto* prog = program();
        return prog && prog->readsTime();
    }

    GeometryPtr cookNode(const CookContext& ctx, std::span<const GeometryPtr> in) override {
        const GeometryPtr first = in.empty() ? nullptr : in[0];
        const lang::Program* prog = program();
        if (!prog) {
            clearRun();
            return first ? first : std::make_shared<Geometry>();  // the error is read below
        }
        auto geo = editableCopy(first);
        lang::RunOptions o;
        static const AttrClass classes[4] = {AttrClass::Point, AttrClass::Primitive, AttrClass::Vertex, AttrClass::Detail};
        o.runOver = classes[std::clamp(params_.getInt("runover", 0), 0, 3)];
        o.group = params_.getString("group", "");
        for (size_t i = 0; i < 4 && i < in.size(); ++i) o.inputs[i] = in[i];
        if (!o.inputs[0]) o.inputs[0] = std::make_shared<Geometry>();
        o.ctx = ctx;
        const WrangleHost host(params_, ctx);
        o.host = &host;

        std::string error;
        lang::RunReport report;
        const bool ok = prog->run(*geo, o, error, &report);
        std::lock_guard<std::mutex> lk(mu_);
        runError_ = ok ? std::string() : error;
        warnings_.clear();
        for (const std::string& w : report.warnings) warnings_ += (warnings_.empty() ? "" : "\n") + w;
        log_ = std::move(report.log);
        if (!ok) return first ? first : std::make_shared<Geometry>();
        return geo;
    }

    /// Empty when the snippet parsed and last ran cleanly.
    std::string cookError() const override {
        program();  // a snippet set since the last cook: parsed now, so its error shows
        std::lock_guard<std::mutex> lk(mu_);
        return !parseError_.empty() ? parseError_ : runError_;
    }
    std::string cookWarning() const override {
        std::lock_guard<std::mutex> lk(mu_);
        return warnings_;
    }
    std::string cookLog() const override {
        std::lock_guard<std::mutex> lk(mu_);
        return log_;
    }

private:
    void clearRun() {
        std::lock_guard<std::mutex> lk(mu_);
        runError_.clear();
        warnings_.clear();
        log_.clear();
    }

    /// Parses on demand and re-parses only when the snippet text changes.
    const lang::Program* program() const {
        const std::string snippet = params_.getString("snippet", "");
        std::lock_guard<std::mutex> lk(mu_);
        if (snippet != cachedSource_) {
            cachedSource_ = snippet;
            parseError_.clear();
            runError_.clear();
            compiled_ = snippet.empty() ? nullptr : lang::Program::parse(snippet, parseError_);
        }
        return compiled_.get();
    }

    mutable std::mutex mu_;
    mutable std::string cachedSource_ = "\x01unset";  // never a valid snippet
    mutable std::unique_ptr<lang::Program> compiled_;
    mutable std::string parseError_, runError_;
    std::string warnings_, log_;
};

}  // namespace

void registerWrangleNodes() {
    for (const char* type : {"pointwrangle", "attribwrangle"}) {
        NodeRegistry::instance().add(type, [type](const std::string& n) { return std::make_unique<AttribWrangleNode>(type, n); });
    }
}

/// Reads back the parse/run error of a wrangle node, if any.
std::string wrangleError(const Node& node) {
    const auto* w = dynamic_cast<const AttribWrangleNode*>(&node);
    return w ? w->cookError() : std::string();
}

}  // namespace pg
