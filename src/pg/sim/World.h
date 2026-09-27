#pragma once
//
// Everything a network simulates, together: the gas of a Pyro Solver, the
// water of a Liquid Solver and the rain, at one frame rate, stepped side by
// side (WorldSolver) and kept as one Frame a step. The rain falls into the
// water: it is stepped after it, and rings its surface.
//
// Plain data, compared as a whole, as the scene of each part is: the
// editor's runner starts again exactly when the World differs.
//
#include "pg/sim/Frame.h"
#include "pg/sim/Liquid.h"
#include "pg/sim/Pyro.h"
#include "pg/sim/Rain.h"
#include "pg/sim/Scene.h"

#include <memory>
#include <vector>

namespace pg::sim {

struct World;

/// A world frame by frame: what an animated network gives each step to take.
/// Shared -- copying a World copies no frames -- and compared by what it holds.
struct Animation {
    std::shared_ptr<const std::vector<World>> frames;  ///< frame 1 first

    bool empty() const { return !frames || frames->empty(); }
    bool operator==(const Animation& other) const;
};

struct World {
    float timeStep = 1.0f / 30.0f;  ///< seconds a frame, for every part
    bool hasGas = false;
    Scene gas;                       ///< the Pyro Solver's, when hasGas
    bool hasWater = false;
    LiquidScene water;               ///< the Liquid Solver's, when hasWater
    bool hasRain = false;
    RainScene rain;                  ///< the Rain's, when hasRain
    /// Frames keep the water's particles: something makes points of them
    /// (Liquid Points). They cost some 19 bytes a particle a frame.
    bool keepParticles = false;
    /// Something is animated: the world at each frame -- the sources, the
    /// forces and the solids where they are then, and how they move. The
    /// step that makes a frame takes it. Empty: the same all along.
    Animation animation;

    /// The world at `frame` (1 on): the animation's, else this one.
    const World& at(int frame) const;

    /// True when there is anything to simulate.
    bool any() const { return hasGas || hasWater || hasRain; }
    /// Each part sanitized (Scene::sanitized), and the one time step in each.
    World sanitized() const;

    bool operator==(const World&) const = default;
};

/// Steps every part of a World by a frame.
class WorldSolver {
public:
    explicit WorldSolver(const World& world);

    void step();
    /// The frame as it is kept: each part's fields as half floats.
    Frame capture() const;

    const World& world() const { return world_; }
    int frame() const { return frame_; }
    float time() const { return time_; }
    PyroSolver* gas() { return gas_.get(); }
    const PyroSolver* gas() const { return gas_.get(); }
    LiquidSolver* water() { return water_.get(); }
    const LiquidSolver* water() const { return water_.get(); }
    RainSolver* rain() { return rain_.get(); }
    const RainSolver* rain() const { return rain_.get(); }

private:
    World world_;
    std::unique_ptr<PyroSolver> gas_;
    std::unique_ptr<LiquidSolver> water_;
    std::unique_ptr<RainSolver> rain_;
    int frame_ = 0;
    float time_ = 0.0f;
};

}  // namespace pg::sim
