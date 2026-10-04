#pragma once
//
// OBJ files: the plainest way geometry travels between programs -- Blender,
// Houdini, Maya and every other 3D package read and write it.
//
// Read: points (`v`), polygons (`f`, closed) and polylines (`l`, open); a
// corner may be written `a`, `a/b`, `a//c` or `a/b/c`, and a negative number
// counts back from the last point or texture coordinate. Texture
// coordinates (`vt`) the corners name become uv on the vertices, (u, v, 0)
// -- 0 for a corner naming one that is not there.
// Normals, materials, groups and smoothing: skipped. Numbers are read the
// same whatever the locale says a decimal point is.
//
// Written: the points, uv as `vt` -- a corner's or a point's -- then the
// primitives, closed ones as `f`, open ones as `l`. Other attributes and
// volumes do not go into an OBJ.
//
#include "pg/core/Geometry.h"

#include <string>
#include <string_view>

namespace pg::io {

/// Replaces `out` with the geometry in OBJ `text`. False, with the line, for
/// a corner naming a point that is not there.
bool parseObj(std::string_view text, Geometry& out, std::string& error);
bool readObj(const std::string& path, Geometry& out, std::string& error);

/// `geo` as OBJ text.
std::string formatObj(const Geometry& geo);
bool writeObj(const Geometry& geo, const std::string& path, std::string& error);

/// A number as C writes it -- "-1.5e3" -- whatever the locale says a
/// decimal point is. Advances `s` past it; false if there is none.
bool readNumber(const char*& s, const char* end, float& out);

}  // namespace pg::io
