#pragma once
//
// What the path tracer (PathTracer.h) renders: the surfaces of a frame as
// rays meet them, and the world round them -- the sun, the sky, the floor --
// as the Output's look has them (sim/Look.h), through the shot's camera.
//
//   meshes   the displayed geometry's polygons (sim::shadedTriangles: the
//            viewport's normals and colours), the pieces and the cloth of
//            the solvers, the water's surface: triangles in a hierarchy of
//            boxes (Bvh.h) each. A prototype (core/Instances.h) is a mesh
//            once, placed on each point that stands for it -- a meadow of
//            122 000 clumps costs 8 clumps and the placements.
//   shapes   the objects of the scene, met exactly (sim::ShapeInstance).
//   floor    the Output's floor at y 0, fading out far away as the
//            viewport's does.
//
// What a surface is made of comes from attributes of its geometry -- of its
// primitive, else its first point, else the detail -- as in Houdini:
//
//   Cd            its colour
//   roughness     0 a mirror, 1 matt (0.5)
//   metallic      1 a metal: its colour tints what it reflects (0)
//   translucency  the share of the light it scatters that goes through it:
//                 a leaf, a blade of grass, paper (0)
//   glass         1 a pane of glass, 2 a crack in it (as the viewport has it)
//
// The water is water: clear, bending light as water does, taking on the
// Water Look's colour with depth.
//
#include "pg/core/Geometry.h"
#include "pg/render/Bvh.h"
#include "pg/sim/Camera.h"
#include "pg/sim/Look.h"
#include "pg/sim/Scene.h"

#include <array>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace pg::render {

struct Material {
    enum class Kind : uint8_t { Surface, Glass, Water };
    Kind kind = Kind::Surface;
    float roughness = 0.5f;
    float metallic = 0.0f;
    float translucency = 0.0f;
    float ior = 1.5f;

    bool operator==(const Material&) const = default;
};

/// Triangles for rays: in the order the leaves of their hierarchy take them.
struct Mesh {
    std::vector<Vec3> v0, e1, e2;       ///< a corner and the edges from it, one a triangle
    std::vector<Vec3> normals, colors;  ///< three a triangle
    std::vector<uint16_t> material;     ///< one a triangle, into `materials`
    std::vector<Material> materials;
    Bvh bvh;                            ///< its leaves' items are the triangles' numbers
    Box box;
    size_t count() const { return v0.size(); }
};

/// The mesh of the closed polygons of `geo` -- what stands on its points
/// (instances) left out -- its materials from its attributes; `water`: all
/// of it water.
std::shared_ptr<const Mesh> meshOf(const Geometry& geo, bool water = false);

/// A mesh where it stands: turned by `turn` (columns: the images of its
/// axes), `scale` times as big, moved to `at`; its colours times `tint`.
struct Placed {
    uint32_t mesh = 0;
    Vec3 at;
    Vec3 x{1.0f, 0.0f, 0.0f}, y{0.0f, 1.0f, 0.0f}, z{0.0f, 0.0f, 1.0f};
    float scale = 1.0f;
    Vec3 tint{1.0f, 1.0f, 1.0f};

    Vec3 toWorld(const Vec3& p) const { return at + (x * p.x + y * p.y + z * p.z) * scale; }
    Vec3 turn(const Vec3& v) const { return x * v.x + y * v.y + z * v.z; }
    /// World to the mesh's own space; directions keep their length's share,
    /// so a ray's t is the same in both.
    Vec3 toLocal(const Vec3& p) const {
        const Vec3 d = p - at;
        return Vec3(dot(x, d), dot(y, d), dot(z, d)) * (1.0f / scale);
    }
    Vec3 dirToLocal(const Vec3& v) const { return Vec3(dot(x, v), dot(y, v), dot(z, v)) * (1.0f / scale); }
};

/// Where a ray met a surface.
struct Hit {
    float t = 1e30f;
    Vec3 position;
    Vec3 normal;    ///< as it is shaded: smooth
    Vec3 face;      ///< the surface's own, as its corners turn
    Vec3 color;
    const Material* material = nullptr;
    bool floor = false;
};

struct Scene {
    std::vector<std::shared_ptr<const Mesh>> meshes;
    std::vector<Placed> placed;
    std::vector<sim::Solid> solids;
    std::vector<sim::ShapeInstance> shapes;  ///< the solids' shapes, placed
    Material floorMaterial{Material::Kind::Surface, 0.8f, 0.0f, 0.0f, 1.5f};
    Material solidMaterial{Material::Kind::Surface, 0.6f, 0.0f, 0.0f, 1.5f};
    Bvh top;     ///< items below placed.size() are placed meshes, the rest solids
    Box bounds;  ///< what there is, the floor aside

    // The world round it: the look's light, as the viewport lights it.
    sim::Look look;
    Vec3 sunDirection;      ///< towards the sun, unit
    Vec3 sunLight;          ///< the look's sun: colour times intensity
    Vec3 skyLight;          ///< the look's sky: colour times intensity
    float sunCosine = 1.0f; ///< of the angle from the sun's middle to its rim
    float floorRadius = 1e30f;  ///< where the floor has faded out, from the origin
    sim::Camera camera;

    /// The first surface a ray from `origin` along the unit `dir` meets
    /// before `tMax`. `fade`, in [0, 1): whether the floor, fading out far
    /// away, is there where the ray meets it.
    bool intersect(const Vec3& origin, const Vec3& dir, float tMax, float fade, Hit& hit) const;
    /// How much of the light from along `dir` gets to `origin` from `tMax`
    /// away: 0 behind something opaque; through glass and water, tinted --
    /// as if they did not bend it.
    Vec3 transmittance(const Vec3& origin, const Vec3& dir, float tMax) const;

    /// The light of the sky from along the unit `dir`: the look's sky --
    /// with Sky Behind, hazy towards the horizon and glowing round the sun,
    /// as the viewport draws it behind everything.
    Vec3 sky(const Vec3& dir) const;
    /// What is behind it all, seen straight from the camera; `up` 0 at the
    /// bottom of the picture, 1 at the top -- a studio's backdrop without
    /// Sky Behind.
    Vec3 background(const Vec3& dir, float up) const;
    /// The sun's light, per unit of solid angle, from within its disc.
    Vec3 sunRadiance() const;
};

/// What a frame shows.
struct SceneInput {
    GeometryPtr geometry;  ///< the displayed node's
    GeometryPtr bodies;    ///< the solvers' pieces and cloth (sim::drawnBodies)
    GeometryPtr water;     ///< the water's surface (sim::waterMesh)
    std::vector<sim::Solid> solids;
    sim::Look look;
    sim::Camera camera;
    float sunAngle = 0.53f;  ///< degrees across the sun's disc
    Box domain;              ///< the simulations' box, for how far the floor goes
};

/// Builds scenes, keeping the meshes of the prototypes that live on -- a
/// clump of grass swaying frame after frame is a mesh once.
class SceneBuilder {
public:
    std::shared_ptr<const Scene> build(const SceneInput& input);

private:
    /// The mesh of a prototype: kept while the geometry lives.
    std::shared_ptr<const Mesh> prototype(const GeometryPtr& geo);
    struct Kept {
        std::weak_ptr<const Geometry> geometry;
        std::shared_ptr<const Mesh> mesh;
    };
    std::map<const Geometry*, Kept> kept_;
    std::mutex mutex_;
};

}  // namespace pg::render
