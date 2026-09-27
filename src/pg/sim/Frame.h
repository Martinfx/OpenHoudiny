#pragma once
//
// A frame of a simulation as it is kept and drawn -- what the editor caches
// for the timeline and the renderer uploads as it is:
//
//   gas    smoke, temperature and flame per cell, as half floats (IEEE 754
//          binary16) -- six bytes a cell, a quarter of what the solver holds;
//   water  the distance to the water's surface and how white it is, on a
//          grid twice as fine as the liquid solver's, a byte each;
//   rain   where each drop and droplet is and how fast it goes, and the
//          ripples on the water.
//
#include "pg/sim/Rigid.h"
#include "pg/sim/Scene.h"

#include <cstdint>
#include <vector>

namespace pg::sim {

class LiquidSolver;
class PyroSolver;
class RainSolver;

/// The water of a frame, as it is drawn.
struct WaterFrame {
    Domain domain;       ///< the grid of `cells`: where the solver's domain is, twice as fine
    float band = 0.0f;   ///< distances beyond +-band are cut off, world units
    /// Per cell in turn, x fastest: the distance to the surface -- 0 at -band
    /// (in the water) to 255 at +band -- and the foam, 0 to 255.
    std::vector<uint8_t> cells;
    size_t particles = 0;  ///< the solver's, when the frame was taken
    double litres = 0.0;
    /// The particles themselves, when the world keeps them
    /// (World::keepParticles): where each is, how fast it goes (half floats,
    /// three a particle) and how white (0 to 255).
    std::vector<Vec3> positions;
    std::vector<uint16_t> velocities;
    std::vector<uint8_t> whiteness;

    bool empty() const { return cells.empty(); }
    size_t bytes() const {
        return cells.size() + positions.size() * sizeof(Vec3) + velocities.size() * sizeof(uint16_t) + whiteness.size();
    }
    /// The distance at cell (i, j, k), world units, below 0 in the water.
    float distance(int i, int j, int k) const;
    /// The foam there, 0 to 1.
    float foam(int i, int j, int k) const;
};

/// The rain of a frame, as it is drawn.
struct RainFrame {
    /// Drops, then droplets: six floats each -- position and velocity,
    /// world units -- one after the other.
    std::vector<float> drops, droplets;
    float timeStep = 0.0f;   ///< seconds a frame: how long a streak of motion blur a drop draws
    /// The ripples on the water: heights, world units, as half floats, over
    /// nx x nz cells of `cell` from `origin` (Rain.h); empty without water.
    Vec3 rippleOrigin;
    float rippleCell = 0.0f;
    int rippleCells[2] = {0, 0};
    std::vector<uint16_t> ripples;

    size_t dropCount() const { return drops.size() / 6; }
    size_t dropletCount() const { return droplets.size() / 6; }
    bool empty() const { return drops.empty() && droplets.empty() && ripples.empty(); }
    size_t bytes() const { return (drops.size() + droplets.size()) * sizeof(float) + ripples.size() * sizeof(uint16_t); }
};

struct Frame {
    int number = 0;     ///< frames simulated to get here: 1 after the first step
    float time = 0.0f;  ///< seconds
    Domain domain;
    /// The gas: smoke, temperature and flame of each cell of `domain` in
    /// turn, x fastest, then y, then z. Empty without gas.
    std::vector<uint16_t> fields;
    WaterFrame water;     ///< empty without water
    RainFrame rain;       ///< empty without rain
    RigidFrame rigid;     ///< empty without rigid bodies
    double stepMs = 0.0;  ///< how long the step to it took

    size_t bytes() const {
        return sizeof(Frame) + fields.size() * sizeof(uint16_t) + water.bytes() + rain.bytes() + rigid.bytes();
    }
    bool empty() const { return fields.empty() && water.empty() && rain.empty() && rigid.empty(); }
    /// The field `channel` (0 smoke, 1 temperature, 2 flame) of cell (i, j, k).
    float at(int channel, int i, int j, int k) const;
};

/// The solver's gas as a frame.
Frame capture(const PyroSolver& sim);
/// The solver's water, as a frame holds it -- and its particles, when
/// `particles` is set.
WaterFrame capture(const LiquidSolver& sim, bool particles = false);
/// The rain, as a frame holds it.
RainFrame capture(const RainSolver& sim);

/// Round to nearest even; beyond the largest half, infinity; NaN stays NaN.
uint16_t toHalf(float value);
float fromHalf(uint16_t value);

}  // namespace pg::sim
