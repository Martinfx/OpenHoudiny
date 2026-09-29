#pragma once
//
// Rain: drops that fall from a box in the sky -- blown by the wind, stopped
// by the floor, by the objects and by water -- and what they do where they
// land: a spray of droplets that bounce off a solid, rings that spread over
// the water.
//
// Each drop falls at its speed (a raindrop soon falls as fast as the air
// lets it: some 7 m/s), and takes on the velocity of the wind the way a
// light thing does, a share of the difference each second; turbulence
// makes it wobble. Where a drop meets something it is gone:
//
//   the floor, an object   a few droplets bounce up and away from where it
//                          landed, fall and are gone in a fraction of a second;
//   water                  a ring spreads out over the surface: the drop
//                          pushes the surface down, and a height field --
//                          the wave equation on a grid over the water --
//                          carries it out as ripples; a droplet or two
//                          jumps up.
//
// The water's surface is the Liquid Solver's (Liquid.h), where there is one.
//
// Deterministic: where a drop starts is a hash of its number; each drop
// moves on its own; droplets are made in the order of the drops that made
// them, after all have moved.
//
#include "pg/sim/Grid.h"
#include "pg/sim/Scene.h"

#include <array>
#include <cstdint>
#include <vector>

namespace pg::sim {

class LiquidSolver;
class StateReader;
class StateWriter;

struct RainSettings {
    /// The box the drops start in: its middle and its size. It is the cloud:
    /// wide as the rain, its height where the drops start.
    Vec3 center{0.0f, 2.5f, 0.0f};
    Vec3 size{3.0f, 0.5f, 3.0f};
    float rate = 800.0f;     ///< drops a second for each square metre under the box
    float speed = 7.0f;      ///< how fast a drop falls once it has settled, m/s
    float splash = 3.0f;     ///< droplets a drop throws up where it lands on something solid
    float ripples = 1.0f;    ///< how hard a drop rings the water
    float start = 0.0f, end = 0.0f;  ///< seconds; end at or before start: it never stops
    uint32_t seed = 1;
    float timeStep = 1.0f / 30.0f;

    bool activeAt(float t) const { return t >= start && (end <= start || t < end); }
    bool operator==(const RainSettings&) const = default;
};

struct RainScene {
    RainSettings rain;
    std::vector<Force> forces;       ///< wind, turbulence: what blows the drops about
    std::vector<Collider> colliders; ///< what they land on, besides the floor and the water

    RainScene sanitized() const;
    bool operator==(const RainScene&) const = default;
};

/// A drop or a droplet: where it is and how fast it goes.
struct RainParticle {
    Vec3 position, velocity;
    float age = 0.0f;       ///< seconds since it was made
    float life = 0.0f;      ///< droplets: seconds it lives; drops: 0 -- until they land
    uint32_t id = 0;        ///< its own number among the drops, or among the droplets
};

/// The ripples on the water: a height field over the xz extent of the
/// water's domain.
struct Ripples {
    Vec3 origin;             ///< corner at the least x and z (y unused)
    float cell = 0.0f;       ///< edge of a cell, world units
    int nx = 0, nz = 0;
    std::vector<float> height;  ///< world units, x fastest

    bool empty() const { return height.empty(); }
    bool operator==(const Ripples&) const = default;
};

class RainSolver {
public:
    explicit RainSolver(const RainScene& scene);

    /// Advances by the time step. `water`, if there is any, is what the drops
    /// fall into: its surface stops them and carries their ripples.
    void step(const LiquidSolver* water = nullptr);
    /// Takes on another scene as it runs: the cloud, the forces, the solids;
    /// the drops in the air stay.
    void setScene(const RainScene& scene);

    /// All the next step needs of what it has come to (State.h); not the
    /// scene, which the solver it is loaded into has already.
    void saveState(StateWriter& out) const;
    /// Takes on a state saveState() wrote; false, with the solver as it
    /// was, when it is not one.
    bool loadState(StateReader& in);

    const RainScene& scene() const { return scene_; }
    int frame() const { return frame_; }
    float time() const { return time_; }
    const std::vector<RainParticle>& drops() const { return drops_; }
    const std::vector<RainParticle>& droplets() const { return droplets_; }
    const Ripples& ripples() const { return ripples_; }
    /// Drops that landed in the last step: on solids, and in the water.
    int lastLandings() const { return lastSolid_; }
    int lastSplashes() const { return lastWater_; }

private:
    void spawn(float dt, const LiquidSolver* water);
    void move(float dt, const LiquidSolver* water);
    void ring(const Vec3& at, float strength);
    void waves(float dt);
    void fitRipples(const LiquidSolver* water);
    Vec3 air(const Vec3& p) const;  ///< the air's velocity at p, from the forces
    /// True if p is in the floor or in an object; `normal`: the way out.
    bool solidAt(const Vec3& p, Vec3& normal) const;

    RainScene scene_;
    std::vector<ShapeInstance> solids_;
    std::vector<RainParticle> drops_, droplets_;
    Ripples ripples_;
    std::vector<float> previous_;   // the ripples a step ago: the wave equation is second order
    std::vector<std::array<Grid, 3>> noise_;
    int frame_ = 0;
    float time_ = 0.0f;
    uint64_t made_ = 0;             // drops made so far: each one's number
    uint32_t splashed_ = 0;         // droplets made so far: the next one's number
    int lastSolid_ = 0, lastWater_ = 0;
};

}  // namespace pg::sim
