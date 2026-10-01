#include "pg/render/Gas.h"

#include "pg/core/Parallel.h"
#include "pg/render/Random.h"
#include "pg/sim/Frame.h"
#include "pg/sim/Look.h"
#include "pg/sim/SparseGrid.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <mutex>
#include <string>
#include <vector>

#ifdef PG_HAVE_NANOVDB
#include <nanovdb/NanoVDB.h>
#include <nanovdb/tools/CreateNanoGrid.h>
#include <nanovdb/tools/GridBuilder.h>
#endif

namespace pg::render {

GasLook GasLook::of(const sim::Look& look) {
    GasLook g;
    g.density = look.smokeDensity;
    g.color = look.smokeColor;
    g.albedo = albedoOf(look.smokeColor);
    g.flame = look.flameIntensity;
    g.flameStart = look.flameStart;
    g.flameRange = look.flameRange;
    return g;
}

Vec3 GasLook::albedoOf(const Vec3& color) {
    Vec3 out;
    for (int c = 0; c < 3; ++c) {
        const float a = std::clamp(color[c], 0.0f, 1.0f);
        const float k = 4.09712f + 4.20863f * a - std::sqrt(9.59217f + 41.6808f * a + 17.7126f * a * a);
        out[c] = std::clamp(1.0f - k * k, 0.0f, 1.0f);
    }
    return out;
}

bool gasAvailable() {
#ifdef PG_HAVE_NANOVDB
    return true;
#else
    return false;
#endif
}

std::string gasLibrary() {
#ifdef PG_HAVE_NANOVDB
    return "NanoVDB " + std::to_string(NANOVDB_MAJOR_VERSION_NUMBER) + "." + std::to_string(NANOVDB_MINOR_VERSION_NUMBER) +
           "." + std::to_string(NANOVDB_PATCH_VERSION_NUMBER);
#else
    return {};
#endif
}

namespace {

float smoothstep01(float x) {
    const float t = std::clamp(x, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

/// The colour of a black body at `kelvin`, normalised: Tanner Helland's fit
/// to the CIE data, in sRGB, made linear -- the viewport's (blackbody()).
Vec3 blackbody(float kelvin) {
    const float t = kelvin / 100.0f;
    Vec3 c;
    c.x = t <= 66.0f ? 1.0f : 1.29293618606f * std::pow(t - 60.0f, -0.1332047592f);
    c.y = t <= 66.0f ? 0.39008157876f * std::log(t) - 0.63184144378f
                     : 1.12989086089f * std::pow(t - 60.0f, -0.0755148492f);
    c.z = t >= 66.0f ? 1.0f : (t <= 19.0f ? 0.0f : 0.54320678911f * std::log(t - 10.0f) - 1.19625408914f);
    return {std::pow(std::clamp(c.x, 0.0f, 1.0f), 2.2f), std::pow(std::clamp(c.y, 0.0f, 1.0f), 2.2f),
            std::pow(std::clamp(c.z, 0.0f, 1.0f), 2.2f)};
}

/// The light of a flame `x` of the way from where it starts to glow to its
/// hottest, relative to the hottest: a black body from 1000 K to 3000 K, its
/// power going with T^4 -- the viewport's glowAt(), in a table.
constexpr int kGlowSteps = 256;
const std::array<Vec3, kGlowSteps + 1>& glowTable() {
    static const std::array<Vec3, kGlowSteps + 1> table = [] {
        std::array<Vec3, kGlowSteps + 1> t;
        for (int i = 0; i <= kGlowSteps; ++i) {
            const float x = static_cast<float>(i) / kGlowSteps;
            const float kelvin = 1000.0f + 2000.0f * x;
            const float k = kelvin / 3000.0f;
            t[static_cast<size_t>(i)] = blackbody(kelvin) * (k * k * k * k * smoothstep01(x / 0.1f));
        }
        return t;
    }();
    return table;
}

Vec3 glow(float x) {
    const auto& table = glowTable();
    const float at = std::clamp(x, 0.0f, 1.0f) * kGlowSteps;
    const int i = std::min(static_cast<int>(at), kGlowSteps - 1);
    const float w = at - static_cast<float>(i);
    return table[static_cast<size_t>(i)] * (1.0f - w) + table[static_cast<size_t>(i) + 1] * w;
}

}  // namespace

struct Gas::Grid {
#ifdef PG_HAVE_NANOVDB
    nanovdb::GridHandle<nanovdb::HostBuffer> handle;
    const nanovdb::Vec3fGrid* grid = nullptr;
#endif
    Vec3 origin;                     // the corner of the simulation's box
    float voxel = 1.0f, inverse = 1.0f;  // world units a cell, cells a world unit
    int cells[3] = {0, 0, 0}, tiles[3] = {0, 0, 0};
    /// Of each tile, x fastest: the most smoke, temperature and flame a
    /// point in it reads -- its cells and the layer round them.
    std::vector<Vec3> most;
    Box box;
    size_t active = 0;

    size_t tileOf(int a, int b, int c) const {
        return static_cast<size_t>(a) +
               static_cast<size_t>(tiles[0]) * (static_cast<size_t>(b) + static_cast<size_t>(tiles[1]) * static_cast<size_t>(c));
    }
};

#ifdef PG_HAVE_NANOVDB
namespace {

using Accessor = nanovdb::Vec3fGrid::AccessorType;

/// Where a ray from `o` along `d` is in `box` between `tMin` and `tMax`.
bool clip(const Box& box, const Vec3& o, const Vec3& d, float tMin, float tMax, float& t0, float& t1) {
    t0 = tMin;
    t1 = tMax;
    for (int a = 0; a < 3; ++a) {
        if (d[a] == 0.0f) {
            if (o[a] < box.lo[a] || o[a] > box.hi[a]) return false;
            continue;
        }
        const float inv = 1.0f / d[a];
        float n = (box.lo[a] - o[a]) * inv, f = (box.hi[a] - o[a]) * inv;
        if (n > f) std::swap(n, f);
        t0 = std::max(t0, n);
        t1 = std::min(t1, f);
    }
    return t0 < t1;
}

/// The fields at world point p, trilinear between the cells' middles: from
/// one leaf at once where the eight cells are in it, as most are.
Vec3 sample(const Gas::Grid& g, Accessor& acc, const Vec3& p) {
    const float x = (p.x - g.origin.x) * g.inverse - 0.5f;
    const float y = (p.y - g.origin.y) * g.inverse - 0.5f;
    const float z = (p.z - g.origin.z) * g.inverse - 0.5f;
    const float fx = std::floor(x), fy = std::floor(y), fz = std::floor(z);
    const int i = static_cast<int>(fx), j = static_cast<int>(fy), k = static_cast<int>(fz);
    const float u = x - fx, v = y - fy, w = z - fz;
    nanovdb::Vec3f c[8];
    const auto* leaf = acc.probeLeaf(nanovdb::Coord(i, j, k));
    if (leaf && (i & 7) != 7 && (j & 7) != 7 && (k & 7) != 7) {
        // A leaf's values: z fastest, then y, then x.
        const uint32_t o = (static_cast<uint32_t>(i & 7) << 6) | (static_cast<uint32_t>(j & 7) << 3) | static_cast<uint32_t>(k & 7);
        c[0] = leaf->getValue(o);
        c[1] = leaf->getValue(o + 64);
        c[2] = leaf->getValue(o + 8);
        c[3] = leaf->getValue(o + 72);
        c[4] = leaf->getValue(o + 1);
        c[5] = leaf->getValue(o + 65);
        c[6] = leaf->getValue(o + 9);
        c[7] = leaf->getValue(o + 73);
    } else {
        c[0] = acc.getValue(nanovdb::Coord(i, j, k));
        c[1] = acc.getValue(nanovdb::Coord(i + 1, j, k));
        c[2] = acc.getValue(nanovdb::Coord(i, j + 1, k));
        c[3] = acc.getValue(nanovdb::Coord(i + 1, j + 1, k));
        c[4] = acc.getValue(nanovdb::Coord(i, j, k + 1));
        c[5] = acc.getValue(nanovdb::Coord(i + 1, j, k + 1));
        c[6] = acc.getValue(nanovdb::Coord(i, j + 1, k + 1));
        c[7] = acc.getValue(nanovdb::Coord(i + 1, j + 1, k + 1));
    }
    Vec3 out;
    for (int a = 0; a < 3; ++a) {
        const float x0 = c[0][a] + u * (c[1][a] - c[0][a]), x1 = c[2][a] + u * (c[3][a] - c[2][a]);
        const float x2 = c[4][a] + u * (c[5][a] - c[4][a]), x3 = c[6][a] + u * (c[7][a] - c[6][a]);
        const float y0 = x0 + v * (x1 - x0), y1 = x2 + v * (x3 - x2);
        out[a] = y0 + w * (y1 - y0);
    }
    return out;
}

/// The tiles a ray crosses between t0 and t1 -- where it is in the box --
/// in turn (Amanatides and Woo): visit(tile, from, to); false stops it.
template <typename Visit>
void walk(const Gas::Grid& g, const Vec3& origin, const Vec3& dir, float t0, float t1, Visit&& visit) {
    const float scale = g.inverse / static_cast<float>(sim::Tiles::kSide);  // tiles a world unit
    const Vec3 start = (origin + dir * t0 - g.origin) * scale;
    int cell[3], step[3];
    float next[3], delta[3];
    for (int a = 0; a < 3; ++a) {
        cell[a] = std::clamp(static_cast<int>(std::floor(start[a])), 0, g.tiles[a] - 1);
        const float d = dir[a] * scale;
        if (d > 0.0f) {
            step[a] = 1;
            next[a] = t0 + (static_cast<float>(cell[a] + 1) - start[a]) / d;
            delta[a] = 1.0f / d;
        } else if (d < 0.0f) {
            step[a] = -1;
            next[a] = t0 + (static_cast<float>(cell[a]) - start[a]) / d;
            delta[a] = -1.0f / d;
        } else {
            step[a] = 0;
            next[a] = std::numeric_limits<float>::infinity();
            delta[a] = std::numeric_limits<float>::infinity();
        }
    }
    float t = t0;
    while (t < t1) {
        const int a = next[0] < next[1] ? (next[0] < next[2] ? 0 : 2) : (next[1] < next[2] ? 1 : 2);
        const float end = std::min(next[a], t1);
        if (end > t && !visit(g.tileOf(cell[0], cell[1], cell[2]), t, end)) return;
        t = std::max(t, end);
        cell[a] += step[a];
        if (cell[a] < 0 || cell[a] >= g.tiles[a]) return;
        next[a] += delta[a];
    }
}

}  // namespace
#endif

Gas::Gas() : grid_(std::make_unique<Grid>()) {}
Gas::~Gas() = default;

const Box& Gas::bounds() const { return grid_->box; }
size_t Gas::cells() const { return grid_->active; }

size_t Gas::bytes() const {
#ifdef PG_HAVE_NANOVDB
    return grid_->handle.bufferSize() + grid_->most.size() * sizeof(Vec3);
#else
    return 0;
#endif
}

float Gas::extinction(const Vec3& f, const GasLook& look) {
    return look.density * std::max(f.x, 0.0f) / (1.0f + 4.0f * std::max(f.z, 0.0f));
}

Vec3 Gas::emission(const Vec3& f, const GasLook& look) {
    if (f.z <= 0.0f || look.flame <= 0.0f) return {};
    const float x = (f.y - look.flameStart) / std::max(look.flameRange, 1e-6f);
    if (x <= 0.0f) return {};
    return glow(x) * (look.flame * (1.0f - std::exp(-4.0f * f.z)));
}

std::shared_ptr<const Gas> Gas::build(const sim::Frame& frame) {
#ifdef PG_HAVE_NANOVDB
    const sim::Domain& d = frame.domain;
    const int nx = d.cells[0], ny = d.cells[1], nz = d.cells[2];
    if (frame.fields.empty() || nx <= 0 || ny <= 0 || nz <= 0 || !(d.voxel > 0.0f)) return nullptr;
    const bool sparse = !frame.gasTiles.empty();
    if (sparse ? frame.fields.size() != 3 * frame.gasTiles.size() * sim::Tiles::kCells
               : frame.fields.size() != 3 * d.cellCount()) {
        return nullptr;
    }
    std::shared_ptr<Gas> gas(new Gas());
    Grid& g = *gas->grid_;
    g.origin = d.origin();
    g.voxel = d.voxel;
    g.inverse = 1.0f / d.voxel;
    g.cells[0] = nx;
    g.cells[1] = ny;
    g.cells[2] = nz;
    for (int a = 0; a < 3; ++a) g.tiles[a] = (g.cells[a] + sim::Tiles::kSide - 1) / sim::Tiles::kSide;
    g.box.lo = g.origin;
    g.box.hi = g.origin + d.size();
    const size_t tileCount = static_cast<size_t>(g.tiles[0]) * static_cast<size_t>(g.tiles[1]) * static_cast<size_t>(g.tiles[2]);

    // The tiles of 8 x 8 x 8 cells: those the frame keeps, or all of them.
    std::vector<uint32_t> tiles = frame.gasTiles;
    if (!sparse) {
        tiles.resize(tileCount);
        for (size_t t = 0; t < tileCount; ++t) tiles[t] = static_cast<uint32_t>(t);
    }
    // Faded out over the last cells before the open sides and the top, as
    // the viewport fades it (fadeAt): it goes on past them, and a hard cut
    // would look like a wall.
    auto fade = [&](int i, int j, int k) {
        const float fi = static_cast<float>(i) + 0.5f, fj = static_cast<float>(j) + 0.5f, fk = static_cast<float>(k) + 0.5f;
        const float side = std::min(std::min(fi, static_cast<float>(nx) - fi), std::min(fk, static_cast<float>(nz) - fk)) / 6.0f;
        const float top = (static_cast<float>(ny) - fj) / 10.0f;
        return smoothstep01(std::min(side, top));
    };

    // Each tile with smoke or flame a leaf of the grid: made side by side,
    // put in the tree -- ordered by where they are -- one by one.
    using Build = nanovdb::tools::build::Grid<nanovdb::Vec3f>;
    Build build(nanovdb::Vec3f(0.0f), "gas");
    auto& root = build.tree().root();
    std::vector<uint8_t> filled(tileCount, 0);
    std::atomic<size_t> active{0};
    std::mutex mutex;
    parallelFor(tiles.size(), 4, [&](size_t begin, size_t end) {
        for (size_t s = begin; s < end; ++s) {
            const size_t t = tiles[s];
            if (t >= tileCount) continue;
            const int ci = static_cast<int>(t % static_cast<size_t>(g.tiles[0])) * sim::Tiles::kSide;
            const int cj = static_cast<int>((t / static_cast<size_t>(g.tiles[0])) % static_cast<size_t>(g.tiles[1])) * sim::Tiles::kSide;
            const int ck = static_cast<int>(t / (static_cast<size_t>(g.tiles[0]) * static_cast<size_t>(g.tiles[1]))) * sim::Tiles::kSide;
            auto* leaf = new Build::Node0(nanovdb::Coord(ci, cj, ck), root.mBackground, false);
            size_t cells = 0;
            bool seen = false;  // smoke or flame: heat alone shows nothing
            for (int z = 0; z < sim::Tiles::kSide && ck + z < nz; ++z) {
                for (int y = 0; y < sim::Tiles::kSide && cj + y < ny; ++y) {
                    for (int x = 0; x < sim::Tiles::kSide && ci + x < nx; ++x) {
                        const int i = ci + x, j = cj + y, k = ck + z;
                        const size_t at = sparse ? 3 * (s * sim::Tiles::kCells + sim::SparseGrid::local(x, y, z))
                                                 : 3 * (static_cast<size_t>(i) + static_cast<size_t>(nx) * (static_cast<size_t>(j) + static_cast<size_t>(ny) * static_cast<size_t>(k)));
                        float smoke = sim::fromHalf(frame.fields[at]), heat = sim::fromHalf(frame.fields[at + 1]),
                              flame = sim::fromHalf(frame.fields[at + 2]);
                        smoke = std::isfinite(smoke) ? std::max(smoke, 0.0f) : 0.0f;
                        heat = std::isfinite(heat) ? heat : 0.0f;
                        flame = std::isfinite(flame) ? std::max(flame, 0.0f) : 0.0f;
                        if (smoke > 0.0f || flame > 0.0f) {
                            const float f = fade(i, j, k);
                            smoke *= f;
                            flame *= f;
                        }
                        if (smoke == 0.0f && heat == 0.0f && flame == 0.0f) continue;
                        seen = seen || smoke > 0.0f || flame > 0.0f;
                        leaf->setValue(nanovdb::Coord(i, j, k), nanovdb::Vec3f(smoke, heat, flame));
                        ++cells;
                    }
                }
            }
            if (!seen) {
                delete leaf;
                continue;
            }
            filled[t] = 1;
            active += cells;
            std::lock_guard<std::mutex> lock(mutex);
            root.addNode(leaf);
        }
    });
    if (active == 0) return nullptr;
    g.active = active;
    g.handle = nanovdb::tools::createNanoGrid<Build, nanovdb::Vec3f>(build, nanovdb::tools::StatsMode::BBox,
                                                                       nanovdb::CheckMode::Disable);
    g.grid = g.handle.grid<nanovdb::Vec3f>();
    if (!g.grid) return nullptr;

    // The most each tile reads: of its cells and the layer round them -- a
    // point in it reads the eight cells round it. Tiles next to none of the
    // gas read nothing.
    std::vector<uint32_t> near;
    {
        std::vector<uint8_t> mark(tileCount, 0);
        for (size_t t = 0; t < tileCount; ++t) {
            if (!filled[t]) continue;
            const int a = static_cast<int>(t % static_cast<size_t>(g.tiles[0]));
            const int b = static_cast<int>((t / static_cast<size_t>(g.tiles[0])) % static_cast<size_t>(g.tiles[1]));
            const int c = static_cast<int>(t / (static_cast<size_t>(g.tiles[0]) * static_cast<size_t>(g.tiles[1])));
            for (int dc = -1; dc <= 1; ++dc) {
                for (int db = -1; db <= 1; ++db) {
                    for (int da = -1; da <= 1; ++da) {
                        const int x = a + da, y = b + db, z = c + dc;
                        if (x < 0 || y < 0 || z < 0 || x >= g.tiles[0] || y >= g.tiles[1] || z >= g.tiles[2]) continue;
                        mark[g.tileOf(x, y, z)] = 1;
                    }
                }
            }
        }
        for (size_t t = 0; t < tileCount; ++t) {
            if (mark[t]) near.push_back(static_cast<uint32_t>(t));
        }
    }
    g.most.assign(tileCount, Vec3(0.0f, 0.0f, 0.0f));
    parallelFor(near.size(), 16, [&](size_t begin, size_t end) {
        Accessor acc = g.grid->getAccessor();
        for (size_t s = begin; s < end; ++s) {
            const size_t t = near[s];
            const int ci = static_cast<int>(t % static_cast<size_t>(g.tiles[0])) * sim::Tiles::kSide;
            const int cj = static_cast<int>((t / static_cast<size_t>(g.tiles[0])) % static_cast<size_t>(g.tiles[1])) * sim::Tiles::kSide;
            const int ck = static_cast<int>(t / (static_cast<size_t>(g.tiles[0]) * static_cast<size_t>(g.tiles[1]))) * sim::Tiles::kSide;
            Vec3 m(0.0f, 0.0f, 0.0f);
            for (int k = ck - 1; k <= ck + sim::Tiles::kSide; ++k) {
                for (int j = cj - 1; j <= cj + sim::Tiles::kSide; ++j) {
                    for (int i = ci - 1; i <= ci + sim::Tiles::kSide; ++i) {
                        const nanovdb::Vec3f v = acc.getValue(nanovdb::Coord(i, j, k));
                        m = Vec3(std::max(m.x, v[0]), std::max(m.y, v[1]), std::max(m.z, v[2]));
                    }
                }
            }
            g.most[t] = m;
        }
    });
    return gas;
#else
    (void)frame;
    return nullptr;
#endif
}

Vec3 Gas::at(const Vec3& p) const {
#ifdef PG_HAVE_NANOVDB
    // Beyond a cell outside the box, nothing: the cells' numbers stay small.
    const Box& b = grid_->box;
    const float v = grid_->voxel;
    if (!(p.x > b.lo.x - v && p.y > b.lo.y - v && p.z > b.lo.z - v && p.x < b.hi.x + v && p.y < b.hi.y + v &&
          p.z < b.hi.z + v)) {
        return {};
    }
    Accessor acc = grid_->grid->getAccessor();
    return sample(*grid_, acc, p);
#else
    (void)p;
    return {};
#endif
}

bool Gas::track(const Vec3& origin, const Vec3& dir, float tMin, float tMax, const GasLook& look, Rng& rng, float& t,
                Vec3& emitted) const {
    emitted = Vec3(0.0f, 0.0f, 0.0f);
#ifdef PG_HAVE_NANOVDB
    const Grid& g = *grid_;
    float t0 = 0.0f, t1 = 0.0f;
    if (!clip(g.box, origin, dir, tMin, tMax, t0, t1)) return false;
    Accessor acc = g.grid->getAccessor();
    bool scattered = false;
    walk(g, origin, dir, t0, t1, [&](size_t tile, float from, float to) {
        const Vec3& m = g.most[tile];
        // Through fire, a step a cell at least: each adds the light it gives off.
        const bool fire = look.flame > 0.0f && m.z > 0.0f && m.y > look.flameStart;
        const float bound = std::max(look.density * m.x, fire ? g.inverse : 0.0f);
        if (!(bound > 0.0f)) return true;
        float s = from;
        for (;;) {
            s -= std::log(1.0f - rng.next()) / bound;
            if (s >= to) return true;
            const Vec3 f = sample(g, acc, origin + dir * s);
            if (fire) emitted = emitted + emission(f, look) * (1.0f / bound);
            // Scattered here as likely as the smoke is dense, to the most it could be.
            if (rng.next() * bound < extinction(f, look)) {
                t = s;
                scattered = true;
                return false;
            }
        }
    });
    return scattered;
#else
    (void)origin, (void)dir, (void)tMin, (void)tMax, (void)look, (void)rng, (void)t;
    return false;
#endif
}

float Gas::transmittance(const Vec3& origin, const Vec3& dir, float tMin, float tMax, const GasLook& look,
                         Rng& rng) const {
#ifdef PG_HAVE_NANOVDB
    const Grid& g = *grid_;
    float t0 = 0.0f, t1 = 0.0f;
    if (!(look.density > 0.0f) || !clip(g.box, origin, dir, tMin, tMax, t0, t1)) return 1.0f;
    Accessor acc = g.grid->getAccessor();
    float through = 1.0f;
    walk(g, origin, dir, t0, t1, [&](size_t tile, float from, float to) {
        const float bound = look.density * g.most[tile].x;
        if (!(bound > 0.0f)) return true;
        float s = from;
        for (;;) {
            s -= std::log(1.0f - rng.next()) / bound;
            if (s >= to) return true;
            through *= std::max(0.0f, 1.0f - extinction(sample(g, acc, origin + dir * s), look) / bound);
            if (through < 0.1f) {
                // Russian roulette: a dim shadow goes on as likely as it is
                // dim, the ones that do carry the share of those that end.
                if (rng.next() * 0.1f >= through) {
                    through = 0.0f;
                    return false;
                }
                through = 0.1f;
            }
        }
    });
    return through;
#else
    (void)origin, (void)dir, (void)tMin, (void)tMax, (void)look, (void)rng;
    return 1.0f;
#endif
}

void Gas::seen(const Vec3& origin, const Vec3& dir, float tMax, const GasLook& look, float& through,
               float& depth) const {
    through = 1.0f;
    depth = std::numeric_limits<float>::infinity();
#ifdef PG_HAVE_NANOVDB
    const Grid& g = *grid_;
    float t0 = 0.0f, t1 = 0.0f;
    if (!(look.density > 0.0f) || !clip(g.box, origin, dir, 0.0f, tMax, t0, t1)) return;
    Accessor acc = g.grid->getAccessor();
    double far = 0.0;  // the distances, each by the share of the light stopped there
    walk(g, origin, dir, t0, t1, [&](size_t tile, float from, float to) {
        if (!(g.most[tile].x > 0.0f)) return true;
        const int steps = std::max(1, static_cast<int>(std::ceil((to - from) * g.inverse)));
        const float h = (to - from) / static_cast<float>(steps);
        for (int i = 0; i < steps; ++i) {
            const float s = from + (static_cast<float>(i) + 0.5f) * h;
            const float after = through * std::exp(-extinction(sample(g, acc, origin + dir * s), look) * h);
            far += static_cast<double>(s) * static_cast<double>(through - after);
            through = after;
        }
        return through > 0.005f;
    });
    if (through < 1.0f) depth = static_cast<float>(far / static_cast<double>(1.0f - through));
#else
    (void)origin, (void)dir, (void)tMax, (void)look;
#endif
}

}  // namespace pg::render
