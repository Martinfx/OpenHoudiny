#pragma once
//
// Geometry to PLY files (.ply) -- points with their attributes, and faces:
// what Houdini, Blender, MeshLab and CloudCompare read point clouds from.
//
// Written binary, little-endian. A point's attributes become properties by
// the names the other programs know:
//
//   P  -> x y z          N  -> nx ny nz          v -> vx vy vz
//   Cd -> red green blue (bytes, 0 to 255; alpha too for a Vec4 colour)
//   a number -> its name (float, or int)          a vector -> name_x name_y ...
//
// Strings are left out. The closed polygons are faces (vertex_indices).
// Read back -- ASCII or binary little-endian -- the same names give the same
// attributes.
//
#include "pg/core/Geometry.h"

#include <string>
#include <string_view>

namespace pg::io {

std::string formatPly(const Geometry& geo);
bool writePly(const Geometry& geo, const std::string& path, std::string& error);

/// Reads a PLY file's vertices (and their properties) and faces into `geo`,
/// replacing what it held. False, with why, for what is not PLY or is
/// big-endian.
bool parsePly(std::string_view data, Geometry& geo, std::string& error);
bool readPly(const std::string& path, Geometry& geo, std::string& error);

}  // namespace pg::io
