#pragma once
//
// The pressure equation of the projection, solved by geometric multigrid.
//
//   sum over the six faces f of a_f (p[neighbour across f] - p) / h^2 = b
//
// on a box of cells. a_f is 1 where the face is open and 0 where it is
// blocked -- by a solid cell on either side, or by a wall. Across an open side
// of the box the atmosphere holds p = 0: the ghost cell outside holds -p, so
// that the pressure passes through 0 on the face. Against a wall nothing
// flows through, and the face drops out of the equation.
//
// Plain iteration (Jacobi, Gauss-Seidel) quickly evens out the error between
// neighbouring cells, but an error that is smooth across the box shrinks by a
// hair per sweep -- the slower the finer the grid. Multigrid hands the smooth
// part to a grid of half the resolution, where it is less smooth, and so on
// down to a few cells, then adds the corrections back on the way up: a
// V-cycle. Each cycle removes most of the error, at any resolution. With
// solids, a coarse face is as open as the four fine faces it covers are on
// average.
//
// Deterministic: red-black ordering, each half-sweep reading only cells of the
// other colour, and no sums across cells.
//
#include "pg/sim/Grid.h"

#include <vector>

namespace pg::sim {

/// What the pressure meets at the sides of the box and inside it.
struct PoissonBoundary {
    /// Walls instead of open sides, in the order -x, +x, -y, +y, -z, +z.
    bool closed[6] = {false, false, false, false, false, false};
    /// Cells a solid takes (value above 0.5), the size of the pressure grid.
    /// Null: no solids.
    const Grid* solid = nullptr;
};

class PoissonSolver {
public:
    /// The boundary of the solves to come; the solid mask is read now.
    void setBoundary(const PoissonBoundary& boundary);

    /// Improves `p`, which comes in as the first guess -- the pressure of the
    /// previous step -- with `cycles` V-cycles. `p` and `b` share their size.
    void solve(Grid& p, const Grid& b, float h, int cycles);

    /// Mean |b - A p| over the cells: how far `p` is from solving the equation.
    double residual(const Grid& p, const Grid& b, float h);

    /// Grids in the hierarchy of the last solve(), the finest included. A grid
    /// is halved while its sides are even and at least 4 cells long.
    int levels() const { return static_cast<int>(coarse_.size()) + 1; }

    /// The coefficient of a face: 1 open, 0 blocked; `axis` 0..2, face (i, j, k)
    /// lies between cells i-1 and i along x (and so on). For tests.
    float faceOpen(int axis, int i, int j, int k) const;

private:
    /// Face coefficients and the diagonal of one level -- only with solids.
    struct Operator {
        Grid a[3];  // (nx+1) x ny x nz faces along x, and so on
        Grid diagonal;
    };
    struct Level {
        Grid p, b, r;
        Operator op;
        float h = 0.0f;
    };
    void build(const Grid& fine, float h);
    void vcycle(Grid& p, const Grid& b, Grid& r, const Operator& op, float h, size_t next);
    void relax(Grid& p, const Grid& b, const Operator& op, float h, int sweeps, float omega) const;
    void computeResidual(const Grid& p, const Grid& b, const Operator& op, float h, Grid& r) const;
    void prolongAdd(const Grid& coarse, Grid& fine) const;

    bool closed_[6] = {false, false, false, false, false, false};
    Grid solid_;          // a copy of the mask, empty without solids
    bool dirty_ = true;   // the hierarchy has to be rebuilt
    std::vector<Level> coarse_;  // coarse_[0] is half the caller's resolution
    Grid fineResidual_;
    Operator fineOp_;
    int dims_[3] = {0, 0, 0};
    float h_ = 0.0f;
};

}  // namespace pg::sim
