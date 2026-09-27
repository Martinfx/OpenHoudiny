#pragma once
//
// Meshes from files: for scenes not made of balls and boxes alone -- a
// rock, a statue, a car. An OBJ file is read into triangles; a signed
// distance field is baked from them once, on a grid round the mesh, and
// answers what the simulations ask: is a point inside, how far is it from
// the surface. The renderer draws the triangles themselves.
//
// Baking follows Bridson's makelevelset3: the exact distance to the nearest
// triangle at the grid points close to each triangle; fast sweeping carries
// those nearest triangles out to the rest of the grid; inside and outside
// by counting how often rays along x, y and z cross the surface -- an odd
// count is inside -- and taking two votes of three, so that a small hole in
// a mesh does not turn it inside out.
//
// A mesh keeps the units and the place of its file; a Shape::Mesh placed in
// the world (Shape.h) maps the box round it onto the shape's size.
//
#include "pg/core/Types.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace pg::sim {

struct TriangleMesh {
    std::vector<Vec3> positions;
    std::vector<std::array<uint32_t, 3>> triangles;

    /// The box round the vertices the triangles use.
    void bounds(Vec3& lo, Vec3& hi) const;
};

/// OBJ text: vertices (`v`) and faces (`f`, any of `a`, `a/b`, `a//c`,
/// `a/b/c`, negative counting from the end), polygons cut into fans of
/// triangles. Normals, texture coordinates, materials, groups: skipped.
/// False, with the line, for a face that names a vertex not there.
bool parseObj(std::string_view text, TriangleMesh& out, std::string& error);
bool readObj(const std::string& path, TriangleMesh& out, std::string& error);

class MeshShape {
public:
    /// Bakes the distance field of `mesh`, `resolution` cells along the
    /// longest side of the box round it (and a margin).
    explicit MeshShape(TriangleMesh mesh, int resolution = 64);

    const TriangleMesh& mesh() const { return mesh_; }
    /// The middle of the box round the mesh, and half its size -- no side
    /// thinner than a hair, so a flat mesh still has a size.
    const Vec3& center() const { return center_; }
    const Vec3& half() const { return half_; }

    /// Signed distance at a point of the mesh's own space, below 0 inside.
    /// Beyond the grid: the distance to the grid added to its edge's value.
    float distance(const Vec3& p) const;
    /// The nearest triangle a ray (mesh space, `dir` of any length) meets at
    /// t >= tMin, in units of `dir`, and its normal, facing the ray.
    bool intersect(const Vec3& origin, const Vec3& dir, float tMin, float& t, Vec3& normal) const;

    /// The baked grid, for the renderer: points at gridLo + (i, j, k) * cell,
    /// x fastest.
    const std::vector<float>& field() const { return field_; }
    int points(int axis) const { return n_[axis]; }
    const Vec3& gridLo() const { return lo_; }
    float cell() const { return cell_; }

    std::string path;  ///< the file it came from, if any

private:
    size_t index(int i, int j, int k) const {
        return static_cast<size_t>(i) + static_cast<size_t>(n_[0]) * (static_cast<size_t>(j) + static_cast<size_t>(n_[1]) * static_cast<size_t>(k));
    }
    void bake();

    TriangleMesh mesh_;
    Vec3 center_, half_;
    int resolution_ = 64;
    int n_[3] = {0, 0, 0};
    Vec3 lo_;
    float cell_ = 1.0f;
    std::vector<float> field_;
};

/// A mesh from a file, read and baked once while anyone holds it: the same
/// file -- the same path, size and time of change -- gives the same mesh.
/// Null, with why, if the file cannot be read or has no triangles.
std::shared_ptr<const MeshShape> loadMesh(const std::string& path, std::string& error);

}  // namespace pg::sim
