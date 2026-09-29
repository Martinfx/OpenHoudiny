#pragma once
//
// A frame of a simulation as it is kept and drawn -- what the editor caches
// for the timeline and the renderer uploads as it is:
//
//   gas    smoke, temperature and flame per cell, as half floats (IEEE 754
//          binary16) -- six bytes a cell, a quarter of what the solver holds;
//          from a sparse solver, only the tiles that hold any;
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
    /// three a particle), how white (0 to 255) and its own number -- the same
    /// from frame to frame (empty in a frame cached before they were kept).
    std::vector<Vec3> positions;
    std::vector<uint16_t> velocities;
    std::vector<uint8_t> whiteness;
    std::vector<uint32_t> ids;
    /// How fast the water goes at the centre of each cell of the solver's
    /// grid -- half as fine as `domain` -- three half floats a cell, x
    /// fastest: what moves its surface, for motion blur. In the water and
    /// some two cells round its surface; 0 further. Empty in a frame cached
    /// before it was kept.
    std::vector<uint16_t> flow;

    bool empty() const { return cells.empty(); }
    size_t bytes() const {
        return cells.size() + positions.size() * sizeof(Vec3) + velocities.size() * sizeof(uint16_t) + whiteness.size() +
               ids.size() * sizeof(uint32_t) + flow.size() * sizeof(uint16_t);
    }
    /// The distance at cell (i, j, k), world units, below 0 in the water.
    float distance(int i, int j, int k) const;
    /// The foam there, 0 to 1.
    float foam(int i, int j, int k) const;
    /// The grid of `flow`: the solver's.
    Domain flowDomain() const;
    /// The flow at a world point, trilinear between the cells' centres, the
    /// nearest cell's beyond them; 0 without it.
    Vec3 flowAt(const Vec3& p) const;
};

/// The rain of a frame, as it is drawn.
struct RainFrame {
    /// Drops, then droplets: six floats each -- position and velocity,
    /// world units -- one after the other; and the number of each, the same
    /// from frame to frame (empty in a frame cached before they were kept).
    std::vector<float> drops, droplets;
    std::vector<uint32_t> dropIds, dropletIds;
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
    size_t bytes() const {
        return (drops.size() + droplets.size()) * sizeof(float) + (dropIds.size() + dropletIds.size()) * sizeof(uint32_t) +
               ripples.size() * sizeof(uint16_t);
    }
};

struct Frame {
    int number = 0;     ///< frames simulated to get here: 1 after the first step
    float time = 0.0f;  ///< seconds
    Domain domain;
    /// The gas: smoke, temperature and flame of each cell of `domain` in
    /// turn, x fastest, then y, then z -- or, sparse, of the cells of the
    /// tiles in gasTiles alone. Empty without gas.
    std::vector<uint16_t> fields;
    /// Sparse gas: the tiles of 8 x 8 x 8 cells of `domain` that hold any,
    /// by number (x fastest, as Tiles numbers them), in order -- one at least.
    /// `fields` then holds their 512 cells each in turn, x fastest within the
    /// tile. Empty: `fields` holds every cell of the domain.
    std::vector<uint32_t> gasTiles;
    WaterFrame water;     ///< empty without water
    RainFrame rain;       ///< empty without rain
    RigidFrame rigid;     ///< empty without rigid bodies
    double stepMs = 0.0;  ///< how long the step to it took

    size_t bytes() const {
        return sizeof(Frame) + fields.size() * sizeof(uint16_t) + gasTiles.size() * sizeof(uint32_t) + water.bytes() +
               rain.bytes() + rigid.bytes();
    }
    bool empty() const { return fields.empty() && water.empty() && rain.empty() && rigid.empty(); }
    /// The field `channel` (0 smoke, 1 temperature, 2 flame) of cell (i, j, k).
    float at(int channel, int i, int j, int k) const;
    /// The gas of every cell of the domain, as `fields` holds it when not
    /// sparse: `fields` itself, or `scratch`, made from the tiles.
    const std::vector<uint16_t>& denseFields(std::vector<uint16_t>& scratch) const;
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
