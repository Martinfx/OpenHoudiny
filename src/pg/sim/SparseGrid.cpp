#include "pg/sim/SparseGrid.h"

#include "pg/core/Parallel.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace pg::sim {

// --- tiles -------------------------------------------------------------------------

Tiles::Tiles(int nx, int ny, int nz)
    : Tiles(nx, ny, nz,
            std::vector<uint8_t>(static_cast<size_t>((nx + kSide - 1) / kSide) *
                                     static_cast<size_t>((ny + kSide - 1) / kSide) *
                                     static_cast<size_t>((nz + kSide - 1) / kSide),
                                 Whole),
            -1) {}

Tiles::Tiles(int nx, int ny, int nz, std::vector<uint8_t> state, int axis) : axis_(axis), state_(std::move(state)) {
    n_[0] = nx;
    n_[1] = ny;
    n_[2] = nz;
    for (int a = 0; a < 3; ++a) t_[a] = (n_[a] + kSide - 1) / kSide;
    state_.resize(static_cast<size_t>(t_[0]) * static_cast<size_t>(t_[1]) * static_cast<size_t>(t_[2]), Off);
    slot_.assign(state_.size(), -1);
    all_ = true;
    for (size_t t = 0; t < state_.size(); ++t) {
        if (state_[t] != Whole) all_ = false;
        if (state_[t] == Off) continue;
        slot_[t] = static_cast<int32_t>(stored_.size());
        stored_.push_back(static_cast<uint32_t>(t));
    }
}

void Tiles::corner(size_t t, int& i, int& j, int& k) const {
    const size_t tx = static_cast<size_t>(t_[0]), ty = static_cast<size_t>(t_[1]);
    i = static_cast<int>(t % tx) * kSide;
    j = static_cast<int>((t / tx) % ty) * kSide;
    k = static_cast<int>(t / (tx * ty)) * kSide;
}

std::shared_ptr<const Tiles> Tiles::faces(const Tiles& cells, int axis) {
    const int n[3] = {cells.nx() + (axis == 0), cells.ny() + (axis == 1), cells.nz() + (axis == 2)};
    int t[3];
    for (int a = 0; a < 3; ++a) t[a] = (n[a] + kSide - 1) / kSide;
    std::vector<uint8_t> state(static_cast<size_t>(t[0]) * static_cast<size_t>(t[1]) * static_cast<size_t>(t[2]), Off);
    auto number = [&](int a, int b, int c) {
        return static_cast<size_t>(a) + static_cast<size_t>(t[0]) * (static_cast<size_t>(b) + static_cast<size_t>(t[1]) * static_cast<size_t>(c));
    };
    for (const uint32_t tile : cells.stored()) {
        if (cells.state(tile) != Whole) continue;
        int i, j, k;
        cells.corner(tile, i, j, k);
        const int a = i / kSide, b = j / kSide, c = k / kSide;
        state[number(a, b, c)] = Whole;
        // The far faces of the tile's last cells: the next tile's first layer.
        const int e[3] = {a + (axis == 0), b + (axis == 1), c + (axis == 2)};
        if (e[0] < t[0] && e[1] < t[1] && e[2] < t[2]) {
            uint8_t& s = state[number(e[0], e[1], e[2])];
            if (s == Off) s = FirstLayer;
        }
    }
    return std::make_shared<const Tiles>(n[0], n[1], n[2], std::move(state), axis);
}

size_t Tiles::activeCells() const {
    size_t count = 0;
    for (const uint32_t t : stored_) {
        int c[3];
        corner(t, c[0], c[1], c[2]);
        size_t cells = 1;
        for (int a = 0; a < 3; ++a) {
            int extent = std::min(kSide, n_[a] - c[a]);
            if (state_[t] == FirstLayer && a == axis_) extent = std::min(extent, 1);
            cells *= static_cast<size_t>(std::max(extent, 0));
        }
        count += cells;
    }
    return count;
}

// --- the grid ------------------------------------------------------------------------

SparseGrid::SparseGrid(int nx, int ny, int nz, float value)
    : tiles_(std::make_shared<const Tiles>(nx, ny, nz)),
      data_(tiles_->stored().size() * Tiles::kCells, value) {
    cache();
}

SparseGrid::SparseGrid(std::shared_ptr<const Tiles> tiles, float background)
    : tiles_(std::move(tiles)), data_(tiles_->stored().size() * Tiles::kCells, background), background_(background) {
    cache();
}

void SparseGrid::cache() {
    slots_ = tiles_ ? tiles_->slot_.data() : nullptr;
    tx_ = tiles_ ? static_cast<size_t>(tiles_->tilesX()) : 0u;
    txy_ = tiles_ ? tx_ * static_cast<size_t>(tiles_->tilesY()) : 0u;
    n_[0] = tiles_ ? tiles_->nx() : 0;
    n_[1] = tiles_ ? tiles_->ny() : 0;
    n_[2] = tiles_ ? tiles_->nz() : 0;
}

float SparseGrid::clamped(int i, int j, int k) const {
    return at(std::clamp(i, 0, nx() - 1), std::clamp(j, 0, ny() - 1), std::clamp(k, 0, nz() - 1));
}

namespace {

inline bool outside(float x, float y, float z, int nx, int ny, int nz) {
    return x < 0.0f || y < 0.0f || z < 0.0f || x > static_cast<float>(nx) || y > static_cast<float>(ny) ||
           z > static_cast<float>(nz);
}

}  // namespace

float SparseGrid::sample(float x, float y, float z, bool zeroOutside, float& lo, float& hi) const {
    const int nx = n_[0], ny = n_[1], nz = n_[2];
    if (zeroOutside && outside(x, y, z, nx, ny, nz)) {
        lo = hi = 0.0f;
        return 0.0f;
    }
    const float fx = std::max(0.0f, std::min(x - 0.5f, static_cast<float>(nx - 1)));
    const float fy = std::max(0.0f, std::min(y - 0.5f, static_cast<float>(ny - 1)));
    const float fz = std::max(0.0f, std::min(z - 0.5f, static_cast<float>(nz - 1)));
    const int i = static_cast<int>(fx), j = static_cast<int>(fy), k = static_cast<int>(fz);
    const float tx = fx - static_cast<float>(i), ty = fy - static_cast<float>(j), tz = fz - static_cast<float>(k);
    float c[8];
    corners(i, j, k, i + 1 < nx ? 1 : 0, j + 1 < ny ? 1 : 0, k + 1 < nz ? 1 : 0, c);
    lo = std::min({c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7]});
    hi = std::max({c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7]});
    const float x00 = c[0] + (c[1] - c[0]) * tx, x10 = c[2] + (c[3] - c[2]) * tx;
    const float x01 = c[4] + (c[5] - c[4]) * tx, x11 = c[6] + (c[7] - c[6]) * tx;
    const float y0 = x00 + (x10 - x00) * ty, y1 = x01 + (x11 - x01) * ty;
    return y0 + (y1 - y0) * tz;
}

void SparseGrid::fill(float value) { std::fill(data_.begin(), data_.end(), value); }

float SparseGrid::max() const {
    if (!tiles_ || nx() <= 0 || ny() <= 0 || nz() <= 0) return 0.0f;
    bool any = false;
    float m = background_;
    if (!tiles_->all()) any = true;  // a cell not stored: the background
    for (size_t s = 0; s < tiles_->stored().size(); ++s) {
        int c[3];
        tiles_->corner(tiles_->stored()[s], c[0], c[1], c[2]);
        const int e[3] = {std::min(Tiles::kSide, nx() - c[0]), std::min(Tiles::kSide, ny() - c[1]),
                          std::min(Tiles::kSide, nz() - c[2])};
        const float* d = data_.data() + s * Tiles::kCells;
        for (int z = 0; z < e[2]; ++z) {
            for (int y = 0; y < e[1]; ++y) {
                for (int x = 0; x < e[0]; ++x) {
                    const float v = d[local(x, y, z)];
                    m = any ? std::max(m, v) : v;
                    any = true;
                }
            }
        }
    }
    return m;
}

double SparseGrid::sum() const {
    double s = 0.0;
    for (int k = 0; k < nz(); ++k) {
        for (int j = 0; j < ny(); ++j) {
            for (int i = 0; i < nx(); ++i) s += at(i, j, k);
        }
    }
    return s;
}

void SparseGrid::retile(std::shared_ptr<const Tiles> tiles) {
    std::vector<float> next(tiles->stored().size() * Tiles::kCells, background_);
    if (tiles_) {
        const Tiles& from = *tiles_;
        const std::vector<uint32_t>& to = tiles->stored();
        pg::parallelFor(to.size(), 64, [&](size_t begin, size_t end) {
            for (size_t s = begin; s < end; ++s) {
                const int32_t old = to[s] < from.tileCount() ? from.slot(to[s]) : -1;
                if (old < 0) continue;
                std::memcpy(next.data() + s * Tiles::kCells, data_.data() + static_cast<size_t>(old) * Tiles::kCells,
                            Tiles::kCells * sizeof(float));
            }
        });
    }
    tiles_ = std::move(tiles);
    data_ = std::move(next);
    cache();
}

void SparseGrid::reshape(std::shared_ptr<const Tiles> tiles) {
    tiles_ = std::move(tiles);
    data_.resize(tiles_->stored().size() * Tiles::kCells);
    cache();
}

Grid SparseGrid::dense() const {
    Grid g(nx(), ny(), nz());
    pg::parallelFor(static_cast<size_t>(nz()), 1, [&](size_t begin, size_t end) {
        for (size_t kk = begin; kk < end; ++kk) {
            const int k = static_cast<int>(kk);
            for (int j = 0; j < ny(); ++j) {
                for (int i = 0; i < nx(); ++i) g.at(i, j, k) = at(i, j, k);
            }
        }
    });
    return g;
}

}  // namespace pg::sim
