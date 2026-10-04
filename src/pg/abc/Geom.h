#pragma once
//
// What an Alembic archive holds as geometry (AbcGeom): its schemas written
// as Alembic's geometry library writes them, and read into the program's
// own.
//
//   Xform     a matrix a sample (Imath's: rows, a point times the matrix,
//             p x M) and whether it is visible; its ops -- translate, rotate,
//             scale, matrix -- read in any mix, the first op the outermost
//   PolyMesh  P, the faces' corner counts and indices, normals (N), uv,
//             velocities and the arbitrary geometry parameters -- Cd among
//             them -- each of a scope: constant, per face (uniform), per
//             point (vertex, varying) or per corner (face-varying)
//   SubD      as a PolyMesh, its creases and corners left out
//   Points    P, ids, velocities, widths
//   Curves    P and the corners of each curve; linear ones as lines, cubic
//             ones by their control points
//   Camera    the lens: focal length (mm), film back and its offsets (cm),
//             clipping, focus, f-stop
//   FaceSet   faces of a mesh by name: primitive groups
//
// Alembic winds faces the other way round from the program (and USD, and
// OpenGL): clockwise seen from the front. They are turned round as they
// are written and read. It keeps no units and no up axis: Y up, as Maya,
// Houdini and Blender write it; Blender turns its Z-up scene so.
//
// A time in an archive is in seconds; the program's frame f at `fps` is at
// f / fps, as Blender, Houdini and Maya write frame f.
//
#include "pg/abc/Archive.h"
#include "pg/core/Geometry.h"

#include <array>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace pg::abc {

/// 4 x 4, as Imath's M44d: rows, p x M; the last row is where it puts the
/// origin.
using Matrix = std::array<double, 16>;
Matrix identity();
/// a, then b: p x a x b.
Matrix multiply(const Matrix& a, const Matrix& b);
Vec3 transformPoint(const Matrix& m, const Vec3& p);
Vec3 transformDirection(const Matrix& m, const Vec3& d);
/// The matrix of a body turned by unit quaternion `q` (x, y, z, w) about
/// the origin and moved to `at`.
Matrix pose(const Vec4& q, const Vec3& at);

// --- Writing ------------------------------------------------------------------------------------

/// The metadata of an object of a schema, as AbcGeom writes it: its
/// schema, its base (AbcGeom_GeomBase_v1 for geometry) and "title" -- the
/// schema and the compound it keeps it in.
MetaData objectMeta(const std::string& schema, const std::string& compound, bool geometry);

/// An Xform object: a matrix and a visibility a sample.
class XformWriter {
public:
    XformWriter(ObjectWriter& parent, const std::string& name, uint32_t timeSampling);
    void sample(const Matrix& m, bool visible = true);
    /// After the last sample: marks it as not always the identity, when it
    /// is not -- what readers look at to skip one that is.
    void finish();
    ObjectWriter& object() { return object_; }

private:
    ObjectWriter& object_;
    CompoundWriter* xform_;
    PropertyWriter *inherits_, *ops_, *vals_, *visible_;
    bool identity_ = true, finished_ = false;
};

/// The arrays of a mesh sample as Alembic keeps them: faces wound
/// clockwise; N and uv a value a corner (face-varying), Cd and v a value a
/// point -- each empty when the mesh has none.
struct MeshSample {
    std::vector<Vec3> P;
    std::vector<int32_t> counts, indices;
    std::vector<Vec3> N;
    std::vector<std::array<float, 2>> uv;
    std::vector<Vec3> v, Cd;
    /// Cd's scope: a point's ("vtx") or a corner's ("fvr" -- a face's colour
    /// is on each of its corners).
    std::string colorScope = "vtx";
    /// Other numbers a point, by name (the water's foam): arbitrary
    /// parameters.
    std::vector<std::pair<std::string, std::vector<float>>> pointFloats;
};

/// The closed polygons of `geo` as a mesh sample (its loose points and
/// lines left out): normals from N (point or corner), uv (point or corner,
/// Vec2 or Vec3), v, Cd (a point's; a corner's, a primitive's or the
/// detail's on the corners), the float point attributes named in `floats`; through `m` when
/// given; of the primitives `prims` alone when not empty.
MeshSample meshSample(const Geometry& geo, const Matrix* m = nullptr, std::span<const uint32_t> prims = {},
                      std::span<const std::string> floats = {});

/// A PolyMesh object: one sample a frame. What a sample lacks that the
/// mesh had -- N, uv, Cd, v -- is empty in it.
class MeshWriter {
public:
    MeshWriter(ObjectWriter& parent, const std::string& name, uint32_t timeSampling);
    void sample(const MeshSample& s);
    ObjectWriter& object() { return object_; }
    size_t samples() const { return samples_; }

private:
    PropertyWriter& arb(PropertyWriter*& p, const std::string& name, Pod pod, uint8_t extent, MetaData meta,
                        CompoundWriter& in);

    ObjectWriter& object_;
    CompoundWriter& geom_;
    CompoundWriter* params_ = nullptr;
    PropertyWriter *bounds_, *P_, *indices_, *counts_;
    PropertyWriter *N_ = nullptr, *uv_ = nullptr, *v_ = nullptr, *Cd_ = nullptr;
    std::vector<std::pair<std::string, PropertyWriter*>> floats_;
    std::string colorScope_;
    uint32_t timeSampling_;
    size_t samples_ = 0;
};

/// Faces of a mesh by name (a FaceSet object under it): the numbers of
/// its faces in the mesh, a sample a frame.
class FaceSetWriter {
public:
    FaceSetWriter(ObjectWriter& mesh, const std::string& name, uint32_t timeSampling);
    void sample(std::span<const int32_t> faces);

private:
    PropertyWriter *faces_, *bounds_;
};

/// A Points object: positions, ids, velocities, widths and colours a
/// sample, as many as there are then.
struct PointsSample {
    std::vector<Vec3> P, v, Cd;
    std::vector<uint64_t> ids;
    std::vector<float> widths;  ///< across, not the radius
};

/// The points of `geo` that no primitive uses, as a points sample: id (else
/// their number), v, pscale (twice it the width), Cd; through `m` when
/// given.
PointsSample pointsSample(const Geometry& geo, const Matrix* m = nullptr);

class PointsWriter {
public:
    PointsWriter(ObjectWriter& parent, const std::string& name, uint32_t timeSampling);
    void sample(const PointsSample& s);
    ObjectWriter& object() { return object_; }

private:
    ObjectWriter& object_;
    CompoundWriter& geom_;
    CompoundWriter* params_ = nullptr;
    PropertyWriter *bounds_, *P_, *ids_, *v_ = nullptr, *widths_ = nullptr, *Cd_ = nullptr;
    uint32_t timeSampling_;
    size_t samples_ = 0;
};

/// A Curves object of linear curves: their points, how many each, widths.
struct CurvesSample {
    std::vector<Vec3> P, Cd;
    std::vector<int32_t> counts;
    std::vector<float> widths;
};

/// The open lines of `geo` as linear curves: a point's width twice its
/// pscale (or its width), Cd a point's; through `m` when given.
CurvesSample curvesSample(const Geometry& geo, const Matrix* m = nullptr);

class CurvesWriter {
public:
    CurvesWriter(ObjectWriter& parent, const std::string& name, uint32_t timeSampling);
    void sample(const CurvesSample& s);

private:
    ObjectWriter& object_;
    CompoundWriter& geom_;
    CompoundWriter* params_ = nullptr;
    PropertyWriter *bounds_, *P_, *counts_, *type_, *widths_ = nullptr, *Cd_ = nullptr;
    uint32_t timeSampling_;
    size_t samples_ = 0;
};

/// A camera's lens, as Alembic's CameraSample keeps it.
struct Lens {
    double focalLength = 35.0;                   ///< mm
    double horizontalAperture = 3.6, verticalAperture = 2.4;  ///< cm
    double horizontalOffset = 0.0, verticalOffset = 0.0;      ///< cm
    double nearClip = 0.1, farClip = 100000.0;
    double focusDistance = 5.0, fStop = 5.6;
    double shutterOpen = 0.0, shutterClose = 0.0;
    std::array<double, 16> core() const;
    static Lens of(const double core[16]);
};

/// A Camera object (under an Xform that places it): its lens a sample.
class CameraWriter {
public:
    CameraWriter(ObjectWriter& parent, const std::string& name, uint32_t timeSampling);
    void sample(const Lens& lens);

private:
    PropertyWriter* core_;
};

// --- Reading ------------------------------------------------------------------------------------

struct ImportOptions {
    /// The objects to read, with what is under them; empty: all.
    std::vector<std::string> roots;
    /// Each primitive's object, as the text attribute `path`.
    bool pathAttribute = true;
    /// Face sets as primitive groups.
    bool faceSets = true;
    /// Read invisible objects too.
    bool hidden = false;
};

/// The world transform of `object` at `time`: its Xforms and those above
/// it (an Xform that does not inherit leaves out the ones above).
Matrix worldTransform(const ArchiveReader& a, const ObjectReader& object, double time);
/// Whether it is visible at `time`: it and every object above it.
bool visible(const ArchiveReader& a, const ObjectReader& object, double time);

/// The geometry of the archive at `time`, in the world: meshes and SubDs
/// with N, uv, Cd, v and their other parameters; points (pscale half their
/// width, id, v); curves as lines. Positions between two samples are
/// blended where the two have as many points. `skipped`: what it read no
/// geometry from, and why.
std::shared_ptr<Geometry> importGeometry(const ArchiveReader& a, double time, const ImportOptions& options,
                                         std::vector<std::string>* skipped = nullptr);
/// Whether what importGeometry() reads may change with time.
bool geometryVaries(const ArchiveReader& a, const ImportOptions& options);
/// The objects importGeometry() reads geometry from.
std::vector<const ObjectReader*> geometryObjects(const ArchiveReader& a, const ImportOptions& options);

/// The archive's cameras, in the order of the tree.
std::vector<const ObjectReader*> cameras(const ArchiveReader& a);
/// A camera at `time`: in the world (looking along its own -z, y up the
/// picture), and its lens.
bool cameraAt(const ArchiveReader& a, const ObjectReader& camera, double time, Matrix& world, Lens& lens,
              std::string& error);
/// Whether it moves or zooms.
bool cameraVaries(const ArchiveReader& a, const ObjectReader& camera);

/// The first and last times the archive's properties have samples at; false
/// for an archive with none that move.
bool timeRange(const ArchiveReader& a, double& first, double& last);

}  // namespace pg::abc
