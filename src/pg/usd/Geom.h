#pragma once
//
// What a stage holds as geometry and cameras (UsdGeom), read into the
// program's own: transforms from xformOps, meshes, curves, points and the
// implicit shapes as polygons, cameras as their lens and place.
//
// Transforms: a prim's xformOpOrder, each op -- translate, scale, rotate
// about one axis or three in any order, orient, transform, their pivots
// inverted with !invert! -- at the time asked, the last op in the order
// applied first; !resetXformStack! leaves out the parents'. Matrices are
// USD's: rows, a point times the matrix (p x M).
//
// The stage's units and up axis are made the program's: metres (a unit is
// metersPerUnit metres, 0.01 when the stage does not say) and Y up (a
// Z-up stage is turned -90 degrees about X).
//
#include "pg/core/Geometry.h"
#include "pg/usd/Stage.h"

#include <glm/ext/matrix_double4x4.hpp>

#include <array>
#include <memory>
#include <string>
#include <vector>

namespace pg::usd {

/// 4 x 4, row by row, as GfMatrix4d: a point a row on the left, p x M. Kept
/// as GLM's dmat4, whose columns are -- the same numbers in memory -- these
/// rows: GLM's M * p is p x M here.
struct Matrix {
    glm::dmat4 m{1.0};

    double& at(int r, int c) { return m[r][c]; }
    double at(int r, int c) const { return m[r][c]; }
    /// This, then `o`: (p x this) x o.
    Matrix operator*(const Matrix& o) const { return {o.m * m}; }
    Matrix inverse() const;
    double determinant3() const;
    void transformPoint(const double in[3], double out[3]) const;
    void transformDirection(const double in[3], double out[3]) const;

    static Matrix translate(double x, double y, double z);
    static Matrix scale(double x, double y, double z);
    /// About axis 0, 1 or 2 (x, y, z) by `degrees`.
    static Matrix rotate(int axis, double degrees);
    /// A quaternion x, y, z, w (as GfMatrix4d::SetRotate: not normalized).
    static Matrix orient(double x, double y, double z, double w);
};

/// The time code the program's frame `frame` (at `fps`) reads: frame 1 is
/// the stage's startTimeCode (1 when it says none), a frame is
/// timeCodesPerSecond / fps codes on; `offset` shifts it by as many frames.
double timeCodeAt(const Stage& stage, double frame, double fps, double offset = 0.0);

/// The prim's own transform at `time`; `resets`: it leaves its parents' out.
Matrix localTransform(const Stage& stage, const Stage::Prim& prim, double time, bool* resets = nullptr);
/// Its transform to the stage's world at `time`.
Matrix worldTransform(const Stage& stage, const Stage::Prim& prim, double time);
/// True when its transform to the world may change with time.
bool transformVaries(const Stage& stage, const Stage::Prim& prim);
/// From the stage's units and up axis to metres, Y up.
Matrix toMetresYUp(const Stage& stage);

/// Its visibility at `time`: false when it or a prim above it is invisible.
bool visible(const Stage& stage, const Stage::Prim& prim, double time);
/// Its purpose: its own, else the nearest above it that has one; "default".
std::string purpose(const Stage& stage, const Stage::Prim& prim);

struct ImportOptions {
    /// The prims to read, with what is under them; empty: the whole stage.
    std::vector<std::string> roots;
    /// Purposes read besides "default": render (on), proxy, guide.
    bool render = true, proxy = false, guide = false;
    /// Into metres, Y up (toMetresYUp); off: the stage's units and axes.
    bool metresYUp = true;
    /// GeomSubsets of faces as primitive groups, named as the subsets.
    bool subsets = true;
    /// Each primitive's prim, as the string attribute `path`.
    bool pathAttribute = true;
    /// The materials bound to the meshes and shapes, and to their
    /// GeomSubsets' faces (Shade.h), as the program's: each primitive's
    /// material's name as `material`; its pictures -- a MaterialX or
    /// UsdPreviewSurface network -- as `texture` ("stage.usda#/its/path",
    /// render/Textures.h reads them), texture_tint, texture_projection and
    /// texture_size; its roughness and metallic; glass for one light goes
    /// through; its colour, where it is a value, as Cd.
    bool materials = true;
    /// The meshes that are subdivision surfaces -- subdivisionScheme
    /// catmullClark, or loop, taken as it -- as the smooth surfaces they
    /// stand for: this many steps of Catmull-Clark as OpenSubdiv takes
    /// them, their creases and corners as sharp as the file says (and
    /// after, as f@creaseweight on the corners and f@cornerweight on the
    /// points, for a Subdivide to go on with); fewer where the faces would
    /// be too many. Their normals the surface's, on the corners. 0: the
    /// coarse mesh, its normals smooth as the surface's.
    int subdivision = 2;
};

/// The geometry of the stage at `time`, in the world: meshes (with N, uv,
/// Cd, Alpha, v and their other primvars; their materials; subdivision
/// surfaces smooth, ImportOptions::subdivision), curves as polylines, points
/// (pscale from widths, id, v), the implicit shapes -- Cube, Sphere,
/// Cylinder, Cone, Capsule, Plane -- as polygons; the instances of
/// PointInstancers as instances (core/Instances.h): their prototypes the
/// geometry's, read in the instancer's space -- each prototype root's own
/// transform kept, what is above it left out --, and a point each instance,
/// placing its prototype where, turned and as big as the instancer does
/// (P, orient, pscale; id, v and its primvars); the fields of Volumes --
/// OpenVDBAssets, a grid of a VDB file each -- as volumes named as the
/// fields are (io/Vdb.h reads them, placed by their transforms).
/// `skipped`: what it read no geometry from, and why.
std::shared_ptr<Geometry> importGeometry(const Stage& stage, double time, const ImportOptions& options,
                                         std::vector<std::string>* skipped = nullptr);
/// True when what importGeometry() reads may change with time.
bool geometryVaries(const Stage& stage, const ImportOptions& options);
/// The prims importGeometry() reads geometry from (the PointInstancers
/// apart).
std::vector<const Stage::Prim*> geometryPrims(const Stage& stage, const ImportOptions& options);
/// The PointInstancers importGeometry() reads instances of: those in no
/// other's prototypes -- those it reads with them.
std::vector<const Stage::Prim*> instancerPrims(const Stage& stage, const ImportOptions& options);

/// A camera at a time, as the stage has it: in the world (converted, when
/// asked, to metres and Y up), looking along its own -z, y up the picture.
/// The lens: focal length and apertures in the same units (their ratio is
/// the view); the apertures' offsets shift the picture.
struct CameraSample {
    Matrix world;
    double focalLength = 50.0;
    double horizontalAperture = 20.955, verticalAperture = 15.2908;
    double horizontalApertureOffset = 0.0, verticalApertureOffset = 0.0;
    double nearClip = 1.0, farClip = 1000000.0;
    double focusDistance = 0.0, fStop = 0.0;
    bool orthographic = false;
};
bool cameraAt(const Stage& stage, const Stage::Prim& prim, double time, bool metresYUp, CameraSample& out);
bool cameraVaries(const Stage& stage, const Stage::Prim& prim);
/// The stage's cameras, in the order of the tree.
std::vector<const Stage::Prim*> cameras(const Stage& stage);

}  // namespace pg::usd
