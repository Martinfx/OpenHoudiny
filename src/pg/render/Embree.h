#pragma once
//
// Rays through Intel Embree 4 (embree.org, Apache 2.0): its hierarchies of
// boxes built and walked with the machine's SIMD, several times as fast as
// our own (Bvh.h) -- which the path tracer keeps for a build without Embree
// (PG_EMBREE=OFF), a machine it does not run on, and PG_RAYS=own.
//
//   a mesh        its triangles in a scene of Embree's of their own, built
//                 once and kept with the mesh: a clump of grass swaying frame
//                 after frame is built once.
//   the placed    each an instance of its mesh's scene, turned, sized and
//                 moved as it is placed -- a meadow's 122 000 clumps cost
//                 their transforms and a hierarchy over their boxes; a mesh
//                 placed as it is (the terrain, the pieces, the water) is in
//                 the scene itself, a ray not turned to meet it.
//
// The hierarchies are built by the surface area heuristic without spatial
// splits (Embree's medium quality): the same hierarchy however many threads
// build it, so a render is the same however it is run. Robust: no ray slips
// through the edge two triangles share.
//
#include "pg/core/Types.h"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace pg::render {

struct Mesh;
struct Placed;

/// What finds the surfaces rays meet.
enum class RayEngine : uint8_t {
    Embree,  ///< Intel Embree
    Own,     ///< our own hierarchies (Bvh.h)
};

/// Whether the build has Embree and it runs on this machine.
bool embreeAvailable();
/// "Embree 4.3.0", or "" without it.
std::string embreeVersion();
/// Embree when it is available -- unless the environment says PG_RAYS=own.
RayEngine defaultRayEngine();
/// "Embree 4.3.0" or "own BVH": what a scene's rays go through.
std::string rayEngineName(RayEngine engine);

/// A mesh's triangles as Embree holds them.
class EmbreeMesh {
public:
    /// From three corners a triangle; null without Embree.
    static std::shared_ptr<const EmbreeMesh> build(std::span<const Vec3> corners);
    ~EmbreeMesh();
    EmbreeMesh(const EmbreeMesh&) = delete;
    EmbreeMesh& operator=(const EmbreeMesh&) = delete;
    void* scene() const { return scene_; }        ///< its RTCScene, to be instanced
    void* geometry() const { return geometry_; }  ///< its RTCGeometry, to be in a scene as it is

private:
    EmbreeMesh() = default;
    void* scene_ = nullptr;
    void* geometry_ = nullptr;
};

/// Where a ray met a placed mesh: which placement, which triangle of its
/// mesh, how far along the ray and where on the triangle.
struct EmbreeHit {
    uint32_t placed = 0, triangle = 0;
    float t = 0.0f, u = 0.0f, v = 0.0f;
};

/// The placed meshes of a scene as Embree finds rays in them: all of them;
/// the opaque ones -- with neither glass nor water -- for shadows; the clear
/// ones, for shadows through glass and water.
class EmbreeScene {
public:
    /// Over `placed`, each its mesh's Embree mesh (Mesh::embree) where it
    /// stands; a placement whose mesh has none is left out. Null without Embree.
    static std::shared_ptr<const EmbreeScene> build(const std::vector<std::shared_ptr<const Mesh>>& meshes,
                                                    const std::vector<Placed>& placed);
    ~EmbreeScene();
    EmbreeScene(const EmbreeScene&) = delete;
    EmbreeScene& operator=(const EmbreeScene&) = delete;

    /// The nearest triangle a ray meets between `tMin` and `tMax`.
    bool nearest(const Vec3& origin, const Vec3& dir, float tMin, float tMax, EmbreeHit& hit) const;
    /// Whether an opaque mesh is in a ray's way before `tMax`.
    bool blocked(const Vec3& origin, const Vec3& dir, float tMax) const;
    /// The nearest triangle of a clear mesh -- one with glass or water,
    /// which may have opaque triangles too -- between `tMin` and `tMax`.
    bool nearestClear(const Vec3& origin, const Vec3& dir, float tMin, float tMax, EmbreeHit& hit) const;
    bool anyClear() const { return clear_.scene != nullptr; }

    /// A scene of Embree's over some of the placed: its geometries' numbers
    /// among them.
    struct Top {
        void* scene = nullptr;  // RTCScene
        std::vector<uint32_t> placed;
    };

private:
    EmbreeScene() = default;
    Top all_, opaque_, clear_;  // opaque_ shares all_'s scene when nothing is clear
};

}  // namespace pg::render
