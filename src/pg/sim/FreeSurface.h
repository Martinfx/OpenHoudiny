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
// coarse grids a cell is liquid if any of its eight is, solid if all eight
// are, a face as open as the four it covers are on average, and the surface
// lies on the faces. The V-cycle is not exactly symmetric, so the conjugate
// gradients are the flexible kind (Polak-Ribiere's beta), which do not need
// it to be.
//
// Sparse (SparseGrid.h): the grids are kept in the tiles of the liquid's
// cells (the liquid's own, Liquid.h), and each coarse grid in the tiles
// over those of the grid below it. A coarse cell reaches further than the
// tiles under it, though -- four fine cells on the second grid, sixteen on
// the fourth -- so what lies round the liquid, the walls and the solids, is
// kept on every grid apart from it, whatever the liquid does (SolidLevels):
// a cell not kept is as solid, and a face as open, as on the dense grids.
// With the liquid's tiles or with all of them, the pressure is the same, to
// the bit.
//
// Deterministic: red-black Gauss-Seidel, and every sum over cells in a fixed
// order (pg::parallelReduce) -- that of the rows of the whole grid, however
// the tiles lie.
//
#include "pg/sim/Grid.h"
#include "pg/sim/SparseGrid.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace pg::sim {

/// The walls and the solids round a liquid -- the part of its pressure's
/// system that is not the liquid's -- on the grid of its cells and on every
/// coarser grid of the multigrid: which cells are solid through and
/// through, and how open each face is. Kept in tiles round the solids; a
/// cell anywhere else is clear and a face open, but on a wall.
class SolidLevels {
public:
    SolidLevels() = default;
    /// A grid of nx x ny x nz cells, and below it every coarser grid of the
    /// multigrid; `closed`: which of its sides are walls, in the order -x,
    /// +x, -y, +y, -z, +z. No solids.
    SolidLevels(int nx, int ny, int nz, const bool closed[6]);
    /// The solids of the finest grid, which make those of the coarser ones:
    /// the tiles kept round them, whether each kept cell is solid (as
    /// SparseGrid::index() has them), and how open each face of those tiles
    /// is (on their Tiles::faces(); a face not kept is open). A wall is
    /// closed whatever these say.
    void set(std::shared_ptr<const Tiles> tiles, std::vector<uint8_t> solid, SparseGrid open[3]);

    int levels() const { return static_cast<int>(levels_.size()); }
    /// The cells of grid `level` along each axis.
    const int* cells(int level) const { return levels_[static_cast<size_t>(level)].n; }
    /// The tiles kept on grid `level`.
    const Tiles& tiles(int level) const { return *levels_[static_cast<size_t>(level)].tiles; }
    /// How open face (i, j, k) along `axis` of grid `level` is, 0 to 1.
    float open(int level, int axis, int i, int j, int k) const;
    /// Cell (i, j, k) of grid `level` is solid: every cell of the finest grid
    /// under it is.
    bool solid(int level, int i, int j, int k) const;
    /// The kept openness of the tile of faces along `axis` of grid `level`
    /// that holds face (i, j, k) -- its 512 values, as SparseGrid::local()
    /// has them -- or null where the tile is not kept: every face of it open
    /// but on a wall.
    const float* openTile(int level, int axis, int i, int j, int k) const;
    /// Is side `side` of the grid -- -x, +x, -y, +y, -z, +z -- a wall?
    bool closed(int side) const { return closed_[side]; }
    /// Is a grid of `n` cells halved for the next: its sides even, 4 cells
    /// at least.
    static bool halves(const int n[3]);

private:
    struct Level {
        int n[3] = {0, 0, 0};
        std::shared_ptr<const Tiles> tiles;
        std::vector<uint8_t> solid;  // per kept cell
        SparseGrid open[3];          // 1 where not kept
    };
    std::vector<Level> levels_;
    bool closed_[6] = {false, false, false, false, false, false};
};

class FreeSurfaceSolver {
public:
    /// What a cell holds. Solid cells are air to the equation -- their open
    /// faces are the caller's to close -- but they guide the coarse grids.
    enum Cell : uint8_t { Air = 0, Liquid = 1, Solid = 2 };

    /// The system to solve on the tiles of a liquid's cells: `cells`, what
    /// each kept cell holds (as SparseGrid::index() has them) -- a cell not
    /// kept is air, or as solid as `solids` says; `phi`, the liquid's
    /// signed distance at the cell centres on those tiles (any unit, below 0
    /// inside; only its ratios across faces are used); `solids`, the walls
    /// and solids of the grid -- read until the next setSystem().
    void setSystem(std::shared_ptr<const Tiles> tiles, const std::vector<uint8_t>& cells, const SparseGrid& phi,
                   const SolidLevels& solids);
    /// Solves for p in the liquid cells, `p` and `rhs` on the system's tiles.
    /// `p` comes in as the first guess -- the last step's pressure -- and goes
    /// out as the solution, 0 outside the liquid. Stops when the largest
    /// |rhs - A p| is at most `tolerance` times the largest |rhs|, or after
    /// `maxIterations`; returns the iterations.
    int solve(SparseGrid& p, const SparseGrid& rhs, float tolerance, int maxIterations);

    /// The same on a whole grid of nx x ny x nz cells -- every tile kept --
    /// for tests: `cells` x fastest; `open`, how open each face is, three
    /// grids the shape of a MAC grid's velocity faces ((nx+1) x ny x nz along
    /// x, and so on), a face on a side of the grid a wall when closed, open
    /// to air when open.
    void setSystem(const std::vector<uint8_t>& cells, const Grid open[3], const Grid& phi);
    int solve(Grid& p, const Grid& rhs, float tolerance, int maxIterations);
    /// (A p)_c for the finest grid of the whole-grid system: for tests.
    void apply(const Grid& p, Grid& out) const;

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

private:
    struct Level {
        int n[3] = {0, 0, 0};
        std::shared_ptr<const Tiles> tiles;
        std::vector<uint8_t> cells;  // per kept cell
        SparseGrid weight[3];        // on the faces: between two liquid cells, open; else 0
        SparseGrid diagonal;         // every face's weight, to liquid and to air
        SparseGrid x, b, r;          // a correction, its right-hand side, its residual
        std::vector<uint32_t> busy;  // the kept tiles with liquid in them, by slot: the others are skipped
    };
    /// What cell (i, j, k) of grid `level` holds, kept or not.
    uint8_t kind(size_t level, int i, int j, int k) const;
    void build(size_t level, const SparseGrid* phi, std::vector<uint8_t>* links);
    void coarsen(size_t fine);
    void applyOn(const Level& level, const SparseGrid& x, SparseGrid& out) const;
    void residualOn(const Level& level, const SparseGrid& x, const SparseGrid& b, SparseGrid& r) const;
    void relax(const Level& level, SparseGrid& x, const SparseGrid& b, int colour, float omega) const;
    void restrictTo(const Level& fine, const SparseGrid& r, Level& coarse) const;
    void prolongAdd(size_t coarse, const Level& fine, SparseGrid& x) const;
    void vcycle(size_t level);
    /// z = M^-1 r: one V-cycle from 0.
    void precondition(const SparseGrid& r, SparseGrid& z);
    /// The finest grid's rows with liquid in them, and their stretches.
    void findRows();
    /// The pockets of the finest grid, from the liquid that reaches air.
    void findPockets();
    /// The conjugate gradients from p, to b.
    int iterate(SparseGrid& p, const SparseGrid& b, float tolerance, int maxIterations);
    /// The whole-grid system's values on its tiles, and back.
    SparseGrid kept(const Grid& g) const;
    void spread(const SparseGrid& g, Grid& out) const;

    /// f(begin, end) for the cells of the rows with liquid in them, eight
    /// at a time -- the stretches of a row in the tiles where it has some
    /// -- in chunks of whole rows; the chunks follow from the rows alone.
    template <class F>
    void forActiveRows(const F& f) const;
    /// The sum of map(begin, end) over a row's stretches, of those over the
    /// rows of a chunk, of those over the chunks, in their order: the order
    /// of the rows of the whole grid, x fastest, as a dense grid's.
    template <class Map>
    double sumActive(const Map& map) const;

    std::vector<Level> levels_;
    const SolidLevels* solids_ = nullptr;
    SolidLevels ownSolids_;              // the whole-grid system's
    // The finest grid's rows with liquid (j + ny * k), in order; row r's
    // stretches of eight cells start at stretch_[rowStart_[r]] up to
    // stretch_[rowStart_[r + 1]], in order along x.
    std::vector<uint32_t> activeRows_, rowStart_, stretch_;
    // The pockets: the cells of each, one pocket after another, and where
    // each starts in pocketCells_ (and where the last ends).
    std::vector<uint32_t> pocketCells_, pocketStart_;
    std::vector<uint8_t> links_;     // the finest grid's: which faces of a liquid cell lead on, to what
    std::vector<uint32_t> reached_;  // findPockets': the cells the liquid reaches from the air
    SparseGrid b_;  // with pockets: the right-hand side less each pocket's mean
    SparseGrid r_, rOld_, z_, d_, q_;
    size_t unknowns_ = 0;
    double residual_ = 0.0;
};

}  // namespace pg::sim
