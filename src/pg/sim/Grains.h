#pragma once
//
// Sand, gravel, soil: grains, each a ball of its own size, that push each
// other apart, hold each other back by friction and -- wet -- stick
// together; what Houdini's Vellum Grains and POP Grains do. Position based
// (Macklin, Müller, Chentanez, Kim, "Unified Particle Physics for Real-Time
// Applications", 2014, §6; Macklin, Storey et al., "Small Steps in Physics
// Simulation", 2019): many small substeps a frame, a few passes over the
// contacts in each.
//
//   contacts  two grains closer than their radii are pushed apart, each by
//             as much as the other is heavier -- a big stone moves a grain
//             of sand, not the other way round;
//   friction  what slides along a contact is held back: wholly while it
//             slides less than Friction times how far the grains went into
//             each other (static), else by that much (kinetic, three
//             quarters as much). Grains do not roll -- they are not balls but
//             what sand is: they pile up, at an angle as steep as Friction
//             holds (0.6: some 30 degrees, dry sand);
//   cohesion  grains a little apart are pulled together: 0 dry sand that
//             runs, 0.5 damp sand that holds a slope, 1 wet sand that
//             stands as a wall -- snow, soil, mud;
//   at rest   a grain in contact that moves less than Rest Speed stays where
//             it was: a pile settles and stays, it does not creep.
//
// The grains are the points of a geometry -- their size their pscale (the
// radius), their colour Cd, their velocity v -- taken at frame 1; with Emit
// Frames above 1, taken again each frame until then wherever no grain is
// in the way: sand poured from a spout, gravel from a chute. Size Variance
// makes each grain a little bigger or smaller than its point says.
//
// They fall on the floor, on the objects and the pieces of an RBD Solver
// (Colliders), and push the pieces back: in the grains' step a piece is a
// body of its weight that the grains slow and turn, and where that leaves
// it the RBD Solver takes on (reactions) -- as the cloth's. The wind of the
// Forces and the flow of a Pyro Solver's gas blow them, as hard as Air
// Drag says.
//
// Deterministic (invariant I5): the contacts are solved as Jacobi does --
// each grain from where every grain was before the pass, its corrections
// averaged -- so the threads write each grain's own and nothing else; the
// grid of neighbours is sorted in order.
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

struct GrainSettings {
    float radius = 0.01f;        ///< metres: a grain's, where its point has no pscale
    /// Each grain this much bigger or smaller than its point says, at random:
    /// 0.2 between 80 and 120 percent.
    float sizeVariance = 0.2f;
    float density = 1600.0f;     ///< kg a cubic metre of grains, the gaps between them in it: dry sand
    /// How hard the grains hold each other back along a contact -- and the
    /// floor and the objects them: 0 ball bearings, 0.6 dry sand, 1 gravel.
    float friction = 0.6f;
    /// How hard grains a little apart are pulled together: 0 dry, 0.5 damp,
    /// 1 wet sand.
    float cohesion = 0.0f;
    /// A grain in contact moving slower than this, metres a second, stays
    /// where it was: a pile settles.
    float restSpeed = 0.01f;
    float damping = 0.0f;        ///< 1/s: how fast its motion dies away of itself
    /// How hard the air pushes a grain toward the speed it goes at -- the
    /// wind of the Forces, the gas's flow -- per second: 0 not at all.
    float airDrag = 0.0f;
    /// Frames the points are emitted on, from frame 1: 1 once, more a
    /// stream -- each frame where no grain is in the way.
    int emitFrames = 1;
    int maxGrains = 1000000;
    int substeps = 10;           ///< steps a frame
    int iterations = 4;          ///< passes over the contacts a substep
    bool floor = true;           ///< a floor at y = 0
    Vec3 gravity{0.0f, -9.81f, 0.0f};
    float timeStep = 1.0f / 30.0f;

    bool operator==(const GrainSettings&) const = default;
};

struct GrainScene {
    GrainSettings solver;
    /// The points the grains are made of: P, pscale, Cd, v. Compared by
    /// pointer: another geometry, another simulation.
    std::shared_ptr<const Geometry> geometry;
    std::vector<Collider> colliders;   ///< the objects, and the pieces of an RBD Solver
    std::vector<Force> forces;         ///< their wind blows the grains
    int node = 0;

    /// Every number in a range the solver can work with.
    GrainScene sanitized() const;
    bool operator==(const GrainScene&) const = default;
};

/// The grains of a frame.
struct GrainFrame {
    std::vector<Vec3> positions;
    std::vector<uint16_t> velocities;  ///< half floats, three a grain
    std::vector<uint16_t> radii;       ///< half floats, metres
    std::vector<uint32_t> ids;         ///< each grain's own number, the same from frame to frame
    /// Red, green, blue, 0 to 255, three a grain; empty: the look's colour.
    std::vector<uint8_t> colors;

    bool empty() const { return positions.empty(); }
    size_t size() const { return positions.size(); }
    size_t bytes() const {
        return positions.size() * sizeof(Vec3) + (velocities.size() + radii.size()) * sizeof(uint16_t) +
               ids.size() * sizeof(uint32_t) + colors.size();
    }
    /// Whether every list holds as many as there are grains.
    bool fits() const;
};

/// The grains as points: P, v, pscale (the radius), id, Cd -- `color` where
/// the frame has none -- and orient, each grain turned its own way (from
/// its id), as the grit is: loose points the renderers draw as chips of
/// stone. Empty without grains.
std::shared_ptr<Geometry> grainPoints(const GrainFrame& f, const Vec3& color);

class GrainSolver {
public:
    explicit GrainSolver(const GrainScene& scene);

    /// The colliders and the forces of the next step; the grains, what they
    /// are made of and how they behave stay.
    void setScene(const GrainScene& scene);
    /// The gas's flow at a world point, for the next step; null: still air.
    void setAir(std::function<Vec3(const Vec3&)> air);
    void step();

    const GrainScene& scene() const { return scene_; }
    int frame() const { return frame_; }
    float time() const { return time_; }
    size_t grainCount() const { return x_.size(); }
    const std::vector<Vec3>& positions() const { return x_; }
    const std::vector<Vec3>& velocities() const { return v_; }
    const std::vector<float>& radii() const { return r_; }
    const std::vector<uint32_t>& ids() const { return id_; }
    GrainFrame capture() const;
    /// Contacts between grains in the last substep.
    size_t contactCount() const { return contacts_; }

    /// What it did to the pieces of an RBD Solver among its colliders in the
    /// last step: how much further than they went it moved each, and how
    /// much faster it made it go and turn (as ClothSolver::Reaction).
    struct Reaction {
        uint32_t piece = 0;
        Vec3 shift, velocity, spin;
    };
    const std::vector<Reaction>& reactions() const { return reactions_; }

    /// All the next step needs of what it has come to (State.h).
    void saveState(StateWriter& out) const;
    bool loadState(StateReader& in);

private:
    void emit();
    /// The grains in the order of the cells they are in.
    void reorder();
    /// The grid of where the grains are: cells as wide as the biggest grain
    /// with its reach, the grains of each in order.
    void sortIntoCells(float cell);
    /// For each grain, the grains near enough to touch it this substep.
    void findNeighbours(float reach);
    void solveContacts(bool last);
    void collide(float h);

    GrainScene scene_;
    std::function<Vec3(const Vec3&)> air_;
    std::vector<ShapeInstance> shapes_;          // the colliders, placed
    std::vector<Vec3> x_, v_, prev_, dx_, wind_;
    std::vector<float> r_, w_;                   // radius; 1 / mass, kg
    std::vector<uint32_t> id_;
    std::vector<uint8_t> color_;                 // three a grain; empty: none
    std::vector<uint8_t> touching_;              // each grain: it touched something this substep
    // The grid: each grain's cell key, the grains in key order, where each
    // bucket of the table starts in it.
    std::vector<uint32_t> key_, order_, bucket_;
    std::vector<int32_t> cellOf_;                // each grain's cell, three a grain
    float cell_ = 1.0f;
    std::vector<uint32_t> nearStart_, near_;     // each grain's neighbours
    std::vector<Vec3> builtAt_;                  // where the grains were when they were found
    std::vector<float> pressed_;                 // ... and how far into it each went, this substep's passes
    std::vector<int32_t> touched_;               // each grain: the piece collider it touched this substep
    std::vector<Vec3> pushed_;                   // ... and how far that moved it
    std::vector<Vec3> drift_, kick_, twist_;     // each collider that gives: as the cloth's
    std::vector<Reaction> reactions_;
    uint32_t nextId_ = 0;
    size_t contacts_ = 0;
    int frame_ = 0;
    float time_ = 0.0f;
};

}  // namespace pg::sim
