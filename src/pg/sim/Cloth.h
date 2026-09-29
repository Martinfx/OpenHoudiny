#pragma once
//
// Cloth, ropes and soft bodies: points held together by constraints, solved
// as XPBD does it (Macklin, Storey, Lu et al., "Small Steps in Physics
// Simulation", 2019) -- many small substeps a frame, one pass over the
// constraints in each, their stiffness a compliance: a sheet is as stiff
// whatever the number of steps. What Houdini's Vellum does, small enough to
// read.
//
//   cloth     the polygons of the geometry: their edges hold their length
//             (stretch), the diagonals of quads their shape (shear), and the
//             two points across each edge how far apart they are (bend)
//   ropes     open polylines: their segments hold their length, and each
//             point and the one after the next how far apart they are (bend)
//   balloons  closed meshes: with Pressure above 0 the volume each encloses
//             is held at Pressure times what it was at rest -- a ball, a
//             cushion, a soft body
//   pins      points whose attribute pin is 1 do not move by themselves: they
//             go where the geometry has them at each frame -- animated, they
//             carry the cloth
//   tearing   with Tear above 0 an edge stretched that much longer than it
//             was tears: where the torn edges cut the faces round a point
//             apart, the point is split in two, and the cloth opens there; a
//             rope parts; a balloon torn open is cloth. The attribute tear of
//             a point scales how far the edges at it stretch before they do.
//
// A point has the mass of the cloth round it -- Density, kilograms a square
// metre, or a metre of rope -- or its attribute mass. The points collide
// with the floor, with the objects and the pieces of an RBD Solver
// (Colliders), a Thickness away, with friction, and with each other: what
// folds does not pass through itself. A piece gives as the cloth pushes
// it: in the cloth's step it is a body of its weight that the cloth slows,
// stops or throws, substep by substep, and where that leaves it the RBD
// Solver takes on (reactions). The air pushes each triangle along
// its normal -- a closed mesh's only from outside: the wind of the Forces,
// and the flow of a Pyro Solver's gas.
//
// Deterministic (invariant I5): the constraints in colours none of whose
// members share a point, each colour on as many threads as there are; the
// rest -- balloons, what is left over -- in a fixed order.
//
#include "pg/core/Geometry.h"
#include "pg/sim/Scene.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace pg::sim {

class StateReader;
class StateWriter;

struct ClothSettings {
    float density = 0.3f;         ///< kg a square metre of cloth, a metre of rope
    /// How hard an edge holds its length, N/m: 1e4 cotton, barely stretching;
    /// a few hundred rubber.
    float stretch = 1e4f;
    /// How hard a quad holds its shape -- a pull along the bias, N/m: 1
    /// woven cloth that drapes and droops; as much as Stretch a tarp, a
    /// sheet of plastic, paper.
    float shear = 1.0f;
    /// How hard the cloth holds its folds, N/m across an edge: 0.1 silk,
    /// 10 canvas, 1000 cardboard.
    float bend = 1.0f;
    /// Closed meshes: the volume held, as a share of the volume at rest; 0 no
    /// balloon -- a closed mesh is cloth all the same.
    float pressure = 0.0f;
    float thickness = 0.01f;      ///< metres: how far from what it touches the cloth stays
    float friction = 0.4f;
    float damping = 0.5f;         ///< 1/s: how fast its motion dies away of itself
    /// How hard the air pushes it -- still air as it falls, the wind, the
    /// gas's flow: 1 as it would, 0 not at all.
    float airDrag = 1.0f;
    /// How much longer than it was an edge -- a rope's segment -- stretches
    /// before it tears: 0.3 thirty percent; 0 never.
    float tear = 0.0f;
    int substeps = 20;            ///< steps a frame
    bool selfCollision = true;
    bool floor = true;            ///< a floor at y = 0
    Vec3 gravity{0.0f, -9.81f, 0.0f};
    float timeStep = 1.0f / 30.0f;

    bool operator==(const ClothSettings&) const = default;
};

struct ClothScene {
    ClothSettings solver;
    /// The cloth as it rests and starts: the geometry at frame 1. Compared by
    /// pointer: another geometry, another simulation.
    std::shared_ptr<const Geometry> geometry;
    /// The geometry at the end of the next step, as many points: where the
    /// pins are to be then. Null: where they rest.
    std::shared_ptr<const Geometry> target;
    std::vector<Collider> colliders;   ///< the objects, and the pieces of an RBD Solver
    std::vector<Force> forces;         ///< its wind blows it
    bool rigidInto = false;            ///< the pieces of the RBD Solver are among its colliders
    int node = 0;

    /// Every number in a range the solver can work with.
    ClothScene sanitized() const;
    bool operator==(const ClothScene&) const = default;
};

/// The cloth of a frame: where each point is, and how fast it goes -- the
/// geometry's points, and those torn off them after.
struct ClothFrame {
    std::shared_ptr<const Geometry> geometry;  ///< at rest: the scene's; null in a frame read back until adoptCloth
    std::vector<Vec3> positions;
    std::vector<uint16_t> velocities;          ///< half floats, three a point
    /// Torn: for each point after the geometry's, the point of the geometry
    /// it was split off; each corner of the geometry's primitives, the point
    /// it is on now (empty: as in the geometry); the corners of lines whose
    /// segment to the next has parted.
    std::vector<uint32_t> copies, corners, cuts;

    bool empty() const { return positions.empty(); }
    bool torn() const { return !copies.empty() || !cuts.empty(); }
    size_t bytes() const {
        return positions.size() * sizeof(Vec3) + velocities.size() * sizeof(uint16_t) +
               (copies.size() + corners.size() + cuts.size()) * sizeof(uint32_t);
    }
};

/// The geometry of `f` where its points are: P moved, their velocity v, and
/// normals N the mean of the faces round each point; torn, the points split
/// off with the attributes of theirs, the corners on them, the lines parted.
/// Null without geometry, or with one it does not fit.
std::shared_ptr<Geometry> posedCloth(const ClothFrame& f);
/// Whether `f` is of `geometry`: its points, corners and cuts in range.
bool clothFits(const ClothFrame& f, const Geometry& geometry);
/// As the solver's look draws it: posed, its faces in the colour Cd they
/// have -- `color` where they have none.
std::shared_ptr<Geometry> drawnCloth(const ClothFrame& f, const Vec3& color);

class ClothSolver {
public:
    explicit ClothSolver(const ClothScene& scene);

    /// The colliders, the forces and where the pins go in the next step; the
    /// cloth and how it is held together stay.
    void setScene(const ClothScene& scene);
    /// The gas's flow at a world point, for the next step; null: still air.
    void setAir(std::function<Vec3(const Vec3&)> air);
    void step();

    const ClothScene& scene() const { return scene_; }
    int frame() const { return frame_; }
    float time() const { return time_; }
    size_t pointCount() const { return x_.size(); }
    const std::vector<Vec3>& positions() const { return x_; }
    const std::vector<Vec3>& velocities() const { return v_; }
    ClothFrame capture() const;

    /// What it did to the pieces of an RBD Solver among its colliders in the
    /// last step: how much further than they went it moved each, and how
    /// much faster it made it go and turn.
    struct Reaction {
        uint32_t piece = 0;
        Vec3 shift, velocity, spin;
    };
    const std::vector<Reaction>& reactions() const { return reactions_; }
    /// How many points it has: the geometry's, and those torn off them.
    size_t tornPoints() const { return x_.size() - (scene_.geometry ? scene_.geometry->pointCount() : 0); }

    /// Constraints by kind: stretch and shear, bend, balloons.
    size_t stretchCount() const { return stretchCount_; }
    size_t bendCount() const { return bendCount_; }
    size_t balloonCount() const { return balloons_.size(); }
    /// The volume of balloon `b` now, and at rest.
    float balloonVolume(size_t b) const;
    float balloonRestVolume(size_t b) const { return balloons_[b].rest; }

    /// All the next step needs of what it has come to (State.h).
    void saveState(StateWriter& out) const;
    bool loadState(StateReader& in);

private:
    static constexpr uint32_t kNone = ~0u;
    struct Link {
        uint32_t a = 0, b = 0;
        float rest = 0.0f;
        float compliance = 0.0f;  ///< m/N
        float limit = 0.0f;       ///< the length it tears at; 0 never
        uint32_t corner = kNone;  ///< a rope's segment: the corner it starts at
    };
    struct Balloon {
        std::vector<uint32_t> triangles;  ///< three points each, as tris_
        float rest = 0.0f;                ///< signed volume at rest
    };
    void build();
    void connect();
    bool tear();
    void split(uint32_t p);
    void aero(std::vector<Vec3>& accel) const;
    void solveLinks(float h);
    void solveBalloons(float h);
    void selfCollide();
    void collide(float h);

    ClothScene scene_;
    std::function<Vec3(const Vec3&)> air_;
    std::vector<ShapeInstance> shapes_;       // the colliders, placed
    std::vector<Vec3> x_, v_, prev_, start_, target_;
    std::vector<Vec3> rest_;                  // where the points were at frame 1
    std::vector<uint32_t> corner_;            // each corner of the geometry: its point now
    std::vector<uint32_t> origin_;            // each point: the geometry's it was split off (itself)
    std::vector<uint8_t> ropeCut_;            // each corner: a line's segment from it to the next parted
    std::vector<uint64_t> cut_;               // edges torn between faces not yet split apart, sorted
    std::vector<float> tearOf_;               // each point of the geometry: its attribute tear
    std::vector<int32_t> touched_;            // each point: the piece collider it touched this substep
    std::vector<Vec3> pushed_;                // ... and how far that moved it
    std::vector<Vec3> drift_, kick_, twist_;  // each collider that gives: moved, sped up, turned faster by the cloth this step
    std::vector<Reaction> reactions_;
    std::vector<float> w_;                    // 1 / mass; 0 pinned
    std::vector<uint8_t> pinned_;
    std::vector<uint32_t> tris_;              // three points a triangle
    std::vector<uint32_t> triStart_, triOf_;  // each point's triangles
    std::vector<Vec3> airOfTri_;              // the gas's flow at each triangle, this step
    std::vector<int8_t> outside_;             // closed meshes: 1 the normal points out, -1 in; 0 open
    std::vector<Link> links_;
    std::vector<std::vector<uint32_t>> colours_;  // links none of which share a point
    std::vector<uint32_t> leftOver_;              // links that fitted no colour
    std::vector<uint32_t> nearStart_, near_;      // each point's constrained neighbours, sorted
    std::vector<Balloon> balloons_;
    size_t stretchCount_ = 0, bendCount_ = 0;
    float selfRadius_ = 0.01f;
    int frame_ = 0;
    float time_ = 0.0f;
};

}  // namespace pg::sim
