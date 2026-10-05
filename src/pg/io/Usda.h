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
#include "pg/io/MaterialX.h"

#include <cstdint>
#include <functional>
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
std::string pairs(std::span<const Vec2> v);                    ///< [(u, v), ...]
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
    /// "def" defines it; "over" only adds to a prim another layer defines --
    /// what a layer of one frame's values has.
    std::string specifier = "def";
    /// Entries in the parentheses: prepend apiSchemas = [...]; one may take
    /// several lines (a dictionary), each indented as it sits.
    std::vector<std::string> metadata;
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

// --- Value clips ------------------------------------------------------------------------------
//
// A prim's values from other layers, one a frame (USD's value clips): the
// stage keeps what does not change and names the layers; each layer has the
// prim's time samples at its frame. A manifest -- a layer of its own --
// declares which attributes they give; a layer without samples for one
// leaves it without a value at that frame.

/// The clips entry of a prim's metadata: `assets` (as the stage finds them)
/// active from their `frames` on, each at its own time; attributes as the
/// layers have them at `primPath`, declared in `manifest`.
std::string clips(const std::vector<std::pair<int, std::string>>& assets, const std::string& manifest,
                  const std::string& primPath);

// --- Geometry ---------------------------------------------------------------------------------
//
// A frame of geometry as the text of its prims: closed primitives a Mesh,
// open ones BasisCurves (linear), loose points -- those no primitive uses
// -- Points. Colour from Cd -- of the vertex, else the point, the
// primitive, the detail, else the viewport's grey -- as displayColor: one
// for all, one a face, or one a corner, as it varies. Normals from the
// points' N; points as wide as pscale (else 2 cm), their v as velocities
// and id as ids; texture coordinates from uv -- of the corners
// (faceVarying), else of the points -- as primvars:st, what renderers lay
// pictures on by. Meshes are polygons as they are: subdivisionScheme none.

/// An attribute of a frame of geometry as USD has it: its type, its name,
/// its metadata (how a primvar spreads over the prim), its value.
struct Field {
    std::string type, name, metadata, value;
};

/// The points' other attributes -- numbers, whole numbers, vectors; not P,
/// N, v, Cd, pscale or id, which have names of their own in USD -- as
/// primvars a point each: foam as float[] primvars:foam; uv as
/// texCoord2f[] primvars:st. `source`: the points, in the order the prim
/// has them.
std::vector<Field> pointPrimvars(const Geometry& geo, std::span<const uint32_t> source);

struct MeshText {
    std::string points, counts, indices, colors, colorHow, normals, velocities, extent;
    std::string normalsHow;       ///< faceVarying for the corners' normals; "" for the points'
    std::string st, stHow;        ///< the corners' uv, faceVarying; "" for none (the points' are a primvar)
    std::vector<Field> primvars;  ///< pointPrimvars
    std::vector<int32_t> inside;  ///< the faces in the group asked for, numbered as the Mesh has them
    std::vector<uint32_t> faces;  ///< the primitive each face is of
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
    std::vector<Field> primvars;  ///< pointPrimvars
    bool empty() const { return points.size() <= 2; }
};
PointsText pointsText(const Geometry& geo);

/// What of each changes from frame to frame: the attributes of a frame, in
/// the order of their names.
std::vector<Field> fields(const MeshText& m);
std::vector<Field> fields(const CurvesText& c);
std::vector<Field> fields(const PointsText& p);

/// The prims of frames of it: what stays the same once, the rest a sample a frame.
Prim meshPrim(const std::string& name, const std::vector<std::pair<int, MeshText>>& frames);

// --- Materials --------------------------------------------------------------------------------
//
// What the faces of a Mesh are made of -- for each closed primitive, as
// whoever knows the materials says (sim/UsdExport.h) -- as GeomSubsets of
// the Mesh, one a material (familyName materialBind), each bound to it; a
// Mesh all of one is bound to it whole. A material as UsdShade has it: a
// Material of Shaders, a node of its MaterialX graph each (MaterialX.h,
// info:id its node definition), and of a UsdPreviewSurface for what reads
// no MaterialX.

/// The materials a geometry's faces are bound to.
struct FaceMaterials {
    std::vector<std::string> names;    ///< each material's name -- its subset's: bark
    std::vector<std::string> targets;  ///< ... and its path on the stage: </World/Materials/bark>
    std::vector<uint32_t> ofPrim;      ///< each primitive's, an index into names; past the end: unbound
};
/// What says which material each primitive of a geometry is bound to.
using MaterialBinder = std::function<FaceMaterials(const Geometry& geo)>;

/// The faces of `mesh` (as meshText made it) bound to each material of
/// `bound`, numbered as the Mesh has them; as many lists as names.
std::vector<std::vector<int32_t>> facesOf(const MeshText& mesh, const FaceMaterials& bound);

/// `subset` a GeomSubset of a Mesh's faces bound to the material `target`:
/// its elementType, its familyName and its binding (not its indices).
void bindSubset(Prim& subset, const std::string& target);

/// A frame of a Mesh's faces by material.
struct BoundFrame {
    int frame = 0;
    size_t faces = 0;                              ///< how many faces the Mesh has then
    std::vector<std::vector<int32_t>> byMaterial;  ///< the faces of each material (facesOf)
};
/// The Mesh `mesh` bound by frames of its faces' materials -- `materials`
/// their names and paths, as many as each frame's lists: whole to the one
/// material all of its faces have at every frame, else a GeomSubset a
/// material, its faces at each frame (none where it has none).
void bindMesh(Prim& mesh, const std::vector<BoundFrame>& frames,
              const std::vector<std::pair<std::string, std::string>>& materials);

/// The Material `name` at `path` of the nodes of a MaterialX graph -- its
/// surfacematerial its outputs:mtlx:surface -- and of a UsdPreviewSurface
/// graph (categories Usd...) -- its outputs:surface: a Shader a node, its
/// normal maps spelled out (mtlx::portable).
Prim materialPrim(const std::string& name, const std::string& path, const std::vector<mtlx::Node>& nodes);
Prim curvesPrim(const std::string& name, const std::vector<std::pair<int, CurvesText>>& frames);
Prim pointsPrim(const std::string& name, const std::vector<std::pair<int, PointsText>>& frames);

// --- Instances ------------------------------------------------------------------------------
//
// The points that stand for prototypes (pg/core/Instances.h) as a
// PointInstancer: its prototypes -- each as geometryPrim makes it -- in the
// scope Prototypes under it; for each point which it stands for
// (protoIndices), where (positions), how it is turned (orientations) and
// how big (scales), its tint as the primvar tint, a point each, and its id
// as ids.

struct InstancesText {
    std::string indices, positions, orientations, scales, tints, ids, extent;
    bool empty() const { return indices.size() <= 2; }
};
InstancesText instancesText(const Geometry& geo);
std::vector<Field> fields(const InstancesText& t);

/// The scope Prototypes under the PointInstancer `instancer` -- at `path`
/// on the stage -- with each of `prototypes` in it, as geometryPrim makes
/// it (proto_0, proto_1...), and the instancer's relationship to them.
void addPrototypes(Prim& instancer, const std::string& path,
                   const std::vector<std::shared_ptr<const Geometry>>& prototypes, const MaterialBinder& bind = {});
/// A PointInstancer of frames of instances and of `prototypes`, at `path`.
Prim instancerPrim(const std::string& name, const std::string& path,
                   const std::vector<std::shared_ptr<const Geometry>>& prototypes,
                   const std::vector<std::pair<int, InstancesText>>& frames, const MaterialBinder& bind = {});

/// Geometry at frames -- one frame: none of it animated -- as an Xform
/// `name` over the mesh, curves and points it has and its instances, as a
/// PointInstancer "instances" of the prototypes of the first frame that
/// has any. `path`: where the prim is on the stage, which the instancer
/// names its prototypes by; /`name` when empty. With `bind`, its meshes --
/// and its prototypes' -- bound to the materials it says (bindMesh).
Prim geometryPrim(const std::string& name, const std::vector<std::pair<int, const Geometry*>>& frames,
                  const std::string& path = {}, const MaterialBinder& bind = {});

/// `geo` alone as a stage: Y up, a unit a metre, the prim /`name` its
/// default -- what writeGeometry() writes to .usda.
Stage geometryStage(const Geometry& geo, const std::string& name);

}  // namespace pg::io::usda
