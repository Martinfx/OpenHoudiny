#include "pg/sim/Frame.h"

#include "pg/core/Half.h"
#include "pg/core/Parallel.h"
#include "pg/sim/Liquid.h"
#include "pg/sim/Look.h"
#include "pg/sim/Pyro.h"
#include "pg/sim/Rain.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>

namespace pg::sim {

uint16_t toHalf(float value) { return halfFromFloat(value); }

float fromHalf(uint16_t value) { return floatFromHalf(value); }

namespace {

/// Tiles of 8 cells a side along each axis of a domain.
void tileCounts(const Domain& d, size_t t[3]) {
    for (int a = 0; a < 3; ++a) t[a] = static_cast<size_t>((d.cells[a] + Tiles::kSide - 1) / Tiles::kSide);
}

}  // namespace

float Frame::at(int channel, int i, int j, int k) const {
    // Steam is laid out as the rest, a value a cell.
    const bool steamy = channel == 3;
    if (steamy && (steam.empty() || !steamFits())) return 0.0f;
    const std::vector<uint16_t>& values = steamy ? steam : fields;
    const size_t width = steamy ? 1 : 3, ch = steamy ? 0 : static_cast<size_t>(channel);
    if (gasTiles.empty()) {
        const size_t cell = static_cast<size_t>(i) +
                            static_cast<size_t>(domain.cells[0]) *
                                (static_cast<size_t>(j) + static_cast<size_t>(domain.cells[1]) * static_cast<size_t>(k));
        return fromHalf(values[width * cell + ch]);
    }
    size_t t[3];
    tileCounts(domain, t);
    const uint32_t tile = static_cast<uint32_t>(static_cast<size_t>(i >> Tiles::kLog) +
                                                t[0] * (static_cast<size_t>(j >> Tiles::kLog) + t[1] * static_cast<size_t>(k >> Tiles::kLog)));
    const auto found = std::lower_bound(gasTiles.begin(), gasTiles.end(), tile);
    if (found == gasTiles.end() || *found != tile) return 0.0f;
    const size_t slot = static_cast<size_t>(found - gasTiles.begin());
    return fromHalf(values[width * (slot * Tiles::kCells + SparseGrid::local(i, j, k)) + ch]);
}

const std::vector<uint16_t>& Frame::denseFields(std::vector<uint16_t>& scratch) const {
    if (gasTiles.empty()) return fields;
    scratch.assign(3 * domain.cellCount(), 0);
    size_t t[3];
    tileCounts(domain, t);
    const size_t nx = static_cast<size_t>(domain.cells[0]), ny = static_cast<size_t>(domain.cells[1]);
    pg::parallelFor(gasTiles.size(), 16, [&](size_t begin, size_t end) {
        for (size_t s = begin; s < end; ++s) {
            const size_t tile = gasTiles[s];
            const int c[3] = {static_cast<int>(tile % t[0]) * Tiles::kSide,
                              static_cast<int>((tile / t[0]) % t[1]) * Tiles::kSide,
                              static_cast<int>(tile / (t[0] * t[1])) * Tiles::kSide};
            for (int z = 0; z < Tiles::kSide && c[2] + z < domain.cells[2]; ++z) {
                for (int y = 0; y < Tiles::kSide && c[1] + y < domain.cells[1]; ++y) {
                    for (int x = 0; x < Tiles::kSide && c[0] + x < domain.cells[0]; ++x) {
                        const size_t from = 3 * (s * Tiles::kCells + SparseGrid::local(x, y, z));
                        const size_t cell = static_cast<size_t>(c[0] + x) +
                                            nx * (static_cast<size_t>(c[1] + y) + ny * static_cast<size_t>(c[2] + z));
                        for (int ch = 0; ch < 3; ++ch) scratch[3 * cell + static_cast<size_t>(ch)] = fields[from + static_cast<size_t>(ch)];
                    }
                }
            }
        }
    });
    return scratch;
}

Frame capture(const PyroSolver& sim) {
    Frame f = gasFrame(sim.domain(), sim.tiles(), sim.density(), sim.temperature(), sim.flame(),
                       sim.steamy() ? &sim.steam() : nullptr);
    f.number = sim.frame();
    f.time = sim.time();
    return f;
}

Frame gasFrame(const Domain& domain, const Tiles& tiles, const SparseGrid& smokeGrid, const SparseGrid& heatGrid,
               const SparseGrid& flameGrid, const SparseGrid* steamGrid) {
    Frame f;
    f.domain = domain;
    const float* smoke = smokeGrid.data();
    const float* heat = heatGrid.data();
    const float* flame = flameGrid.data();
    const float* steam = steamGrid ? steamGrid->data() : nullptr;
    // No steam that a half holds: none.
    auto dry = [&]() {
        if (std::all_of(f.steam.begin(), f.steam.end(), [](uint16_t h) { return h == 0; })) f.steam.clear();
    };
    if (tiles.all()) {
        // Every cell, x fastest.
        f.fields.assign(3 * f.domain.cellCount(), 0);
        if (steam) f.steam.assign(f.domain.cellCount(), 0);
        uint16_t* out = f.fields.data();
        uint16_t* vapour = f.steam.data();
        const size_t nx = static_cast<size_t>(f.domain.cells[0]), ny = static_cast<size_t>(f.domain.cells[1]);
        forEachCounted(tiles, [&](int i, int j, int k, size_t c) {
            const size_t cell = static_cast<size_t>(i) + nx * (static_cast<size_t>(j) + ny * static_cast<size_t>(k));
            out[3 * cell] = toHalf(smoke[c]);
            out[3 * cell + 1] = toHalf(heat[c]);
            out[3 * cell + 2] = toHalf(flame[c]);
            if (steam) vapour[cell] = toHalf(std::max(steam[c], 0.0f));
        });
        dry();
        return f;
    }
    // Sparse: the tiles with any gas -- as halves -- in them, as the solver
    // keeps them; one, empty, when there is none.
    const std::vector<uint32_t>& stored = tiles.stored();
    std::vector<uint8_t> any(stored.size(), 0);
    pg::parallelFor(stored.size(), 16, [&](size_t begin, size_t end) {
        for (size_t s = begin; s < end; ++s) {
            const size_t base = s * Tiles::kCells;
            for (size_t c = base; c < base + Tiles::kCells && !any[s]; ++c) {
                any[s] = toHalf(smoke[c]) != 0 || toHalf(heat[c]) != 0 || toHalf(flame[c]) != 0 ||
                         (steam && toHalf(std::max(steam[c], 0.0f)) != 0);
            }
        }
    });
    std::vector<size_t> slots;
    for (size_t s = 0; s < stored.size(); ++s) {
        if (!any[s]) continue;
        f.gasTiles.push_back(stored[s]);
        slots.push_back(s);
    }
    if (f.gasTiles.empty()) {
        f.gasTiles.push_back(0);
        f.fields.assign(3 * Tiles::kCells, 0);
        return f;
    }
    f.fields.resize(3 * Tiles::kCells * slots.size());
    if (steam) f.steam.assign(Tiles::kCells * slots.size(), 0);
    uint16_t* out = f.fields.data();
    uint16_t* vapour = f.steam.data();
    pg::parallelFor(slots.size(), 16, [&](size_t begin, size_t end) {
        for (size_t t = begin; t < end; ++t) {
            const size_t from = slots[t] * Tiles::kCells, to = t * Tiles::kCells;
            for (size_t c = 0; c < Tiles::kCells; ++c) {
                out[3 * (to + c)] = toHalf(smoke[from + c]);
                out[3 * (to + c) + 1] = toHalf(heat[from + c]);
                out[3 * (to + c) + 2] = toHalf(flame[from + c]);
                if (steam) vapour[to + c] = toHalf(std::max(steam[from + c], 0.0f));
            }
        }
    });
    dry();
    return f;
}

namespace {

/// Where tile `tile` is in a sorted list of tiles, -1 when it is not there.
int64_t slotIn(const std::vector<uint32_t>& tiles, uint32_t tile) {
    const auto found = std::lower_bound(tiles.begin(), tiles.end(), tile);
    return found == tiles.end() || *found != tile ? -1 : static_cast<int64_t>(found - tiles.begin());
}

/// The number of the tile of cell (i, j, k) of a domain.
uint32_t tileOfCell(const Domain& d, int i, int j, int k) {
    size_t t[3];
    tileCounts(d, t);
    return static_cast<uint32_t>(static_cast<size_t>(i >> Tiles::kLog) +
                                 t[0] * (static_cast<size_t>(j >> Tiles::kLog) + t[1] * static_cast<size_t>(k >> Tiles::kLog)));
}

/// f(s, i, j, k, l) for each cell (i, j, k) of `d` in each of `tiles` --
/// s the tile's place among them, l the cell's within it -- the tiles in
/// parallel.
template <class F>
void forTileCells(const Domain& d, const std::vector<uint32_t>& tiles, const F& f) {
    size_t t[3];
    tileCounts(d, t);
    pg::parallelFor(tiles.size(), 16, [&](size_t begin, size_t end) {
        for (size_t s = begin; s < end; ++s) {
            const size_t tile = tiles[s];
            const int c[3] = {static_cast<int>(tile % t[0]) * Tiles::kSide, static_cast<int>((tile / t[0]) % t[1]) * Tiles::kSide,
                              static_cast<int>(tile / (t[0] * t[1])) * Tiles::kSide};
            for (int z = 0; z < Tiles::kSide && c[2] + z < d.cells[2]; ++z) {
                for (int y = 0; y < Tiles::kSide && c[1] + y < d.cells[1]; ++y) {
                    for (int x = 0; x < Tiles::kSide && c[0] + x < d.cells[0]; ++x) {
                        f(s, c[0] + x, c[1] + y, c[2] + z, SparseGrid::local(x, y, z));
                    }
                }
            }
        }
    });
}

/// `values` of `width` a cell, kept in `tiles` of `d`, into every cell of
/// it, x fastest; `background` (`width` of them) where no tile is kept.
template <class T>
void spreadTiles(const Domain& d, const std::vector<uint32_t>& tiles, const std::vector<T>& values, size_t width,
                 const T* background, std::vector<T>& out) {
    out.resize(width * d.cellCount());
    pg::parallelFor(d.cellCount(), 65536, [&](size_t begin, size_t end) {
        for (size_t c = begin; c < end; ++c) std::copy(background, background + width, out.begin() + static_cast<std::ptrdiff_t>(width * c));
    });
    const size_t nx = static_cast<size_t>(d.cells[0]), ny = static_cast<size_t>(d.cells[1]);
    forTileCells(d, tiles, [&](size_t s, int i, int j, int k, size_t l) {
        const size_t from = width * (s * Tiles::kCells + l);
        const size_t cell = static_cast<size_t>(i) + nx * (static_cast<size_t>(j) + ny * static_cast<size_t>(k));
        for (size_t w = 0; w < width; ++w) out[width * cell + w] = values[from + w];
    });
}

/// Is `tiles`, of a grid of `count` tiles, in order and within it?
bool inOrder(const std::vector<uint32_t>& tiles, size_t count) {
    for (size_t s = 0; s < tiles.size(); ++s) {
        if (tiles[s] >= count || (s > 0 && tiles[s - 1] >= tiles[s])) return false;
    }
    return true;
}

}  // namespace

int64_t WaterFrame::cellOf(int i, int j, int k) const {
    if (tiles.empty()) {
        return 2 * static_cast<int64_t>(static_cast<size_t>(i) +
                                        static_cast<size_t>(domain.cells[0]) *
                                            (static_cast<size_t>(j) + static_cast<size_t>(domain.cells[1]) * static_cast<size_t>(k)));
    }
    const int64_t slot = slotIn(tiles, tileOfCell(domain, i, j, k));
    return slot < 0 ? -1 : 2 * (slot * Tiles::kCells + static_cast<int64_t>(SparseGrid::local(i, j, k)));
}

float WaterFrame::distance(int i, int j, int k) const {
    const int64_t c = cellOf(i, j, k);
    if (c < 0) return !deepTiles.empty() && slotIn(deepTiles, tileOfCell(domain, i, j, k)) >= 0 ? -band : band;
    return (static_cast<float>(cells[static_cast<size_t>(c)]) / 255.0f * 2.0f - 1.0f) * band;
}

float WaterFrame::foam(int i, int j, int k) const {
    const int64_t c = cellOf(i, j, k);
    return c < 0 ? 0.0f : static_cast<float>(cells[static_cast<size_t>(c) + 1]) / 255.0f;
}

bool WaterFrame::fits() const {
    if (tiles.empty()) return deepTiles.empty() && cells.size() == 2 * domain.cellCount();
    size_t t[3];
    tileCounts(domain, t);
    const size_t count = t[0] * t[1] * t[2];
    if (!inOrder(tiles, count) || !inOrder(deepTiles, count)) return false;
    // A tile is kept one way or the other.
    std::vector<uint32_t> both;
    std::set_intersection(tiles.begin(), tiles.end(), deepTiles.begin(), deepTiles.end(), std::back_inserter(both));
    return both.empty() && cells.size() == 2 * Tiles::kCells * tiles.size();
}

namespace {

/// `values` of `width` a cell, laid out as `frame`'s gas, of the grid
/// `factor` times as coarse: the mean of the cells under each.
void coarseOf(const Frame& frame, const std::vector<uint16_t>& values, size_t width, int factor,
              std::vector<uint16_t>& out) {
    const Domain& domain = frame.domain;
    const int f = factor;
    const int n[3] = {domain.cells[0] / f, domain.cells[1] / f, domain.cells[2] / f};
    const size_t count = static_cast<size_t>(n[0]) * static_cast<size_t>(n[1]) * static_cast<size_t>(n[2]);
    // The sums of the cells under each coarse one.
    std::vector<float> sum(width * count, 0.0f);
    auto add = [&](size_t from, int i, int j, int k) {
        const size_t to = width * (static_cast<size_t>(i / f) +
                                   static_cast<size_t>(n[0]) * (static_cast<size_t>(j / f) +
                                                                static_cast<size_t>(n[1]) * static_cast<size_t>(k / f)));
        for (size_t ch = 0; ch < width; ++ch) sum[to + ch] += fromHalf(values[from + ch]);
    };
    if (frame.gasTiles.empty()) {
        // Each coarse layer of z apart: what it sums is its own.
        pg::parallelFor(static_cast<size_t>(n[2]), 1, [&](size_t begin, size_t end) {
            for (int k = static_cast<int>(begin) * f; k < static_cast<int>(end) * f; ++k) {
                for (int j = 0; j < domain.cells[1]; ++j) {
                    for (int i = 0; i < domain.cells[0]; ++i) {
                        add(width * (static_cast<size_t>(i) + static_cast<size_t>(domain.cells[0]) *
                                                                  (static_cast<size_t>(j) + static_cast<size_t>(domain.cells[1]) *
                                                                                                static_cast<size_t>(k))),
                            i, j, k);
                    }
                }
            }
        });
    } else {
        // A tile covers whole coarse cells, which no other tile does; where
        // no tile is, there is no gas.
        forTileCells(domain, frame.gasTiles,
                     [&](size_t s, int i, int j, int k, size_t l) { add(width * (s * Tiles::kCells + l), i, j, k); });
    }
    out.resize(width * count);
    const float under = 1.0f / static_cast<float>(f * f * f);
    for (size_t c = 0; c < width * count; ++c) out[c] = toHalf(sum[c] * under);
}

}  // namespace

void Frame::coarseFields(int factor, std::vector<uint16_t>& out) const { coarseOf(*this, fields, 3, factor, out); }

void Frame::coarseSteam(int factor, std::vector<uint16_t>& out) const {
    out.clear();
    if (!steam.empty() && steamFits()) coarseOf(*this, steam, 1, factor, out);
}

const std::vector<uint16_t>& Frame::denseSteam(std::vector<uint16_t>& scratch) const {
    if (steam.empty() || !steamFits()) {
        scratch.clear();
        return scratch;
    }
    if (gasTiles.empty()) return steam;
    const uint16_t none = 0;
    spreadTiles(domain, gasTiles, steam, 1, &none, scratch);
    return scratch;
}

namespace {

/// Blocks of 2 x 2 x 2 cells along a tile's side, and in a tile.
constexpr int kTileBlocks = Tiles::kSide / Frame::kBlock;
constexpr size_t kBlocksInTile = static_cast<size_t>(kTileBlocks) * kTileBlocks * kTileBlocks;

/// f(slot, bi, bj, bk) for each block of the frame's gas -- `slot` its place
/// in Frame::velocity, (bi, bj, bk) where it is among the domain's blocks --
/// in parallel. Not the blocks of a tile past the domain's last: their
/// slots stay as they are.
template <class F>
void forBlocks(const Frame& f, const F& visit) {
    const int bx = f.blocks(0), by = f.blocks(1), bz = f.blocks(2);
    if (f.gasTiles.empty()) {
        pg::parallelFor(static_cast<size_t>(by) * static_cast<size_t>(bz), 16, [&](size_t begin, size_t end) {
            for (size_t row = begin; row < end; ++row) {
                const int bj = static_cast<int>(row % static_cast<size_t>(by)), bk = static_cast<int>(row / static_cast<size_t>(by));
                for (int bi = 0; bi < bx; ++bi) visit(row * static_cast<size_t>(bx) + static_cast<size_t>(bi), bi, bj, bk);
            }
        });
        return;
    }
    size_t t[3];
    tileCounts(f.domain, t);
    pg::parallelFor(f.gasTiles.size(), 8, [&](size_t begin, size_t end) {
        for (size_t s = begin; s < end; ++s) {
            const size_t tile = f.gasTiles[s];
            const int c[3] = {static_cast<int>(tile % t[0]) * kTileBlocks, static_cast<int>((tile / t[0]) % t[1]) * kTileBlocks,
                              static_cast<int>(tile / (t[0] * t[1])) * kTileBlocks};
            for (int z = 0; z < kTileBlocks && c[2] + z < bz; ++z) {
                for (int y = 0; y < kTileBlocks && c[1] + y < by; ++y) {
                    for (int x = 0; x < kTileBlocks && c[0] + x < bx; ++x) {
                        const size_t local = static_cast<size_t>(x + kTileBlocks * (y + kTileBlocks * z));
                        visit(s * kBlocksInTile + local, c[0] + x, c[1] + y, c[2] + z);
                    }
                }
            }
        }
    });
}

size_t blockCount(const Frame& f) {
    if (!f.gasTiles.empty()) return kBlocksInTile * f.gasTiles.size();
    return static_cast<size_t>(f.blocks(0)) * static_cast<size_t>(f.blocks(1)) * static_cast<size_t>(f.blocks(2));
}

}  // namespace

bool Frame::velocityFits() const { return velocity.empty() || velocity.size() == 3 * blockCount(*this); }

const std::vector<uint16_t>& Frame::denseVelocity(std::vector<uint16_t>& scratch) const {
    if (velocity.empty() || !velocityFits() || fields.empty()) {
        scratch.clear();
        return scratch;
    }
    if (gasTiles.empty()) return velocity;
    const size_t bx = static_cast<size_t>(blocks(0)), by = static_cast<size_t>(blocks(1));
    scratch.assign(3 * bx * by * static_cast<size_t>(blocks(2)), 0);
    forBlocks(*this, [&](size_t slot, int bi, int bj, int bk) {
        const size_t to = static_cast<size_t>(bi) + bx * (static_cast<size_t>(bj) + by * static_cast<size_t>(bk));
        for (size_t a = 0; a < 3; ++a) scratch[3 * to + a] = velocity[3 * slot + a];
    });
    return scratch;
}

void setVelocity(Frame& frame, const std::function<Vec3(const Vec3&)>& at) {
    frame.velocity.clear();
    if (frame.fields.empty()) return;
    const Vec3 origin = frame.domain.origin();
    const float edge = frame.domain.voxel * static_cast<float>(Frame::kBlock);
    frame.velocity.assign(3 * blockCount(frame), 0);
    uint16_t* out = frame.velocity.data();
    forBlocks(frame, [&](size_t slot, int bi, int bj, int bk) {
        const Vec3 middle = origin + Vec3(static_cast<float>(bi) + 0.5f, static_cast<float>(bj) + 0.5f,
                                          static_cast<float>(bk) + 0.5f) * edge;
        const Vec3 v = at(middle);
        out[3 * slot] = toHalf(std::isfinite(v.x) ? v.x : 0.0f);
        out[3 * slot + 1] = toHalf(std::isfinite(v.y) ? v.y : 0.0f);
        out[3 * slot + 2] = toHalf(std::isfinite(v.z) ? v.z : 0.0f);
    });
    // Still gas keeps none.
    if (std::all_of(frame.velocity.begin(), frame.velocity.end(), [](uint16_t h) { return (h & 0x7fff) == 0; })) {
        frame.velocity.clear();
    }
}

void addVelocity(Frame& frame, const PyroSolver& solver) {
    setVelocity(frame, [&](const Vec3& p) { return solver.flowAt(p); });
}

void addCoarseSteam(Frame& fine, const PyroSolver& coarse, int scale) {
    if (!coarse.steamy() || scale < 1) return;
    const SparseGrid& steam = coarse.steam();
    const Tiles& tiles = coarse.tiles();
    const Domain& d = fine.domain;
    // The fine tiles under the solver's tiles that hold any steam.
    size_t t[3];
    tileCounts(d, t);
    std::vector<uint32_t> wanted;
    const std::vector<uint32_t>& stored = tiles.stored();
    const int ct[3] = {tiles.tilesX(), tiles.tilesY(), tiles.tilesZ()};
    for (size_t s = 0; s < stored.size(); ++s) {
        const float* v = steam.data() + s * Tiles::kCells;
        if (std::none_of(v, v + Tiles::kCells, [](float x) { return toHalf(std::max(x, 0.0f)) != 0; })) continue;
        const int a = static_cast<int>(stored[s] % static_cast<uint32_t>(ct[0]));
        const int b = static_cast<int>((stored[s] / static_cast<uint32_t>(ct[0])) % static_cast<uint32_t>(ct[1]));
        const int c = static_cast<int>(stored[s] / static_cast<uint32_t>(ct[0] * ct[1]));
        // A cell more round them: what is read between the cells' middles
        // spills over.
        for (int z = std::max(c * scale - 1, 0); z <= std::min((c + 1) * scale, static_cast<int>(t[2]) - 1); ++z) {
            for (int y = std::max(b * scale - 1, 0); y <= std::min((b + 1) * scale, static_cast<int>(t[1]) - 1); ++y) {
                for (int x = std::max(a * scale - 1, 0); x <= std::min((a + 1) * scale, static_cast<int>(t[0]) - 1); ++x) {
                    wanted.push_back(static_cast<uint32_t>(static_cast<size_t>(x) +
                                                           t[0] * (static_cast<size_t>(y) + t[1] * static_cast<size_t>(z))));
                }
            }
        }
    }
    if (wanted.empty()) return;
    std::sort(wanted.begin(), wanted.end());
    wanted.erase(std::unique(wanted.begin(), wanted.end()), wanted.end());
    if (!fine.gasTiles.empty()) {
        // The frame's tiles and those, the gas of the new ones nothing.
        std::vector<uint32_t> all;
        std::set_union(fine.gasTiles.begin(), fine.gasTiles.end(), wanted.begin(), wanted.end(), std::back_inserter(all));
        if (all.size() != fine.gasTiles.size()) {
            std::vector<uint16_t> fields(3 * Tiles::kCells * all.size(), 0);
            size_t from = 0;
            for (size_t to = 0; to < all.size() && from < fine.gasTiles.size(); ++to) {
                if (all[to] != fine.gasTiles[from]) continue;
                std::copy_n(fine.fields.begin() + static_cast<std::ptrdiff_t>(3 * Tiles::kCells * from), 3 * Tiles::kCells,
                            fields.begin() + static_cast<std::ptrdiff_t>(3 * Tiles::kCells * to));
                ++from;
            }
            fine.gasTiles = std::move(all);
            fine.fields = std::move(fields);
        }
    }
    // Each fine cell's middle in the solver's cells.
    const float inv = 1.0f / static_cast<float>(scale);
    auto read = [&](int i, int j, int k) {
        return toHalf(std::max(steam.sample((static_cast<float>(i) + 0.5f) * inv, (static_cast<float>(j) + 0.5f) * inv,
                                            (static_cast<float>(k) + 0.5f) * inv),
                               0.0f));
    };
    if (fine.gasTiles.empty()) {
        fine.steam.assign(d.cellCount(), 0);
        const size_t nx = static_cast<size_t>(d.cells[0]), ny = static_cast<size_t>(d.cells[1]);
        pg::parallelFor(static_cast<size_t>(d.cells[2]), 1, [&](size_t begin, size_t end) {
            for (size_t k = begin; k < end; ++k) {
                for (int j = 0; j < d.cells[1]; ++j) {
                    for (int i = 0; i < d.cells[0]; ++i) {
                        fine.steam[static_cast<size_t>(i) + nx * (static_cast<size_t>(j) + ny * k)] =
                            read(i, j, static_cast<int>(k));
                    }
                }
            }
        });
    } else {
        fine.steam.assign(Tiles::kCells * fine.gasTiles.size(), 0);
        forTileCells(d, fine.gasTiles,
                     [&](size_t s, int i, int j, int k, size_t l) { fine.steam[s * Tiles::kCells + l] = read(i, j, k); });
    }
    if (std::all_of(fine.steam.begin(), fine.steam.end(), [](uint16_t h) { return h == 0; })) fine.steam.clear();
}

void WaterFrame::coarseCells(int factor, std::vector<uint8_t>& out) const {
    const int f = factor;
    const int n[3] = {domain.cells[0] / f, domain.cells[1] / f, domain.cells[2] / f};
    const size_t count = static_cast<size_t>(n[0]) * static_cast<size_t>(n[1]) * static_cast<size_t>(n[2]);
    const uint32_t under = static_cast<uint32_t>(f * f * f);
    // The sums of the cells under each coarse one, distance and foam.
    std::vector<uint32_t> sum(2 * count, 0);
    auto coarse = [&](int i, int j, int k) {
        return static_cast<size_t>(i / f) +
               static_cast<size_t>(n[0]) * (static_cast<size_t>(j / f) + static_cast<size_t>(n[1]) * static_cast<size_t>(k / f));
    };
    if (tiles.empty()) {
        pg::parallelFor(static_cast<size_t>(n[2]), 1, [&](size_t begin, size_t end) {
            for (int k = static_cast<int>(begin) * f; k < static_cast<int>(end) * f; ++k) {
                for (int j = 0; j < domain.cells[1]; ++j) {
                    for (int i = 0; i < domain.cells[0]; ++i) {
                        const size_t c = 2 * (static_cast<size_t>(i) + static_cast<size_t>(domain.cells[0]) *
                                                                            (static_cast<size_t>(j) + static_cast<size_t>(domain.cells[1]) * static_cast<size_t>(k)));
                        const size_t to = coarse(i, j, k);
                        sum[2 * to] += cells[c];
                        sum[2 * to + 1] += cells[c + 1];
                    }
                }
            }
        });
    } else {
        // Far from the water, without foam, but in the tiles kept -- and
        // deep in it in the deep ones; a tile covers whole coarse cells,
        // which no other tile does.
        for (size_t c = 0; c < count; ++c) sum[2 * c] = 255u * under;
        forTileCells(domain, tiles, [&](size_t s, int i, int j, int k, size_t l) {
            const size_t from = 2 * (s * Tiles::kCells + l);
            const size_t to = coarse(i, j, k);
            sum[2 * to] = sum[2 * to] + cells[from] - 255u;
            sum[2 * to + 1] += cells[from + 1];
        });
        forTileCells(domain, deepTiles, [&](size_t, int i, int j, int k, size_t) { sum[2 * coarse(i, j, k)] -= 255u; });
    }
    out.resize(2 * count);
    for (size_t c = 0; c < 2 * count; ++c) out[c] = static_cast<uint8_t>((sum[c] + under / 2) / under);
}

const std::vector<uint8_t>& WaterFrame::denseCells(std::vector<uint8_t>& scratch) const {
    if (tiles.empty()) return cells;
    // Far from the water: as far as the band, without foam; deep in it, as
    // far into it.
    const uint8_t far[2] = {255, 0};
    spreadTiles<uint8_t>(domain, tiles, cells, 2, far, scratch);
    const size_t nx = static_cast<size_t>(domain.cells[0]), ny = static_cast<size_t>(domain.cells[1]);
    forTileCells(domain, deepTiles, [&](size_t, int i, int j, int k, size_t) {
        scratch[2 * (static_cast<size_t>(i) + nx * (static_cast<size_t>(j) + ny * static_cast<size_t>(k)))] = 0;
    });
    return scratch;
}

Domain WaterFrame::flowDomain() const {
    Domain d;
    for (int a = 0; a < 3; ++a) d.cells[a] = domain.cells[a] / 2;
    d.voxel = 2.0f * domain.voxel;
    return d;
}

bool WaterFrame::hasFlow() const {
    if (flow.empty()) return false;
    if (flowTiles.empty()) return flow.size() == 3 * flowDomain().cellCount();
    return flow.size() == 3 * Tiles::kCells * flowTiles.size();
}

const std::vector<uint16_t>& WaterFrame::denseFlow(std::vector<uint16_t>& scratch) const {
    if (flowTiles.empty()) return flow;
    const uint16_t still[3] = {0, 0, 0};
    spreadTiles<uint16_t>(flowDomain(), flowTiles, flow, 3, still, scratch);
    return scratch;
}

Vec3 WaterFrame::flowAt(const Vec3& p) const {
    if (!hasFlow()) return {};
    const Domain d = flowDomain();
    // In cells, from the first cell's centre.
    const Vec3 g = (p - d.origin()) * (1.0f / d.voxel) - Vec3(0.5f, 0.5f, 0.5f);
    int i0[3];
    float t[3];
    const float at[3] = {g.x, g.y, g.z};
    for (int a = 0; a < 3; ++a) {
        const float x = std::clamp(at[a], 0.0f, static_cast<float>(d.cells[a] - 1));
        i0[a] = std::min(static_cast<int>(x), d.cells[a] - 2 < 0 ? 0 : d.cells[a] - 2);
        t[a] = d.cells[a] > 1 ? x - static_cast<float>(i0[a]) : 0.0f;
    }
    Vec3 v;
    for (int c = 0; c < 8; ++c) {
        const int di = c & 1, dj = (c >> 1) & 1, dk = (c >> 2) & 1;
        const int i = std::min(i0[0] + di, d.cells[0] - 1), j = std::min(i0[1] + dj, d.cells[1] - 1),
                  k = std::min(i0[2] + dk, d.cells[2] - 1);
        const float w = (di ? t[0] : 1.0f - t[0]) * (dj ? t[1] : 1.0f - t[1]) * (dk ? t[2] : 1.0f - t[2]);
        if (w == 0.0f) continue;
        size_t cell;
        if (flowTiles.empty()) {
            cell = static_cast<size_t>(i) +
                   static_cast<size_t>(d.cells[0]) * (static_cast<size_t>(j) + static_cast<size_t>(d.cells[1]) * static_cast<size_t>(k));
        } else {
            // A cell of a tile not kept moves with no flow: it adds nothing.
            const int64_t slot = slotIn(flowTiles, tileOfCell(d, i, j, k));
            if (slot < 0) continue;
            cell = static_cast<size_t>(slot) * Tiles::kCells + SparseGrid::local(i, j, k);
        }
        v = v + Vec3(fromHalf(flow[3 * cell]), fromHalf(flow[3 * cell + 1]), fromHalf(flow[3 * cell + 2])) * w;
    }
    return v;
}

WaterFrame capture(const LiquidSolver& sim, bool particles) {
    WaterFrame w;
    if (particles) {
        const size_t n = sim.particleCount();
        w.positions = sim.positions();
        w.ids = sim.ids();
        w.velocities.resize(3 * n);
        w.whiteness.resize(n);
        const auto& v = sim.velocities();
        const auto& white = sim.foam();
        for (size_t i = 0; i < n; ++i) {
            for (int a = 0; a < 3; ++a) w.velocities[3 * i + static_cast<size_t>(a)] = toHalf(v[i][a]);
            w.whiteness[i] = static_cast<uint8_t>(std::lround(std::clamp(white[i], 0.0f, 1.0f) * 255.0f));
        }
    }
    const Domain& d = sim.domain();
    for (int a = 0; a < 3; ++a) w.domain.cells[a] = 2 * d.cells[a];
    w.domain.voxel = 0.5f * d.voxel;
    // Three cells of the fine grid either side of the surface: as far as a
    // ray steps at once, and enough for its normal.
    w.band = 1.5f * d.voxel;
    w.particles = sim.particleCount();
    w.litres = sim.volume();
    SparseGrid distance, foam;
    sim.surfaceField(2, w.band, distance, foam);
    const Tiles& fine = distance.tiles();
    const float* dist = distance.data();
    const float* white = foam.data();
    const float scale = 0.5f / w.band;
    auto distanceByte = [&](float x) {
        return static_cast<uint8_t>(std::lround(std::clamp(x * scale + 0.5f, 0.0f, 1.0f) * 255.0f));
    };
    auto foamByte = [](float x) { return static_cast<uint8_t>(std::lround(std::clamp(x, 0.0f, 1.0f) * 255.0f)); };
    if (fine.all()) {
        // Every cell, x fastest.
        w.cells.resize(2 * w.domain.cellCount());
        uint8_t* out = w.cells.data();
        const size_t nx = static_cast<size_t>(w.domain.cells[0]), ny = static_cast<size_t>(w.domain.cells[1]);
        forEachCounted(fine, [&](int i, int j, int k, size_t c) {
            const size_t cell = static_cast<size_t>(i) + nx * (static_cast<size_t>(j) + ny * static_cast<size_t>(k));
            out[2 * cell] = distanceByte(dist[c]);
            out[2 * cell + 1] = foamByte(white[c]);
        });
    } else {
        // Sparse: the tiles with anything but the far air in them -- those
        // deep in the water apart; one, of far air, when there is none.
        const std::vector<uint32_t>& stored = fine.stored();
        std::vector<uint8_t> any(stored.size(), 0);
        pg::parallelFor(stored.size(), 16, [&](size_t begin, size_t end) {
            for (size_t s = begin; s < end; ++s) {
                const size_t base = s * Tiles::kCells;
                for (size_t c = base; c < base + Tiles::kCells && !any[s]; ++c) {
                    any[s] = distanceByte(dist[c]) != 255 || foamByte(white[c]) != 0;
                }
            }
        });
        // Deep in the water: every cell as far into it as the band, without
        // foam, and every tile round it in the domain and with anything in
        // it -- the surface passes by none: kept without its cells.
        size_t t[3];
        tileCounts(w.domain, t);
        std::vector<uint8_t> deep(stored.size(), 0);
        pg::parallelFor(stored.size(), 16, [&](size_t begin, size_t end) {
            for (size_t s = begin; s < end; ++s) {
                const size_t base = s * Tiles::kCells;
                bool all = any[s] != 0;
                for (size_t c = base; c < base + Tiles::kCells && all; ++c) {
                    all = distanceByte(dist[c]) == 0 && foamByte(white[c]) == 0;
                }
                if (!all) continue;
                const size_t tile = stored[s];
                const int at[3] = {static_cast<int>(tile % t[0]), static_cast<int>((tile / t[0]) % t[1]),
                                   static_cast<int>(tile / (t[0] * t[1]))};
                for (int q = 0; q < 27 && all; ++q) {
                    const int x = at[0] + q % 3 - 1, y = at[1] + q / 3 % 3 - 1, z = at[2] + q / 9 - 1;
                    if (x < 0 || y < 0 || z < 0 || static_cast<size_t>(x) >= t[0] || static_cast<size_t>(y) >= t[1] ||
                        static_cast<size_t>(z) >= t[2]) {
                        all = false;
                        break;
                    }
                    const int32_t n = distance.slotOf(x * Tiles::kSide, y * Tiles::kSide, z * Tiles::kSide);
                    all = n >= 0 && any[static_cast<size_t>(n)];
                }
                deep[s] = all;
            }
        });
        std::vector<size_t> slots;
        for (size_t s = 0; s < stored.size(); ++s) {
            if (!any[s]) continue;
            if (deep[s]) {
                w.deepTiles.push_back(stored[s]);
                continue;
            }
            w.tiles.push_back(stored[s]);
            slots.push_back(s);
        }
        if (w.tiles.empty()) {
            w.tiles.push_back(0);
            w.cells.assign(2 * Tiles::kCells, 0);
            for (size_t c = 0; c < Tiles::kCells; ++c) w.cells[2 * c] = 255;
        } else {
            w.cells.resize(2 * Tiles::kCells * slots.size());
            uint8_t* out = w.cells.data();
            pg::parallelFor(slots.size(), 16, [&](size_t begin, size_t end) {
                for (size_t t = begin; t < end; ++t) {
                    const size_t from = slots[t] * Tiles::kCells, to = t * Tiles::kCells;
                    for (size_t c = 0; c < Tiles::kCells; ++c) {
                        out[2 * (to + c)] = distanceByte(dist[from + c]);
                        out[2 * (to + c) + 1] = foamByte(white[from + c]);
                    }
                }
            });
        }
    }

    // The velocity at the centre of each cell of the solver's -- the mean of
    // its faces' -- in the water and a cell round what is near its surface;
    // 0 further out, where the air has only what gravity gave it. Near: a
    // fine cell of it within the band. All that is in the solver's tiles.
    const int nx = d.cells[0], ny = d.cells[1], nz = d.cells[2];
    const Tiles& tiles = sim.tiles();
    const SparseGrid& at = sim.surface();  // the solver's cells, as they are kept
    std::vector<uint8_t> near(at.size(), 0);
    forEachCounted(tiles, [&](int i, int j, int k, size_t c) {
        bool in = false;
        for (int q = 0; q < 8 && !in; ++q) {
            in = distance.at(2 * i + (q & 1), 2 * j + ((q >> 1) & 1), 2 * k + ((q >> 2) & 1)) < w.band;
        }
        near[c] = in;
    });
    const SparseGrid* v[3] = {&sim.velocity(0), &sim.velocity(1), &sim.velocity(2)};
    std::vector<uint8_t> kept(at.size(), 0);
    std::vector<uint8_t> anyKept(tiles.stored().size(), 0);
    forEachCounted(tiles, [&](int i, int j, int k, size_t c) {
        bool keep = false;
        for (int dk = -1; dk <= 1 && !keep; ++dk) {
            for (int dj = -1; dj <= 1 && !keep; ++dj) {
                for (int di = -1; di <= 1 && !keep; ++di) {
                    const int a = i + di, b = j + dj, e = k + dk;
                    if (a < 0 || b < 0 || e < 0 || a >= nx || b >= ny || e >= nz) continue;
                    const int64_t g = at.find(a, b, e);
                    keep = g >= 0 && near[static_cast<size_t>(g)];
                }
            }
        }
        kept[c] = keep;
        if (keep) anyKept[c / Tiles::kCells] = 1;
    });
    auto flowOf = [&](int i, int j, int k, uint16_t* out) {
        out[0] = toHalf(0.5f * (v[0]->at(i, j, k) + v[0]->at(i + 1, j, k)));
        out[1] = toHalf(0.5f * (v[1]->at(i, j, k) + v[1]->at(i, j + 1, k)));
        out[2] = toHalf(0.5f * (v[2]->at(i, j, k) + v[2]->at(i, j, k + 1)));
    };
    if (tiles.all()) {
        w.flow.assign(3 * d.cellCount(), 0);
        uint16_t* flow = w.flow.data();
        forEachCounted(tiles, [&](int i, int j, int k, size_t c) {
            if (!kept[c]) return;
            flowOf(i, j, k, flow + 3 * (static_cast<size_t>(i) + static_cast<size_t>(nx) * (static_cast<size_t>(j) + static_cast<size_t>(ny) * static_cast<size_t>(k))));
        });
        return w;
    }
    // Sparse: the solver's tiles with any flow kept; one, still, when none has.
    std::vector<int64_t> slotOf(tiles.stored().size(), -1);
    for (size_t s = 0; s < tiles.stored().size(); ++s) {
        if (!anyKept[s]) continue;
        slotOf[s] = static_cast<int64_t>(w.flowTiles.size());
        w.flowTiles.push_back(tiles.stored()[s]);
    }
    if (w.flowTiles.empty()) {
        w.flowTiles.push_back(0);
        w.flow.assign(3 * Tiles::kCells, 0);
        return w;
    }
    w.flow.assign(3 * Tiles::kCells * w.flowTiles.size(), 0);
    uint16_t* flow = w.flow.data();
    forEachCounted(tiles, [&](int i, int j, int k, size_t c) {
        if (!kept[c]) return;
        const int64_t slot = slotOf[c / Tiles::kCells];
        flowOf(i, j, k, flow + 3 * (static_cast<size_t>(slot) * Tiles::kCells + c % Tiles::kCells));
    });
    return w;
}

RainFrame capture(const RainSolver& sim) {
    RainFrame r;
    auto pack = [](const std::vector<RainParticle>& from, std::vector<float>& to, std::vector<uint32_t>& ids) {
        to.resize(6 * from.size());
        ids.resize(from.size());
        for (size_t i = 0; i < from.size(); ++i) {
            const RainParticle& p = from[i];
            float* o = to.data() + 6 * i;
            o[0] = p.position.x, o[1] = p.position.y, o[2] = p.position.z;
            o[3] = p.velocity.x, o[4] = p.velocity.y, o[5] = p.velocity.z;
            ids[i] = p.id;
        }
    };
    pack(sim.drops(), r.drops, r.dropIds);
    pack(sim.droplets(), r.droplets, r.dropletIds);
    r.timeStep = sim.scene().rain.timeStep;
    const Ripples& w = sim.ripples();
    if (!w.empty()) {
        r.rippleOrigin = w.origin;
        r.rippleCell = w.cell;
        r.rippleCells[0] = w.nx;
        r.rippleCells[1] = w.nz;
        r.ripples.resize(w.height.size());
        for (size_t i = 0; i < w.height.size(); ++i) r.ripples[i] = toHalf(w.height[i]);
    }
    return r;
}

std::shared_ptr<Geometry> drawnBodies(const Frame& frame, const Look& look) {
    std::shared_ptr<Geometry> all;
    auto add = [&](std::shared_ptr<Geometry> part) {
        if (!part || part->pointCount() == 0) return;
        if (!all) {
            all = std::move(part);
            return;
        }
        auto both = std::make_shared<Geometry>(*all);
        both->append(*part);
        all = std::move(both);
    };
    if (look.pieces && !frame.rigid.empty()) {
        add(drawnPieces(frame.rigid, look.piecesColor, look.piecesInside, look.insideGroup, look.rebarColor));
    }
    if (look.cloth && !frame.cloth.empty()) add(drawnCloth(frame.cloth, look.clothColor));
    if (look.grains && !frame.grains.empty()) add(grainPoints(frame.grains, look.grainColor));
    return all;
}

std::vector<Volume> gasVolumes(const Frame& frame) {
    std::vector<Volume> volumes;
    const Domain& d = frame.domain;
    const size_t cells = d.cellCount();
    std::vector<uint16_t> scratch;
    const std::vector<uint16_t>& gas = frame.fields.empty() ? frame.fields : frame.denseFields(scratch);
    if (gas.empty() || gas.size() < 3 * cells) return volumes;
    const char* names[3] = {"density", "temperature", "flame"};
    for (int channel = 0; channel < 3; ++channel) {
        std::vector<float> values(cells);
        for (size_t c = 0; c < cells; ++c) values[c] = fromHalf(gas[3 * c + static_cast<size_t>(channel)]);
        volumes.push_back(
            Volume::make(names[channel], d.origin(), d.voxel, d.cells[0], d.cells[1], d.cells[2], std::move(values)));
    }
    std::vector<uint16_t> steamScratch;
    const std::vector<uint16_t>& steam = frame.denseSteam(steamScratch);
    if (steam.size() == cells) {
        std::vector<float> values(cells);
        for (size_t c = 0; c < cells; ++c) values[c] = fromHalf(steam[c]);
        volumes.push_back(Volume::make("steam", d.origin(), d.voxel, d.cells[0], d.cells[1], d.cells[2], std::move(values)));
    }
    // How fast it goes: a voxel a block of 2 x 2 x 2 cells -- written as one
    // vector grid, "vel" (io::formatVdb).
    std::vector<uint16_t> velocityScratch;
    const std::vector<uint16_t>& velocity = frame.denseVelocity(velocityScratch);
    const int bx = frame.blocks(0), by = frame.blocks(1), bz = frame.blocks(2);
    const size_t blocks = static_cast<size_t>(bx) * static_cast<size_t>(by) * static_cast<size_t>(bz);
    if (velocity.size() == 3 * blocks && blocks > 0) {
        const char* names[3] = {"vel.x", "vel.y", "vel.z"};
        for (size_t a = 0; a < 3; ++a) {
            std::vector<float> values(blocks);
            for (size_t b = 0; b < blocks; ++b) values[b] = fromHalf(velocity[3 * b + a]);
            volumes.push_back(Volume::make(names[a], d.origin(), d.voxel * static_cast<float>(Frame::kBlock), bx, by, bz,
                                           std::move(values)));
        }
    }
    return volumes;
}

}  // namespace pg::sim
