#pragma once
//
// Everything a network simulates, together: the gas of a Pyro Solver --
// and, as they come, the water and the rain -- at one frame rate, stepped
// side by side (WorldSolver) and kept as one Frame a step.
//
// Plain data, compared as a whole, as the Scene of each part is: the
// editor's runner starts again exactly when the World differs.
//
#include "pg/sim/Frame.h"
#include "pg/sim/Pyro.h"
#include "pg/sim/Scene.h"

#include <memory>

namespace pg::sim {

struct World {
    float timeStep = 1.0f / 30.0f;  ///< seconds a frame, for every part
    bool hasGas = false;
    Scene gas;                       ///< the Pyro Solver's, when hasGas

    /// True when there is anything to simulate.
    bool any() const { return hasGas; }
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

private:
    World world_;
    std::unique_ptr<PyroSolver> gas_;
    int frame_ = 0;
    float time_ = 0.0f;
};

}  // namespace pg::sim
