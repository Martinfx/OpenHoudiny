#pragma once
//
// A 3D grid of floats kept in tiles of 8 x 8 x 8 cells, only the active tiles
// stored -- what production keeps in OpenVDB, flattened to one level: a table
// of the tiles of the whole box, and the values of the active ones.
//
// Cells, positions and sampling are those of Grid (Grid.h): cell (i, j, k)
// covers [i, i+1) x [j, j+1) x [k, k+1) and its value sits at its centre. A
// cell that is not stored reads the grid's background: 0 -- still, empty air
// -- unless the grid was made with another, as a distance far from anything.
//
// Fields of one shape share their Tiles, so a cell has the same index() in
// each: a loop over the active cells reads and writes all of them at once.
//
// The faces of a MAC grid (the velocity) are a grid of their own, one longer
// along their axis. The faces a solver works on are those of its active
// cells: all the faces of an active tile, and the first layer of the tile
// after it along the axis -- the far faces of its last cells. Tiles::faces()
// makes that set; such a tile is stored whole, but only its first layer
// counts (has()).
//
#include "pg/core/Parallel.h"
#include "pg/sim/Grid.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace pg::sim {

class Tiles {
public:
    static constexpr int kLog = 3;
    static constexpr int kSide = 8;
    static constexpr int kCells = 512;

    /// How a tile is kept: not at all, whole, or only its first layer along
    /// the axis counts (faces).
    enum State : uint8_t { Off = 0, Whole = 1, FirstLayer = 2 };

    Tiles() = default;
    /// Every tile of a grid of nx x ny x nz cells, whole.
    Tiles(int nx, int ny, int nz);
    /// The tiles `state` (one per tile, x fastest) keeps; `axis` is the one
    /// FirstLayer refers to, -1 when there are none.
    Tiles(int nx, int ny, int nz, std::vector<uint8_t> state, int axis);

    /// The faces along `axis` of the cells of `cells`: a grid one longer
    /// along it.
    static std::shared_ptr<const Tiles> faces(const Tiles& cells, int axis);

    int nx() const { return n_[0]; }
    int ny() const { return n_[1]; }
    int nz() const { return n_[2]; }
    int tilesX() const { return t_[0]; }
    int tilesY() const { return t_[1]; }
    int tilesZ() const { return t_[2]; }
    int axis() const { return axis_; }
    size_t tileCount() const { return state_.size(); }

    size_t tileOf(int i, int j, int k) const {
        return static_cast<size_t>(i >> kLog) +
               static_cast<size_t>(t_[0]) *
                   (static_cast<size_t>(j >> kLog) + static_cast<size_t>(t_[1]) * static_cast<size_t>(k >> kLog));
    }
    /// Where tile t is stored, -1 when it is not.
    int32_t slot(size_t t) const { return slot_[t]; }
    uint8_t state(size_t t) const { return state_[t]; }
    const std::vector<uint8_t>& states() const { return state_; }
    /// The stored tiles, by number, in order: slot s is tile stored()[s].
    const std::vector<uint32_t>& stored() const { return stored_; }
    /// The first cell of tile t.
    void corner(size_t t, int& i, int& j, int& k) const;

    /// Is cell (i, j, k) -- inside the grid -- one that counts?
    bool has(int i, int j, int k) const {
        const uint8_t s = state_[tileOf(i, j, k)];
        if (s != FirstLayer) return s == Whole;
        const int at = axis_ == 0 ? i : axis_ == 1 ? j : k;
        return (at & (kSide - 1)) == 0;
    }
    /// The cells that count, over the grid's.
    size_t activeCells() const;
    bool all() const { return all_; }

    bool operator==(const Tiles& other) const {
        return n_[0] == other.n_[0] && n_[1] == other.n_[1] && n_[2] == other.n_[2] && axis_ == other.axis_ &&
               state_ == other.state_;
    }

private:
    int n_[3] = {0, 0, 0}, t_[3] = {0, 0, 0};
    int axis_ = -1;
    bool all_ = false;
    std::vector<uint8_t> state_;
    std::vector<int32_t> slot_;
    std::vector<uint32_t> stored_;
    friend class SparseGrid;
};

class SparseGrid {
public:
    SparseGrid() = default;
    /// Every tile stored, every cell `value`.
    SparseGrid(int nx, int ny, int nz, float value = 0.0f);
    /// The tiles `tiles` keeps, every value `background` -- what the cells
    /// not stored read too.
    explicit SparseGrid(std::shared_ptr<const Tiles> tiles, float background = 0.0f);

    int nx() const { return n_[0]; }
    int ny() const { return n_[1]; }
    int nz() const { return n_[2]; }
    const Tiles& tiles() const { return *tiles_; }
    const std::shared_ptr<const Tiles>& shared() const { return tiles_; }
    /// What a cell that is not stored reads.
    float background() const { return background_; }

    /// Is cell (i, j, k) stored (its tile is), and does it count?
    bool stored(int i, int j, int k) const { return slotOf(i, j, k) >= 0; }
    /// Where the tile of cell (i, j, k) is stored, -1 when it is not: the
    /// table looked up directly, without going through the Tiles.
    int32_t slotOf(int i, int j, int k) const {
        return slots_[static_cast<size_t>(i >> Tiles::kLog) + tx_ * static_cast<size_t>(j >> Tiles::kLog) +
                      txy_ * static_cast<size_t>(k >> Tiles::kLog)];
    }
    bool has(int i, int j, int k) const { return tiles_->has(i, j, k); }
    /// Where a stored cell's value is in data(): the same in every grid of
    /// these Tiles.
    size_t index(int i, int j, int k) const {
        return static_cast<size_t>(slotOf(i, j, k)) * Tiles::kCells + local(i, j, k);
    }
    /// Where cell (i, j, k) is in data(), -1 when its tile is not kept.
    int64_t find(int i, int j, int k) const {
        const int32_t s = slotOf(i, j, k);
        return s < 0 ? -1 : static_cast<int64_t>(s) * Tiles::kCells + static_cast<int64_t>(local(i, j, k));
    }
    static size_t local(int i, int j, int k) {
        return static_cast<size_t>(i & (Tiles::kSide - 1)) +
               Tiles::kSide * (static_cast<size_t>(j & (Tiles::kSide - 1)) +
                               Tiles::kSide * static_cast<size_t>(k & (Tiles::kSide - 1)));
    }
    /// The value of a cell inside the grid; the background where it is not
    /// stored.
    float at(int i, int j, int k) const {
        const int32_t s = slotOf(i, j, k);
        return s < 0 ? background_ : data_[static_cast<size_t>(s) * Tiles::kCells + local(i, j, k)];
    }
    /// A stored cell's value, to write. (Not an overload of at(): a read in a
    /// method that may write would take it, and fail where nothing is stored.)
    float& ref(int i, int j, int k) { return data_[index(i, j, k)]; }
    /// The nearest cell inside the grid: a boundary that repeats itself.
    float clamped(int i, int j, int k) const;

    /// Trilinear interpolation at a position in cell units, as Grid::sample.
    /// (In line, the lookups with it: the solvers make millions a step.)
    [[gnu::always_inline]] float sample(float x, float y, float z, bool zeroOutside = false) const {
        const int nx = n_[0], ny = n_[1], nz = n_[2];
        if (zeroOutside && (x < 0.0f || y < 0.0f || z < 0.0f || x > static_cast<float>(nx) ||
                            y > static_cast<float>(ny) || z > static_cast<float>(nz))) {
            return 0.0f;
        }
        // As Grid: cell centres at the integers, a NaN on 0.
        const float fx = std::max(0.0f, std::min(x - 0.5f, static_cast<float>(nx - 1)));
        const float fy = std::max(0.0f, std::min(y - 0.5f, static_cast<float>(ny - 1)));
        const float fz = std::max(0.0f, std::min(z - 0.5f, static_cast<float>(nz - 1)));
        const int i = static_cast<int>(fx), j = static_cast<int>(fy), k = static_cast<int>(fz);
        const float tx = fx - static_cast<float>(i), ty = fy - static_cast<float>(j), tz = fz - static_cast<float>(k);
        float c[8];
        corners(i, j, k, i + 1 < nx ? 1 : 0, j + 1 < ny ? 1 : 0, k + 1 < nz ? 1 : 0, c);
        const float x00 = c[0] + (c[1] - c[0]) * tx, x10 = c[2] + (c[3] - c[2]) * tx;
        const float x01 = c[4] + (c[5] - c[4]) * tx, x11 = c[6] + (c[7] - c[6]) * tx;
        const float y0 = x00 + (x10 - x00) * ty, y1 = x01 + (x11 - x01) * ty;
        return y0 + (y1 - y0) * tz;
    }
    float sample(float x, float y, float z, bool zeroOutside, float& lo, float& hi) const;

    /// Where a trilinear lookup at (x, y, z) reads -- the eight cells
    /// sample() reads, as places in data(), -1 where not stored (the
    /// background) -- and how far between them it is. The same for every
    /// grid of these tiles: many are sampled for the price of one lookup,
    /// each to the bit as sample() would.
    struct Corners {
        int64_t at[8];
        float tx, ty, tz;
    };
    [[gnu::always_inline]] void cornersAt(float x, float y, float z, Corners& out) const {
        const int nx = n_[0], ny = n_[1], nz = n_[2];
        const float fx = std::max(0.0f, std::min(x - 0.5f, static_cast<float>(nx - 1)));
        const float fy = std::max(0.0f, std::min(y - 0.5f, static_cast<float>(ny - 1)));
        const float fz = std::max(0.0f, std::min(z - 0.5f, static_cast<float>(nz - 1)));
        const int i = static_cast<int>(fx), j = static_cast<int>(fy), k = static_cast<int>(fz);
        out.tx = fx - static_cast<float>(i);
        out.ty = fy - static_cast<float>(j);
        out.tz = fz - static_cast<float>(k);
        const int di = i + 1 < nx ? 1 : 0, dj = j + 1 < ny ? 1 : 0, dk = k + 1 < nz ? 1 : 0;
        constexpr int kLast = Tiles::kSide - 1;
        const int li = i & kLast, lj = j & kLast, lk = k & kLast;
        const size_t tile = static_cast<size_t>(i >> Tiles::kLog) + tx_ * static_cast<size_t>(j >> Tiles::kLog) +
                            txy_ * static_cast<size_t>(k >> Tiles::kLog);
        const bool cx = li + di > kLast, cy = lj + dj > kLast, cz = lk + dk > kLast;
        if (!(cx || cy || cz)) {
            const int32_t s = slots_[tile];
            if (s < 0) {
                for (int64_t& a : out.at) a = -1;
                return;
            }
            const int64_t base = static_cast<int64_t>(s) * Tiles::kCells + static_cast<int64_t>(local(i, j, k));
            const int64_t ox = di, oy = dj ? Tiles::kSide : 0, oz = dk ? Tiles::kSide * Tiles::kSide : 0;
            out.at[0] = base;
            out.at[1] = base + ox;
            out.at[2] = base + oy;
            out.at[3] = base + oy + ox;
            out.at[4] = base + oz;
            out.at[5] = base + oz + ox;
            out.at[6] = base + oz + oy;
            out.at[7] = base + oz + oy + ox;
            return;
        }
        const int32_t* t = slots_ + tile;
        const size_t sx = cx ? 1u : 0u, sy = cy ? tx_ : 0u, sz = cz ? txy_ : 0u;
        const int32_t s[8] = {t[0], t[sx], t[sy], t[sy + sx], t[sz], t[sz + sx], t[sz + sy], t[sz + sy + sx]};
        const int64_t px[2] = {li, (li + di) & kLast};
        const int64_t py[2] = {lj * Tiles::kSide, ((lj + dj) & kLast) * Tiles::kSide};
        const int64_t pz[2] = {lk * Tiles::kSide * Tiles::kSide, ((lk + dk) & kLast) * Tiles::kSide * Tiles::kSide};
        for (int q = 0; q < 8; ++q) {
            out.at[q] = s[q] < 0 ? -1
                                 : static_cast<int64_t>(s[q]) * Tiles::kCells + px[q & 1] + py[(q >> 1) & 1] + pz[q >> 2];
        }
    }
    /// The value at a lookup cornersAt() made -- on a grid of these tiles.
    [[gnu::always_inline]] float sampleAt(const Corners& at) const {
        float c[8];
        for (int q = 0; q < 8; ++q) c[q] = at.at[q] < 0 ? background_ : data_[static_cast<size_t>(at.at[q])];
        return lerp(c, at);
    }
    /// ... and the least and the most of the eight, as sample() gives them.
    [[gnu::always_inline]] float sampleAt(const Corners& at, float& lo, float& hi) const {
        float c[8];
        for (int q = 0; q < 8; ++q) c[q] = at.at[q] < 0 ? background_ : data_[static_cast<size_t>(at.at[q])];
        lo = std::min({c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7]});
        hi = std::max({c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7]});
        return lerp(c, at);
    }

    /// Every stored value.
    void fill(float value);
    float* data() { return data_.data(); }
    const float* data() const { return data_.data(); }
    /// Values stored: 512 a tile.
    size_t size() const { return data_.size(); }
    const std::vector<float>& values() const { return data_; }

    /// Over every cell of the grid, those not stored as the background.
    float max() const;
    double sum() const;

    /// Keeps the values of the tiles both keep; the rest the background.
    void retile(std::shared_ptr<const Tiles> tiles);
    /// Onto `tiles` as scratch: the values whatever they come to -- to be
    /// written before they are read -- and the memory kept.
    void reshape(std::shared_ptr<const Tiles> tiles);
    /// Every cell, as a dense grid.
    Grid dense() const;

private:
    /// The table, its strides and the grid's size, from tiles_.
    void cache();
    /// Trilinear between eight values, as sample() does it.
    [[gnu::always_inline]] static float lerp(const float c[8], const Corners& at) {
        const float x00 = c[0] + (c[1] - c[0]) * at.tx, x10 = c[2] + (c[3] - c[2]) * at.tx;
        const float x01 = c[4] + (c[5] - c[4]) * at.tx, x11 = c[6] + (c[7] - c[6]) * at.tx;
        const float y0 = x00 + (x10 - x00) * at.ty, y1 = x01 + (x11 - x01) * at.ty;
        return y0 + (y1 - y0) * at.tz;
    }
    /// The eight values a trilinear lookup at cell (i, j, k) reads -- with
    /// the next cell along each axis di, dj, dk (0 or 1) on -- x fastest:
    /// from one tile mostly, else from each of the tiles they lie in.
    [[gnu::always_inline]] void corners(int i, int j, int k, int di, int dj, int dk, float c[8]) const {
        constexpr int kLast = Tiles::kSide - 1;
        const int li = i & kLast, lj = j & kLast, lk = k & kLast;
        const size_t tile = static_cast<size_t>(i >> Tiles::kLog) + tx_ * static_cast<size_t>(j >> Tiles::kLog) +
                            txy_ * static_cast<size_t>(k >> Tiles::kLog);
        const bool cx = li + di > kLast, cy = lj + dj > kLast, cz = lk + dk > kLast;
        const float* d = data_.data();
        if (!(cx || cy || cz)) {
            // The eight in one tile: one look in the table.
            const int32_t s = slots_[tile];
            if (s < 0) {
                for (int q = 0; q < 8; ++q) c[q] = background_;
                return;
            }
            const float* p = d + static_cast<size_t>(s) * Tiles::kCells + local(i, j, k);
            const size_t ox = static_cast<size_t>(di), oy = dj ? Tiles::kSide : 0u,
                         oz = dk ? Tiles::kSide * Tiles::kSide : 0u;
            c[0] = p[0];
            c[1] = p[ox];
            c[2] = p[oy];
            c[3] = p[oy + ox];
            c[4] = p[oz];
            c[5] = p[oz + ox];
            c[6] = p[oz + oy];
            c[7] = p[oz + oy + ox];
            return;
        }
        // Across the edge of a tile: each corner from the tile it lies in.
        const int32_t* t = slots_ + tile;
        const size_t sx = cx ? 1u : 0u, sy = cy ? tx_ : 0u, sz = cz ? txy_ : 0u;
        const int32_t s[8] = {t[0], t[sx], t[sy], t[sy + sx], t[sz], t[sz + sx], t[sz + sy], t[sz + sy + sx]};
        const size_t x[2] = {static_cast<size_t>(li), static_cast<size_t>((li + di) & kLast)};
        const size_t y[2] = {static_cast<size_t>(lj) * Tiles::kSide, static_cast<size_t>((lj + dj) & kLast) * Tiles::kSide};
        const size_t z[2] = {static_cast<size_t>(lk) * Tiles::kSide * Tiles::kSide,
                             static_cast<size_t>((lk + dk) & kLast) * Tiles::kSide * Tiles::kSide};
        for (int q = 0; q < 8; ++q) {
            c[q] = s[q] < 0 ? background_
                            : d[static_cast<size_t>(s[q]) * Tiles::kCells + x[q & 1] + y[(q >> 1) & 1] + z[q >> 2]];
        }
    }

    std::shared_ptr<const Tiles> tiles_;
    std::vector<float> data_;
    float background_ = 0.0f;
    const int32_t* slots_ = nullptr;
    size_t tx_ = 0, txy_ = 0;
    int n_[3] = {0, 0, 0};
};

/// f(i, j, k, index) for every cell of `tiles` that counts, in parallel,
/// `grain` tiles a chunk: each cell visited once, by one thread -- whichever,
/// never changing a result.
template <class F>
void forEachCounted(const Tiles& tiles, const F& f, size_t grain = 16) {
    const std::vector<uint32_t>& stored = tiles.stored();
    const int n[3] = {tiles.nx(), tiles.ny(), tiles.nz()};
    pg::parallelFor(stored.size(), grain, [&](size_t begin, size_t end) {
        for (size_t s = begin; s < end; ++s) {
            int c[3];
            tiles.corner(stored[s], c[0], c[1], c[2]);
            int e[3];
            for (int a = 0; a < 3; ++a) e[a] = std::min(Tiles::kSide, n[a] - c[a]);
            if (tiles.state(stored[s]) == Tiles::FirstLayer) e[tiles.axis()] = std::min(e[tiles.axis()], 1);
            const size_t base = s * Tiles::kCells;
            for (int z = 0; z < e[2]; ++z) {
                for (int y = 0; y < e[1]; ++y) {
                    for (int x = 0; x < e[0]; ++x) {
                        f(c[0] + x, c[1] + y, c[2] + z, base + SparseGrid::local(x, y, z));
                    }
                }
            }
        }
    });
}

}  // namespace pg::sim
