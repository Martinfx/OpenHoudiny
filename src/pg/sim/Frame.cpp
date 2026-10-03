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
    if (gasTiles.empty()) {
        const size_t cell = static_cast<size_t>(i) +
                            static_cast<size_t>(domain.cells[0]) *
                                (static_cast<size_t>(j) + static_cast<size_t>(domain.cells[1]) * static_cast<size_t>(k));
        return fromHalf(fields[3 * cell + static_cast<size_t>(channel)]);
    }
    size_t t[3];
    tileCounts(domain, t);
    const uint32_t tile = static_cast<uint32_t>(static_cast<size_t>(i >> Tiles::kLog) +
                                                t[0] * (static_cast<size_t>(j >> Tiles::kLog) + t[1] * static_cast<size_t>(k >> Tiles::kLog)));
    const auto found = std::lower_bound(gasTiles.begin(), gasTiles.end(), tile);
    if (found == gasTiles.end() || *found != tile) return 0.0f;
    const size_t slot = static_cast<size_t>(found - gasTiles.begin());
    return fromHalf(fields[3 * (slot * Tiles::kCells + SparseGrid::local(i, j, k)) + static_cast<size_t>(channel)]);
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
    Frame f;
    f.number = sim.frame();
    f.time = sim.time();
    f.domain = sim.domain();
    const float* smoke = sim.density().data();
    const float* heat = sim.temperature().data();
    const float* flame = sim.flame().data();
    const Tiles& tiles = sim.tiles();
    if (tiles.all()) {
        // Every cell, x fastest.
        f.fields.assign(3 * f.domain.cellCount(), 0);
        uint16_t* out = f.fields.data();
        const size_t nx = static_cast<size_t>(f.domain.cells[0]), ny = static_cast<size_t>(f.domain.cells[1]);
        forEachCounted(tiles, [&](int i, int j, int k, size_t c) {
            const size_t cell = static_cast<size_t>(i) + nx * (static_cast<size_t>(j) + ny * static_cast<size_t>(k));
            out[3 * cell] = toHalf(smoke[c]);
            out[3 * cell + 1] = toHalf(heat[c]);
            out[3 * cell + 2] = toHalf(flame[c]);
        });
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
                any[s] = toHalf(smoke[c]) != 0 || toHalf(heat[c]) != 0 || toHalf(flame[c]) != 0;
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
    uint16_t* out = f.fields.data();
    pg::parallelFor(slots.size(), 16, [&](size_t begin, size_t end) {
        for (size_t t = begin; t < end; ++t) {
            const size_t from = slots[t] * Tiles::kCells, to = t * Tiles::kCells;
            for (size_t c = 0; c < Tiles::kCells; ++c) {
                out[3 * (to + c)] = toHalf(smoke[from + c]);
                out[3 * (to + c) + 1] = toHalf(heat[from + c]);
                out[3 * (to + c) + 2] = toHalf(flame[from + c]);
            }
        }
    });
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

/// `values` of `width` a cell, kept in `tiles` of `d`, into every cell of
/// it, x fastest; `background` (`width` of them) where no tile is kept.
template <class T>
void spreadTiles(const Domain& d, const std::vector<uint32_t>& tiles, const std::vector<T>& values, size_t width,
                 const T* background, std::vector<T>& out) {
    out.resize(width * d.cellCount());
    pg::parallelFor(d.cellCount(), 65536, [&](size_t begin, size_t end) {
        for (size_t c = begin; c < end; ++c) std::copy(background, background + width, out.begin() + static_cast<std::ptrdiff_t>(width * c));
    });
    size_t t[3];
    tileCounts(d, t);
    const size_t nx = static_cast<size_t>(d.cells[0]), ny = static_cast<size_t>(d.cells[1]);
    pg::parallelFor(tiles.size(), 16, [&](size_t begin, size_t end) {
        for (size_t s = begin; s < end; ++s) {
            const size_t tile = tiles[s];
            const int c[3] = {static_cast<int>(tile % t[0]) * Tiles::kSide, static_cast<int>((tile / t[0]) % t[1]) * Tiles::kSide,
                              static_cast<int>(tile / (t[0] * t[1])) * Tiles::kSide};
            for (int z = 0; z < Tiles::kSide && c[2] + z < d.cells[2]; ++z) {
                for (int y = 0; y < Tiles::kSide && c[1] + y < d.cells[1]; ++y) {
                    for (int x = 0; x < Tiles::kSide && c[0] + x < d.cells[0]; ++x) {
                        const size_t from = width * (s * Tiles::kCells + SparseGrid::local(x, y, z));
                        const size_t cell = static_cast<size_t>(c[0] + x) + nx * (static_cast<size_t>(c[1] + y) + ny * static_cast<size_t>(c[2] + z));
                        for (size_t w = 0; w < width; ++w) out[width * cell + w] = values[from + w];
                    }
                }
            }
        }
    });
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
    if (c < 0) return band;
    return (static_cast<float>(cells[static_cast<size_t>(c)]) / 255.0f * 2.0f - 1.0f) * band;
}

float WaterFrame::foam(int i, int j, int k) const {
    const int64_t c = cellOf(i, j, k);
    return c < 0 ? 0.0f : static_cast<float>(cells[static_cast<size_t>(c) + 1]) / 255.0f;
}

bool WaterFrame::fits() const {
    if (tiles.empty()) return cells.size() == 2 * domain.cellCount();
    size_t t[3];
    tileCounts(domain, t);
    for (size_t s = 0; s < tiles.size(); ++s) {
        if (tiles[s] >= t[0] * t[1] * t[2] || (s > 0 && tiles[s - 1] >= tiles[s])) return false;
    }
    return cells.size() == 2 * Tiles::kCells * tiles.size();
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
        // Far from the water, without foam, but in the tiles kept; a tile
        // covers whole coarse cells, which no other tile does.
        for (size_t c = 0; c < count; ++c) sum[2 * c] = 255u * under;
        size_t t[3];
        tileCounts(domain, t);
        pg::parallelFor(tiles.size(), 16, [&](size_t begin, size_t end) {
            for (size_t s = begin; s < end; ++s) {
                const size_t tile = tiles[s];
                const int c[3] = {static_cast<int>(tile % t[0]) * Tiles::kSide, static_cast<int>((tile / t[0]) % t[1]) * Tiles::kSide,
                                  static_cast<int>(tile / (t[0] * t[1])) * Tiles::kSide};
                for (int z = 0; z < Tiles::kSide && c[2] + z < domain.cells[2]; ++z) {
                    for (int y = 0; y < Tiles::kSide && c[1] + y < domain.cells[1]; ++y) {
                        for (int x = 0; x < Tiles::kSide && c[0] + x < domain.cells[0]; ++x) {
                            const size_t from = 2 * (s * Tiles::kCells + SparseGrid::local(x, y, z));
                            const size_t to = coarse(c[0] + x, c[1] + y, c[2] + z);
                            sum[2 * to] = sum[2 * to] + cells[from] - 255u;
                            sum[2 * to + 1] += cells[from + 1];
                        }
                    }
                }
            }
        });
    }
    out.resize(2 * count);
    for (size_t c = 0; c < 2 * count; ++c) out[c] = static_cast<uint8_t>((sum[c] + under / 2) / under);
}

const std::vector<uint8_t>& WaterFrame::denseCells(std::vector<uint8_t>& scratch) const {
    if (tiles.empty()) return cells;
    // Far from the water: as far as the band, without foam.
    const uint8_t far[2] = {255, 0};
    spreadTiles<uint8_t>(domain, tiles, cells, 2, far, scratch);
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
        // Sparse: the tiles with anything but the far air in them; one, of
        // far air, when there is none.
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
        std::vector<size_t> slots;
        for (size_t s = 0; s < stored.size(); ++s) {
            if (!any[s]) continue;
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
    std::shared_ptr<Geometry> pieces, cloth;
    if (look.pieces && !frame.rigid.empty()) {
        pieces = drawnPieces(frame.rigid, look.piecesColor, look.piecesInside, look.insideGroup, look.rebarColor);
    }
    if (look.cloth && !frame.cloth.empty()) cloth = drawnCloth(frame.cloth, look.clothColor);
    if (!pieces) return cloth;
    if (cloth) {
        auto both = std::make_shared<Geometry>(*pieces);
        both->append(*cloth);
        return both;
    }
    return pieces;
}

}  // namespace pg::sim
