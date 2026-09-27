#pragma once
//
// Rigid bodies: the pieces of something broken -- a Voronoi Fracture's --
// that fall, knock into each other and into what is in the scene, glued to
// the pieces they touch until a pull stronger than the glue tears them
// apart. Jolt Physics (MIT) steps them, on one thread: the same on every
// machine and on any number of threads.
//
//   pieces     the geometry, its primitives numbered by an attribute
//              (piece); each piece a body -- the convex hull of its points,
//              as heavy as its volume at Density
//   glue       pieces that touch, joined where they touch; a joint that
//              pulls harder than Glue Strength breaks for good, and puffs
//              dust
//   colliders  the floor, and the objects linked in: still, or keyed --
//              those push what is in their way
//
// A frame keeps where each piece is, how it is turned and how it moves
// (RigidFrame); the RBD Pieces node puts the pieces there, and the solver's
// look draws them there -- their cut faces in a colour of their own. The
// water, the gas and the rain can take the pieces as colliders that move
// with them, and the gas the dust as smoke.
//
#include "pg/core/Geometry.h"
#include "pg/sim/Scene.h"

#include <memory>
#include <string>
#include <vector>

namespace pg::sim {

struct RigidSettings {
    float density = 2000.0f;          ///< kg per cubic metre
    float friction = 0.6f;
    float bounce = 0.1f;              ///< how much of its speed a piece keeps in a knock
    float glue = 20000.0f;            ///< newtons: a joint pulled harder breaks; 0: no glue
    Vec3 gravity{0.0f, -9.81f, 0.0f};
    int substeps = 2;                 ///< steps of the solver a frame
    bool floor = true;                ///< a floor at y = 0
    float dust = 1.0f;                ///< smoke a broken joint gives off
    float dustSize = 0.3f;            ///< metres: how big a puff is
    float timeStep = 1.0f / 30.0f;    ///< seconds a frame (the World's)

    bool operator==(const RigidSettings&) const = default;
};

struct RigidScene {
    RigidSettings solver;
    /// The pieces as they stand; compared by pointer -- another geometry,
    /// another simulation.
    std::shared_ptr<const Geometry> pieces;
    std::string attribute = "piece";   ///< what says which piece a primitive is of
    std::vector<Collider> colliders;   ///< the objects: still, or moving (velocity, spin)
    bool intoGas = false;              ///< the pieces are colliders of the gas
    bool intoWater = false;            ///< ... of the water
    bool intoRain = false;             ///< ... of the rain
    bool dustIntoGas = false;          ///< broken glue puffs smoke into the gas
    int node = 0;

    /// The settings kept to what the solver can do: no negative density,
    /// friction or glue, a bounce of 0 to 1, 1 to 16 substeps.
    RigidScene sanitized() const;

    bool operator==(const RigidScene&) const = default;
};

/// A piece where it is: its rest points p go to position + rotate(p).
struct RigidPose {
    Vec3 position;
    Vec4 rotation{0.0f, 0.0f, 0.0f, 1.0f};  ///< a unit quaternion: x, y, z, w
    Vec3 velocity;                          ///< of the point at `position`, world units a second
    Vec3 spin;                              ///< the axis it turns about, as long as radians a second

    Vec3 apply(const Vec3& p) const;
    /// How fast the point that rests at `p` goes.
    Vec3 velocityAt(const Vec3& p) const { return velocity + cross(spin, apply(p) - position); }
    bool operator==(const RigidPose& o) const {
        return position == o.position && rotation.x == o.rotation.x && rotation.y == o.rotation.y &&
               rotation.z == o.rotation.z && rotation.w == o.rotation.w && velocity == o.velocity && spin == o.spin;
    }
};

/// The rigid bodies of a frame.
struct RigidFrame {
    std::shared_ptr<const Geometry> pieces;  ///< at rest: the scene's
    std::string attribute;
    std::vector<RigidPose> poses;            ///< piece by piece, in the order of their numbers
    size_t joints = 0, broken = 0;           ///< the glue: how many joints, how many broken so far

    bool empty() const { return poses.empty(); }
    size_t bytes() const { return poses.size() * sizeof(RigidPose); }
};

/// The pieces of `f` where it puts them: points and normals moved and
/// turned, and each point's velocity in v.
std::shared_ptr<Geometry> posedPieces(const RigidFrame& f);

/// How the solver's look draws the pieces: posed, their faces in the colour
/// Cd they have -- `color` where they have none -- and those of the group
/// `insideGroup`, the faces a fracture cut, in `inside`.
std::shared_ptr<Geometry> drawnPieces(const RigidFrame& f, const Vec3& color, const Vec3& inside,
                                      const std::string& insideGroup);

/// For each primitive of `pieces`, which piece it is of -- 0, 1, ... in the
/// order of the values of `attribute` (on the primitives, else the points),
/// or of what touches what when it has none. The count in `count`.
std::vector<int32_t> pieceOfPrimitives(const Geometry& pieces, const std::string& attribute, int& count);

/// Whether this build has rigid bodies (it was built with Jolt).
bool rigidAvailable();

class RigidSolver {
public:
    explicit RigidSolver(const RigidScene& scene);
    ~RigidSolver();
    RigidSolver(const RigidSolver&) = delete;
    RigidSolver& operator=(const RigidSolver&) = delete;

    /// Where the objects are to be at the end of the next step.
    void setColliders(const std::vector<Collider>& colliders);
    void step();

    const RigidScene& scene() const { return scene_; }
    size_t pieceCount() const;
    RigidFrame capture() const;
    /// The pieces as colliders of the water and the gas: meshes that move.
    std::vector<Collider> colliders() const;
    /// Where the glue broke in the last few steps -- and how much dust is
    /// left to puff there, 1 fading to 0.
    std::vector<std::pair<Vec3, float>> dust() const;
    /// Why nothing is simulated, if nothing is.
    const std::string& error() const { return error_; }

private:
    struct Impl;
    RigidScene scene_;
    std::unique_ptr<Impl> impl_;
    std::string error_;
};

}  // namespace pg::sim
