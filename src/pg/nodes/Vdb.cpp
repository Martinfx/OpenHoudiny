// VDB Import: an OpenVDB file's grids as volumes, or the surfaces they hold (pg/io/Vdb.h).
#include "pg/nodes/Nodes.h"

#include "pg/io/Picture.h"
#include "pg/io/Vdb.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <mutex>
#include <sstream>

namespace pg {

namespace {

/// The grids of an OpenVDB file -- of a numbered sequence of them
/// ("smoke.$F4.vdb", "smoke.####.vdb"), the file of the frame cooked plus
/// Offset -- each a volume of its name, a vector grid three (vel.x, vel.y,
/// vel.z). With Surface, the polygons where they hold something instead: a
/// level set where it crosses 0, inside below; anything else where it
/// crosses Iso, inside above -- for an object to collide with, a shape to
/// pour or puff from. `stamp` is whatever says the file changed.
class VdbImportNode : public Node {
public:
    explicit VdbImportNode(std::string name) : Node("vdbimport", std::move(name)) {
        setInputCount(0);
        params_.setString("file", "");
        params_.setString("stamp", "");
        params_.setString("grids", "");
        params_.setInt("offset", 0);
        params_.setBool("zup", false);
        params_.setFloat("maxvoxels", 32.0f);  // millions a volume
        params_.setBool("surface", false);
        params_.setFloat("iso", 0.1f);
    }

    bool isTimeDependentSelf() const override {
        return Node::isTimeDependentSelf() || io::isSequence(params_.getString("file", ""));
    }

    GeometryPtr cookNode(const CookContext& ctx, std::span<const GeometryPtr>) override {
        auto geo = std::make_shared<Geometry>();
        std::string error, warning;
        const std::string pattern = params_.getString("file", "");
        if (!pattern.empty()) {
            const int frame = ctx.frame + params_.getInt("offset", 0);
            const std::string path = io::sequenceFile(pattern, frame);
            io::VdbReadOptions o;
            std::istringstream names(params_.getString("grids", ""));
            for (std::string n; names >> n;) o.grids.push_back(n);
            o.zUp = params_.getBool("zup", false);
            const float millions = params_.evalFloat("maxvoxels", ctx, 32.0f);
            o.maxVoxels = static_cast<size_t>(std::clamp(std::isfinite(millions) ? millions : 32.0f, 0.001f, 4096.0f) * 1e6f);
            io::VdbVolumes read;
            std::error_code ec;
            if (io::isSequence(pattern) && !std::filesystem::exists(path, ec)) {
                // A frame the sequence has no file for -- before the smoke
                // began, after it ended: nothing.
                warning = "no file for frame " + std::to_string(frame) + ": " + path;
            } else if (io::readVdb(path, read, error, o)) {
                if (params_.getBool("surface", false)) {
                    const float iso = params_.evalFloat("iso", ctx, 0.1f);
                    for (size_t i = 0; i < read.volumes.size(); ++i) {
                        if (read.components[i] != 1) continue;  // a vector has no surface
                        const bool levelSet = read.classes[i] == "level set";
                        geo->append(*volumeToMesh(read.volumes[i], levelSet ? 0.0f : iso, levelSet));
                    }
                } else {
                    for (Volume& v : read.volumes) geo->addVolume(std::move(v));
                }
                std::ostringstream notes;
                for (size_t i = 0; i < read.notes.size() && i < 20; ++i) notes << (i ? "\n" : "") << read.notes[i];
                if (read.notes.size() > 20) notes << "\n... and " << read.notes.size() - 20 << " more";
                warning = notes.str();
            }
        }
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
    mutable std::mutex mu_;
    std::string error_, warning_;
};

}  // namespace

void registerVdbNodes() {
    NodeRegistry::instance().add("vdbimport", [](const std::string& n) { return std::make_unique<VdbImportNode>(n); });
}

}  // namespace pg
