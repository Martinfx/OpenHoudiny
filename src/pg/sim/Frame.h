#pragma once
//
// A frame of a simulation as it is kept and drawn: smoke, temperature and
// flame per cell, as half floats (IEEE 754 binary16) -- six bytes a cell, a
// quarter of what the solver holds. What the editor caches for the timeline
// and the renderer uploads as it is.
//
#include "pg/sim/Scene.h"

#include <cstdint>
#include <vector>

namespace pg::sim {

class PyroSolver;

struct Frame {
    int number = 0;     ///< frames simulated to get here: 1 after the first step
    float time = 0.0f;  ///< seconds
    Domain domain;
    /// Smoke, temperature and flame of each cell in turn, x fastest, then y, then z.
    std::vector<uint16_t> fields;
    double stepMs = 0.0;  ///< how long the step to it took

    size_t bytes() const { return sizeof(Frame) + fields.size() * sizeof(uint16_t); }
    bool empty() const { return fields.empty(); }
    /// The field `channel` (0 smoke, 1 temperature, 2 flame) of cell (i, j, k).
    float at(int channel, int i, int j, int k) const;
};

/// The solver's gas as a frame.
Frame capture(const PyroSolver& sim);

/// Round to nearest even; beyond the largest half, infinity; NaN stays NaN.
uint16_t toHalf(float value);
float fromHalf(uint16_t value);

}  // namespace pg::sim
