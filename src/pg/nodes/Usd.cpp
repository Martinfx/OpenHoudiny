// USD Import: a USD file's geometry at the frame cooked (pg/usd).
#include "pg/nodes/Nodes.h"

#include "pg/usd/Geom.h"
#include "pg/usd/Stage.h"

#include <algorithm>
#include <mutex>
#include <sstream>

namespace pg {

namespace {

/// The stage of `file` -- composed from every layer it brings in -- at the
/// frame cooked: meshes, curves, points and the implicit shapes, in metres,
/// Y up. `stamp` is whatever says the file changed; a new one reads it again.
class UsdImportNode : public Node {
public:
    explicit UsdImportNode(std::string name) : Node("usdimport", std::move(name)) {
        setInputCount(0);
        params_.setString("file", "");
        params_.setString("stamp", "");
        params_.setString("prims", "");
        params_.setFloat("offset", 0.0f);
        params_.setBool("render", true);
        params_.setBool("proxy", false);
        params_.setBool("guide", false);
        params_.setBool("metres", true);
        params_.setBool("subsets", true);
        params_.setBool("path", true);
        params_.setBool("materials", true);
        params_.setInt("subdivision", 2);
    }

    bool isTimeDependentSelf() const override {
        if (Node::isTimeDependentSelf()) return true;
        std::string error;
        const auto stage = open(error);
        if (!stage) return false;
        // Asked often: the answer holds while the stage and what is read of it do.
        const std::string key = params_.getString("prims", "") + (params_.getBool("render", true) ? "r" : "") +
                                (params_.getBool("proxy", false) ? "p" : "") + (params_.getBool("guide", false) ? "g" : "");
        std::lock_guard<std::mutex> lk(mu_);
        if (stage != variesStage_ || key != variesKey_) {
            variesStage_ = stage;
            variesKey_ = key;
            varies_ = usd::geometryVaries(*stage, options());
        }
        return varies_;
    }

    GeometryPtr cookNode(const CookContext& ctx, std::span<const GeometryPtr>) override {
        std::string error, warning;
        std::shared_ptr<Geometry> geo;
        const auto stage = open(error);
        if (stage) {
            const double time = usd::timeCodeAt(*stage, ctx.frame, ctx.fps, params_.getFloat("offset", 0.0f));
            std::vector<std::string> notes;
            geo = usd::importGeometry(*stage, time, options(), &notes);
            for (const std::string& w : stage->warnings()) notes.push_back(w);
            std::ostringstream o;
            for (size_t i = 0; i < notes.size() && i < 20; ++i) o << (i ? "\n" : "") << notes[i];
            if (notes.size() > 20) o << "\n... and " << notes.size() - 20 << " more";
            warning = o.str();
        }
        if (!geo) geo = std::make_shared<Geometry>();
        std::lock_guard<std::mutex> lk(mu_);
        error_ = error;
        warning_ = warning;
        return geo;
    }

    std::string cookError() const override {
        std::lock_guard<std::mutex> lk(mu_);
        return error_;
    }
    std::string cookWarning() const override {
        std::lock_guard<std::mutex> lk(mu_);
        return warning_;
    }

private:
    std::shared_ptr<const usd::Stage> open(std::string& error) const {
        const std::string file = params_.getString("file", "");
        if (file.empty()) return nullptr;
        return usd::Stage::openCached(file, error);
    }

    usd::ImportOptions options() const {
        usd::ImportOptions o;
        std::istringstream prims(params_.getString("prims", ""));
        for (std::string p; prims >> p;) o.roots.push_back(p);
        o.render = params_.getBool("render", true);
        o.proxy = params_.getBool("proxy", false);
        o.guide = params_.getBool("guide", false);
        o.metresYUp = params_.getBool("metres", true);
        o.subsets = params_.getBool("subsets", true);
        o.pathAttribute = params_.getBool("path", true);
        o.materials = params_.getBool("materials", true);
        o.subdivision = std::clamp(params_.getInt("subdivision", 2), 0, 6);
        return o;
    }

    mutable std::mutex mu_;
    std::string error_, warning_;
    mutable std::shared_ptr<const usd::Stage> variesStage_;
    mutable std::string variesKey_;
    mutable bool varies_ = false;
};

}  // namespace

void registerUsdNodes() {
    NodeRegistry::instance().add("usdimport", [](const std::string& n) { return std::make_unique<UsdImportNode>(n); });
}

}  // namespace pg
