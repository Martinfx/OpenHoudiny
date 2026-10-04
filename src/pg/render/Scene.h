#pragma once
//
// What the path tracer (PathTracer.h) renders: the surfaces of a frame as
// rays meet them, and the world round them -- the sun, the sky, the floor --
// as the Output's look has them (sim/Look.h), through the shot's camera.
//
//   meshes   the displayed geometry's polygons (sim::shadedTriangles: the
//            viewport's normals and colours), the pieces and the cloth of
//            the solvers, the water's surface: triangles in a hierarchy of
//            boxes each -- Embree's (Embree.h), else our own (Bvh.h). A
//            prototype (core/Instances.h) is a mesh once, placed on each
//            point that stands for it -- a meadow of 122 000 clumps costs 8
//            clumps and the placements.
//   grit     the pieces' loose points: chips of stone and slivers of glass
//            (Particles.h), a few shapes placed on each point -- as big as
//            its pscale, turned as it tumbles.
//   rain     the frame's drops and droplets, each the streak it falls in a
//            share of a frame (Look::rainStreak): water, there as much of
//            the time as Look::rainOpacity says, casting no shadow. What it
//            falls on is wet (Scene::wetAt): darker, and shining.
//   shapes   the objects of the scene, met exactly (sim::ShapeInstance), in
//            a hierarchy of our own.
//   floor    the Output's floor at y 0, fading out far away as the
//            viewport's does.
//   gas      the smoke and the fire of the frame, as the Volume Look has
//            them (Gas.h): rays go through it, scattered in the smoke, lit
//            by the flames.
//   plate    what the camera filmed, rendered through that camera (Plate.h):
//            the CG goes over it, the objects that are holdouts and shadow
//            catchers (sim::Matte) -- the floor too -- the real things it
//            shows. Without it, they are drawn as themselves.
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
#include "pg/core/Material.h"
#include "pg/render/Bvh.h"
#include "pg/render/Embree.h"
#include "pg/render/Gas.h"
#include "pg/sim/Camera.h"
#include "pg/sim/Look.h"
#include "pg/sim/Scene.h"

#include <array>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace pg::render {

struct Plate;

struct Material {
    /// Rain: a drop's streak (Particles.h) -- water where a ray meets it from
    /// the front, as much of the time as `opacity` says, its drop there;
    /// passed through the rest of the time, and from behind. It casts no
    /// shadow.
    enum class Kind : uint8_t { Surface, Glass, Water, Rain };
    Kind kind = Kind::Surface;
    float roughness = 0.5f;
    float metallic = 0.0f;
    float translucency = 0.0f;
    float ior = 1.5f;
    float opacity = 1.0f;  ///< Rain: how much of the time its drop is there
    /// What it is made of (s@material): Cycles draws its pattern, both
    /// renderers its photographs (render/Textures.h).
    MaterialPreset preset = MaterialPreset::None;
    /// A texture of its own (the Material node's s@texture): what names the
    /// set; "" for its material's.
    std::string texture;
    /// How many metres one picture of it covers (f@texture_size): 0 as the
    /// set says.
    float textureSize = 0.0f;
    /// Whether the colour Cd tints the pictures (i@texture_tint): -1 as the
    /// set says (TextureSet::tint).
    int8_t textureTint = -1;
    /// Its pictures laid on by the corners' uv (Mesh::uv), a picture a unit
    /// of it -- as the Material node's Projection says, where the triangles
    /// have uv -- not from three sides.
    bool byUv = false;
    /// How strongly the set's normal map bends the light, laid on by uv
    /// (f@texture_normal): 0 not at all.
    float normalStrength = 1.0f;
    /// Laid on by uv, its set has an alpha (TextureSet::alpha): the surface
    /// cut out where the picture has none -- a leaf's edge. Rays and shadows
    /// pass there (Coverage).
    bool cutout = false;

    /// A plain surface, as rough as `roughness`.
    static Material surface(float roughness) {
        Material m;
        m.roughness = roughness;
        return m;
    }
    bool operator==(const Material&) const = default;
};

/// How rough and how metal a surface of `preset` is -- what both renderers
/// make of it; the attributes roughness and metallic, where there are any,
/// before it -- and its colour where the geometry has no Cd (linear light;
/// for a set of photographs, about their own).
struct PresetSurface {
    float roughness = 0.5f;
    float metallic = 0.0f;
    Vec3 color{0.72f, 0.72f, 0.74f};
};
PresetSurface presetSurface(MaterialPreset preset);

/// Triangles for rays -- with our own hierarchy, in the order its leaves
/// take them.
struct Mesh {
    std::vector<Vec3> v0, e1, e2;       ///< a corner and the edges from it, one a triangle
    std::vector<Vec3> normals, colors;  ///< three a triangle
    /// Three a triangle where the points had rest -- where they were before
    /// they moved: what a material's pattern sticks to -- else none.
    std::vector<Vec3> rest;
    /// One a triangle where some of it is a window: a number 0..1 of the
    /// face it is of, the same from frame to frame -- what tells one room
    /// behind the glass from the next; else none.
    std::vector<float> random;
    /// Three a triangle where the points had a velocity v -- how fast each
    /// corner goes, m/s: what the renderers blur it along while the shutter
    /// is open (Settings::shutter) -- else none.
    std::vector<Vec3> velocity;
    /// Three a triangle where a material lays its pictures on by uv
    /// (Material::byUv) and the corners had uv; else none.
    std::vector<Vec2> uv;
    /// Three a triangle with `uv`: the way u goes along the surface at the
    /// corner, a unit long and across its normal -- the mean of the
    /// triangles round a corner of the same place, normal and uv, as
    /// MikkTSpace makes it -- and in w, +1 where v goes along normal x
    /// tangent, -1 where the uv is mirrored. The space a normal map bends the
    /// normal in.
    std::vector<Vec4> tangents;
    /// Seconds either side of now its triangles may be met moving along
    /// their velocity -- half a frame, the longest a shutter is open: its
    /// hierarchy's boxes take them in. 0: met as they are now.
    float sweep = 0.0f;
    std::vector<uint16_t> material;     ///< one a triangle, into `materials`
    std::vector<Material> materials;
    Bvh bvh;                                   ///< our own: its leaves' items are the triangles' numbers
    std::shared_ptr<const EmbreeMesh> embree;  ///< Embree's: its triangles' numbers are ours
    Box box;
    bool clear = false;   ///< some of it is glass or water
    bool cutout = false;  ///< some of it is cut out (Material::cutout)
    bool shadows = true;  ///< it casts shadows -- rain casts none
    size_t count() const { return v0.size(); }
};

/// What each closed polygon of `geo` is made of, as the renderers see it:
/// `out` the materials told apart, `ofPrim` each primitive's (0 for what is
/// not drawn) -- all water with `water`; laid on by uv where asked and
/// `hasUv` (its corners have uv).
void primitiveMaterials(const Geometry& geo, bool water, bool hasUv, std::vector<Material>& out, std::vector<uint16_t>& ofPrim);

/// The mesh of the closed polygons of `geo` -- what stands on its points
/// (instances) left out -- its materials from its attributes; `water`: all
/// of it water. Its hierarchy `engine`'s, taking in where its corners go
/// `sweep` seconds either side of now (Mesh::sweep).
std::shared_ptr<const Mesh> meshOf(const Geometry& geo, bool water = false, RayEngine engine = defaultRayEngine(),
                                   float sweep = 0.0f);

/// A mesh of triangles as they are -- three corners each, a normal at each
/// corner -- all of `material`, of `color`. Its hierarchy `engine`'s.
std::shared_ptr<const Mesh> meshOfTriangles(const std::vector<Vec3>& corners, const std::vector<Vec3>& normals,
                                            const Material& material, const Vec3& color,
                                            RayEngine engine = defaultRayEngine());

/// A mesh where it stands: turned by `axes` (its columns the images of the
/// mesh's axes), `scale` times as big, moved to `at`; its colours times `tint`.
struct Placed {
    uint32_t mesh = 0;
    Vec3 at;
    Mat3 axes{1.0f};
    float scale = 1.0f;
    Vec3 tint{1.0f, 1.0f, 1.0f};
    Vec3 velocity;  ///< m/s, where it flies -- a chip of grit: what Cycles blurs it along

    /// Where it stands `t` seconds from now, flying on.
    Placed movedBy(float t) const {
        Placed q = *this;
        q.at = at + velocity * t;
        return q;
    }

    Vec3 toWorld(const Vec3& p) const { return at + (axes * p) * scale; }
    Vec3 turn(const Vec3& v) const { return axes * v; }
    /// World to the mesh's own space -- by the transpose; directions keep
    /// their length's share, so a ray's t is the same in both.
    Vec3 toLocal(const Vec3& p) const { return ((p - at) * axes) * (1.0f / scale); }
    Vec3 dirToLocal(const Vec3& v) const { return (v * axes) * (1.0f / scale); }
};

/// How much of a surface there is where a ray meets it, as a renderer has
/// its pictures' alpha (Material::cutout): 0 none, 1 all of it.
struct Coverage {
    virtual ~Coverage() = default;
    virtual float at(const Material& m, const Vec2& uv) const = 0;
};

/// Where a ray met a surface.
struct Hit {
    float t = 1e30f;
    Vec3 position;
    Vec3 normal;    ///< as it is shaded: smooth
    Vec3 face;      ///< the surface's own, as its corners turn
    Vec3 color;
    /// Where on its mesh -- where it was before it moved, where the points
    /// have rest -- and the way that face faced there (not of unit length):
    /// what a texture is laid on by (render/Textures.h).
    Vec3 rest, restFace;
    /// Where on its pictures, laid on by uv (Mesh::uv); and the way u goes
    /// there, across the normal (0 for none), v going along normal x
    /// tangent times `handed`.
    Vec2 uv;
    Vec3 tangent;
    float handed = 1.0f;
    Vec3 tint{1.0f, 1.0f, 1.0f};  ///< its copy's (an instance's): in `color` already
    const Material* material = nullptr;
    bool floor = false;
    int solid = -1;  ///< which of the scene's objects (Scene::solids), or -1
};

struct Scene {
    RayEngine engine = RayEngine::Own;
    std::vector<std::shared_ptr<const Mesh>> meshes;
    std::vector<Placed> placed;
    std::vector<sim::Solid> solids;
    std::vector<sim::ShapeInstance> shapes;  ///< the solids' shapes, placed
    Material floorMaterial = Material::surface(0.8f);
    Material solidMaterial = Material::surface(0.6f);
    /// Our own engine: items below placed.size() are placed meshes, the
    /// rest solids. Embree's: the solids alone, item i shapes[i].
    Bvh top;
    std::shared_ptr<const EmbreeScene> embree;  ///< the placed meshes, with Embree's engine
    std::shared_ptr<const Gas> gas;  ///< the smoke and the fire; null without them
    GasLook gasLook;                 ///< ... as the look has them
    Box bounds;  ///< what there is, the floor aside

    // The world round it: the look's light, as the viewport lights it.
    sim::Look look;
    Vec3 sunDirection;      ///< towards the sun, unit
    Vec3 sunLight;          ///< the look's sun: colour times intensity
    Vec3 skyLight;          ///< the look's sky: colour times intensity
    float sunCosine = 1.0f; ///< of the angle from the sun's middle to its rim
    float floorRadius = 1e30f;  ///< where the floor has faded out, from the origin
    sim::Camera camera;
    /// Where the camera is a frame before and a frame after -- what Cycles
    /// blurs a moving camera toward while the shutter is open -- as
    /// `camera` when it stays (SceneInput::cameraBefore).
    sim::Camera cameraBefore, cameraAfter;
    bool cameraMoves = false;
    float frameTime = 1.0f / 30.0f;  ///< seconds from a frame to the next (SceneInput::frameTime)
    float time = 0.0f;          ///< seconds into the shot (SceneInput::time)
    /// Seconds either side of the frame's moment what moves may be met
    /// where it then is -- half a frame, the longest a shutter is open: the
    /// hierarchies take in where it goes so far.
    float sweep = 0.0f;
    bool moving = false;  ///< anything moves: the gas, a mesh, a chip, an object, the camera
    /// What the camera filmed, the CG goes over (SceneInput::plate); null
    /// for none.
    std::shared_ptr<const Plate> plate;
    /// Where the rain wets what it falls on, as the viewport has it: within
    /// the drops' extent in x and z (wetLo to wetHi), drying off some 35 cm
    /// out; how wet there (Look::wetness) -- 0 without rain.
    float wetness = 0.0f;
    Vec2 wetLo, wetHi;

    /// Whether anything moves while a shutter is open.
    bool moves() const { return moving; }
    /// The camera `time` seconds from the frame's moment: on its way to
    /// where it is a frame before (time below 0) or after.
    sim::Camera cameraAt(float time) const;

    /// The first surface a ray from `origin` along the unit `dir` meets
    /// before `tMax`. `fade`, in [0, 1): whether the floor, fading out far
    /// away, is there where the ray meets it -- and, by a number made of
    /// it, whether a surface cut out (Material::cutout) is: as often as
    /// `coverage` says; with none, it is whole. What moves, where it is
    /// `time` seconds from the frame's moment (within `sweep`).
    bool intersect(const Vec3& origin, const Vec3& dir, float tMax, float fade, Hit& hit, float time = 0.0f,
                   const Coverage* coverage = nullptr) const;
    /// How much of the light from along `dir` gets to `origin` from `tMax`
    /// away: 0 behind something opaque; through glass and water, tinted --
    /// as if they did not bend it; past a surface cut out, as much as
    /// `coverage` says is not there.
    Vec3 transmittance(const Vec3& origin, const Vec3& dir, float tMax, float time = 0.0f,
                       const Coverage* coverage = nullptr) const;
    /// As intersect, every surface whole.
    bool intersectWhole(const Vec3& origin, const Vec3& dir, float tMax, float fade, Hit& hit, float time = 0.0f) const;

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
    /// How wet the rain has made a surface at `p` facing `n` (its side seen),
    /// 0 to 1: under it, and facing up -- as the viewport wets the floor.
    float wetAt(const Vec3& p, const Vec3& n) const;
    /// What a surface met is over the plate: an object's matte, the floor's
    /// (Look::floorMatte); Matte::None without a plate -- everything is
    /// then drawn as itself.
    sim::Matte matteOf(const Hit& hit) const;
    /// Whether a holdout or a catcher -- one of the real things the plate
    /// shows -- is in the way along the unit `dir` from `origin` before
    /// `tMax`: what hides the light from a catcher in the real scene.
    bool realBlocks(const Vec3& origin, const Vec3& dir, float tMax, float time = 0.0f) const;
    /// Where solid `s` meets a ray first, `time` seconds from the frame's
    /// moment -- turned and carried as it moves -- and its normal there.
    bool meetSolid(size_t s, const Vec3& origin, const Vec3& dir, float time, float& t, Vec3& normal) const;
};

/// What a frame shows.
struct SceneInput {
    GeometryPtr geometry;  ///< the displayed node's
    GeometryPtr bodies;    ///< the solvers' pieces and cloth (sim::drawnBodies)
    GeometryPtr water;     ///< the water's surface (sim::waterMesh)
    /// The frame whose smoke and fire, and rain, are rendered; null for none.
    std::shared_ptr<const sim::Frame> frame;
    std::vector<sim::Solid> solids;
    sim::Look look;
    sim::Camera camera;
    /// Where the camera is a frame before this one and a frame after, when
    /// it moves -- `cameraMotion`: as the shutter opens it is shutter / 2 of
    /// the way to the one before, as it closes to the one after
    /// (Settings::shutter), and Cycles blurs it between.
    sim::Camera cameraBefore, cameraAfter;
    bool cameraMotion = false;
    /// Seconds from a frame to the next: how far what moves -- its velocity
    /// v -- goes while the shutter is open.
    float frameTime = 1.0f / 30.0f;
    float sunAngle = 0.53f;  ///< degrees across the sun's disc
    Box domain;              ///< the simulations' box, for how far the floor goes
    float time = 0.0f;       ///< seconds into the shot: how far the clouds have drifted
    /// What the camera filmed (loadPlate): when the shot is rendered through
    /// that camera, the CG goes over it. Null: none.
    std::shared_ptr<const Plate> plate;
};

/// Builds scenes, keeping the meshes of the prototypes that live on -- a
/// clump of grass swaying frame after frame is a mesh once. Their rays go
/// through `engine` -- our own when Embree is not available.
class SceneBuilder {
public:
    explicit SceneBuilder(RayEngine engine = defaultRayEngine());
    RayEngine engine() const { return engine_; }
    std::shared_ptr<const Scene> build(const SceneInput& input);

private:
    RayEngine engine_;
    /// The mesh of a prototype: kept while the geometry lives.
    std::shared_ptr<const Mesh> prototype(const GeometryPtr& geo);
    struct Kept {
        std::weak_ptr<const Geometry> geometry;
        std::shared_ptr<const Mesh> mesh;
    };
    std::map<const Geometry*, Kept> kept_;
    /// The gas of the last frame: kept while the frame lives, so that a
    /// render of it from elsewhere does not make it again.
    std::weak_ptr<const sim::Frame> gasFrame_;
    float gasFrameTime_ = 0.0f;
    std::shared_ptr<const Gas> gas_;
    std::mutex mutex_;
};

}  // namespace pg::render
