#include "pg/io/Export.h"

#include "pg/io/Obj.h"
#include "pg/io/Ply.h"
#include "pg/io/Vdb.h"

#include <cctype>
#include <cstdio>
#include <filesystem>

namespace pg::io {

const char* const* geometryExtensions() {
    static const char* const kExtensions[] = {".ply", ".obj", ".vdb", nullptr};
    return kExtensions;
}

bool writeGeometry(const Geometry& geo, const std::string& path, std::string& error) {
    std::string ext = std::filesystem::path(path).extension().string();
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (ext == ".ply") return writePly(geo, path, error);
    if (ext == ".obj") return writeObj(geo, path, error);
    if (ext == ".vdb") {
        if (geo.volumeCount() == 0) {
            error = "the geometry has no volume to write to " + path + " -- a Gas Volume node makes some";
            return false;
        }
        return writeVdb(geo.volumes(), path, error);
    }
    error = path + ": geometry goes to .ply (points), .obj (polygons) or .vdb (volumes)";
    return false;
}

std::string framePath(const std::string& pattern, int frame) {
    char four[16], plain[16];
    std::snprintf(four, sizeof four, "%04d", frame);
    std::snprintf(plain, sizeof plain, "%d", frame);
    std::string out = pattern;
    bool numbered = false;
    for (size_t at = out.find("$F4"); at != std::string::npos; at = out.find("$F4", at)) {
        out.replace(at, 3, four);
        numbered = true;
    }
    for (size_t at = out.find("$F"); at != std::string::npos; at = out.find("$F", at)) {
        out.replace(at, 2, plain);
        numbered = true;
    }
    if (numbered) return out;
    const std::filesystem::path p(pattern);
    return (p.parent_path() / (p.stem().string() + "." + four + p.extension().string())).string();
}

}  // namespace pg::io
