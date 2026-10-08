#pragma once
//
// What a gas simulation is made of: the description the solver runs, and what
// a node network compiles to (Network.h). Plain data, compared as a whole --
// a UI knows the simulation has to start again exactly when the scene differs.
//
// World units, y up. The domain stands on the floor, y = 0, centred on the y
// axis: x in [-size.x/2, size.x/2], y in [0, size.y], z in [-size.z/2, size.z/2].
//
#include "pg/core/Types.h"
#include "pg/sim/Look.h"
#include "pg/sim/Mesh.h"
#include "pg/sim/Shape.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace pg::sim {

/// How a source moves: it stays, circles the vertical axis through its
/// centre, or sways from side to side along x.
enum class Motion : uint8_t { Static, Circle, Sway };

/// Where a force acts.
enum class Mask : uint8_t { Everywhere, Heat, Smoke };

/// A place gas comes from: a shape (Shape.h) placed in the world.
struct Emitter {
    Shape shape = Shape::Sphere;
    Vec3 center{0.0f, 0.12f, 0.0f};
    Vec3 rotation;                  ///< degrees about x, then y, then z
    Vec3 size{0.2f, 0.2f, 0.2f};    ///< along its own axes: a ball's diameters, a box's edges
    float fuel = 0.0f;              ///< added per second, at full strength -- fire
    float smoke = 0.0f;
    float heat = 0.0f;
    /// The gas leaves the source at least this fast, along the source's own
    /// axes: turn the source and its jet turns with it.
    Vec3 velocity{0.0f, 0.5f, 0.0f};
    /// 1/s: the gas in it swells this fast -- pushed out on all sides, as
    /// the air a collapsing building squeezes out. 0: it does not.
    float expansion = 0.0f;
    float flicker = 0.0f;           ///< 0 steady, 1 strongly flickering
    float flickerSize = 0.07f;      ///< size of the patches that flicker together
    float start = 0.0f;             ///< seconds
    float end = 0.0f;               ///< seconds; at or before start: never stops
    Motion motion = Motion::Static;
    float motionSize = 0.25f;       ///< radius of the circle, reach of the sway
    float motionPeriod = 4.0f;      ///< seconds for a round
    /// How fast it is carried, when it is animated (keys on its place): the
    /// gas it gives off leaves with that velocity too.
    Vec3 moving;
    uint32_t seed = 1;
    std::shared_ptr<const MeshShape> mesh;  ///< Shape::Mesh: what it is (Mesh.h)
    int node = 0;                   ///< the network node it came from, 0 if none

    bool activeAt(float t) const { return t >= start && (end <= start || t < end); }
    /// Where the source is at time t, and how fast it moves.
    Vec3 centerAt(float t) const;
    Vec3 motionVelocityAt(float t) const;
    /// Its shape where it is at time t.
    ShapeInstance shapeAt(float t) const { return {shape, centerAt(t), rotation, size, mesh}; }

    bool operator==(const Emitter&) const = default;
};

enum class ForceKind : uint8_t {
    Turbulence,  ///< random whirls that change over time
    Wind,        ///< pulls the gas towards a velocity
    Vortex,      ///< pulls the gas round an axis, along it and in towards it
    Attractor,   ///< pulls towards a point (pushes away when negative)
    Drag,        ///< slows everything down
};

struct Force {
    ForceKind kind = ForceKind::Turbulence;
    /// Turbulence, attractor: the push, world units/s^2. Wind, vortex: how
    /// fast the gas takes their speeds, per second. Drag: how fast it slows.
    float strength = 1.0f;
    Mask mask = Mask::Everywhere;
    float scale = 0.05f;             ///< turbulence: size of the whirls
    /// Turbulence: new whirls per second. Wind: its speed. Vortex: the speed
    /// round the axis, halfway out to the radius -- below 0 the other way round.
    float speed = 4.0f;
    Vec3 direction{1.0f, 0.0f, 0.0f};  ///< wind: where it blows; vortex: its axis
    float gusts = 0.0f;              ///< wind: 0 steady, 1 strongly gusting
    Vec3 center{0.0f, 0.5f, 0.0f};   ///< vortex, attractor
    float radius = 0.5f;             ///< vortex, attractor: how far they reach
    float height = 0.0f;             ///< vortex: its length along the axis; 0 all the way through
    float lift = 0.0f;               ///< vortex: speed along the axis
    float suction = 0.0f;            ///< vortex: speed in towards the axis, at its edge
    uint32_t seed = 1;
    int node = 0;

    bool operator==(const Force&) const = default;
};

/// A solid the gas flows around: a shape (Shape.h) placed in the world.
struct Collider {
    Shape shape = Shape::Sphere;
    Vec3 center{0.0f, 0.6f, 0.0f};
    Vec3 rotation;                  ///< degrees about x, then y, then z
    Vec3 size{0.3f, 0.3f, 0.3f};    ///< along its own axes
    std::shared_ptr<const MeshShape> mesh;  ///< Shape::Mesh: what it is (Mesh.h); the same file, the same mesh
    int node = 0;
    /// How it moves, when it is animated: its velocity, world units per
    /// second, and its spin -- along the axis it turns round, radians per
    /// second. The gas and the water it pushes take it on.
    Vec3 velocity, spin;
    /// A piece of an RBD Solver: which (RigidSolver::colliders); -1 an object.
    int32_t piece = -1;
    /// A piece: how heavy its body is, kg -- what pushes it moves it; 0 it
    /// does not give (an object, a piece held still).
    float mass = 0.0f;

    ShapeInstance instance() const { return {shape, center, rotation, size, mesh}; }
    bool contains(const Vec3& p) const { return instance().contains(p); }
    bool moves() const { return velocity != Vec3() || spin != Vec3(); }
    /// The velocity of its point p.
    Vec3 velocityAt(const Vec3& p) const { return velocity + cross(spin, p - center); }
    /// How it is turned `t` seconds from now, going on as it goes: about the
    /// axis of its spin, as far as it turns in that time. Its point p is
    /// then at turnAt(t) * (p - center) + center + velocity * t.
    Mat3 turnAt(float t) const {
        const float speed = length(spin), angle = speed * t;
        if (!(speed > 0.0f) || angle == 0.0f) return Mat3(1.0f);
        const Vec3 k = spin / speed;
        // Rodrigues: I + sin K + (1 - cos) K^2, K the cross product with k.
        const Mat3 cross(0.0f, k.z, -k.y, -k.z, 0.0f, k.x, k.y, -k.x, 0.0f);
        return Mat3(1.0f) + cross * std::sin(angle) + (cross * cross) * (1.0f - std::cos(angle));
    }
    bool operator==(const Collider&) const = default;
};

/// An object of the scene as it is drawn: its body, its colour, and how it
/// stands over a plate (Matte, Look.h). Every object of a network is drawn,
/// collided with or not; the colour is kept apart from the Collider so that
/// painting an object simulates nothing again.
struct Solid {
    Collider body;
    Vec3 color{0.45f, 0.45f, 0.46f};
    Matte matte = Matte::None;

    bool operator==(const Solid&) const = default;
};

/// The grid a domain is simulated on: cells of one size, a multiple of 8 of
/// them along each axis, so that multigrid can halve the grid a few times.
struct Domain {
    int cells[3] = {8, 8, 8};
    float voxel = 0.125f;  ///< edge of a cell, world units

    Vec3 size() const {
        return {voxel * static_cast<float>(cells[0]), voxel * static_cast<float>(cells[1]),
                voxel * static_cast<float>(cells[2])};
    }
    /// The corner at the least x, y and z: (-size.x/2, 0, -size.z/2).
    Vec3 origin() const { return {-0.5f * size().x, 0.0f, -0.5f * size().z}; }
    size_t cellCount() const {
        return static_cast<size_t>(cells[0]) * static_cast<size_t>(cells[1]) * static_cast<size_t>(cells[2]);
    }

    /// The grid of a box of `size` with `resolution` cells along its longest
    /// side; each count rounded up to a multiple of 8, so the domain may come
    /// out a little larger.
    static Domain ofBox(const Vec3& size, int resolution);

    bool operator==(const Domain&) const = default;
};

struct SolverSettings {
    Vec3 size{1.0f, 1.5f, 1.0f};  ///< the domain, world units
    /// Cells along the longest side, 16 to 1024; each count is rounded up to
    /// a multiple of 8, so the domain may come out a little larger.
    int resolution = 96;
    /// Work and keep only the tiles of 8 x 8 x 8 cells the gas is in, and
    /// those round them (SparseGrid.h): the rest of the domain is still,
    /// empty air. Off, every cell of it is.
    bool sparse = false;
    /// With sparse: a tile whose smoke, heat, fuel and flame all stay below
    /// this is let go.
    float cutoff = 1e-3f;
    bool closedFloor = true;      ///< a floor at y = 0 the gas cannot pass; else open like the rest

    float timeStep = 1.0f / 30.0f;
    int substeps = 1;
    int pressureCycles = 2;       ///< multigrid V-cycles per step
    uint32_t seed = 1;

    float buoyancy = 1.0f;        ///< lift per unit of heat
    float weight = 0.05f;         ///< sink per unit of smoke
    float vorticity = 0.6f;       ///< vorticity confinement: the swirls a coarse grid loses

    float burnRate = 10.0f;       ///< share of the fuel that burns per second
    float heatRelease = 2.5f;     ///< heat per unit of fuel burnt
    float sootRelease = 0.5f;     ///< smoke per unit of fuel burnt
    float expansion = 0.8f;       ///< expansion of the gas per unit of fuel burnt
    float flameLife = 0.1f;       ///< seconds the flame of burning fuel lasts

    float cooling = 1.0f;         ///< per second
    float smokeDecay = 0.1f;      ///< per second

    /// How hard water puts the fire out where it gets into the gas -- the
    /// particles of a Liquid Solver, the drops of a Rain: it cools the gas,
    /// soaks the fuel, quenches the flame, and soaks the sources it falls
    /// on, which then give less and less. 0: it does nothing.
    float quench = 1.0f;
    /// Steam -- a field of its own, white -- for each unit of heat the water
    /// takes.
    float steam = 1.0f;
    /// How hard steam rises, as a unit of heat lifts the gas: lighter than
    /// the air, and warm.
    float steamLift = 1.5f;
    /// Per second: how fast steam thins out into clear air.
    float steamFade = 0.7f;
    /// How fast the fire boils away the water in it -- a Liquid Solver's
    /// particles, a Rain's drops: each goes as likely, per second, as
    /// Evaporate times how much hotter than kBoil the gas round it is. 0:
    /// the water stays, however hot.
    float evaporate = 1.0f;
    /// Step the gas on the GPU, through Vulkan (docs/gpu.md), with the same
    /// result to the bit. Without a GPU the CPU does it all, as off.
    bool gpu = false;

    Domain domain() const;
    bool operator==(const SolverSettings&) const = default;
};

struct Scene {
    SolverSettings solver;
    std::vector<Emitter> emitters;
    std::vector<Force> forces;
    std::vector<Collider> colliders;

    /// Every number in a range the solver can work with -- 16 to 1024 cells, a
    /// time step above 0 and at most 1 s, no negative rates, sizes above 0 --
    /// and what is not a number replaced by its default. The solver takes its
    /// scene this way, so no input divides by zero or allocates the machine away.
    Scene sanitized() const;

    /// A campfire, and a column of smoke: what the node network's examples
    /// build, for tests and for code without a network.
    static Scene fire();
    static Scene smoke();

    bool operator==(const Scene&) const = default;
};

}  // namespace pg::sim
