#pragma once
//
// A frame of a simulation as it is kept and drawn -- what the editor caches
// for the timeline and the renderer uploads as it is:
//
//   gas    smoke, temperature and flame per cell, as half floats (IEEE 754
//          binary16) -- six bytes a cell, a quarter of what the solver holds;
//   water  the distance to the water's surface and how white it is, on a
//          grid twice as fine as the liquid solver's, a byte each.
//
#include "pg/sim/Scene.h"

#include <cstdint>
#include <vector>

namespace pg::sim {

class LiquidSolver;
class PyroSolver;

/// The water of a frame, as it is drawn.
struct WaterFrame {
    Domain domain;       ///< the grid of `cells`: where the solver's domain is, twice as fine
    float band = 0.0f;   ///< distances beyond +-band are cut off, world units
    /// Per cell in turn, x fastest: the distance to the surface -- 0 at -band
    /// (in the water) to 255 at +band -- and the foam, 0 to 255.
    std::vector<uint8_t> cells;
    size_t particles = 0;  ///< the solver's, when the frame was taken
    double litres = 0.0;

    bool empty() const { return cells.empty(); }
    size_t bytes() const { return cells.size(); }
    /// The distance at cell (i, j, k), world units, below 0 in the water.
    float distance(int i, int j, int k) const;
    /// The foam there, 0 to 1.
    float foam(int i, int j, int k) const;
};

struct Frame {
    int number = 0;     ///< frames simulated to get here: 1 after the first step
    float time = 0.0f;  ///< seconds
    Domain domain;
    /// The gas: smoke, temperature and flame of each cell of `domain` in
    /// turn, x fastest, then y, then z. Empty without gas.
    std::vector<uint16_t> fields;
    WaterFrame water;     ///< empty without water
    double stepMs = 0.0;  ///< how long the step to it took

    size_t bytes() const { return sizeof(Frame) + fields.size() * sizeof(uint16_t) + water.bytes(); }
    bool empty() const { return fields.empty() && water.empty(); }
    /// The field `channel` (0 smoke, 1 temperature, 2 flame) of cell (i, j, k).
    float at(int channel, int i, int j, int k) const;
};

/// The solver's gas as a frame.
Frame capture(const PyroSolver& sim);
/// The solver's water, as a frame holds it.
WaterFrame capture(const LiquidSolver& sim);

/// Round to nearest even; beyond the largest half, infinity; NaN stays NaN.
uint16_t toHalf(float value);
float fromHalf(uint16_t value);

}  // namespace pg::sim
