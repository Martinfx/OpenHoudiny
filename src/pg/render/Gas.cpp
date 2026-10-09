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
    g.steamDensity = look.steamDensity;
    g.steamColor = look.steamColor;
    g.steamAlbedo = albedoOf(look.steamColor);
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

/// Gas that goes less far than this while the shutter is open, in cells, is
/// read where it is: a 64th of a cell.
constexpr float kStill = 1.0f / 64.0f;

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
    const nanovdb::Vec4fGrid* grid = nullptr;
#endif
    Vec3 origin;                     // the corner of the simulation's box
    float voxel = 1.0f, inverse = 1.0f;  // world units a cell, cells a world unit
    int cells[3] = {0, 0, 0}, tiles[3] = {0, 0, 0};
    /// Of each tile, x fastest: the most smoke, temperature, flame and steam
    /// a point in it reads -- its cells and the layer round them.
    std::vector<Vec4> most;
    bool steamy = false;  // any cell holds steam
    Box box;
    size_t active = 0;
    int lo[3] = {0, 0, 0}, hi[3] = {-1, -1, -1};  // the tiles filled, from and to
    /// How fast the gas goes: of each tile, where its 4 x 4 x 4 blocks of
    /// 2 x 2 x 2 cells start in `velocity` (x fastest within the tile), -1
    /// for none -- the gas's tiles, and the ring round them it spreads to.
    std::vector<int32_t> velocityTile;
    std::vector<Vec3> velocity;
    /// Of each tile: velocityTile of it and of the seven after it -- +x, +y
    /// and +z as bits 1, 2 and 4 say, -1 past the domain. The eight blocks
    /// round a point are in these, from the tile of the lowest.
    std::vector<std::array<int32_t, 8>> velocityNear;
    float fastest = 0.0f;  // the most of it, world units a second
    /// Of each tile: the most the gas goes round it, world units a second --
    /// over its blocks and those of the tiles next to it.
    std::vector<float> speedNear;
    /// Of the tiles next to the gas -- reachMost[nearSlot[tile]], -1 for the
    /// rest -- the most a point in it reads where the gas may have come r =
    /// 2 ... 7 cells to it: of its cells and r layers round them, those of
    /// the tiles next to it bounded by the most of their layers.
    std::vector<int32_t> nearSlot;
    std::vector<std::array<Vec4, 6>> reachMost;
    /// `most` as far round as the fastest gas goes in half a frame, in
    /// whole tiles: for tiles where the gas round them may come further
    /// than 6 cells. Empty where none goes so far.
    std::vector<Vec4> moving;

    size_t tileOf(int a, int b, int c) const {
        return static_cast<size_t>(a) +
               static_cast<size_t>(tiles[0]) * (static_cast<size_t>(b) + static_cast<size_t>(tiles[1]) * static_cast<size_t>(c));
    }
    /// The velocity of block (i, j, k) of the domain; 0 where none is kept.
    Vec3 blockVelocity(int i, int j, int k) const {
        constexpr int kBlocks = sim::Tiles::kSide / sim::Frame::kBlock;
        if (i < 0 || j < 0 || k < 0) return Vec3(0.0f);
        const int a = i / kBlocks, b = j / kBlocks, c = k / kBlocks;
        if (a >= tiles[0] || b >= tiles[1] || c >= tiles[2]) return Vec3(0.0f);
        const int32_t first = velocityTile[tileOf(a, b, c)];
        if (first < 0) return Vec3(0.0f);
        return velocity[static_cast<size_t>(first) + static_cast<size_t>(i % kBlocks + kBlocks * (j % kBlocks + kBlocks * (k % kBlocks)))];
    }
    Vec3 velocityAt(const Vec3& p) const {
        if (velocity.empty()) return Vec3(0.0f);
        constexpr unsigned kBlocks = sim::Tiles::kSide / sim::Frame::kBlock;
        // Between the blocks' middles: the lowest of the eight round it, and
        // how far on from it.
        const float scale = inverse / static_cast<float>(sim::Frame::kBlock);
        const float x = (p.x - origin.x) * scale - 0.5f, y = (p.y - origin.y) * scale - 0.5f, z = (p.z - origin.z) * scale - 0.5f;
        if (!(x >= 0.0f && y >= 0.0f && z >= 0.0f && x < static_cast<float>(kBlocks * static_cast<unsigned>(tiles[0])) &&
              y < static_cast<float>(kBlocks * static_cast<unsigned>(tiles[1])) &&
              z < static_cast<float>(kBlocks * static_cast<unsigned>(tiles[2])))) {
            return edgeVelocityAt(x, y, z);
        }
        const unsigned i = static_cast<unsigned>(x), j = static_cast<unsigned>(y), k = static_cast<unsigned>(z);
        const float u = x - static_cast<float>(i), v = y - static_cast<float>(j), w = z - static_cast<float>(k);
        // From the tile of the lowest, and -- for a block on its far side --
        // the one after it (velocityNear).
        const std::array<int32_t, 8>& near =
            velocityNear[tileOf(static_cast<int>(i / kBlocks), static_cast<int>(j / kBlocks), static_cast<int>(k / kBlocks))];
        const unsigned li = i % kBlocks, lj = j % kBlocks, lk = k % kBlocks;
        const unsigned lx[2] = {li, (li + 1) % kBlocks}, ly[2] = {lj, (lj + 1) % kBlocks}, lz[2] = {lk, (lk + 1) % kBlocks};
        const unsigned sx[2] = {0u, li + 1 == kBlocks ? 1u : 0u}, sy[2] = {0u, lj + 1 == kBlocks ? 2u : 0u},
                       sz[2] = {0u, lk + 1 == kBlocks ? 4u : 0u};
        Vec3 c[8];
        for (unsigned n = 0; n < 8; ++n) {
            const unsigned a = n & 1u, b = (n >> 1) & 1u, e = (n >> 2) & 1u;
            const int32_t first = near[sx[a] | sy[b] | sz[e]];
            c[n] = first < 0 ? Vec3(0.0f) : velocity[static_cast<size_t>(first) + lx[a] + kBlocks * (ly[b] + kBlocks * lz[e])];
        }
        return lerp3(c, u, v, w);
    }
    /// velocityAt() where the eight blocks may be past the domain's tiles:
    /// block by block, at (x, y, z) among them.
    Vec3 edgeVelocityAt(float x, float y, float z) const {
        if (!(std::isfinite(x) && std::isfinite(y) && std::isfinite(z))) return Vec3(0.0f);
        const float fx = std::floor(x), fy = std::floor(y), fz = std::floor(z);
        const float far = static_cast<float>(1 << 24);
        if (std::fabs(fx) > far || std::fabs(fy) > far || std::fabs(fz) > far) return Vec3(0.0f);
        const int i = static_cast<int>(fx), j = static_cast<int>(fy), k = static_cast<int>(fz);
        Vec3 c[8];
        for (int n = 0; n < 8; ++n) c[n] = blockVelocity(i + (n & 1), j + ((n >> 1) & 1), k + ((n >> 2) & 1));
        return lerp3(c, x - fx, y - fy, z - fz);
    }
    /// Trilinear between eight corners, x fastest.
    static Vec3 lerp3(const Vec3 (&c)[8], float u, float v, float w) {
        const Vec3 x0 = c[0] + (c[1] - c[0]) * u, x1 = c[2] + (c[3] - c[2]) * u, x2 = c[4] + (c[5] - c[4]) * u,
                   x3 = c[6] + (c[7] - c[6]) * u;
        const Vec3 y0 = x0 + (x1 - x0) * v, y1 = x2 + (x3 - x2) * v;
        return y0 + (y1 - y0) * w;
    }
    /// The most a point of `tile` reads when the gas there may have come
    /// `d` cells to it -- the bound of the steps through it then.
    Vec4 mostWithin(size_t tile, float d) const {
        if (d > 6.0f) return moving.empty() ? most[tile] : moving[tile];
        const int r = static_cast<int>(std::ceil(d + 0.5f));
        if (r <= 1) return most[tile];
        const int32_t s = nearSlot[tile];
        return s < 0 ? Vec4(0.0f) : reachMost[static_cast<size_t>(s)][static_cast<size_t>(r - 2)];
    }
    Vec3 advected(const Vec3& p, float time) const {
        if (time == 0.0f || velocity.empty()) return p;
        return p - velocityAt(p - velocityAt(p) * time) * time;
    }
};

#ifdef PG_HAVE_NANOVDB
namespace {

using Accessor = nanovdb::Vec4fGrid::AccessorType;

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
Vec4 sample(const Gas::Grid& g, Accessor& acc, const Vec3& p) {
    const float x = (p.x - g.origin.x) * g.inverse - 0.5f;
    const float y = (p.y - g.origin.y) * g.inverse - 0.5f;
    const float z = (p.z - g.origin.z) * g.inverse - 0.5f;
    const float fx = std::floor(x), fy = std::floor(y), fz = std::floor(z);
    const int i = static_cast<int>(fx), j = static_cast<int>(fy), k = static_cast<int>(fz);
    const float u = x - fx, v = y - fy, w = z - fz;
    nanovdb::Vec4f c[8];
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
    Vec4 out;
    for (int a = 0; a < 4; ++a) {
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
    return grid_->handle.bufferSize() + grid_->most.size() * sizeof(Vec4);
#else
    return 0;
#endif
}

float Gas::extinction(const Vec3& f, const GasLook& look) {
    return look.density * std::max(f.x, 0.0f) / (1.0f + 4.0f * std::max(f.z, 0.0f));
}

float Gas::extinction(const Vec4& f, const GasLook& look) {
    return extinction(Vec3(f.x, f.y, f.z), look) + look.steamDensity * std::max(f.w, 0.0f);
}

Vec3 Gas::albedo(const Vec4& f, const GasLook& look) {
    const float smoke = extinction(Vec3(f.x, f.y, f.z), look), steam = look.steamDensity * std::max(f.w, 0.0f);
    if (!(steam > 0.0f)) return look.albedo;
    return (look.albedo * smoke + look.steamAlbedo * steam) * (1.0f / (smoke + steam));
}

Vec3 Gas::emission(const Vec3& f, const GasLook& look) {
    if (f.z <= 0.0f || look.flame <= 0.0f) return {};
    const float x = (f.y - look.flameStart) / std::max(look.flameRange, 1e-6f);
    if (x <= 0.0f) return {};
    return glow(x) * (look.flame * (1.0f - std::exp(-4.0f * f.z)));
}

namespace {

#ifdef PG_HAVE_NANOVDB
/// The frame's velocity onto the grid's tiles (Grid::velocity): the gas's
/// tiles, and `ring` tiles round them -- each block there the mean of those
/// next to it that have one, spread out a block at a time.
void buildVelocity(Gas::Grid& g, const sim::Frame& frame, int ring) {
    constexpr int kBlocks = sim::Tiles::kSide / sim::Frame::kBlock;
    constexpr size_t kInTile = static_cast<size_t>(kBlocks) * kBlocks * kBlocks;
    const size_t tileCount = static_cast<size_t>(g.tiles[0]) * static_cast<size_t>(g.tiles[1]) * static_cast<size_t>(g.tiles[2]);
    g.velocityTile.assign(tileCount, -1);
    std::vector<uint32_t> own = frame.gasTiles;
    if (own.empty()) {
        own.resize(tileCount);
        for (size_t t = 0; t < tileCount; ++t) own[t] = static_cast<uint32_t>(t);
        ring = 0;
    }
    g.velocity.assign(kInTile * own.size(), Vec3(0.0f));
    std::vector<uint8_t> known(g.velocity.size(), 1);
    for (size_t s = 0; s < own.size(); ++s) {
        if (own[s] < tileCount) g.velocityTile[own[s]] = static_cast<int32_t>(kInTile * s);
    }
    std::vector<uint16_t> scratch;
    const std::vector<uint16_t>& dense = frame.denseVelocity(scratch);
    const int bx = frame.blocks(0), by = frame.blocks(1), bz = frame.blocks(2);
    for (size_t s = 0; s < own.size(); ++s) {
        const size_t t = own[s];
        if (t >= tileCount) continue;
        const int a = static_cast<int>(t % static_cast<size_t>(g.tiles[0])) * kBlocks;
        const int b = static_cast<int>((t / static_cast<size_t>(g.tiles[0])) % static_cast<size_t>(g.tiles[1])) * kBlocks;
        const int c = static_cast<int>(t / (static_cast<size_t>(g.tiles[0]) * static_cast<size_t>(g.tiles[1]))) * kBlocks;
        // A tile past the domain's last block: none there.
        for (int z = 0; z < kBlocks && c + z < bz; ++z) {
            for (int y = 0; y < kBlocks && b + y < by; ++y) {
                for (int x = 0; x < kBlocks && a + x < bx; ++x) {
                    const size_t from = static_cast<size_t>(a + x) +
                                        static_cast<size_t>(bx) * (static_cast<size_t>(b + y) + static_cast<size_t>(by) * static_cast<size_t>(c + z));
                    Vec3 v(sim::fromHalf(dense[3 * from]), sim::fromHalf(dense[3 * from + 1]), sim::fromHalf(dense[3 * from + 2]));
                    if (!(std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z))) v = Vec3(0.0f);
                    g.velocity[kInTile * s + static_cast<size_t>(x + kBlocks * (y + kBlocks * z))] = v;
                    g.fastest = std::max(g.fastest, length(v));
                }
            }
        }
    }
    if (ring <= 0 || !(g.fastest > 0.0f)) return;
    // The ring: the tiles within `ring` of the gas's, none of their blocks
    // known yet.
    std::vector<uint32_t> added;
    for (const uint32_t t : own) {
        const int a = static_cast<int>(t % static_cast<uint32_t>(g.tiles[0]));
        const int b = static_cast<int>((t / static_cast<uint32_t>(g.tiles[0])) % static_cast<uint32_t>(g.tiles[1]));
        const int c = static_cast<int>(t / static_cast<uint32_t>(g.tiles[0] * g.tiles[1]));
        for (int dz = -ring; dz <= ring; ++dz) {
            for (int dy = -ring; dy <= ring; ++dy) {
                for (int dx = -ring; dx <= ring; ++dx) {
                    const int x = a + dx, y = b + dy, z = c + dz;
                    if (x < 0 || y < 0 || z < 0 || x >= g.tiles[0] || y >= g.tiles[1] || z >= g.tiles[2]) continue;
                    const size_t n = g.tileOf(x, y, z);
                    if (g.velocityTile[n] >= 0) continue;
                    g.velocityTile[n] = static_cast<int32_t>(g.velocity.size());
                    g.velocity.resize(g.velocity.size() + kInTile, Vec3(0.0f));
                    known.resize(known.size() + kInTile, 0);
                    added.push_back(static_cast<uint32_t>(n));
                }
            }
        }
    }
    // Spread out: each pass, the unknown blocks next to known ones take the
    // mean of those.
    std::vector<uint8_t> next = known;
    for (int pass = 0; pass < kBlocks * ring; ++pass) {
        bool any = false;
        for (const uint32_t t : added) {
            const int a = static_cast<int>(t % static_cast<uint32_t>(g.tiles[0])) * kBlocks;
            const int b = static_cast<int>((t / static_cast<uint32_t>(g.tiles[0])) % static_cast<uint32_t>(g.tiles[1])) * kBlocks;
            const int c = static_cast<int>(t / static_cast<uint32_t>(g.tiles[0] * g.tiles[1])) * kBlocks;
            const size_t first = static_cast<size_t>(g.velocityTile[t]);
            for (int z = 0; z < kBlocks; ++z) {
                for (int y = 0; y < kBlocks; ++y) {
                    for (int x = 0; x < kBlocks; ++x) {
                        const size_t at = first + static_cast<size_t>(x + kBlocks * (y + kBlocks * z));
                        if (known[at]) continue;
                        Vec3 sum(0.0f);
                        int count = 0;
                        const int i = a + x, j = b + y, k = c + z;
                        const int n[6][3] = {{i - 1, j, k}, {i + 1, j, k}, {i, j - 1, k}, {i, j + 1, k}, {i, j, k - 1}, {i, j, k + 1}};
                        for (const auto& q : n) {
                            if (q[0] < 0 || q[1] < 0 || q[2] < 0) continue;
                            const int ta = q[0] / kBlocks, tb = q[1] / kBlocks, tc = q[2] / kBlocks;
                            if (ta >= g.tiles[0] || tb >= g.tiles[1] || tc >= g.tiles[2]) continue;
                            const int32_t f = g.velocityTile[g.tileOf(ta, tb, tc)];
                            if (f < 0) continue;
                            const size_t o = static_cast<size_t>(f) + static_cast<size_t>(q[0] % kBlocks + kBlocks * (q[1] % kBlocks + kBlocks * (q[2] % kBlocks)));
                            if (!known[o]) continue;
                            sum = sum + g.velocity[o];
                            ++count;
                        }
                        if (count == 0) continue;
                        g.velocity[at] = sum * (1.0f / static_cast<float>(count));
                        next[at] = 1;
                        any = true;
                    }
                }
            }
        }
        known = next;
        if (!any) break;
    }
}

/// Grid::velocityNear, of velocityTile as it is.
void nearVelocity(Gas::Grid& g) {
    g.velocityNear.assign(g.velocityTile.size(), {-1, -1, -1, -1, -1, -1, -1, -1});
    for (int c = 0; c < g.tiles[2]; ++c) {
        for (int b = 0; b < g.tiles[1]; ++b) {
            for (int a = 0; a < g.tiles[0]; ++a) {
                std::array<int32_t, 8>& near = g.velocityNear[g.tileOf(a, b, c)];
                for (int n = 0; n < 8; ++n) {
                    const int x = a + (n & 1), y = b + ((n >> 1) & 1), z = c + ((n >> 2) & 1);
                    if (x < g.tiles[0] && y < g.tiles[1] && z < g.tiles[2]) near[static_cast<size_t>(n)] = g.velocityTile[g.tileOf(x, y, z)];
                }
            }
        }
    }
}

/// Grid::speedNear, nearSlot and reachMost: of the tiles `near` the gas,
/// from the most of the layers of each tile of it (`layers`, of the frame's
/// tiles; `slotOf` which, -1 for none).
void reachBounds(Gas::Grid& g, const std::vector<uint32_t>& near, const std::vector<std::array<Vec4, 24>>& layers,
                 const std::vector<int32_t>& slotOf) {
    constexpr size_t kInTile = static_cast<size_t>(sim::Tiles::kSide / sim::Frame::kBlock) *
                               (sim::Tiles::kSide / sim::Frame::kBlock) * (sim::Tiles::kSide / sim::Frame::kBlock);
    const size_t tileCount = g.velocityTile.size();
    // The fastest of each tile's blocks, then of it and those next to it.
    std::vector<float> speed(tileCount, 0.0f);
    parallelFor(tileCount, 256, [&](size_t begin, size_t end) {
        for (size_t t = begin; t < end; ++t) {
            if (g.velocityTile[t] < 0) continue;
            float most = 0.0f;
            for (size_t b = 0; b < kInTile; ++b) most = std::max(most, length(g.velocity[static_cast<size_t>(g.velocityTile[t]) + b]));
            speed[t] = most;
        }
    });
    g.speedNear = speed;
    for (int axis = 0; axis < 3; ++axis) {
        const std::vector<float> from = g.speedNear;
        for (int c = 0; c < g.tiles[2]; ++c) {
            for (int b = 0; b < g.tiles[1]; ++b) {
                for (int a = 0; a < g.tiles[0]; ++a) {
                    const int at[3] = {a, b, c};
                    float most = 0.0f;
                    for (int d = -1; d <= 1; ++d) {
                        int n[3] = {at[0], at[1], at[2]};
                        n[axis] += d;
                        if (n[axis] < 0 || n[axis] >= g.tiles[axis]) continue;
                        most = std::max(most, from[g.tileOf(n[0], n[1], n[2])]);
                    }
                    g.speedNear[g.tileOf(a, b, c)] = most;
                }
            }
        }
    }
    // Of each tile next to the gas, for r = 2 ... 7: its own `most`, and of
    // each tile next to it with gas, the most of its r layers that face it
    // -- the least of those along each way it lies off, for a tile at an
    // edge or a corner: the cells read there are in all of them.
    g.nearSlot.assign(tileCount, -1);
    g.reachMost.assign(near.size(), {});
    for (size_t s = 0; s < near.size(); ++s) g.nearSlot[near[s]] = static_cast<int32_t>(s);
    parallelFor(near.size(), 16, [&](size_t begin, size_t end) {
        for (size_t s = begin; s < end; ++s) {
            const size_t t = near[s];
            const int at[3] = {static_cast<int>(t % static_cast<size_t>(g.tiles[0])),
                               static_cast<int>((t / static_cast<size_t>(g.tiles[0])) % static_cast<size_t>(g.tiles[1])),
                               static_cast<int>(t / (static_cast<size_t>(g.tiles[0]) * static_cast<size_t>(g.tiles[1])))};
            for (int r = 2; r <= 7; ++r) {
                Vec4 m = g.most[t];
                for (int dz = -1; dz <= 1; ++dz) {
                    for (int dy = -1; dy <= 1; ++dy) {
                        for (int dx = -1; dx <= 1; ++dx) {
                            const int d[3] = {dx, dy, dz};
                            const int n[3] = {at[0] + dx, at[1] + dy, at[2] + dz};
                            if ((dx == 0 && dy == 0 && dz == 0) || n[0] < 0 || n[1] < 0 || n[2] < 0 || n[0] >= g.tiles[0] ||
                                n[1] >= g.tiles[1] || n[2] >= g.tiles[2]) {
                                continue;
                            }
                            const int32_t slot = slotOf[g.tileOf(n[0], n[1], n[2])];
                            if (slot < 0) continue;
                            const std::array<Vec4, 24>& layer = layers[static_cast<size_t>(slot)];
                            Vec4 bound(std::numeric_limits<float>::infinity());
                            for (int a = 0; a < 3; ++a) {
                                if (d[a] == 0) continue;
                                // Its r layers on the side toward this one.
                                Vec4 facing(0.0f);
                                for (int l = 0; l < r; ++l) {
                                    const int which = d[a] > 0 ? l : sim::Tiles::kSide - 1 - l;
                                    facing = glm::max(facing, layer[static_cast<size_t>(8 * a + which)]);
                                }
                                bound = glm::min(bound, facing);
                            }
                            m = glm::max(m, bound);
                        }
                    }
                }
                g.reachMost[s][static_cast<size_t>(r - 2)] = m;
            }
        }
    });
}

/// `most` of each tile and of those within `reach` tiles round it: a
/// separable maximum over the grid of tiles.
std::vector<Vec4> dilated(const Gas::Grid& g, int reach) {
    std::vector<Vec4> out = g.most, line;
    for (int axis = 0; axis < 3; ++axis) {
        std::vector<Vec4> from = out;
        const int n = g.tiles[axis];
        const int other[2] = {(axis + 1) % 3, (axis + 2) % 3};
        for (int p = 0; p < g.tiles[other[1]]; ++p) {
            for (int q = 0; q < g.tiles[other[0]]; ++q) {
                for (int i = 0; i < n; ++i) {
                    Vec4 m(0.0f);
                    for (int d = std::max(0, i - reach); d <= std::min(n - 1, i + reach); ++d) {
                        int c[3];
                        c[axis] = d;
                        c[other[0]] = q;
                        c[other[1]] = p;
                        m = glm::max(m, from[g.tileOf(c[0], c[1], c[2])]);
                    }
                    int c[3];
                    c[axis] = i;
                    c[other[0]] = q;
                    c[other[1]] = p;
                    out[g.tileOf(c[0], c[1], c[2])] = m;
                }
            }
        }
    }
    return out;
}
#endif

}  // namespace

std::shared_ptr<const Gas> Gas::build(const sim::Frame& frame, float frameTime) {
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

    // The steam, laid out as the rest; none where it does not fit.
    const bool steamy = !frame.steam.empty() && frame.steamFits();
    // Each tile with smoke, flame or steam a leaf of the grid: made side by
    // side, put in the tree -- ordered by where they are -- one by one.
    using Build = nanovdb::tools::build::Grid<nanovdb::Vec4f>;
    Build build(nanovdb::Vec4f(0.0f), "gas");
    auto& root = build.tree().root();
    std::vector<uint8_t> filled(tileCount, 0);
    // Moving gas: the most of each layer of cells of each tile, along x, y
    // and z (8 each) -- what the bounds of reading it further round are
    // made of (reachMost); which of `tiles` each one is.
    const bool flowing = !frame.velocity.empty() && frame.velocityFits();
    std::vector<std::array<Vec4, 24>> layers(flowing ? tiles.size() : 0);
    std::vector<int32_t> slotOf(flowing ? tileCount : 0, -1);
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
            bool seen = false;  // smoke, flame or steam: heat alone shows nothing
            std::array<Vec4, 24> layer;
            layer.fill(Vec4(0.0f));
            for (int z = 0; z < sim::Tiles::kSide && ck + z < nz; ++z) {
                for (int y = 0; y < sim::Tiles::kSide && cj + y < ny; ++y) {
                    for (int x = 0; x < sim::Tiles::kSide && ci + x < nx; ++x) {
                        const int i = ci + x, j = cj + y, k = ck + z;
                        const size_t at = sparse ? 3 * (s * sim::Tiles::kCells + sim::SparseGrid::local(x, y, z))
                                                 : 3 * (static_cast<size_t>(i) + static_cast<size_t>(nx) * (static_cast<size_t>(j) + static_cast<size_t>(ny) * static_cast<size_t>(k)));
                        float smoke = sim::fromHalf(frame.fields[at]), heat = sim::fromHalf(frame.fields[at + 1]),
                              flame = sim::fromHalf(frame.fields[at + 2]);
                        float steam = steamy ? sim::fromHalf(frame.steam[at / 3]) : 0.0f;
                        smoke = std::isfinite(smoke) ? std::max(smoke, 0.0f) : 0.0f;
                        heat = std::isfinite(heat) ? heat : 0.0f;
                        flame = std::isfinite(flame) ? std::max(flame, 0.0f) : 0.0f;
                        steam = std::isfinite(steam) ? std::max(steam, 0.0f) : 0.0f;
                        if (smoke > 0.0f || flame > 0.0f || steam > 0.0f) {
                            const float f = fade(i, j, k);
                            smoke *= f;
                            flame *= f;
                            steam *= f;
                        }
                        if (smoke == 0.0f && heat == 0.0f && flame == 0.0f && steam == 0.0f) continue;
                        seen = seen || smoke > 0.0f || flame > 0.0f || steam > 0.0f;
                        leaf->setValue(nanovdb::Coord(i, j, k), nanovdb::Vec4f(smoke, heat, flame, steam));
                        ++cells;
                        if (flowing) {
                            const Vec4 v(smoke, heat, flame, steam);
                            layer[static_cast<size_t>(x)] = glm::max(layer[static_cast<size_t>(x)], v);
                            layer[static_cast<size_t>(8 + y)] = glm::max(layer[static_cast<size_t>(8 + y)], v);
                            layer[static_cast<size_t>(16 + z)] = glm::max(layer[static_cast<size_t>(16 + z)], v);
                        }
                    }
                }
            }
            if (!seen) {
                delete leaf;
                continue;
            }
            filled[t] = 1;
            active += cells;
            if (flowing) {
                layers[s] = layer;
                slotOf[t] = static_cast<int32_t>(s);
            }
            std::lock_guard<std::mutex> lock(mutex);
            root.addNode(leaf);
        }
    });
    if (active == 0) return nullptr;
    g.active = active;
    for (size_t t = 0; t < tileCount; ++t) {
        if (!filled[t]) continue;
        const int at[3] = {static_cast<int>(t % static_cast<size_t>(g.tiles[0])),
                           static_cast<int>((t / static_cast<size_t>(g.tiles[0])) % static_cast<size_t>(g.tiles[1])),
                           static_cast<int>(t / (static_cast<size_t>(g.tiles[0]) * static_cast<size_t>(g.tiles[1])))};
        for (int a = 0; a < 3; ++a) {
            g.lo[a] = g.hi[a] < g.lo[a] ? at[a] : std::min(g.lo[a], at[a]);
            g.hi[a] = std::max(g.hi[a], at[a]);
        }
    }
    g.handle = nanovdb::tools::createNanoGrid<Build, nanovdb::Vec4f>(build, nanovdb::tools::StatsMode::BBox,
                                                                       nanovdb::CheckMode::Disable);
    g.grid = g.handle.grid<nanovdb::Vec4f>();
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
    g.most.assign(tileCount, Vec4(0.0f, 0.0f, 0.0f, 0.0f));
    parallelFor(near.size(), 16, [&](size_t begin, size_t end) {
        Accessor acc = g.grid->getAccessor();
        for (size_t s = begin; s < end; ++s) {
            const size_t t = near[s];
            const int ci = static_cast<int>(t % static_cast<size_t>(g.tiles[0])) * sim::Tiles::kSide;
            const int cj = static_cast<int>((t / static_cast<size_t>(g.tiles[0])) % static_cast<size_t>(g.tiles[1])) * sim::Tiles::kSide;
            const int ck = static_cast<int>(t / (static_cast<size_t>(g.tiles[0]) * static_cast<size_t>(g.tiles[1]))) * sim::Tiles::kSide;
            Vec4 m(0.0f, 0.0f, 0.0f, 0.0f);
            for (int k = ck - 1; k <= ck + sim::Tiles::kSide; ++k) {
                for (int j = cj - 1; j <= cj + sim::Tiles::kSide; ++j) {
                    for (int i = ci - 1; i <= ci + sim::Tiles::kSide; ++i) {
                        const nanovdb::Vec4f v = acc.getValue(nanovdb::Coord(i, j, k));
                        m = Vec4(std::max(m.x, v[0]), std::max(m.y, v[1]), std::max(m.z, v[2]), std::max(m.w, v[3]));
                    }
                }
            }
            g.most[t] = m;
        }
    });
    g.steamy = steamy && std::any_of(g.most.begin(), g.most.end(), [](const Vec4& m) { return m.w > 0.0f; });

    // How fast it goes, and how far round it may then be read: half a frame
    // at the most it goes, in tiles, a ring of them -- up to four.
    if (flowing) {
        const float tile = g.voxel * static_cast<float>(sim::Tiles::kSide);
        float speed = 0.0f;
        for (size_t i = 0; i + 2 < frame.velocity.size(); i += 3) {
            const Vec3 v(sim::fromHalf(frame.velocity[i]), sim::fromHalf(frame.velocity[i + 1]), sim::fromHalf(frame.velocity[i + 2]));
            if (std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z)) speed = std::max(speed, length(v));
        }
        const float reach = 0.5f * std::max(frameTime, 0.0f) * speed;
        const int ring = std::clamp(static_cast<int>(std::ceil(reach / tile)), 1, 4);
        buildVelocity(g, frame, ring);
        if (g.fastest > 0.0f) {
            nearVelocity(g);
            reachBounds(g, near, layers, slotOf);
            // Further than 6 cells: whole tiles, as many as cover it and the
            // cell the eight round a point reach past it.
            if (reach * g.inverse > 6.0f) {
                g.moving = dilated(g, std::max(1, static_cast<int>(std::ceil((reach * g.inverse + 0.5f) / sim::Tiles::kSide))));
            }
        } else {
            g.velocity.clear();
        }
    }
    return gas;
#else
    (void)frame;
    (void)frameTime;
    return nullptr;
#endif
}

Gas::Dense Gas::dense(const GasLook& look, size_t most, float reach) const {
    Dense out;
#ifdef PG_HAVE_NANOVDB
    const Grid& g = *grid_;
    if (g.hi[0] < g.lo[0]) return out;
    // The cells of the tiles filled and one round them -- where reading
    // between the cells' middles takes in the last ones.
    int from[3], count[3];
    size_t cells = 1;
    for (int a = 0; a < 3; ++a) {
        from[a] = std::max(g.lo[a] * sim::Tiles::kSide - 1, 0);
        const int to = std::min((g.hi[a] + 1) * sim::Tiles::kSide + 1, g.cells[a]);
        count[a] = std::max(to - from[a], 1);
        cells *= static_cast<size_t>(count[a]);
    }
    // Blocks of k x k x k cells where they are too many.
    int k = 1;
    while (cells / (static_cast<size_t>(k) * k * k) > std::max<size_t>(most, 1)) ++k;
    for (int a = 0; a < 3; ++a) {
        out.size[a] = (count[a] + k - 1) / k;
        out.box.lo[a] = g.origin[a] + static_cast<float>(from[a]) * g.voxel;
        out.box.hi[a] = out.box.lo[a] + static_cast<float>(out.size[a] * k) * g.voxel;
    }
    const size_t n = static_cast<size_t>(out.size[0]) * static_cast<size_t>(out.size[1]) * static_cast<size_t>(out.size[2]);
    out.extinction.assign(n, 0.0f);
    std::vector<Vec3> emission(n);
    if (g.steamy) out.albedo.assign(n, look.albedo);
    std::atomic<bool> glows{false};
    const size_t row = static_cast<size_t>(out.size[0]);
    const size_t rows = static_cast<size_t>(out.size[1]) * static_cast<size_t>(out.size[2]);
    parallelFor(rows, 16, [&](size_t begin, size_t end) {
        Accessor acc = g.grid->getAccessor();
        bool lit = false;
        for (size_t r = begin; r < end; ++r) {
            const int y = static_cast<int>(r % static_cast<size_t>(out.size[1])), z = static_cast<int>(r / static_cast<size_t>(out.size[1]));
            for (int x = 0; x < out.size[0]; ++x) {
                // A cell's middle: its own numbers; a block's: read there.
                Vec4 f;
                if (k == 1) {
                    const nanovdb::Vec4f v = acc.getValue(nanovdb::Coord(from[0] + x, from[1] + y, from[2] + z));
                    f = Vec4(v[0], v[1], v[2], v[3]);
                } else {
                    const Vec3 p(out.box.lo.x + (static_cast<float>(x) + 0.5f) * static_cast<float>(k) * g.voxel,
                                 out.box.lo.y + (static_cast<float>(y) + 0.5f) * static_cast<float>(k) * g.voxel,
                                 out.box.lo.z + (static_cast<float>(z) + 0.5f) * static_cast<float>(k) * g.voxel);
                    f = sample(g, acc, p);
                }
                const size_t i = r * row + static_cast<size_t>(x);
                out.extinction[i] = extinction(f, look);
                emission[i] = Gas::emission(Vec3(f.x, f.y, f.z), look);
                if (g.steamy) out.albedo[i] = albedo(f, look);
                lit = lit || emission[i].x > 0.0f || emission[i].y > 0.0f || emission[i].z > 0.0f;
            }
        }
        if (lit) glows = true;
    });
    if (glows) out.emission = std::move(emission);
    // How fast it goes, where it may be read as it moves; cells twice as
    // large.
    if (reach > 0.0f) denseVelocity(out.box, 2.0f * static_cast<float>(k) * g.voxel, reach, out);
#else
    (void)look;
    (void)most;
    (void)reach;
#endif
    return out;
}

void Gas::denseVelocity(const Box& box, float edge, float reach, Dense& out) const {
#ifdef PG_HAVE_NANOVDB
    const Grid& g = *grid_;
    if (!g.velocity.empty() && reach > 0.0f) {
        for (int a = 0; a < 3; ++a) {
            const float lo = std::max(box.lo[a] - reach, g.box.lo[a]), hi = std::min(box.hi[a] + reach, g.box.hi[a]);
            out.velocitySize[a] = std::max(1, static_cast<int>(std::ceil((hi - lo) / edge)));
            out.velocityBox.lo[a] = lo;
            out.velocityBox.hi[a] = lo + static_cast<float>(out.velocitySize[a]) * edge;
        }
        const size_t nv = static_cast<size_t>(out.velocitySize[0]) * static_cast<size_t>(out.velocitySize[1]) *
                          static_cast<size_t>(out.velocitySize[2]);
        out.velocity.assign(nv, Vec3(0.0f));
        const size_t vrow = static_cast<size_t>(out.velocitySize[0]);
        parallelFor(static_cast<size_t>(out.velocitySize[1]) * static_cast<size_t>(out.velocitySize[2]), 16,
                    [&](size_t begin, size_t end) {
                        for (size_t r = begin; r < end; ++r) {
                            const int y = static_cast<int>(r % static_cast<size_t>(out.velocitySize[1]));
                            const int z = static_cast<int>(r / static_cast<size_t>(out.velocitySize[1]));
                            for (int x = 0; x < out.velocitySize[0]; ++x) {
                                const Vec3 p = out.velocityBox.lo + (Vec3(static_cast<float>(x), static_cast<float>(y),
                                                                          static_cast<float>(z)) + 0.5f) * edge;
                                out.velocity[r * vrow + static_cast<size_t>(x)] = g.velocityAt(p);
                            }
                        }
                    });
    }
#else
    (void)box;
    (void)edge;
    (void)reach;
    (void)out;
#endif
}

#ifdef PG_HAVE_NANOVDB
namespace {

/// A grid of `from`'s leaves -- the gas's tiles -- of what `make` makes of
/// each of their cells, the cells it makes the background of left out; its
/// buffer.
template <typename T, typename Make>
std::vector<uint8_t> leavesOf(const nanovdb::Vec4fGrid& from, const T& background, Make make) {
    using Build = nanovdb::tools::build::Grid<T>;
    Build build(background, "gas");
    auto& root = build.tree().root();
    const auto& tree = from.tree();
    const uint32_t leaves = tree.nodeCount(0);
    const auto* first = tree.template getFirstNode<0>();
    std::mutex mutex;
    std::atomic<bool> any{false};
    parallelFor(leaves, 16, [&](size_t begin, size_t end) {
        for (size_t l = begin; l < end; ++l) {
            const auto& leaf = first[l];
            const nanovdb::Coord at = leaf.origin();
            auto* made = new typename Build::Node0(at, background, false);
            bool kept = false;
            for (int x = 0; x < 8; ++x) {
                for (int y = 0; y < 8; ++y) {
                    for (int z = 0; z < 8; ++z) {
                        const nanovdb::Coord ijk = at.offsetBy(x, y, z);
                        const T v = make(leaf.getValue(ijk));
                        if (v == background) continue;
                        made->setValue(ijk, v);
                        kept = true;
                    }
                }
            }
            if (!kept) {
                delete made;
                continue;
            }
            any = true;
            std::lock_guard<std::mutex> lock(mutex);
            root.addNode(made);
        }
    });
    if (!any) return {};
    auto handle = nanovdb::tools::createNanoGrid<Build, T>(build, nanovdb::tools::StatsMode::BBox, nanovdb::CheckMode::Disable);
    const auto* bytes = static_cast<const uint8_t*>(handle.data());
    return std::vector<uint8_t>(bytes, bytes + handle.bufferSize());
}

}  // namespace
#endif

Gas::Sparse Gas::sparse(const GasLook& look) const {
    Sparse out;
#ifdef PG_HAVE_NANOVDB
    const Grid& g = *grid_;
    if (g.hi[0] < g.lo[0]) return out;
    out.origin = g.origin;
    out.cell = g.voxel;
    for (int a = 0; a < 3; ++a) {
        out.box.lo[a] = g.origin[a] + static_cast<float>(std::max(g.lo[a] * sim::Tiles::kSide - 1, 0)) * g.voxel;
        out.box.hi[a] = g.origin[a] + static_cast<float>(std::min((g.hi[a] + 1) * sim::Tiles::kSide + 1, g.cells[a])) * g.voxel;
    }
    // Their fourth number 1: not a share of the colour to divide it by.
    auto four = [](const Vec3& v) { return nanovdb::Vec4f(v.x, v.y, v.z, 1.0f); };
    auto fields = [](const nanovdb::Vec4f& v) { return Vec4(v[0], v[1], v[2], v[3]); };
    out.extinction = leavesOf<float>(*g.grid, 0.0f, [&](const nanovdb::Vec4f& v) { return extinction(fields(v), look); });
    out.emission = leavesOf<nanovdb::Vec4f>(*g.grid, four(Vec3(0.0f)), [&](const nanovdb::Vec4f& v) {
        return four(emission(Vec3(v[0], v[1], v[2]), look));
    });
    if (g.steamy) {
        out.albedo = leavesOf<nanovdb::Vec4f>(*g.grid, four(look.albedo),
                                              [&](const nanovdb::Vec4f& v) { return four(albedo(fields(v), look)); });
    }
#else
    (void)look;
#endif
    return out;
}

Vec3 Gas::velocityAt(const Vec3& p) const { return grid_->velocityAt(p); }
bool Gas::moves() const { return !grid_->velocity.empty(); }
float Gas::fastest() const { return grid_->fastest; }
Vec3 Gas::advected(const Vec3& p, float time) const { return grid_->advected(p, time); }

Vec3 Gas::at(const Vec3& p) const {
    const Vec4 f = fieldsAt(p);
    return {f.x, f.y, f.z};
}

Vec4 Gas::fieldsAt(const Vec3& p) const {
#ifdef PG_HAVE_NANOVDB
    // Beyond a cell outside the box, nothing: the cells' numbers stay small.
    const Box& b = grid_->box;
    const float v = grid_->voxel;
    if (!(p.x > b.lo.x - v && p.y > b.lo.y - v && p.z > b.lo.z - v && p.x < b.hi.x + v && p.y < b.hi.y + v &&
          p.z < b.hi.z + v)) {
        return Vec4(0.0f);
    }
    Accessor acc = grid_->grid->getAccessor();
    return sample(*grid_, acc, p);
#else
    (void)p;
    return Vec4(0.0f);
#endif
}

bool Gas::steamy() const { return grid_->steamy; }

bool Gas::track(const Vec3& origin, const Vec3& dir, float tMin, float tMax, const GasLook& look, Rng& rng, float& t,
                Vec3& emitted, Vec3& kept, float time) const {
    emitted = Vec3(0.0f, 0.0f, 0.0f);
    kept = look.albedo;
#ifdef PG_HAVE_NANOVDB
    const Grid& g = *grid_;
    float t0 = 0.0f, t1 = 0.0f;
    if (!clip(g.box, origin, dir, tMin, tMax, t0, t1)) return false;
    Accessor acc = g.grid->getAccessor();
    // While the shutter is open: the gas where it was, read in each tile
    // where it may have come from by then -- the bound of the steps as far
    // round as that.
    const float away = time != 0.0f && !g.velocity.empty() ? std::fabs(time) * g.inverse : 0.0f;
    bool scattered = false;
    walk(g, origin, dir, t0, t1, [&](size_t tile, float from, float to) {
        const float d = away > 0.0f ? g.speedNear[tile] * away : 0.0f;
        const bool moving = d > kStill;
        const Vec4 m = moving ? g.mostWithin(tile, d) : g.most[tile];
        // Through fire, a step a cell at least: each adds the light it gives off.
        const bool fire = look.flame > 0.0f && m.z > 0.0f && m.y > look.flameStart;
        const float bound = std::max(look.density * m.x + look.steamDensity * m.w, fire ? g.inverse : 0.0f);
        if (!(bound > 0.0f)) return true;
        float s = from;
        for (;;) {
            s -= std::log(1.0f - rng.next()) / bound;
            if (s >= to) return true;
            const Vec3 at = origin + dir * s;
            const Vec4 f = sample(g, acc, moving ? g.advected(at, time) : at);
            if (fire) emitted = emitted + emission(Vec3(f.x, f.y, f.z), look) * (1.0f / bound);
            // Scattered here as likely as the smoke and the steam are dense,
            // to the most they could be.
            if (rng.next() * bound < extinction(f, look)) {
                t = s;
                kept = albedo(f, look);
                scattered = true;
                return false;
            }
        }
    });
    return scattered;
#else
    (void)origin, (void)dir, (void)tMin, (void)tMax, (void)look, (void)rng, (void)t, (void)time;
    return false;
#endif
}

float Gas::transmittance(const Vec3& origin, const Vec3& dir, float tMin, float tMax, const GasLook& look,
                         Rng& rng, float time) const {
#ifdef PG_HAVE_NANOVDB
    const Grid& g = *grid_;
    float t0 = 0.0f, t1 = 0.0f;
    if (!(look.density > 0.0f || (g.steamy && look.steamDensity > 0.0f)) || !clip(g.box, origin, dir, tMin, tMax, t0, t1)) {
        return 1.0f;
    }
    Accessor acc = g.grid->getAccessor();
    const float away = time != 0.0f && !g.velocity.empty() ? std::fabs(time) * g.inverse : 0.0f;
    float through = 1.0f;
    walk(g, origin, dir, t0, t1, [&](size_t tile, float from, float to) {
        const float d = away > 0.0f ? g.speedNear[tile] * away : 0.0f;
        const bool moving = d > kStill;
        const Vec4 m = moving ? g.mostWithin(tile, d) : g.most[tile];
        const float bound = look.density * m.x + look.steamDensity * m.w;
        if (!(bound > 0.0f)) return true;
        float s = from;
        for (;;) {
            s -= std::log(1.0f - rng.next()) / bound;
            if (s >= to) return true;
            const Vec3 at = origin + dir * s;
            through *= std::max(0.0f, 1.0f - extinction(sample(g, acc, moving ? g.advected(at, time) : at), look) / bound);
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
    (void)origin, (void)dir, (void)tMin, (void)tMax, (void)look, (void)rng, (void)time;
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
    if (!(look.density > 0.0f || (g.steamy && look.steamDensity > 0.0f)) || !clip(g.box, origin, dir, 0.0f, tMax, t0, t1)) {
        return;
    }
    Accessor acc = g.grid->getAccessor();
    double far = 0.0;  // the distances, each by the share of the light stopped there
    walk(g, origin, dir, t0, t1, [&](size_t tile, float from, float to) {
        if (!(g.most[tile].x > 0.0f || g.most[tile].w > 0.0f)) return true;
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
