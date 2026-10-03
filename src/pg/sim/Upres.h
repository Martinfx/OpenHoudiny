#pragma once
//
// Pyro Upres: the gas of a Pyro Solver again, on a grid 2 to 4 times as fine
// -- the detail of a big simulation at a fraction of its cost, as Houdini's
// Pyro Upres and wavelet turbulence (Kim et al. 2008) make it.
//
// The solver moves the gas on its own grid; the upres takes that motion as
// it is and carries a fine copy of what the gas holds -- smoke, heat, fuel
// and flame -- with it, adding the small whirls the coarse grid cannot hold.
// Each step, after the solver's:
//
//   1. tiles     the fine grid is sparse, in tiles of 8 x 8 x 8 fine cells:
//                those with gas, round the sources, and round them as far as
//                the gas can go in a step;
//   2. emit      the sources add fuel, smoke and heat at the fine resolution
//                -- their edges and their flicker as fine as the grid;
//   3. advect    the solver's velocity, interpolated, plus turbulence carries
//                everything along (MacCormack, as the solver);
//   4. combust   fuel burns into heat, soot and flame, as in the solver;
//   5. dissipate smoke thins out, heat cools, flames die.
//
// The turbulence is curl noise -- whirls that neither squeeze nor spread
// the gas -- as strong where the solver's flow swirls (its vorticity) as the
// whirls of that size would be in a turbulent flow of it, so calm gas stays
// calm. The noise is carried with the flow: its coordinates are advected on
// the solver's grid, in two layers that start afresh in turn and cross-fade
// (Neyret 2003), so the whirls move with the gas and stretch with it rather
// than standing still as it passes through them, and change as they go.
//
// Nothing goes back into the solver: the upres follows it and draws finer.
// Its frames replace the solver's (capture()): the renderers, the cache and
// the exports take the fine grid as they take any.
//
// Deterministic (invariant I5), as the solver: every loop a parallelFor over
// tiles or cells, each cell written by one chunk.
//
#include "pg/sim/Frame.h"
#include "pg/sim/Scene.h"
#include "pg/sim/SparseGrid.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace pg::sim {

class PyroSolver;
class StateReader;
class StateWriter;

struct UpresSettings {
    /// Fine cells along each side of a cell of the solver's: 2 to 4 -- 8 to
    /// 64 times the cells.
    int scale = 2;
    /// How strongly the fine gas swirls where the solver's does: 1 as a
    /// turbulent flow would, 0 not at all -- the gas carried finer, no new
    /// whirls.
    float turbulence = 1.0f;
    /// The largest whirls it adds, in cells of the solver's grid; smaller
    /// ones are added down to a few fine cells.
    float swirlSize = 2.0f;
    /// Seconds a pattern of whirls is carried before it fades into a new one.
    float swirlLife = 0.5f;
    uint32_t seed = 1;
    int node = 0;  ///< the network node it came from, 0 if none

    /// Every number in range: a scale of 2 to 4, sizes and times above 0.
    UpresSettings sanitized() const;
    /// The fine grid over `coarse`.
    Domain domain(const Domain& coarse) const;

    bool operator==(const UpresSettings&) const = default;
};

class UpresSolver {
public:
    explicit UpresSolver(const UpresSettings& settings);

    /// One frame of `gas`, which has just stepped: its scene, its velocity
    /// and its time step -- in as many steps as it takes.
    void step(const PyroSolver& gas);

    const UpresSettings& settings() const { return settings_; }
    /// The fine grid; empty before the first step.
    const Domain& domain() const { return domain_; }
    const SparseGrid& density() const { return density_; }
    const SparseGrid& temperature() const { return temperature_; }
    const SparseGrid& fuel() const { return fuel_; }
    const SparseGrid& flame() const { return flame_; }
    const Tiles& tiles() const { return *cells_; }
    /// The fine cells worked on.
    size_t activeCells() const { return cells_ ? cells_->activeCells() : 0; }
    int frame() const { return frame_; }
    float time() const { return time_; }

    /// Milliseconds each stage has taken, summed over the steps so far.
    struct Times {
        double tiles = 0.0, solids = 0.0, emit = 0.0, swirl = 0.0, advect = 0.0, combust = 0.0;
        double total() const { return tiles + solids + emit + swirl + advect + combust; }
    };
    const Times& times() const { return times_; }

    /// The whirls at fine cell (i, j, k) as the last step had them, m/s:
    /// for tests.
    Vec3 turbulenceAt(int i, int j, int k) const;

    /// All it takes to go on from here (State.h), as PyroSolver's.
    void saveState(StateWriter& out) const;
    /// Takes on a state saveState() wrote over `gas`'s grid, as it is after
    /// its own state was read; false, with the upres as it was, when it is
    /// not one.
    bool loadState(StateReader& in, const PyroSolver& gas);

private:
    /// The fine grid over the solver's, fields and all, empty.
    void reset(const Domain& coarse);
    /// Every field onto the tiles `cells`; the new ones listed in fresh_.
    void retile(std::shared_ptr<const Tiles> cells);
    /// Lets go of the tiles the gas has left and takes on those it may reach
    /// in a step of dt: round each tile with gas or a source, as far as the
    /// gas there goes.
    void updateTiles(const PyroSolver& gas, float dt);
    /// The cells inside `colliders`: in every tile, or in the new ones.
    void updateSolids(const std::vector<Collider>& colliders, bool all);
    /// The solver's flow at its cells' centres, and how strong the whirls
    /// are there, by its swirl.
    void updateFlow(const PyroSolver& gas);
    /// A layer of noise whose time is up starts afresh; the layers' weights.
    void renewLayers();
    /// The noise's coordinates carried by the solver's flow over dt.
    void advectShift(float dt);
    /// What the gas carries carried by the flow and the whirls over dt.
    void advect(bool closedFloor, float dt);
    /// Burning, then the fading of what the gas carries.
    void combust(const SolverSettings& s, float dt);
    /// The whirls at (x, y, z), fine cell units, m/s -- at a lookup on the
    /// solver's grid there.
    void turbulence(float x, float y, float z, const SparseGrid::Corners& at, float out[3]) const;

    UpresSettings settings_;
    Domain coarse_, domain_;
    int n_[3] = {0, 0, 0};
    int scale_ = 2;  // fine cells a solver's cell, along each axis
    std::shared_ptr<const Tiles> cells_;
    std::vector<uint32_t> fresh_;  // slots of the tiles new since solids were found
    SparseGrid density_, temperature_, fuel_, flame_;
    /// 1 + the index of the collider a cell's centre is in; 0 outside them.
    SparseGrid solid_;
    std::vector<Collider> colliders_;  // what solid_ was found for
    bool solidsFound_ = false, anySolid_ = false;
    SparseGrid expansion_;  // how fast the gas swells, 1/s
    // Scratch, for the four fields in turn: what the first pass of
    // MacCormack takes, the least and the most it interpolated from; and
    // where each cell's gas goes, fine cell units.
    SparseGrid predicted_[4], lows_[4], highs_[4], forward_[3];
    // On the solver's grid, on its tiles: its flow at the cells' centres
    // (m/s), how strong the whirls are (m/s), and how far each layer's
    // noise coordinates have been carried from where they started (cell
    // units) -- with room for the next.
    std::shared_ptr<const Tiles> coarseTiles_;
    SparseGrid centre_[3], strength_, shift_[2][3], shiftNext_[2][3];
    /// Curl noise, kTile samples a side, periodic: the whirls are read from
    /// it (Upres.cpp).
    std::vector<float> noise_;
    int cycle_[2] = {0, 0};  // how many times each layer has started afresh
    float weight_[2] = {0.0f, 1.0f};
    Vec3 offset_[2];  // where in the noise each layer's pattern is
    int octaves_ = 1;
    float octaveWeight_[4] = {1.0f, 0.0f, 0.0f, 0.0f};
    int frame_ = 0;
    float time_ = 0.0f;
    Times times_;
};

/// The upres's gas as a frame, on its fine grid.
Frame capture(const UpresSolver& upres);

}  // namespace pg::sim
