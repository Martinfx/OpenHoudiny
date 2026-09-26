#pragma once
//
// The pressure equation of the projection, solved by geometric multigrid.
//
//   sum over the six neighbours n of (p[n] - p) / h^2 = b
//
// on a box of cells, with p = 0 on the faces of the box: open boundaries, the
// atmosphere outside. A cell next to a face sees a ghost neighbour of -p, so
// that the pressure passes through 0 on the face.
//
// Plain iteration (Jacobi, Gauss-Seidel) quickly evens out the error between
// neighbouring cells, but an error that is smooth across the box shrinks by a
// hair per sweep -- the slower the finer the grid. Multigrid hands the smooth
// part to a grid of half the resolution, where it is less smooth, and so on
// down to a few cells, then adds the corrections back on the way up: a
// V-cycle. Each cycle removes most of the error, at any resolution.
//
// Deterministic: red-black ordering, each half-sweep reading only cells of the
// other colour, and no sums across cells.
//
#include "pg/sim/Grid.h"

#include <vector>

namespace pg::sim {

class PoissonSolver {
public:
    /// Improves `p`, which comes in as the first guess -- the pressure of the
    /// previous step -- with `cycles` V-cycles. `p` and `b` share their size.
    void solve(Grid& p, const Grid& b, float h, int cycles);

    /// Mean |b - A p| over the cells: how far `p` is from solving the equation.
    static double residual(const Grid& p, const Grid& b, float h);

    /// Grids in the hierarchy of the last solve(), the finest included. A grid
    /// is halved while its sides are even and at least 4 cells long.
    int levels() const { return static_cast<int>(coarse_.size()) + 1; }

private:
    struct Level {
        Grid p, b, r;
        float h = 0.0f;
    };
    void build(const Grid& fine, float h);
    void vcycle(Grid& p, const Grid& b, Grid& r, float h, size_t next);

    std::vector<Level> coarse_;  // coarse_[0] is half the caller's resolution
    Grid fineResidual_;
};

}  // namespace pg::sim
