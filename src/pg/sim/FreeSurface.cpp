#include "pg/sim/FreeSurface.h"

#include "pg/core/Parallel.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace pg::sim {
namespace {

/// Cells a chunk of the sums over rows takes at least: a dense grid's
/// chunks, whose sums the sparse ones add up to the bit.
constexpr size_t kChunkCells = 16384;
/// Tiles a chunk of the loops over tiles takes: 2048 cells, about what
/// handing a chunk to another thread costs. The tiles with liquid in them
/// are few on the coarse grids, and full or nearly empty: small chunks
/// spread them evenly.
constexpr size_t kChunkTiles = 4;

/// Of a liquid cell's links (FreeSurfaceSolver::links_): bit 2a + side for
/// the face behind (side 0) or ahead (1) along axis a when it leads on into
/// the liquid -- an open face between two liquid cells, one with a weight;
/// kToAir when a face leads to air or out of an open side; kReached once a
/// flood got to the cell.
constexpr uint8_t kToAir = 64, kReached = 128;

/// Strides of a cell's neighbours in a tile, along each axis.
constexpr int kStride[3] = {1, Tiles::kSide, Tiles::kSide * Tiles::kSide};

/// The weights of the eight coarse cells a fine cell takes its correction
/// from (prolongAdd): along each axis 3/4 its own, 1/4 the next towards it,
/// multiplied in the order of the axes.
constexpr std::array<float, 8> kTrilinear = [] {
    std::array<float, 8> w{};
    for (int q = 0; q < 8; ++q) {
        float v = 1.0f;
        for (int a = 0; a < 3; ++a) v *= (q >> a) & 1 ? 0.25f : 0.75f;
        w[static_cast<size_t>(q)] = v;
    }
    return w;
}();

size_t tileNumber(const int t[3], int a, int b, int c) {
    return static_cast<size_t>(a) +
           static_cast<size_t>(t[0]) * (static_cast<size_t>(b) + static_cast<size_t>(t[1]) * static_cast<size_t>(c));
}

/// The tiles of a grid half as fine as one of `fine`: those over its tiles.
std::shared_ptr<const Tiles> coarser(const Tiles& fine, const int n[3]) {
    int t[3];
    for (int a = 0; a < 3; ++a) t[a] = (n[a] + Tiles::kSide - 1) / Tiles::kSide;
    std::vector<uint8_t> state(static_cast<size_t>(t[0]) * static_cast<size_t>(t[1]) * static_cast<size_t>(t[2]),
                               Tiles::Off);
    for (const uint32_t tile : fine.stored()) {
        int c[3];
        fine.corner(tile, c[0], c[1], c[2]);
        state[tileNumber(t, c[0] / (2 * Tiles::kSide), c[1] / (2 * Tiles::kSide), c[2] / (2 * Tiles::kSide))] =
            Tiles::Whole;
    }
    return std::make_shared<const Tiles>(n[0], n[1], n[2], std::move(state), -1);
}

/// The cells of a kept tile of a grid: where it is, how much of it lies in
/// the grid, and where the cells and faces round its cells are kept -- the
/// six tiles beside it (-1 where not kept), and on each axis its own tile of
/// faces and the next one.
struct TileView {
    int corner[3] = {0, 0, 0};
    int extent[3] = {0, 0, 0};
    int64_t base = 0;                    // its first cell in data()
    int64_t beside[6] = {-1, -1, -1, -1, -1, -1};  // first cell of the tile along -x, +x, -y, +y, -z, +z
    int64_t faces[3] = {-1, -1, -1};     // first face of its own tile of faces
    int64_t ahead[3] = {-1, -1, -1};     // first face of the next tile of faces along the axis
};

TileView viewOf(const Tiles& tiles, uint32_t slot, const SparseGrid& cells, const SparseGrid* faces) {
    TileView v;
    tiles.corner(tiles.stored()[slot], v.corner[0], v.corner[1], v.corner[2]);
    const int n[3] = {tiles.nx(), tiles.ny(), tiles.nz()};
    for (int a = 0; a < 3; ++a) v.extent[a] = std::min(Tiles::kSide, n[a] - v.corner[a]);
    v.base = static_cast<int64_t>(slot) * Tiles::kCells;
    for (int d = 0; d < 6; ++d) {
        int c[3] = {v.corner[0], v.corner[1], v.corner[2]};
        c[d / 2] += d % 2 == 0 ? -Tiles::kSide : Tiles::kSide;
        if (c[d / 2] < 0 || c[d / 2] >= n[d / 2]) continue;
        const int32_t s = cells.slotOf(c[0], c[1], c[2]);
        if (s >= 0) v.beside[d] = static_cast<int64_t>(s) * Tiles::kCells;
    }
    if (faces) {
        for (int a = 0; a < 3; ++a) {
            const int32_t own = faces[a].slotOf(v.corner[0], v.corner[1], v.corner[2]);
            if (own >= 0) v.faces[a] = static_cast<int64_t>(own) * Tiles::kCells;
            int c[3] = {v.corner[0], v.corner[1], v.corner[2]};
            c[a] += Tiles::kSide;
            const int fn = (a == 0 ? faces[a].nx() : a == 1 ? faces[a].ny() : faces[a].nz());
            if (c[a] < fn) {
                const int32_t next = faces[a].slotOf(c[0], c[1], c[2]);
                if (next >= 0) v.ahead[a] = static_cast<int64_t>(next) * Tiles::kCells;
            }
        }
    }
    return v;
}

/// f(i, j, k, index) for every cell in the grid of the kept tiles `slots`,
/// in parallel, tile by tile.
template <class F>
void forTiles(const Tiles& tiles, const std::vector<uint32_t>& slots, const F& f) {
    const int n[3] = {tiles.nx(), tiles.ny(), tiles.nz()};
    pg::parallelFor(slots.size(), kChunkTiles, [&](size_t begin, size_t end) {
        for (size_t s = begin; s < end; ++s) {
            int c[3];
            tiles.corner(tiles.stored()[slots[s]], c[0], c[1], c[2]);
            const int e[3] = {std::min(Tiles::kSide, n[0] - c[0]), std::min(Tiles::kSide, n[1] - c[1]),
                              std::min(Tiles::kSide, n[2] - c[2])};
            const size_t base = static_cast<size_t>(slots[s]) * Tiles::kCells;
            for (int z = 0; z < e[2]; ++z) {
                for (int y = 0; y < e[1]; ++y) {
                    for (int x = 0; x < e[0]; ++x) f(c[0] + x, c[1] + y, c[2] + z, base + SparseGrid::local(x, y, z));
                }
            }
        }
    });
}

}  // namespace

// --- the walls and the solids, on every grid ----------------------------------------

bool SolidLevels::halves(const int n[3]) {
    return n[0] % 2 == 0 && n[1] % 2 == 0 && n[2] % 2 == 0 && std::min({n[0], n[1], n[2]}) >= 4;
}

SolidLevels::SolidLevels(int nx, int ny, int nz, const bool closed[6]) {
    for (int s = 0; s < 6; ++s) closed_[s] = closed[s];
    Level fine;
    fine.n[0] = nx;
    fine.n[1] = ny;
    fine.n[2] = nz;
    levels_.push_back(fine);
    while (halves(levels_.back().n)) {
        Level coarse;
        for (int a = 0; a < 3; ++a) coarse.n[a] = levels_.back().n[a] / 2;
        levels_.push_back(coarse);
    }
    for (Level& L : levels_) {
        L.tiles = std::make_shared<const Tiles>(L.n[0], L.n[1], L.n[2], std::vector<uint8_t>(), -1);
        for (int a = 0; a < 3; ++a) L.open[a] = SparseGrid(Tiles::faces(*L.tiles, a), 1.0f);
    }
}

void SolidLevels::set(std::shared_ptr<const Tiles> tiles, std::vector<uint8_t> solid, SparseGrid open[3]) {
    Level& fine = levels_.front();
    fine.tiles = std::move(tiles);
    fine.solid = std::move(solid);
    for (int a = 0; a < 3; ++a) fine.open[a] = std::move(open[a]);
    // Each coarser grid kept over the tiles of the finer: a cell solid if
    // all eight under it are, a face as open as the four it covers.
    for (size_t l = 1; l < levels_.size(); ++l) {
        Level& C = levels_[l];
        C.tiles = coarser(*levels_[l - 1].tiles, C.n);
        C.solid.assign(C.tiles->stored().size() * Tiles::kCells, 0);
        const int finer = static_cast<int>(l) - 1;
        forEachCounted(*C.tiles, [&](int i, int j, int k, size_t c) {
            bool all = true;
            for (int q = 0; q < 8 && all; ++q) all = this->solid(finer, 2 * i + (q & 1), 2 * j + ((q >> 1) & 1), 2 * k + (q >> 2));
            C.solid[c] = all ? 1 : 0;
        });
        for (int a = 0; a < 3; ++a) {
            C.open[a] = SparseGrid(Tiles::faces(*C.tiles, a), 1.0f);
            float* faces = C.open[a].data();
            forEachCounted(C.open[a].tiles(), [&](int i, int j, int k, size_t f) {
                float sum = 0.0f;
                for (int q = 0; q < 4; ++q) {
                    const int u = q & 1, w = q >> 1;  // the two directions across the face
                    const int fi = a == 0 ? 2 * i : 2 * i + u;
                    const int fj = a == 1 ? 2 * j : 2 * j + (a == 0 ? u : w);
                    const int fk = a == 2 ? 2 * k : 2 * k + w;
                    sum += this->open(finer, a, fi, fj, fk);
                }
                faces[f] = 0.25f * sum;
            });
        }
    }
}

float SolidLevels::open(int level, int axis, int i, int j, int k) const {
    const Level& L = levels_[static_cast<size_t>(level)];
    const int f = axis == 0 ? i : axis == 1 ? j : k;
    if ((f == 0 && closed_[2 * axis]) || (f == L.n[axis] && closed_[2 * axis + 1])) return 0.0f;
    return L.open[axis].at(i, j, k);
}

const float* SolidLevels::openTile(int level, int axis, int i, int j, int k) const {
    const SparseGrid& open = levels_[static_cast<size_t>(level)].open[axis];
    const int32_t s = open.slotOf(i, j, k);
    return s < 0 ? nullptr : open.data() + static_cast<size_t>(s) * Tiles::kCells;
}

bool SolidLevels::solid(int level, int i, int j, int k) const {
    const Level& L = levels_[static_cast<size_t>(level)];
    const int32_t s = L.tiles->slot(L.tiles->tileOf(i, j, k));
    return s >= 0 && L.solid[static_cast<size_t>(s) * Tiles::kCells + SparseGrid::local(i, j, k)] != 0;
}

// --- the system ------------------------------------------------------------------------

template <class F>
void FreeSurfaceSolver::forActiveRows(const F& f) const {
    const size_t nx = static_cast<size_t>(levels_.front().n[0]);
    const size_t grain = std::max<size_t>(1, kChunkCells / nx);
    pg::parallelFor(activeRows_.size(), grain, [&](size_t begin, size_t end) {
        for (size_t r = begin; r < end; ++r) {
            for (uint32_t s = rowStart_[r]; s < rowStart_[r + 1]; ++s) f(size_t{stretch_[s]}, size_t{stretch_[s]} + Tiles::kSide);
        }
    });
}

template <class Map>
double FreeSurfaceSolver::sumActive(const Map& map) const {
    const size_t nx = static_cast<size_t>(levels_.front().n[0]);
    const size_t grain = std::max<size_t>(1, kChunkCells / nx);
    return pg::parallelReduce(
        activeRows_.size(), grain, 0.0,
        [&](size_t begin, size_t end) {
            double s = 0.0;
            for (size_t r = begin; r < end; ++r) {
                // A row's sum from 0, its cells in order: the cells between
                // its stretches hold 0 and add nothing.
                double row = 0.0;
                for (uint32_t t = rowStart_[r]; t < rowStart_[r + 1]; ++t) {
                    for (size_t c = stretch_[t]; c < size_t{stretch_[t]} + Tiles::kSide; ++c) row += map(c);
                }
                s += row;
            }
            return s;
        },
        [](double a, double b) { return a + b; });
}

float FreeSurfaceSolver::surfaceFraction(float phiLiquid, float phiAir) {
    if (!(phiLiquid < 0.0f)) return kMinTheta;  // the surface is at the liquid cell's centre
    if (!(phiAir > 0.0f)) return 1.0f;          // no crossing seen: at the air cell's centre
    return std::clamp(phiLiquid / (phiLiquid - phiAir), kMinTheta, 1.0f);
}

float FreeSurfaceSolver::airWeight(float open, float phiLiquid, float phiAir) const {
    return open / surfaceFraction(phiLiquid, phiAir);
}

uint8_t FreeSurfaceSolver::kind(size_t level, int i, int j, int k) const {
    const Level& L = levels_[level];
    const int32_t s = L.tiles->slot(L.tiles->tileOf(i, j, k));
    if (s >= 0) return L.cells[static_cast<size_t>(s) * Tiles::kCells + SparseGrid::local(i, j, k)];
    // Not kept: no liquid there, and as solid as the solids say.
    return solids_->solid(static_cast<int>(level), i, j, k) ? Solid : Air;
}

void FreeSurfaceSolver::build(size_t level, const SparseGrid* phi, std::vector<uint8_t>* links) {
    Level& L = levels_[level];
    const int n[3] = {L.n[0], L.n[1], L.n[2]};
    const Tiles& tiles = *L.tiles;
    auto withLiquid = [&](std::vector<uint32_t>& slots) {
        std::vector<uint32_t> kept;
        for (const uint32_t s : slots) {
            const uint8_t* c = L.cells.data() + static_cast<size_t>(s) * Tiles::kCells;
            if (std::find(c, c + Tiles::kCells, static_cast<uint8_t>(Liquid)) != c + Tiles::kCells) kept.push_back(s);
        }
        slots.swap(kept);
    };
    L.busy.resize(tiles.stored().size());
    for (size_t s = 0; s < L.busy.size(); ++s) L.busy[s] = static_cast<uint32_t>(s);
    withLiquid(L.busy);
    for (int a = 0; a < 3; ++a) L.weight[a] = SparseGrid(Tiles::faces(tiles, a));
    L.diagonal = SparseGrid(L.tiles);
    if (links) links->assign(L.cells.size(), 0);
    // Between two liquid cells, a face weighs as much as it is open. The
    // diagonal: every open face of a liquid cell, towards liquid as it
    // weighs, towards air as far as the surface is. With `links`, which of
    // those faces lead on into the liquid, which to air.
    const int lv = static_cast<int>(level);
    forTiles(tiles, L.busy, [&](int i, int j, int k, size_t c) {
        if (L.cells[c] != Liquid) return;
        float sum = 0.0f;
        uint8_t bits = 0;
        const int here[3] = {i, j, k};
        for (int a = 0; a < 3; ++a) {
            for (int side = 0; side < 2; ++side) {
                // The face on this side, and the cell beyond it.
                int fi = i, fj = j, fk = k;
                if (side == 1) (a == 0 ? fi : a == 1 ? fj : fk) += 1;
                const float open = solids_->open(lv, a, fi, fj, fk);
                if (open <= 0.0f) continue;
                const int beyond = here[a] + (side == 0 ? -1 : 1);
                if (beyond < 0 || beyond >= n[a]) {
                    sum += 2.0f * open;  // an open side: p = 0 on it, half a cell away
                    bits |= kToAir;
                    continue;
                }
                const int bi = a == 0 ? beyond : i, bj = a == 1 ? beyond : j, bk = a == 2 ? beyond : k;
                if (kind(level, bi, bj, bk) == Liquid) {
                    sum += open;
                    bits |= static_cast<uint8_t>(1u << (2 * a + side));
                    if (side == 1) L.weight[a].ref(fi, fj, fk) = open;  // written once: by the cell behind it
                } else {
                    const float theta = phi ? surfaceFraction(phi->data()[c], phi->at(bi, bj, bk)) : 1.0f;
                    sum += open / theta;
                    bits |= kToAir;
                }
            }
        }
        L.diagonal.data()[c] = sum;
        if (links) (*links)[c] = bits;
    });
    // A liquid cell with no open face takes no part.
    forTiles(tiles, L.busy, [&](int, int, int, size_t c) {
        if (L.cells[c] == Liquid && L.diagonal.data()[c] <= 0.0f) L.cells[c] = Solid;
    });
    withLiquid(L.busy);
    L.x = SparseGrid(L.tiles);
    L.b = SparseGrid(L.tiles);
    L.r = SparseGrid(L.tiles);
}

void FreeSurfaceSolver::coarsen(size_t fine) {
    Level C;
    for (int a = 0; a < 3; ++a) C.n[a] = levels_[fine].n[a] / 2;
    C.tiles = coarser(*levels_[fine].tiles, C.n);
    // A coarse cell is liquid if any of its eight is, solid if all are; the
    // eight not kept as the solids say.
    C.cells.assign(C.tiles->stored().size() * Tiles::kCells, Air);
    forEachCounted(*C.tiles, [&](int i, int j, int k, size_t c) {
        bool liquid = false, solid = true;
        for (int q = 0; q < 8; ++q) {
            const uint8_t t = kind(fine, 2 * i + (q & 1), 2 * j + ((q >> 1) & 1), 2 * k + (q >> 2));
            liquid = liquid || t == Liquid;
            solid = solid && t == Solid;
        }
        C.cells[c] = liquid ? Liquid : solid ? Solid : Air;
    });
    levels_.push_back(std::move(C));
    build(fine + 1, nullptr, nullptr);
}

void FreeSurfaceSolver::setSystem(std::shared_ptr<const Tiles> tiles, const std::vector<uint8_t>& cells,
                                  const SparseGrid& phi, const SolidLevels& solids) {
    solids_ = &solids;
    levels_.clear();
    levels_.reserve(static_cast<size_t>(solids.levels()));
    Level fine;
    fine.n[0] = tiles->nx();
    fine.n[1] = tiles->ny();
    fine.n[2] = tiles->nz();
    fine.tiles = std::move(tiles);
    fine.cells = cells;
    levels_.push_back(std::move(fine));
    build(0, &phi, &links_);
    const std::vector<uint8_t>& kinds = levels_.front().cells;
    unknowns_ = static_cast<size_t>(std::count(kinds.begin(), kinds.end(), static_cast<uint8_t>(Liquid)));
    while (SolidLevels::halves(levels_.back().n)) coarsen(levels_.size() - 1);
    const Level& f = levels_.front();
    for (SparseGrid* g : {&r_, &rOld_, &z_, &d_, &q_}) *g = SparseGrid(f.tiles);
    findRows();
    findPockets();
    b_ = pockets() > 0 ? SparseGrid(f.tiles) : SparseGrid();
}

void FreeSurfaceSolver::findRows() {
    // The rows of the finest grid with liquid in them -- in each tile with
    // some, each of its rows that has any -- in the order of the rows of the
    // whole grid, and in a row along x: the tiles are kept in that order.
    const Level& L = levels_.front();
    const Tiles& tiles = *L.tiles;
    const int ny = L.n[1];
    const size_t rows = static_cast<size_t>(ny) * static_cast<size_t>(L.n[2]);
    std::vector<uint32_t> count(rows + 1, 0);
    auto each = [&](const auto& f) {
        for (const uint32_t s : L.busy) {
            int c[3];
            tiles.corner(tiles.stored()[s], c[0], c[1], c[2]);
            const int e[3] = {std::min(Tiles::kSide, L.n[0] - c[0]), std::min(Tiles::kSide, L.n[1] - c[1]),
                              std::min(Tiles::kSide, L.n[2] - c[2])};
            for (int z = 0; z < e[2]; ++z) {
                for (int y = 0; y < e[1]; ++y) {
                    const size_t first = static_cast<size_t>(s) * Tiles::kCells + SparseGrid::local(0, y, z);
                    const uint8_t* cell = L.cells.data() + first;
                    if (std::find(cell, cell + e[0], static_cast<uint8_t>(Liquid)) == cell + e[0]) continue;
                    f(static_cast<size_t>(c[1] + y) + static_cast<size_t>(ny) * static_cast<size_t>(c[2] + z), first);
                }
            }
        }
    };
    each([&](size_t row, size_t) { ++count[row + 1]; });
    activeRows_.clear();
    rowStart_.clear();
    for (size_t row = 0; row < rows; ++row) {
        if (count[row + 1] == 0) continue;
        activeRows_.push_back(static_cast<uint32_t>(row));
    }
    for (size_t row = 0; row < rows; ++row) count[row + 1] += count[row];
    stretch_.assign(count[rows], 0);
    std::vector<uint32_t> next(count.begin(), count.end() - 1);
    each([&](size_t row, size_t first) { stretch_[next[row]++] = static_cast<uint32_t>(first); });
    rowStart_.reserve(activeRows_.size() + 1);
    for (const uint32_t row : activeRows_) rowStart_.push_back(count[row]);
    rowStart_.push_back(count[rows]);
}

void FreeSurfaceSolver::findPockets() {
    pocketCells_.clear();
    pocketStart_.clear();
    const Level& L = levels_.front();
    const Tiles& tiles = *L.tiles;
    const uint8_t* cells = L.cells.data();
    // The cell across the face behind or ahead along an axis, kept: a link
    // leads only to liquid.
    auto across = [&](size_t c, int a, int side) {
        const size_t slot = c / Tiles::kCells, local = c % Tiles::kCells;
        const int l[3] = {static_cast<int>(local % Tiles::kSide), static_cast<int>(local / Tiles::kSide % Tiles::kSide),
                          static_cast<int>(local / (Tiles::kSide * Tiles::kSide))};
        const int step = side == 0 ? -1 : 1;
        if (l[a] + step >= 0 && l[a] + step < Tiles::kSide) return static_cast<size_t>(static_cast<int64_t>(c) + step * kStride[a]);
        int g[3];
        tiles.corner(tiles.stored()[slot], g[0], g[1], g[2]);
        for (int b = 0; b < 3; ++b) g[b] += l[b];
        g[a] += step;
        return static_cast<size_t>(L.x.find(g[0], g[1], g[2]));
    };
    // From every cell next to air, through the liquid: breadth first, in the
    // order of the cells of the whole grid, the same on any number of threads.
    reached_.clear();
    auto flood = [&](size_t from) {
        for (size_t q = from; q < reached_.size(); ++q) {
            const size_t c = reached_[q];
            const uint8_t bits = links_[c];
            for (int a = 0; a < 3; ++a) {
                for (int side = 0; side < 2; ++side) {
                    if (!(bits & (1u << (2 * a + side)))) continue;
                    const size_t next = across(c, a, side);
                    if (links_[next] & kReached) continue;
                    links_[next] |= kReached;
                    reached_.push_back(static_cast<uint32_t>(next));
                }
            }
        }
    };
    for (size_t r = 0; r < activeRows_.size(); ++r) {
        for (uint32_t s = rowStart_[r]; s < rowStart_[r + 1]; ++s) {
            for (size_t c = stretch_[s]; c < size_t{stretch_[s]} + Tiles::kSide; ++c) {
                if (cells[c] != Liquid || !(links_[c] & kToAir)) continue;
                links_[c] |= kReached;
                reached_.push_back(static_cast<uint32_t>(c));
            }
        }
    }
    flood(0);
    if (reached_.size() == unknowns_) return;  // no pocket: the usual case
    // What the air does not reach: pockets, each flooded from its first cell.
    for (size_t r = 0; r < activeRows_.size(); ++r) {
        for (uint32_t s = rowStart_[r]; s < rowStart_[r + 1]; ++s) {
            for (size_t c = stretch_[s]; c < size_t{stretch_[s]} + Tiles::kSide; ++c) {
                if (cells[c] != Liquid || (links_[c] & kReached)) continue;
                const size_t from = reached_.size();
                links_[c] |= kReached;
                reached_.push_back(static_cast<uint32_t>(c));
                flood(from);
                pocketStart_.push_back(static_cast<uint32_t>(pocketCells_.size()));
                pocketCells_.insert(pocketCells_.end(), reached_.begin() + static_cast<std::ptrdiff_t>(from), reached_.end());
            }
        }
    }
    pocketStart_.push_back(static_cast<uint32_t>(pocketCells_.size()));
}

// --- the operator and the multigrid ------------------------------------------------------

void FreeSurfaceSolver::applyOn(const Level& L, const SparseGrid& x, SparseGrid& out) const {
    const int n[3] = {L.n[0], L.n[1], L.n[2]};
    const float* v = x.data();
    float* o = out.data();
    const float* diagonal = L.diagonal.data();
    const float* weight[3] = {L.weight[0].data(), L.weight[1].data(), L.weight[2].data()};
    pg::parallelFor(L.busy.size(), kChunkTiles, [&](size_t begin, size_t end) {
        for (size_t t = begin; t < end; ++t) {
            const TileView w = viewOf(*L.tiles, L.busy[t], L.x, L.weight);
            for (int z = 0; z < w.extent[2]; ++z) {
                for (int y = 0; y < w.extent[1]; ++y) {
                    for (int xx = 0; xx < w.extent[0]; ++xx) {
                        const int l[3] = {xx, y, z};
                        const int64_t local = static_cast<int64_t>(SparseGrid::local(xx, y, z));
                        const int64_t c = w.base + local;
                        if (L.cells[static_cast<size_t>(c)] != Liquid) {
                            o[c] = 0.0f;
                            continue;
                        }
                        float s = diagonal[c] * v[c];
                        for (int a = 0; a < 3; ++a) {
                            const int at = w.corner[a] + l[a];
                            // Behind: the face of its own, and the cell before.
                            if (at > 0) {
                                const float f = weight[a][w.faces[a] + local];
                                const int64_t b = l[a] > 0 ? c - kStride[a] : w.beside[2 * a] < 0 ? -1 : w.beside[2 * a] + local + (Tiles::kSide - 1) * kStride[a];
                                s -= f * (b < 0 ? 0.0f : v[b]);
                            }
                            // Ahead: the next face, and the cell after.
                            if (at < n[a] - 1) {
                                const int64_t face = l[a] < Tiles::kSide - 1 ? w.faces[a] + local + kStride[a]
                                                                            : w.ahead[a] + local - (Tiles::kSide - 1) * kStride[a];
                                const int64_t b = l[a] < Tiles::kSide - 1 ? c + kStride[a] : w.beside[2 * a + 1] < 0 ? -1 : w.beside[2 * a + 1] + local - (Tiles::kSide - 1) * kStride[a];
                                s -= weight[a][face] * (b < 0 ? 0.0f : v[b]);
                            }
                        }
                        o[c] = s;
                    }
                }
            }
        }
    });
}

void FreeSurfaceSolver::residualOn(const Level& L, const SparseGrid& x, const SparseGrid& b, SparseGrid& r) const {
    applyOn(L, x, r);
    const float* bb = b.data();
    float* rr = r.data();
    forTiles(*L.tiles, L.busy, [&](int, int, int, size_t c) {
        rr[c] = L.cells[c] == Liquid ? bb[c] - rr[c] : 0.0f;
    });
}

void FreeSurfaceSolver::relax(const Level& L, SparseGrid& x, const SparseGrid& b, int colour, float omega) const {
    const int n[3] = {L.n[0], L.n[1], L.n[2]};
    float* v = x.data();
    const float* bb = b.data();
    const float* diagonal = L.diagonal.data();
    const float* weight[3] = {L.weight[0].data(), L.weight[1].data(), L.weight[2].data()};
    pg::parallelFor(L.busy.size(), kChunkTiles, [&](size_t begin, size_t end) {
        for (size_t t = begin; t < end; ++t) {
            const TileView w = viewOf(*L.tiles, L.busy[t], L.x, L.weight);
            for (int z = 0; z < w.extent[2]; ++z) {
                for (int y = 0; y < w.extent[1]; ++y) {
                    const int first = (colour + w.corner[0] + w.corner[1] + y + w.corner[2] + z) & 1;
                    for (int xx = first; xx < w.extent[0]; xx += 2) {
                        const int l[3] = {xx, y, z};
                        const int64_t local = static_cast<int64_t>(SparseGrid::local(xx, y, z));
                        const int64_t c = w.base + local;
                        if (L.cells[static_cast<size_t>(c)] != Liquid) continue;
                        float s = bb[c];
                        for (int a = 0; a < 3; ++a) {
                            const int at = w.corner[a] + l[a];
                            if (at > 0) {
                                const float f = weight[a][w.faces[a] + local];
                                const int64_t q = l[a] > 0 ? c - kStride[a] : w.beside[2 * a] < 0 ? -1 : w.beside[2 * a] + local + (Tiles::kSide - 1) * kStride[a];
                                s += f * (q < 0 ? 0.0f : v[q]);
                            }
                            if (at < n[a] - 1) {
                                const int64_t face = l[a] < Tiles::kSide - 1 ? w.faces[a] + local + kStride[a]
                                                                            : w.ahead[a] + local - (Tiles::kSide - 1) * kStride[a];
                                const int64_t q = l[a] < Tiles::kSide - 1 ? c + kStride[a] : w.beside[2 * a + 1] < 0 ? -1 : w.beside[2 * a + 1] + local - (Tiles::kSide - 1) * kStride[a];
                                s += weight[a][face] * (q < 0 ? 0.0f : v[q]);
                            }
                        }
                        v[c] += omega * (s / diagonal[c] - v[c]);
                    }
                }
            }
        }
    });
}

/// Fine to coarse: the coarse grid's cells are twice as wide, so its equation
/// -- multiplied by its h^2 -- takes four times the mean of the eight
/// residuals: half their sum.
void FreeSurfaceSolver::restrictTo(const Level& fine, const SparseGrid& r, Level& coarse) const {
    (void)fine;
    float* b = coarse.b.data();
    forTiles(*coarse.tiles, coarse.busy, [&](int i, int j, int k, size_t c) {
        if (coarse.cells[c] != Liquid) {
            b[c] = 0.0f;
            return;
        }
        float sum = 0.0f;
        for (int q = 0; q < 8; ++q) sum += r.at(2 * i + (q & 1), 2 * j + ((q >> 1) & 1), 2 * k + (q >> 2));
        b[c] = 0.5f * sum;
    });
}

/// Coarse to fine, added to the liquid cells: trilinear, each fine cell
/// between its coarse cell (3/4 along each axis) and the next one towards it
/// (1/4). A neighbour of air counts 0 -- the pressure is 0 there; one of solid,
/// or past a closed side, counts as the cell itself -- nothing flows there,
/// the correction goes on level.
void FreeSurfaceSolver::prolongAdd(size_t level, const Level& fine, SparseGrid& x) const {
    const Level& coarse = levels_[level];
    const int cn[3] = {coarse.n[0], coarse.n[1], coarse.n[2]};
    const int lv = static_cast<int>(level);
    const Tiles& tiles = *fine.tiles;
    float* xx = x.data();
    pg::parallelFor(fine.busy.size(), kChunkTiles, [&](size_t begin, size_t end) {
        // The coarse cells over a fine tile and the ring round them, looked
        // up once: their corrections, and what they hold.
        constexpr int kSpan = Tiles::kSide / 2 + 2;
        float value[kSpan * kSpan * kSpan];
        uint8_t what[kSpan * kSpan * kSpan];
        for (size_t t = begin; t < end; ++t) {
            const uint32_t slot = fine.busy[t];
            int c[3];
            tiles.corner(tiles.stored()[slot], c[0], c[1], c[2]);
            const int e[3] = {std::min(Tiles::kSide, fine.n[0] - c[0]), std::min(Tiles::kSide, fine.n[1] - c[1]),
                              std::min(Tiles::kSide, fine.n[2] - c[2])};
            const int base[3] = {c[0] / 2 - 1, c[1] / 2 - 1, c[2] / 2 - 1};
            for (int z = 0; z < kSpan; ++z) {
                for (int y = 0; y < kSpan; ++y) {
                    for (int w = 0; w < kSpan; ++w) {
                        const int g[3] = {base[0] + w, base[1] + y, base[2] + z};
                        const int at = w + kSpan * (y + kSpan * z);
                        if (g[0] < 0 || g[1] < 0 || g[2] < 0 || g[0] >= cn[0] || g[1] >= cn[1] || g[2] >= cn[2]) {
                            what[at] = Air;
                            value[at] = 0.0f;
                            continue;
                        }
                        what[at] = kind(level, g[0], g[1], g[2]);
                        value[at] = coarse.x.at(g[0], g[1], g[2]);
                    }
                }
            }
            for (int z = 0; z < e[2]; ++z) {
                for (int y = 0; y < e[1]; ++y) {
                    for (int w = 0; w < e[0]; ++w) {
                        const size_t f = static_cast<size_t>(slot) * Tiles::kCells + SparseGrid::local(w, y, z);
                        if (fine.cells[f] != Liquid) continue;
                        const int here[3] = {c[0] + w, c[1] + y, c[2] + z};
                        const int p[3] = {here[0] >> 1, here[1] >> 1, here[2] >> 1};
                        const int o[3] = {(here[0] & 1) ? 1 : -1, (here[1] & 1) ? 1 : -1, (here[2] & 1) ? 1 : -1};
                        const int mine = (p[0] - base[0]) + kSpan * ((p[1] - base[1]) + kSpan * (p[2] - base[2]));
                        const float own = value[mine];
                        float sum = 0.0f;
                        if (p[0] + o[0] >= 0 && p[0] + o[0] < cn[0] && p[1] + o[1] >= 0 && p[1] + o[1] < cn[1] &&
                            p[2] + o[2] >= 0 && p[2] + o[2] < cn[2]) {
                            // Inside the coarse grid: the eight straight from the lookups.
                            const int step[3] = {o[0], o[1] * kSpan, o[2] * kSpan * kSpan};
                            for (int q = 0; q < 8; ++q) {
                                const int at = mine + ((q & 1) ? step[0] : 0) + ((q & 2) ? step[1] : 0) + ((q & 4) ? step[2] : 0);
                                const float v = what[at] == Liquid ? value[at] : what[at] == Solid ? own : 0.0f;
                                sum += kTrilinear[q] * v;
                            }
                            xx[f] += sum;
                            continue;
                        }
                        for (int q = 0; q < 8; ++q) {
                            int cc[3];
                            float weight = 1.0f;
                            bool outside = false, openSide = false;
                            for (int a = 0; a < 3; ++a) {
                                const bool step = (q >> a) & 1;
                                cc[a] = p[a] + (step ? o[a] : 0);
                                weight *= step ? 0.25f : 0.75f;
                                if (cc[a] < 0 || cc[a] >= cn[a]) {
                                    outside = true;
                                    // The side's face next to the parent: a wall, or open.
                                    int fi = p[0], fj = p[1], fk = p[2];
                                    (a == 0 ? fi : a == 1 ? fj : fk) = cc[a] < 0 ? 0 : cn[a];
                                    if (solids_->open(lv, a, fi, fj, fk) >= 0.5f) openSide = true;
                                }
                            }
                            float v;
                            if (outside) {
                                v = openSide ? 0.0f : own;
                            } else {
                                const int at = (cc[0] - base[0]) + kSpan * ((cc[1] - base[1]) + kSpan * (cc[2] - base[2]));
                                v = what[at] == Liquid ? value[at] : what[at] == Solid ? own : 0.0f;
                            }
                            sum += weight * v;
                        }
                        xx[f] += sum;
                    }
                }
            }
        }
    });
}

void FreeSurfaceSolver::vcycle(size_t level) {
    Level& L = levels_[level];
    if (level + 1 == levels_.size()) {
        // The coarsest grid: a few cells a side, over-relaxed sweeps.
        const double pi = 3.14159265358979323846;
        const double rate = (std::cos(pi / L.n[0]) + std::cos(pi / L.n[1]) + std::cos(pi / L.n[2])) / 3.0;
        const float omega = static_cast<float>(2.0 / (1.0 + std::sqrt(1.0 - rate * rate)));
        const int sweeps = 2 * std::max({L.n[0], L.n[1], L.n[2]});
        for (int s = 0; s < sweeps; ++s) {
            relax(L, L.x, L.b, 0, omega);
            relax(L, L.x, L.b, 1, omega);
        }
        return;
    }
    relax(L, L.x, L.b, 0, 1.0f);
    relax(L, L.x, L.b, 1, 1.0f);
    residualOn(L, L.x, L.b, L.r);
    Level& C = levels_[level + 1];
    restrictTo(L, L.r, C);
    C.x.fill(0.0f);
    vcycle(level + 1);
    prolongAdd(level + 1, L, L.x);
    relax(L, L.x, L.b, 1, 1.0f);
    relax(L, L.x, L.b, 0, 1.0f);
}

void FreeSurfaceSolver::precondition(const SparseGrid& r, SparseGrid& z) {
    // Only the rows with liquid: the rest of the finest grid's x stays 0, of
    // b and z unread.
    Level& L = levels_.front();
    forActiveRows([&](size_t begin, size_t end) {
        std::copy(r.data() + begin, r.data() + end, L.b.data() + begin);
        std::fill(L.x.data() + begin, L.x.data() + end, 0.0f);
    });
    vcycle(0);
    forActiveRows([&](size_t begin, size_t end) { std::copy(L.x.data() + begin, L.x.data() + end, z.data() + begin); });
}

// --- solving ---------------------------------------------------------------------------

int FreeSurfaceSolver::solve(SparseGrid& p, const SparseGrid& rhs, float tolerance, int maxIterations) {
    residual_ = 0.0;
    if (levels_.empty()) return 0;
    const uint8_t* cells = levels_.front().cells.data();
    // Nothing outside the liquid.
    float* pp = p.data();
    for (size_t c = 0; c < p.size(); ++c) {
        if (cells[c] != Liquid) pp[c] = 0.0f;
    }
    const size_t pockets = this->pockets();
    if (pockets == 0) return iterate(p, rhs, tolerance, maxIterations);
    // In each pocket the right-hand side less its mean, which sums to 0; the
    // mean of its pressure as it came, the level it keeps.
    forActiveRows([&](size_t begin, size_t end) { std::copy(rhs.data() + begin, rhs.data() + end, b_.data() + begin); });
    std::vector<double> level(pockets);
    for (size_t q = 0; q < pockets; ++q) {
        const uint32_t* first = pocketCells_.data() + pocketStart_[q];
        const size_t count = pocketStart_[q + 1] - pocketStart_[q];
        double inflow = 0.0, sum = 0.0;
        for (size_t c = 0; c < count; ++c) {
            inflow += rhs.data()[first[c]];
            sum += p.data()[first[c]];
        }
        const float mean = static_cast<float>(inflow / static_cast<double>(count));
        for (size_t c = 0; c < count; ++c) b_.data()[first[c]] -= mean;
        level[q] = sum / static_cast<double>(count);
    }
    const int iterations = iterate(p, b_, tolerance, maxIterations);
    for (size_t q = 0; q < pockets; ++q) {
        const uint32_t* first = pocketCells_.data() + pocketStart_[q];
        const size_t count = pocketStart_[q + 1] - pocketStart_[q];
        double sum = 0.0;
        for (size_t c = 0; c < count; ++c) sum += p.data()[first[c]];
        const float shift = static_cast<float>(level[q] - sum / static_cast<double>(count));
        for (size_t c = 0; c < count; ++c) p.data()[first[c]] += shift;
    }
    return iterations;
}

int FreeSurfaceSolver::iterate(SparseGrid& p, const SparseGrid& rhs, float tolerance, int maxIterations) {
    const Level& L = levels_.front();
    const uint8_t* cells = L.cells.data();
    const size_t grain = std::max<size_t>(1, kChunkCells / static_cast<size_t>(L.n[0]));
    // Every vector below is read in the rows with liquid alone: those rows of
    // r, z, d and q are what the iterations are about; elsewhere z and d stay
    // 0 (as set up), which is what the operator reads next to the liquid.
    auto largest = [&](const SparseGrid& g) {
        const float* v = g.data();
        return pg::parallelReduce(
            activeRows_.size(), grain, 0.0,
            [&](size_t begin, size_t end) {
                double m = 0.0;
                for (size_t r = begin; r < end; ++r) {
                    for (uint32_t s = rowStart_[r]; s < rowStart_[r + 1]; ++s) {
                        for (size_t c = stretch_[s]; c < size_t{stretch_[s]} + Tiles::kSide; ++c) {
                            if (cells[c] == Liquid) m = std::max(m, static_cast<double>(std::fabs(v[c])));
                        }
                    }
                }
                return m;
            },
            [](double a, double b) { return std::max(a, b); });
    };
    auto dot = [&](const SparseGrid& a, const SparseGrid& b) {
        const float* u = a.data();
        const float* v = b.data();
        return sumActive([&](size_t c) { return static_cast<double>(u[c]) * static_cast<double>(v[c]); });
    };
    residualOn(L, p, rhs, r_);
    const double bNorm = largest(rhs);
    if (bNorm <= 0.0) {
        p.fill(0.0f);
        return 0;
    }
    double rNorm = largest(r_);
    residual_ = rNorm / bNorm;
    if (rNorm <= tolerance * bNorm) return 0;

    precondition(r_, z_);
    double rz = dot(r_, z_);
    if (!(rz > 0.0)) {  // the V-cycle went astray: plain steepest descent this once
        forActiveRows([&](size_t begin, size_t end) { std::copy(r_.data() + begin, r_.data() + end, z_.data() + begin); });
        rz = dot(r_, z_);
    }
    forActiveRows([&](size_t begin, size_t end) { std::copy(z_.data() + begin, z_.data() + end, d_.data() + begin); });
    for (int it = 1; it <= maxIterations; ++it) {
        applyOn(L, d_, q_);
        const double dq = dot(d_, q_);
        if (!(dq > 0.0)) return it;
        const float alpha = static_cast<float>(rz / dq);
        forActiveRows([&](size_t begin, size_t end) {
            float* pp = p.data();
            float* rr = r_.data();
            float* old = rOld_.data();
            const float* dd = d_.data();
            const float* qq = q_.data();
            for (size_t c = begin; c < end; ++c) {
                old[c] = rr[c];
                pp[c] += alpha * dd[c];
                rr[c] -= alpha * qq[c];
            }
        });
        rNorm = largest(r_);
        residual_ = rNorm / bNorm;
        if (rNorm <= tolerance * bNorm) return it;
        precondition(r_, z_);
        // Polak-Ribiere: what is new in z since the last step.
        double rzNew = 0.0, zOld = 0.0;
        {
            struct Pair {
                double a = 0.0, b = 0.0;
            };
            const float* zz = z_.data();
            const float* rr = r_.data();
            const float* old = rOld_.data();
            const Pair both = pg::parallelReduce(
                activeRows_.size(), grain, Pair{},
                [&](size_t begin, size_t end) {
                    Pair s;
                    for (size_t r = begin; r < end; ++r) {
                        for (uint32_t t = rowStart_[r]; t < rowStart_[r + 1]; ++t) {
                            for (size_t c = stretch_[t]; c < size_t{stretch_[t]} + Tiles::kSide; ++c) {
                                const double z = zz[c];
                                s.a += z * static_cast<double>(rr[c]);
                                s.b += z * static_cast<double>(old[c]);
                            }
                        }
                    }
                    return s;
                },
                [](Pair x, Pair y) { return Pair{x.a + y.a, x.b + y.b}; });
            rzNew = both.a;
            zOld = both.b;
        }
        if (!(rzNew > 0.0)) {
            forActiveRows([&](size_t begin, size_t end) { std::copy(r_.data() + begin, r_.data() + end, z_.data() + begin); });
            rzNew = dot(z_, r_);
            zOld = dot(z_, rOld_);
        }
        const float beta = static_cast<float>(std::max(0.0, (rzNew - zOld) / rz));
        forActiveRows([&](size_t begin, size_t end) {
            float* d = d_.data();
            const float* zz = z_.data();
            for (size_t c = begin; c < end; ++c) d[c] = zz[c] + beta * d[c];
        });
        rz = rzNew;
    }
    return maxIterations;
}

// --- the whole grid -------------------------------------------------------------------

SparseGrid FreeSurfaceSolver::kept(const Grid& g) const {
    SparseGrid out(levels_.front().tiles);
    float* o = out.data();
    forEachCounted(*levels_.front().tiles, [&](int i, int j, int k, size_t c) { o[c] = g.at(i, j, k); });
    return out;
}

void FreeSurfaceSolver::spread(const SparseGrid& g, Grid& out) const {
    const float* v = g.data();
    forEachCounted(g.tiles(), [&](int i, int j, int k, size_t c) { out.at(i, j, k) = v[c]; });
}

void FreeSurfaceSolver::setSystem(const std::vector<uint8_t>& cells, const Grid open[3], const Grid& phi) {
    // Every tile kept; the walls are the faces `open` closes.
    const int n[3] = {phi.nx(), phi.ny(), phi.nz()};
    const auto tiles = std::make_shared<const Tiles>(n[0], n[1], n[2]);
    const bool closed[6] = {false, false, false, false, false, false};
    ownSolids_ = SolidLevels(n[0], n[1], n[2], closed);
    const size_t count = tiles->stored().size() * Tiles::kCells;
    std::vector<uint8_t> kinds(count, Air), solid(count, 0);
    SparseGrid distance(tiles);
    forEachCounted(*tiles, [&](int i, int j, int k, size_t c) {
        const size_t d = phi.index(i, j, k);
        kinds[c] = cells[d];
        solid[c] = cells[d] == Solid ? 1 : 0;
        distance.data()[c] = phi.data()[d];
    });
    SparseGrid faces[3];
    for (int a = 0; a < 3; ++a) {
        faces[a] = SparseGrid(Tiles::faces(*tiles, a), 1.0f);
        float* f = faces[a].data();
        forEachCounted(faces[a].tiles(), [&](int i, int j, int k, size_t c) { f[c] = open[a].at(i, j, k); });
    }
    ownSolids_.set(tiles, std::move(solid), faces);
    setSystem(tiles, kinds, distance, ownSolids_);
}

int FreeSurfaceSolver::solve(Grid& p, const Grid& rhs, float tolerance, int maxIterations) {
    if (levels_.empty()) return 0;
    SparseGrid pressure = kept(p);
    const int iterations = solve(pressure, kept(rhs), tolerance, maxIterations);
    spread(pressure, p);
    return iterations;
}

void FreeSurfaceSolver::apply(const Grid& p, Grid& out) const {
    if (levels_.empty()) return;
    SparseGrid result(levels_.front().tiles);
    applyOn(levels_.front(), kept(p), result);
    spread(result, out);
}

}  // namespace pg::sim
