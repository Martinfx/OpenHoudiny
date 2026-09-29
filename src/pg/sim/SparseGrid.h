#pragma once
//
// A 3D grid of floats kept in tiles of 8 x 8 x 8 cells, only the active tiles
// stored -- what production keeps in OpenVDB, flattened to one level: a table
// of the tiles of the whole box, and the values of the active ones.
//
// Cells, positions and sampling are those of Grid (Grid.h): cell (i, j, k)
// covers [i, i+1) x [j, j+1) x [k, k+1) and its value sits at its centre. A
// cell that is not stored reads 0 -- still, empty air.
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
};

class SparseGrid {
public:
    SparseGrid() = default;
    /// Every tile stored, every cell `value`.
    SparseGrid(int nx, int ny, int nz, float value = 0.0f);
    /// The tiles `tiles` keeps, all 0.
    explicit SparseGrid(std::shared_ptr<const Tiles> tiles);

    int nx() const { return tiles_ ? tiles_->nx() : 0; }
    int ny() const { return tiles_ ? tiles_->ny() : 0; }
    int nz() const { return tiles_ ? tiles_->nz() : 0; }
    const Tiles& tiles() const { return *tiles_; }
    const std::shared_ptr<const Tiles>& shared() const { return tiles_; }

    /// Is cell (i, j, k) stored (its tile is), and does it count?
    bool stored(int i, int j, int k) const { return tiles_->slot(tiles_->tileOf(i, j, k)) >= 0; }
    bool has(int i, int j, int k) const { return tiles_->has(i, j, k); }
    /// Where a stored cell's value is in data(): the same in every grid of
    /// these Tiles.
    size_t index(int i, int j, int k) const {
        return static_cast<size_t>(tiles_->slot(tiles_->tileOf(i, j, k))) * Tiles::kCells + local(i, j, k);
    }
    static size_t local(int i, int j, int k) {
        return static_cast<size_t>(i & (Tiles::kSide - 1)) +
               Tiles::kSide * (static_cast<size_t>(j & (Tiles::kSide - 1)) +
                               Tiles::kSide * static_cast<size_t>(k & (Tiles::kSide - 1)));
    }
    /// The value of a cell inside the grid; 0 where it is not stored.
    float at(int i, int j, int k) const {
        const int32_t s = tiles_->slot(tiles_->tileOf(i, j, k));
        return s < 0 ? 0.0f : data_[static_cast<size_t>(s) * Tiles::kCells + local(i, j, k)];
    }
    /// A stored cell's value, to write. (Not an overload of at(): a read in a
    /// method that may write would take it, and fail where nothing is stored.)
    float& ref(int i, int j, int k) { return data_[index(i, j, k)]; }
    /// The nearest cell inside the grid: a boundary that repeats itself.
    float clamped(int i, int j, int k) const;

    /// Trilinear interpolation at a position in cell units, as Grid::sample.
    float sample(float x, float y, float z, bool zeroOutside = false) const;
    float sample(float x, float y, float z, bool zeroOutside, float& lo, float& hi) const;

    /// Every stored value.
    void fill(float value);
    float* data() { return data_.data(); }
    const float* data() const { return data_.data(); }
    /// Values stored: 512 a tile.
    size_t size() const { return data_.size(); }
    const std::vector<float>& values() const { return data_; }

    /// Over every cell of the grid, those not stored as 0.
    float max() const;
    double sum() const;

    /// Keeps the values of the tiles both keep; the rest 0.
    void retile(std::shared_ptr<const Tiles> tiles);
    /// Every cell, as a dense grid.
    Grid dense() const;

private:
    std::shared_ptr<const Tiles> tiles_;
    std::vector<float> data_;
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
