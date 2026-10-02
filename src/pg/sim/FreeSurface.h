#pragma once
//
// The pressure of a liquid with a free surface (Liquid.h): the equation of
// the projection on the cells the liquid fills, the pressure held at 0 where
// the air is, and nothing flowing through solids.
//
//   sum over the six faces f of liquid cell c:  w_f (p_c - p_n) = rhs_c
//
// with p = 0 in every cell that is not liquid -- the equation of Poisson.h
// multiplied by h^2. The weight of a face:
//
//   between two liquid cells   open_f, how much of the face a solid leaves
//                              open, 0 to 1 (Batty, Bertails and Bridson
//                              2007: a wall that cuts through cells is felt
//                              where it is, not as a staircase);
//   from liquid into air       open_f / theta: the surface crosses the face a
//                              share theta of the way from the liquid cell's
//                              centre (from the liquid's signed distance), and
//                              the pressure is 0 there, not at the air cell's
//                              centre (the ghost fluid method, Gibou et al.
//                              2002). Past an open side of the grid, theta is
//                              1/2: the pressure is 0 on the side.
//
// Liquid that reaches no air -- a pocket shut in by solids and closed sides
// -- has its pressure known only up to a constant, and only if as much flows
// into it as out: a solid moving into a pocket, or the round-off of its
// faces, leaves an equation no pressure solves, and conjugate gradients on it
// go off to infinity. In a pocket, what flows in squeezes the liquid alike
// in all its cells -- each pocket's mean taken off the right-hand side -- and
// its pressure keeps the level it came with.
//
// Solved by conjugate gradients, preconditioned by a multigrid V-cycle
// (McAdams, Sifakis and Teran 2010): the V-cycle alone struggles where the
// surface is irregular, CG alone would take hundreds of iterations. On the
// coarse grids a cell is liquid if any of its eight is, a face as open as the
// four it covers are on average, and the surface lies on the faces. The
// V-cycle is not exactly symmetric, so the conjugate gradients are the
// flexible kind (Polak-Ribiere's beta), which do not need it to be.
//
// Deterministic: red-black Gauss-Seidel, and every sum over cells in a fixed
// order (pg::parallelReduce).
//
#include "pg/sim/Grid.h"

#include <cstdint>
#include <vector>

namespace pg::sim {

class FreeSurfaceSolver {
public:
    /// What a cell holds. Solid cells are air to the equation -- their open
    /// faces are the caller's to close -- but they guide the coarse grids.
    enum Cell : uint8_t { Air = 0, Liquid = 1, Solid = 2 };

    /// The system to solve on a grid of nx x ny x nz cells. `cells`: what each
    /// cell holds, x fastest. `open`: how open each face is, three grids the
    /// shape of a MAC grid's velocity faces ((nx+1) x ny x nz along x, and so
    /// on); a face on a side of the grid is a wall when closed, open to air
    /// when open. `phi`: the liquid's signed distance at the cell centres (any
    /// unit), below 0 inside -- only its ratios across faces are used.
    void setSystem(const std::vector<uint8_t>& cells, const Grid open[3], const Grid& phi);

    /// Solves for p in the liquid cells. `p` comes in as the first guess --
    /// the last step's pressure -- and goes out as the solution, 0 outside the
    /// liquid. Stops when the largest |rhs - A p| is at most `tolerance` times
    /// the largest |rhs|, or after `maxIterations`; returns the iterations.
    int solve(Grid& p, const Grid& rhs, float tolerance, int maxIterations);
    /// Of the last solve: the largest |rhs - A p| over the largest |rhs|.
    double residual() const { return residual_; }

    /// The weight of the face between liquid and air as the equation uses it,
    /// for the gradient that goes with the pressure: open_f / theta.
    float airWeight(float open, float phiLiquid, float phiAir) const;
    /// theta: where the surface crosses from a liquid cell to an air cell, as
    /// a share of the way from the liquid cell's centre -- at least kMinTheta,
    /// so that a surface grazing a centre does not make the equation stiff.
    static float surfaceFraction(float phiLiquid, float phiAir);
    static constexpr float kMinTheta = 0.05f;

    int levels() const { return static_cast<int>(levels_.size()); }
    /// Liquid cells of the finest grid that take part: those with an open face.
    size_t unknowns() const { return unknowns_; }
    /// Pockets of the last system: liquid that reaches no air.
    size_t pockets() const { return pocketStart_.empty() ? 0 : pocketStart_.size() - 1; }

    /// (A p)_c for the finest grid: for tests.
    void apply(const Grid& p, Grid& out) const;

private:
    struct Level {
        int n[3] = {0, 0, 0};
        std::vector<uint8_t> cells;
        Grid open[3];     // how open each face is
        Grid weight[3];   // between two liquid cells: open; else 0
        Grid diagonal;    // every face's weight, to liquid and to air
        Grid x, b, r;     // a correction, its right-hand side, its residual
        std::vector<uint8_t> rows;  // a row of cells (j + ny * k) with liquid in it: the others are skipped
    };
    void build(Level& level, const Grid* phi, std::vector<uint8_t>* links) const;
    void coarsen(const Level& fine, Level& coarse) const;
    void applyOn(const Level& level, const Grid& x, Grid& out) const;
    void residualOn(const Level& level, const Grid& x, const Grid& b, Grid& r) const;
    void relax(const Level& level, Grid& x, const Grid& b, int colour, float omega) const;
    void restrictTo(const Level& fine, const Grid& r, Level& coarse) const;
    void prolongAdd(const Level& coarse, const Level& fine, Grid& x) const;
    void vcycle(size_t level);
    /// z = M^-1 r: one V-cycle from 0.
    void precondition(const Grid& r, Grid& z);
    /// The pockets of the finest grid, from the liquid that reaches air.
    void findPockets();
    /// The conjugate gradients from p, to b.
    int iterate(Grid& p, const Grid& b, float tolerance, int maxIterations);

    /// f(begin, end) for the cells of the rows with liquid in them, in
    /// chunks of whole rows; the chunks follow from the rows alone.
    template <class F>
    void forActiveRows(const F& f) const;
    /// The sum of map(begin, end) over those chunks, in their order.
    template <class Map>
    double sumActive(const Map& map) const;

    std::vector<Level> levels_;
    std::vector<uint32_t> activeRows_;  // the finest grid's rows with liquid, in order
    // The pockets: the cells of each, one pocket after another, and where
    // each starts in pocketCells_ (and where the last ends).
    std::vector<uint32_t> pocketCells_, pocketStart_;
    std::vector<uint8_t> links_;     // the finest grid's: which faces of a liquid cell lead on, to what
    std::vector<uint32_t> reached_;  // findPockets': the cells the liquid reaches from the air
    Grid b_;  // with pockets: the right-hand side less each pocket's mean
    Grid r_, rOld_, z_, d_, q_;
    size_t unknowns_ = 0;
    double residual_ = 0.0;
};

}  // namespace pg::sim
