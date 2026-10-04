// Alembic Import: an Alembic file's geometry at the frame cooked (pg/abc).
#include "pg/nodes/Nodes.h"

#include "pg/abc/Geom.h"

#include <mutex>
#include <sstream>

namespace pg {

namespace {

/// The archive of `file` at the frame cooked: meshes, points and curves in
/// the world, as its transforms place them. Frame f is at (f + offset) /
/// fps seconds -- as Houdini, Maya and Blender write frame f. `stamp` is
/// whatever says the file changed; a new one reads it again.
class AbcImportNode : public Node {
public:
    explicit AbcImportNode(std::string name) : Node("abcimport", std::move(name)) {
        setInputCount(0);
        params_.setString("file", "");
        params_.setString("stamp", "");
        params_.setString("objects", "");
        params_.setFloat("offset", 0.0f);
        params_.setBool("hidden", false);
        params_.setBool("facesets", true);
        params_.setBool("path", true);
    }

    bool isTimeDependentSelf() const override {
        if (Node::isTimeDependentSelf()) return true;
        std::string error;
        const auto archive = open(error);
        if (!archive) return false;
        // Asked often: the answer holds while the archive and what is read of it do.
        const std::string key = params_.getString("objects", "");
        std::lock_guard<std::mutex> lk(mu_);
        if (archive != variesArchive_ || key != variesKey_) {
            variesArchive_ = archive;
            variesKey_ = key;
            varies_ = abc::geometryVaries(*archive, options());
        }
        return varies_;
    }

    GeometryPtr cookNode(const CookContext& ctx, std::span<const GeometryPtr>) override {
        std::string error, warning;
        std::shared_ptr<Geometry> geo;
        const auto archive = open(error);
        if (archive) {
            const double fps = ctx.fps > 0.0f ? ctx.fps : 30.0;
            const double time = (static_cast<double>(ctx.frame) + params_.getFloat("offset", 0.0f)) / fps;
            std::vector<std::string> notes;
            geo = abc::importGeometry(*archive, time, options(), &notes);
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
    std::shared_ptr<const abc::ArchiveReader> open(std::string& error) const {
        const std::string file = params_.getString("file", "");
        if (file.empty()) return nullptr;
        return abc::ArchiveReader::openCached(file, error);
    }

    abc::ImportOptions options() const {
        abc::ImportOptions o;
        std::istringstream objects(params_.getString("objects", ""));
        for (std::string p; objects >> p;) o.roots.push_back(p);
        o.hidden = params_.getBool("hidden", false);
        o.faceSets = params_.getBool("facesets", true);
        o.pathAttribute = params_.getBool("path", true);
        return o;
    }

    mutable std::mutex mu_;
    std::string error_, warning_;
    mutable std::shared_ptr<const abc::ArchiveReader> variesArchive_;
    mutable std::string variesKey_;
    mutable bool varies_ = false;
};

}  // namespace

void registerAlembicNodes() {
    NodeRegistry::instance().add("abcimport", [](const std::string& n) { return std::make_unique<AbcImportNode>(n); });
}

}  // namespace pg
