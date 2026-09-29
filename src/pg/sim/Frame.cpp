#include "pg/sim/Frame.h"

#include "pg/core/Half.h"
#include "pg/core/Parallel.h"
#include "pg/sim/Liquid.h"
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

float WaterFrame::distance(int i, int j, int k) const {
    const size_t cell = static_cast<size_t>(i) +
                        static_cast<size_t>(domain.cells[0]) *
                            (static_cast<size_t>(j) + static_cast<size_t>(domain.cells[1]) * static_cast<size_t>(k));
    return (static_cast<float>(cells[2 * cell]) / 255.0f * 2.0f - 1.0f) * band;
}

float WaterFrame::foam(int i, int j, int k) const {
    const size_t cell = static_cast<size_t>(i) +
                        static_cast<size_t>(domain.cells[0]) *
                            (static_cast<size_t>(j) + static_cast<size_t>(domain.cells[1]) * static_cast<size_t>(k));
    return static_cast<float>(cells[2 * cell + 1]) / 255.0f;
}

Domain WaterFrame::flowDomain() const {
    Domain d;
    for (int a = 0; a < 3; ++a) d.cells[a] = domain.cells[a] / 2;
    d.voxel = 2.0f * domain.voxel;
    return d;
}

Vec3 WaterFrame::flowAt(const Vec3& p) const {
    const Domain d = flowDomain();
    if (flow.size() != 3 * d.cellCount() || flow.empty()) return {};
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
        const size_t cell = static_cast<size_t>(i) +
                            static_cast<size_t>(d.cells[0]) * (static_cast<size_t>(j) + static_cast<size_t>(d.cells[1]) * static_cast<size_t>(k));
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
    Grid distance, foam;
    sim.surfaceField(2, w.band, distance, foam);
    w.cells.resize(2 * distance.size());
    const float* dist = distance.data();
    const float* white = foam.data();
    uint8_t* out = w.cells.data();
    const float scale = 0.5f / w.band;
    pg::parallelFor(distance.size(), 16384, [&](size_t begin, size_t end) {
        for (size_t c = begin; c < end; ++c) {
            const float x = std::clamp(dist[c] * scale + 0.5f, 0.0f, 1.0f);
            out[2 * c] = static_cast<uint8_t>(std::lround(x * 255.0f));
            out[2 * c + 1] = static_cast<uint8_t>(std::lround(std::clamp(white[c], 0.0f, 1.0f) * 255.0f));
        }
    });

    // The velocity at the centre of each cell of the solver's -- the mean of
    // its faces' -- in the water and a cell round what is near its surface;
    // 0 further out, where the air has only what gravity gave it.
    const int nx = d.cells[0], ny = d.cells[1], nz = d.cells[2];
    auto cellOf = [&](int i, int j, int k) {
        return static_cast<size_t>(i) + static_cast<size_t>(nx) * (static_cast<size_t>(j) + static_cast<size_t>(ny) * static_cast<size_t>(k));
    };
    std::vector<uint8_t> near(d.cellCount(), 0);
    pg::parallelFor(static_cast<size_t>(nz), 1, [&](size_t begin, size_t end) {
        for (int k = static_cast<int>(begin); k < static_cast<int>(end); ++k) {
            for (int j = 0; j < ny; ++j) {
                for (int i = 0; i < nx; ++i) {
                    bool in = false;
                    for (int q = 0; q < 8 && !in; ++q) {
                        in = distance.at(2 * i + (q & 1), 2 * j + ((q >> 1) & 1), 2 * k + ((q >> 2) & 1)) < w.band;
                    }
                    near[cellOf(i, j, k)] = in;
                }
            }
        }
    });
    const Grid* v[3] = {&sim.velocity(0), &sim.velocity(1), &sim.velocity(2)};
    w.flow.assign(3 * d.cellCount(), 0);
    uint16_t* flow = w.flow.data();
    pg::parallelFor(static_cast<size_t>(nz), 1, [&](size_t begin, size_t end) {
        for (int k = static_cast<int>(begin); k < static_cast<int>(end); ++k) {
            for (int j = 0; j < ny; ++j) {
                for (int i = 0; i < nx; ++i) {
                    bool kept = false;
                    for (int dk = -1; dk <= 1 && !kept; ++dk) {
                        for (int dj = -1; dj <= 1 && !kept; ++dj) {
                            for (int di = -1; di <= 1 && !kept; ++di) {
                                const int a = i + di, b = j + dj, c = k + dk;
                                kept = a >= 0 && b >= 0 && c >= 0 && a < nx && b < ny && c < nz && near[cellOf(a, b, c)];
                            }
                        }
                    }
                    if (!kept) continue;
                    const size_t c = cellOf(i, j, k);
                    flow[3 * c] = toHalf(0.5f * (v[0]->at(i, j, k) + v[0]->at(i + 1, j, k)));
                    flow[3 * c + 1] = toHalf(0.5f * (v[1]->at(i, j, k) + v[1]->at(i, j + 1, k)));
                    flow[3 * c + 2] = toHalf(0.5f * (v[2]->at(i, j, k) + v[2]->at(i, j, k + 1)));
                }
            }
        }
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

}  // namespace pg::sim
