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
// Sparse (SparseGrid.h): the equation holds in the cells that count; a cell
// that does not holds p = 0 -- the still air round the gas, as open as the
// sides of the box. A coarse cell counts when all eight of its cells do: the
// still air never reaches further on a coarse grid than on the fine one
// (after McAdams, Sifakis and Teran, 2010). With every tile active, the
// solve is the dense one, to the bit.
//
// Deterministic: red-black ordering, each half-sweep reading only cells of the
// other colour, and no sums across cells. A sweep multiplies by 1 / the
// diagonal, worked out once, rather than dividing: a GPU's division is not
// rounded as the CPU's is, its products are -- the GPU's solve (PyroGpu)
// gives this one's to the bit.
//
#include "pg/sim/SparseGrid.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace pg::sim {

/// What the pressure meets at the sides of the box and inside it.
struct PoissonBoundary {
    /// Walls instead of open sides, in the order -x, +x, -y, +y, -z, +z.
    bool closed[6] = {false, false, false, false, false, false};
    /// Cells a solid takes (value above 0.5), the size of the pressure grid
    /// and of its tiles. Null: no solids.
    const SparseGrid* solid = nullptr;
};

class PoissonSolver {
public:
    /// The boundary of the solves to come; the solid mask is read now.
    void setBoundary(const PoissonBoundary& boundary);

    /// Improves `p`, which comes in as the first guess -- the pressure of the
    /// previous step -- with `cycles` V-cycles. `p` and `b` share their tiles.
    void solve(SparseGrid& p, const SparseGrid& b, float h, int cycles);

    /// Mean |b - A p| over the cells that count: how far `p` is from solving
    /// the equation.
    double residual(const SparseGrid& p, const SparseGrid& b, float h);

    /// Grids in the hierarchy of the last solve(), the finest included. A grid
    /// is halved while its sides are even and at least 4 cells long.
    int levels() const { return static_cast<int>(coarse_.size()) + 1; }

    /// The coefficient of a face: 1 open, 0 blocked; `axis` 0..2, face (i, j, k)
    /// lies between cells i-1 and i along x (and so on). For tests.
    float faceOpen(int axis, int i, int j, int k) const;

    /// 1 / d for the diagonal d of a box without solids: a whole number, 6,
    /// one more for each open side the cell touches, one less for each wall.
    static float inverseOf(int diagonal);

    /// Sweeps before and after the coarser level's correction.
    static constexpr int kPreSmooth = 2, kPostSmooth = 2;
    /// The coarsest level, solved by over-relaxed sweeps: how many, and by
    /// how much -- the optimum for Jacobi's rate on that box.
    static int coarsestSweeps(int nx, int ny, int nz);
    static float coarsestOmega(int nx, int ny, int nz);

private:
    friend class PyroGpu;
    /// Face coefficients and the diagonal of one level -- only with solids.
    /// The finest level's faces are open or not: a bit each, the six of a
    /// cell in a byte (open), not grids of faces (a).
    struct Operator {
        SparseGrid a[3];  // (nx+1) x ny x nz faces along x, and so on
        std::vector<uint8_t> open;  // bit 2a: the face below along a, 2a + 1: above
        SparseGrid diagonal;
        SparseGrid inverse;  // 1 / diagonal; 0 where it is 0: walled in
    };
    /// Which stored cells of a level count, one byte each, as data() has them.
    using Counts = std::vector<uint8_t>;
    struct Level {
        SparseGrid p, b, r;
        Counts on;
        Operator op;
        float h = 0.0f;
    };
    void build(const SparseGrid& fine, float h);
    void vcycle(SparseGrid& p, const SparseGrid& b, SparseGrid& r, const Counts& on, const Operator& op, float h,
                size_t next);
    void relax(SparseGrid& p, const SparseGrid& b, const Counts& on, const Operator& op, float h, int sweeps,
               float omega) const;
    void computeResidual(const SparseGrid& p, const SparseGrid& b, const Counts& on, const Operator& op, float h,
                         SparseGrid& r) const;
    void prolongAdd(const SparseGrid& coarse, SparseGrid& fine, const Counts& on) const;
    /// The coefficient of face (i, j, k) along `axis` of a level whose cells
    /// are p's, and whose operator is `op` -- open between cells that do
    /// not count, but at a wall.
    float faceOf(const SparseGrid& p, const Operator& op, int axis, int i, int j, int k) const;

    bool closed_[6] = {false, false, false, false, false, false};
    SparseGrid solid_;    // a copy of the mask, empty without solids
    bool solids_ = false;
    bool dirty_ = true;   // the hierarchy has to be rebuilt
    std::vector<Level> coarse_;  // coarse_[0] is half the caller's resolution
    SparseGrid fineResidual_;
    Counts fineOn_;
    Operator fineOp_;
    std::shared_ptr<const Tiles> tiles_;  // the fine level's, as last built
    int dims_[3] = {0, 0, 0};
    float h_ = 0.0f;
    uint64_t generation_ = 0;  // one more each time the hierarchy is built
};

}  // namespace pg::sim
