#pragma once
//
// USD stages as text (.usda) -- what Houdini, Blender, Maya, usdview and
// the renderers read a whole shot from -- written without the library.
//
// A stage is a tree of prims under metadata of its own (which prim is the
// default one, which way is up, how long a unit is, the time codes). A prim
// has a type (Xform, Mesh, Points, Camera...), attributes and
// relationships; an attribute a value -- the default -- or a value for each
// time code it takes a new one at (time samples), or both. Values are
// written as USD writes them: numbers as short as reads back the same
// float, vectors in parentheses, arrays in brackets, asset paths between
// @s, paths between <>.
//
#include "pg/core/Geometry.h"
#include "pg/core/Types.h"

#include <cstdint>
#include <iosfwd>
#include <list>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace pg::io::usda {

// Values as text.
std::string number(float x);
std::string tuple(const Vec3& v);                              ///< (x, y, z)
std::string quat(const Vec4& q);                               ///< x, y, z, w as USD has it: (w, x, y, z)
std::string quoted(const std::string& s);                      ///< "s", its quotes and backslashes escaped
std::string tuples(std::span<const Vec3> v);                   ///< [(x, y, z), ...]
std::string numbers(std::span<const float> v);                 ///< [a, b, ...]
std::string integers(std::span<const int32_t> v);              ///< [1, 2, ...]
std::string tokens(const std::vector<std::string>& v);         ///< ["a", "b"]
std::string asset(const std::string& path);                    ///< @path@
/// A name USD takes for a prim: letters, digits and _, not starting with a
/// digit; anything else becomes _. Empty: "_".
std::string identifier(const std::string& name);

struct Attribute {
    std::string type;                                      ///< "point3f[]", "token", "double3"...
    std::string name;                                      ///< "points", "xformOp:orient"...
    std::string value;                                     ///< the default as text; empty: none
    std::vector<std::pair<double, std::string>> samples;   ///< time code and value, in order
    std::string metadata;                                  ///< inside the parentheses: interpolation = "vertex"
    bool uniform = false;                                  ///< the same at every time
};

struct Prim {
    std::string type;                   ///< "Xform", "Mesh"...; empty: a typeless prim
    std::string name;                   ///< an identifier
    std::vector<std::string> metadata;  ///< lines in the parentheses: prepend apiSchemas = [...]
    std::vector<Attribute> attributes;
    std::vector<std::pair<std::string, std::string>> relationships;  ///< name and target(s): </World/x>
    /// A list: a child held by reference stays where it is as more are added.
    std::list<Prim> children;

    Prim() = default;
    Prim(std::string type, std::string name) : type(std::move(type)), name(std::move(name)) {}
    /// An attribute with a default value.
    Attribute& set(const std::string& type, const std::string& name, std::string value);
    /// ... uniform: it does not change over time.
    Attribute& setUniform(const std::string& type, const std::string& name, std::string value);
    Prim& child(std::string type, std::string name);
    void relate(const std::string& name, const std::string& target) { relationships.emplace_back(name, target); }
};

struct Stage {
    std::vector<std::pair<std::string, std::string>> metadata;  ///< name and value as text
    std::list<Prim> prims;

    /// The whole file.
    std::string text() const;
    void write(std::ostream& out) const;
};

/// Writes `stage` to `path`. False, with why, when the file cannot be written.
bool writeStage(const Stage& stage, const std::string& path, std::string& error);

// --- Values that change ---------------------------------------------------------------------

/// "interpolation = \"vertex\"": the metadata of a primvar.
std::string interpolation(const char* how);
/// An attribute with a value a frame -- time code and text: the default
/// when they are all alike, else a time sample each.
void animate(Prim& prim, const std::string& type, const std::string& name,
             const std::vector<std::pair<int, std::string>>& values, const std::string& metadata = {});

/// A box round points; its extent as USD has it: [(lo), (hi)].
struct Bounds {
    Vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
    void grow(const Vec3& p);
    void grow(const Bounds& b);
    bool empty() const { return lo.x > hi.x; }
    /// float3[] extent, `pad` wider on every side; a point at the origin when empty.
    std::string extent(float pad = 0.0f) const;
};

// --- Geometry ---------------------------------------------------------------------------------
//
// A frame of geometry as the text of its prims: closed primitives a Mesh,
// open ones BasisCurves (linear), loose points -- those no primitive uses
// -- Points. Colour from Cd -- of the vertex, else the point, the
// primitive, the detail, else the viewport's grey -- as displayColor: one
// for all, one a face, or one a corner, as it varies. Normals from the
// points' N; points as wide as pscale (else 2 cm), their v as velocities
// and id as ids. Meshes are polygons as they are: subdivisionScheme none.

struct MeshText {
    std::string points, counts, indices, colors, colorHow, normals, extent;
    std::vector<int32_t> inside;  ///< the faces in the group asked for, numbered as the Mesh has them
    bool empty() const { return counts.size() <= 2; }
};
/// The closed primitives `prims` of `geo`, their points less `middle`.
/// `scratch`: kept between calls, so that many small meshes cost little.
MeshText meshText(const Geometry& geo, std::span<const uint32_t> prims, const Vec3& middle, const Group* inside,
                  std::vector<int32_t>& scratch);

struct CurvesText {
    std::string points, counts, colors, extent;
    bool empty() const { return counts.size() <= 2; }
};
CurvesText curvesText(const Geometry& geo);

struct PointsText {
    std::string points, widths, colors, velocities, ids, extent;
    bool empty() const { return points.size() <= 2; }
};
PointsText pointsText(const Geometry& geo);

/// The prims of frames of it: what stays the same once, the rest a sample a frame.
Prim meshPrim(const std::string& name, const std::vector<std::pair<int, MeshText>>& frames);
Prim curvesPrim(const std::string& name, const std::vector<std::pair<int, CurvesText>>& frames);
Prim pointsPrim(const std::string& name, const std::vector<std::pair<int, PointsText>>& frames);

/// Geometry at frames -- one frame: none of it animated -- as an Xform
/// `name` over the mesh, curves and points it has.
Prim geometryPrim(const std::string& name, const std::vector<std::pair<int, const Geometry*>>& frames);

/// `geo` alone as a stage: Y up, a unit a metre, the prim /`name` its
/// default -- what writeGeometry() writes to .usda.
Stage geometryStage(const Geometry& geo, const std::string& name);

}  // namespace pg::io::usda
