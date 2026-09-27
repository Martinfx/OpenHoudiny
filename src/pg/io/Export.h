#pragma once
//
// Geometry out to the other programs, in the format a file's extension
// names -- and the files of a sequence, a frame each.
//
#include "pg/core/Geometry.h"

#include <string>

namespace pg::io {

/// Writes `geo` to `path` as its extension says: .ply -- points, their
/// attributes, faces (Ply.h); .obj -- points, polygons, lines (Obj.h);
/// .vdb -- volumes (Vdb.h); .usda -- a USD stage of its polygons, lines
/// and points (Usda.h). False, with why, for another extension, or a .vdb
/// of geometry that has no volume.
bool writeGeometry(const Geometry& geo, const std::string& path, std::string& error);
/// The extensions writeGeometry() knows, with their dots.
const char* const* geometryExtensions();

/// The file of frame `frame` of a sequence: "$F4" in `pattern` is the frame
/// with four digits ("smoke.$F4.vdb" -> "smoke.0007.vdb"), "$F" the frame as
/// it is; a pattern with neither gets ".0007" before its extension.
std::string framePath(const std::string& pattern, int frame);

}  // namespace pg::io
