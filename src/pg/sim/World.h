#pragma once
//
// Everything a network simulates, together: the gas of a Pyro Solver, the
// water of a Liquid Solver, the rain and the pieces of an RBD Solver, at one
// frame rate, stepped side by side (WorldSolver) and kept as one Frame a
// step. The pieces are stepped first: the water, the gas and the rain go
// round them where they have got to, and the dust of their broken glue
// puffs into the gas. The rain falls into the water: it is stepped after
// it, and rings its surface.
//
// Plain data, compared as a whole, as the scene of each part is: the
// editor's runner starts again exactly when the World differs.
//
#include "pg/sim/Cloth.h"
#include "pg/sim/Frame.h"
#include "pg/sim/Liquid.h"
#include "pg/sim/Pyro.h"
#include "pg/sim/Rain.h"
#include "pg/sim/Rigid.h"
#include "pg/sim/Scene.h"

#include <memory>
#include <string>
#include <string_view>
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
    bool hasRigid = false;
    RigidScene rigid;                ///< the RBD Solver's, when hasRigid
    bool hasCloth = false;
    ClothScene cloth;                ///< the Cloth Solver's, when hasCloth
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
    bool any() const { return hasGas || hasWater || hasRain || hasRigid || hasCloth; }
    /// Each part sanitized (Scene::sanitized), and the one time step in each.
    World sanitized() const;

    bool operator==(const World&) const = default;
};

/// The world with the grids of its gas and its water `fraction` as fine (at
/// least 16 cells along the longest side): a preview of it, to work on
/// quickly, before the final one is simulated. 1: the world as it is.
World preview(const World& world, float fraction);

/// The box the pictures of `world` frame: as large along each axis as its
/// gas's and its water's, round the origin on the floor; some ground for
/// rain alone; a default scene's without either.
Domain sceneDomain(const World& world);

/// Steps every part of a World by a frame.
class WorldSolver {
public:
    explicit WorldSolver(const World& world);

    void step();
    /// The frame as it is kept: each part's fields as half floats.
    Frame capture() const;

    /// All it would take to go on from this frame as if it had never stopped
    /// (State.h): a checkpoint. The gas's, the water's and the rain's state;
    /// not the pieces' -- they cost little to step, and are stepped again.
    std::string saveState() const;
    /// Takes a solver fresh from its world to the frame of a state
    /// saveState() wrote of the same world: the pieces stepped there again,
    /// frame by frame, the rest read. False, with why, for a state of
    /// another world or a file cut short -- the solver then is of no use.
    bool loadState(std::string_view bytes, std::string& error);

    const World& world() const { return world_; }
    /// Where the time of the last step went (Frame::Profile).
    const Frame::Profile& profile() const { return profile_; }
    int frame() const { return frame_; }
    float time() const { return time_; }
    PyroSolver* gas() { return gas_.get(); }
    const PyroSolver* gas() const { return gas_.get(); }
    LiquidSolver* water() { return water_.get(); }
    const LiquidSolver* water() const { return water_.get(); }
    RainSolver* rain() { return rain_.get(); }
    const RainSolver* rain() const { return rain_.get(); }
    RigidSolver* rigid() { return rigid_.get(); }
    const RigidSolver* rigid() const { return rigid_.get(); }
    ClothSolver* cloth() { return cloth_.get(); }
    const ClothSolver* cloth() const { return cloth_.get(); }

private:
    /// What a step does before the gas, the water and the rain move: the
    /// pieces stepped, and each part given its scene of the frame.
    void prepare();
    /// The water and the gas as the pieces feel them now.
    RigidFluids fluids() const;

    World world_;
    Frame::Profile profile_;  // of the last step
    std::unique_ptr<PyroSolver> gas_;
    std::unique_ptr<LiquidSolver> water_;
    std::unique_ptr<RainSolver> rain_;
    std::unique_ptr<RigidSolver> rigid_;
    std::unique_ptr<ClothSolver> cloth_;
    int frame_ = 0;
    float time_ = 0.0f;
    /// The water and the gas push the pieces (RigidSolver::feel): what they
    /// did in each step so far, frame by frame -- a checkpoint keeps it, and
    /// the pieces take it again when they are stepped there again.
    bool coupled_ = false;      // something pushes the pieces: the flows are kept
    bool fluidsPush_ = false;   // the water or the gas
    bool clothPushes_ = false;  // the cloth they fall on
    std::vector<RigidFlow> flows_;
};

}  // namespace pg::sim
