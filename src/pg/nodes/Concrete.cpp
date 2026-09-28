// Concrete Fracture: a closed mesh broken as concrete breaks -- chunks of
// every size, corners chipped off, the faces of the cracks rough:
//
//   seeds    Count points inside the mesh -- or those linked into Points --
//            denser where a noise says (Uneven) and round Impact (Focus,
//            within Reach): big chunks and small ones, the smallest where
//            it was struck
//   cells    each point's Voronoi cell (voronoiCell, as Voronoi Fracture)
//   rough    the cut faces cut into triangles Detail across and moved by
//            smooth 3D noise, Rough in and out, its bumps Rough Scale apart.
//            The same noise on both sides of a crack and the same triangles,
//            so the pieces still fit; less and less towards the outside,
//            which stays as it was -- so nothing pokes out of the object
//   spalls   Chips of the cells' corners cut off by a plane Chip Size deep,
//            through the rough faces: a flat spall, a small piece of its own
//            glued on by that face
//
// Where each point was before the cut faces were made rough stays in the
// point attribute proxy: the RBD Solver simulates the pieces as the proxy
// has them -- convex, face to face, glued where they touch -- and draws them
// as they are. The pieces carry piece (on primitives and points), the cut
// faces are in Inside Group, the spalls' primitives have chip 1.
#include "pg/core/Parallel.h"
#include "pg/nodes/Nodes.h"
#include "pg/nodes/Rebuild.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numeric>
#include <unordered_map>

namespace pg {
namespace {

constexpr const char* kSpall = "__spall";  // the spalls' flat faces, while the node works
constexpr uint32_t kNone = std::numeric_limits<uint32_t>::max();

uint64_t splitmix(uint64_t& state) {
    uint64_t z = (state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

float unit(uint64_t& state) { return static_cast<float>(splitmix(state) >> 40) / static_cast<float>(1ull << 24); }

uint32_t hashCell(int x, int y, int z, uint32_t seed) {
    uint32_t h = seed * 0x9E3779B9u + 0x7F4A7C15u;
    h ^= static_cast<uint32_t>(x) * 0x85EBCA6Bu;
    h = (h << 13) | (h >> 19);
    h ^= static_cast<uint32_t>(y) * 0xC2B2AE35u;
    h = (h << 13) | (h >> 19);
    h ^= static_cast<uint32_t>(z) * 0x27D4EB2Fu;
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    h *= 0x846CA68Bu;
    h ^= h >> 16;
    return h;
}

/// Perlin's improved gradient noise: smooth, about -1 to 1, the same on
/// every machine.
float gradientNoise(const Vec3& p, uint32_t seed) {
    const float fx = std::floor(p.x), fy = std::floor(p.y), fz = std::floor(p.z);
    const int ix = static_cast<int>(fx), iy = static_cast<int>(fy), iz = static_cast<int>(fz);
    const float x = p.x - fx, y = p.y - fy, z = p.z - fz;
    auto fade = [](float t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); };
    auto grad = [&](int i, int j, int k, float dx, float dy, float dz) {
        switch (hashCell(ix + i, iy + j, iz + k, seed) % 12u) {
            case 0: return dx + dy;
            case 1: return -dx + dy;
            case 2: return dx - dy;
            case 3: return -dx - dy;
            case 4: return dx + dz;
            case 5: return -dx + dz;
            case 6: return dx - dz;
            case 7: return -dx - dz;
            case 8: return dy + dz;
            case 9: return -dy + dz;
            case 10: return dy - dz;
            default: return -dy - dz;
        }
    };
    auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
    const float u = fade(x), v = fade(y), w = fade(z);
    const float x00 = lerp(grad(0, 0, 0, x, y, z), grad(1, 0, 0, x - 1.0f, y, z), u);
    const float x10 = lerp(grad(0, 1, 0, x, y - 1.0f, z), grad(1, 1, 0, x - 1.0f, y - 1.0f, z), u);
    const float x01 = lerp(grad(0, 0, 1, x, y, z - 1.0f), grad(1, 0, 1, x - 1.0f, y, z - 1.0f), u);
    const float x11 = lerp(grad(0, 1, 1, x, y - 1.0f, z - 1.0f), grad(1, 1, 1, x - 1.0f, y - 1.0f, z - 1.0f), u);
    return lerp(lerp(x00, x10, v), lerp(x01, x11, v), w);
}

/// How a point of a rough face moves, each component between -1 and 1:
/// three octaves of noise, bumps `scale` apart and finer ones on them,
/// gently saturated so that none goes further than 1.
Vec3 bumps(const Vec3& p, float scale, uint32_t seed) {
    Vec3 sum;
    float amplitude = 1.0f, frequency = 1.0f / std::max(scale, 1e-4f), total = 0.0f;
    for (uint32_t octave = 0; octave < 3; ++octave) {
        const Vec3 q = p * frequency;
        const uint32_t s = seed + 101u * octave;
        sum += Vec3(gradientNoise(q, s), gradientNoise(q + Vec3(17.31f, 5.13f, 9.71f), s + 1u),
                    gradientNoise(q + Vec3(-3.71f, 23.93f, 11.37f), s + 2u)) *
               amplitude;
        total += amplitude;
        amplitude *= 0.5f;
        frequency *= 2.0f;
    }
    const Vec3 n = sum * (2.5f / total);
    return Vec3(std::tanh(n.x), std::tanh(n.y), std::tanh(n.z));
}

float smooth01(float x) {
    const float t = std::clamp(x, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

bool lexLess(const Vec3& a, const Vec3& b) {
    if (a.x != b.x) return a.x < b.x;
    if (a.y != b.y) return a.y < b.y;
    return a.z < b.z;
}

// --- where the pieces are: seeds ------------------------------------------------------

void boundsOf(const Geometry& g, Vec3& lo, Vec3& hi) {
    lo = Vec3(1e30f, 1e30f, 1e30f);
    hi = Vec3(-1e30f, -1e30f, -1e30f);
    for (const Vec3& p : g.positions()) {
        for (int a = 0; a < 3; ++a) {
            lo[a] = std::min(lo[a], p[a]);
            hi[a] = std::max(hi[a], p[a]);
        }
    }
}

/// `count` points inside `mesh`, the same for the same `seed`: drawn where
/// the density is -- a noise, Uneven strong, times a bump round `impact`,
/// Focus high and Reach wide.
std::vector<Vec3> seedsInside(const Geometry& mesh, int count, float uneven, const Vec3& impact, float focus,
                              float reach, uint64_t seed) {
    std::vector<Vec3> out;
    if (mesh.pointCount() == 0 || count <= 0) return out;
    Vec3 lo, hi;
    boundsOf(mesh, lo, hi);
    const Vec3 size = hi - lo;
    const float extent = std::max({size.x, size.y, size.z, 1e-3f});
    const float spread = 4.0f * std::clamp(uneven, 0.0f, 1.0f);
    const float boost = 15.0f * std::clamp(focus, 0.0f, 1.0f);
    const float r2 = 2.0f * std::max(reach, 1e-3f) * std::max(reach, 1e-3f);
    const uint32_t noiseSeed = static_cast<uint32_t>(seed * 2654435761ull);
    const Vec3 offset(0.37f, 0.61f, 0.19f);  // off the lattice, where the noise is not 0
    auto density = [&](const Vec3& p) {
        const float n = spread > 0.0f ? gradientNoise(p * (3.0f / extent) + offset, noiseSeed) : 0.0f;
        const Vec3 d = p - impact;
        return std::exp(spread * n) * (1.0f + boost * std::exp(-dot(d, d) / r2));
    };
    const float most = std::exp(spread) * (1.0f + boost);
    uint64_t state = seed * 0x2545F4914F6CDD1Dull + 1;
    for (long tries = static_cast<long>(count) * 4000; tries > 0 && static_cast<int>(out.size()) < count; --tries) {
        const Vec3 p(lo.x + size.x * unit(state), lo.y + size.y * unit(state), lo.z + size.z * unit(state));
        if (unit(state) * most > density(p)) continue;
        if (insideMesh(mesh, p)) out.push_back(p);
    }
    return out;
}

// --- spalls: corners chipped off --------------------------------------------------------

struct Piece {
    std::shared_ptr<Geometry> geo;
    bool chip = false;
};

/// A corner of a cell -- a point where three or more faces meet -- and the
/// way it points out: its faces' directions together.
struct Corner {
    uint32_t point;
    Vec3 out;
};

std::vector<Corner> cornersOf(const Geometry& cell) {
    const size_t n = cell.pointCount();
    std::vector<Vec3> facing(n);
    std::vector<int> faces(n, 0);
    for (size_t prim = 0; prim < cell.primitiveCount(); ++prim) {
        const auto c = cell.primitivePoints(prim);
        if (c.size() < 3 || !cell.primitiveClosed(prim)) continue;
        const Vec3 d = normalize(polygonNormal(cell, c));
        for (const uint32_t pt : c) {
            facing[pt] += d;
            ++faces[pt];
        }
    }
    std::vector<Corner> out;
    for (size_t pt = 0; pt < n; ++pt) {
        if (faces[pt] >= 3 && length(facing[pt]) >= 0.5f) out.push_back({static_cast<uint32_t>(pt), normalize(facing[pt])});
    }
    return out;
}

/// A piece and the spalls chipped off its corners -- `chips` of `corners`,
/// found on the plain cell: each cut off by a plane across the way it
/// points, tilted at random, `chipSize` deep or a little less, as long as
/// what comes off is small and what stays keeps the middle. Cut where the
/// piece is -- through its rough faces, which are the same on the other
/// side of the crack whatever is cut from them here. The piece first, then
/// its spalls.
std::vector<Piece> chipped(std::shared_ptr<Geometry> piece, const std::vector<Corner>& corners, float chips, float chipSize,
                           uint64_t seed) {
    std::vector<Piece> out;
    Vec3 lo, hi;
    boundsOf(*piece, lo, hi);
    const float extent = std::max({hi.x - lo.x, hi.y - lo.y, hi.z - lo.z});
    if (chips <= 0.0f || chipSize <= 0.0f || corners.empty() || extent < 4.0f * chipSize) {
        out.push_back({piece, false});
        return out;
    }
    Vec3 middle;
    const auto P = piece->positions();
    for (const Vec3& p : P) middle += p;
    middle = middle * (1.0f / static_cast<float>(std::max<size_t>(P.size(), 1)));
    uint64_t state = seed;
    struct Cut {
        Vec3 at, n;
        float depth;
    };
    std::vector<Cut> cuts;
    for (const Corner& corner : corners) {
        const bool chosen = unit(state) < chips;
        const Vec3 tilt(2.0f * unit(state) - 1.0f, 2.0f * unit(state) - 1.0f, 2.0f * unit(state) - 1.0f);
        const float depth = chipSize * (0.55f + 0.45f * unit(state));
        if (!chosen || corner.point >= P.size()) continue;
        const Vec3 n = normalize(corner.out + tilt * 0.45f);
        cuts.push_back({P[corner.point], n, depth});
    }
    std::shared_ptr<Geometry> left = std::move(piece);
    for (const Cut& cut : cuts) {
        // Still a corner of what is left: another spall may have taken it.
        bool there = false;
        for (const Vec3& p : left->positions()) there = there || length(p - cut.at) < 1e-5f * extent;
        if (!there) continue;
        const Vec3 at = cut.at - cut.n * cut.depth;
        if (dot(middle - at, cut.n) > -0.5f * cut.depth) continue;  // it would take the middle
        std::shared_ptr<Geometry> chip = clipGeometry(*left, at, cut.n, true, kSpall);
        if (chip->primitiveCount() < 4) continue;
        Vec3 clo, chi;
        boundsOf(*chip, clo, chi);
        if (length(chi - clo) > 3.0f * chipSize) continue;  // not a spall: a slice
        std::shared_ptr<Geometry> rest = clipGeometry(*left, at, -cut.n, true, kSpall);
        if (rest->primitiveCount() < 4) continue;
        out.push_back({chip, true});
        left = std::move(rest);
    }
    out.insert(out.begin(), Piece{left, false});
    return out;
}

// --- rough faces --------------------------------------------------------------------------

/// The faces nothing roughens -- the outside -- of all the pieces, as
/// triangles in a sparse grid: how far a point is from them, up to `reach`.
class Anchors {
public:
    Anchors(const std::vector<std::shared_ptr<Geometry>>& pieces, const std::string& inside, float reach)
        : reach_(std::max(reach, 1e-5f)) {
        Vec3 lo(1e30f, 1e30f, 1e30f), hi(-1e30f, -1e30f, -1e30f);
        for (const std::shared_ptr<Geometry>& piece : pieces) {
            if (!piece) continue;
            const Geometry& g = *piece;
            const Group* in = g.findGroup(inside);
            const Group* flat = g.findGroup(kSpall);
            const auto P = g.positions();
            for (size_t prim = 0; prim < g.primitiveCount(); ++prim) {
                const auto c = g.primitivePoints(prim);
                if (c.size() < 3 || !g.primitiveClosed(prim)) continue;
                if (in && in->contains(prim) && !(flat && flat->contains(prim))) continue;  // rough
                for (size_t k = 1; k + 1 < c.size(); ++k) tris_.push_back({P[c[0]], P[c[k]], P[c[k + 1]]});
            }
            for (const Vec3& p : P) {
                for (int a = 0; a < 3; ++a) {
                    lo[a] = std::min(lo[a], p[a]);
                    hi[a] = std::max(hi[a], p[a]);
                }
            }
        }
        if (tris_.empty()) return;
        const float extent = std::max({hi.x - lo.x, hi.y - lo.y, hi.z - lo.z, 1e-3f});
        cell_ = std::max(reach_, extent / 256.0f);
        for (uint32_t t = 0; t < tris_.size(); ++t) {
            Vec3 tlo = tris_[t][0], thi = tris_[t][0];
            for (const Vec3& p : tris_[t]) {
                for (int a = 0; a < 3; ++a) {
                    tlo[a] = std::min(tlo[a], p[a]);
                    thi[a] = std::max(thi[a], p[a]);
                }
            }
            const int i0 = at(tlo.x - reach_), i1 = at(thi.x + reach_);
            const int j0 = at(tlo.y - reach_), j1 = at(thi.y + reach_);
            const int k0 = at(tlo.z - reach_), k1 = at(thi.z + reach_);
            for (int k = k0; k <= k1; ++k) {
                for (int j = j0; j <= j1; ++j) {
                    for (int i = i0; i <= i1; ++i) cells_[key(i, j, k)].push_back(t);
                }
            }
        }
    }

    /// How far `p` is from the nearest of them -- `reach` when none is nearer.
    float distance(const Vec3& p) const {
        const auto it = cells_.find(key(at(p.x), at(p.y), at(p.z)));
        if (it == cells_.end()) return reach_;
        float best = reach_;
        for (const uint32_t t : it->second) {
            const auto& tri = tris_[t];
            best = std::min(best, length(p - closest(p, tri[0], tri[1], tri[2])));
        }
        return best;
    }

private:
    int at(float x) const { return static_cast<int>(std::floor(std::clamp(x / cell_, -1e6f, 1e6f))); }
    static uint64_t key(int i, int j, int k) {
        auto part = [](int v) { return static_cast<uint64_t>(static_cast<uint32_t>(v + (1 << 20)) & 0x1FFFFFu); };
        return (part(i) << 42) | (part(j) << 21) | part(k);
    }
    /// The point of triangle abc nearest p (Ericson, Real-Time Collision Detection 5.1.5).
    static Vec3 closest(const Vec3& p, const Vec3& a, const Vec3& b, const Vec3& c) {
        const Vec3 ab = b - a, ac = c - a, ap = p - a;
        const float d1 = dot(ab, ap), d2 = dot(ac, ap);
        if (d1 <= 0.0f && d2 <= 0.0f) return a;
        const Vec3 bp = p - b;
        const float d3 = dot(ab, bp), d4 = dot(ac, bp);
        if (d3 >= 0.0f && d4 <= d3) return b;
        const float vc = d1 * d4 - d3 * d2;
        if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) return a + ab * (d1 / (d1 - d3));
        const Vec3 cp = p - c;
        const float d5 = dot(ab, cp), d6 = dot(ac, cp);
        if (d6 >= 0.0f && d5 <= d6) return c;
        const float vb = d5 * d2 - d1 * d6;
        if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) return a + ac * (d2 / (d2 - d6));
        const float va = d3 * d6 - d5 * d4;
        if (va <= 0.0f && d4 - d3 >= 0.0f && d5 - d6 >= 0.0f) return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
        const float sum = va + vb + vc;
        if (!(sum > 0.0f)) return a;  // no area
        return a + ab * (vb / sum) + ac * (vc / sum);
    }

    float reach_, cell_ = 1.0f;
    std::vector<std::array<Vec3, 3>> tris_;
    std::unordered_map<uint64_t, std::vector<uint32_t>> cells_;
};

struct Rough {
    float amount = 0.02f;  ///< metres, as far as a point goes -- each way
    float scale = 0.3f;    ///< metres between the bumps
    float detail = 0.03f;  ///< metres: the triangles no longer
    uint32_t seed = 1;
};

struct Tri {
    std::array<uint32_t, 3> v;
    uint32_t prim;
    bool rough;
};

/// Polygon `c` as triangles with its turn: a fan from its corner that lies
/// first in x, y, z -- the same corner, so the same triangles, from both
/// sides of a crack -- or, where it is not convex, ears cut off from there.
void triangulate(const Geometry& g, std::span<const uint32_t> c, uint32_t prim, bool rough, std::vector<Tri>& out) {
    const auto P = g.positions();
    const size_t n = c.size();
    size_t first = 0;
    for (size_t i = 1; i < n; ++i) {
        if (lexLess(P[c[i]], P[c[first]])) first = i;
    }
    const Vec3 normal = polygonNormal(g, c);
    bool convex = true;
    for (size_t i = 0; i < n && convex; ++i) {
        const Vec3& a = P[c[i]];
        const Vec3& b = P[c[(i + 1) % n]];
        const Vec3& d = P[c[(i + 2) % n]];
        convex = dot(cross(b - a, d - b), normal) >= -1e-12f * length(normal);
    }
    std::vector<uint32_t> ring(n);
    for (size_t i = 0; i < n; ++i) ring[i] = c[(first + i) % n];
    if (convex) {
        for (size_t i = 1; i + 1 < n; ++i) out.push_back({{ring[0], ring[i], ring[i + 1]}, prim, rough});
        return;
    }
    // Ears, in the polygon's plane.
    auto inside = [&](const Vec3& p, const Vec3& a, const Vec3& b, const Vec3& d) {
        return dot(cross(b - a, p - a), normal) > 0.0f && dot(cross(d - b, p - b), normal) > 0.0f &&
               dot(cross(a - d, p - d), normal) > 0.0f;
    };
    size_t guard = 0;
    while (ring.size() > 3 && guard++ < n * n) {
        bool cut = false;
        for (size_t i = 0; i < ring.size() && !cut; ++i) {
            const uint32_t a = ring[(i + ring.size() - 1) % ring.size()], b = ring[i], d = ring[(i + 1) % ring.size()];
            if (dot(cross(P[b] - P[a], P[d] - P[b]), normal) <= 0.0f) continue;  // a reflex corner
            bool empty = true;
            for (const uint32_t q : ring) {
                if (q != a && q != b && q != d && inside(P[q], P[a], P[b], P[d])) empty = false;
            }
            if (!empty) continue;
            out.push_back({{a, b, d}, prim, rough});
            ring.erase(ring.begin() + static_cast<long>(i));
            cut = true;
        }
        if (!cut) break;
    }
    for (size_t i = 1; i + 1 < ring.size(); ++i) out.push_back({{ring[0], ring[i], ring[i + 1]}, prim, rough});
}

/// Triangle `t` with its edges split where `mid` says: into two, three or
/// four, each the way both sides of a crack cut it -- by where the points
/// are, not by their numbers.
void split(const Tri& t, const std::unordered_map<uint64_t, uint32_t>& mid, const std::vector<Vec3>& pos,
           std::vector<Tri>& out) {
    auto m = [&](uint32_t x, uint32_t y) {
        const auto it = mid.find(edgeKey(x, y));
        return it == mid.end() ? kNone : it->second;
    };
    auto push = [&](uint32_t x, uint32_t y, uint32_t z) { out.push_back({{x, y, z}, t.prim, t.rough}); };
    const uint32_t a = t.v[0], b = t.v[1], c = t.v[2];
    const uint32_t mab = m(a, b), mbc = m(b, c), mca = m(c, a);
    const int n = (mab != kNone) + (mbc != kNone) + (mca != kNone);
    if (n == 0) {
        out.push_back(t);
    } else if (n == 3) {
        push(a, mab, mca);
        push(mab, b, mbc);
        push(mca, mbc, c);
        push(mab, mbc, mca);
    } else if (n == 1) {
        if (mab != kNone) {
            push(a, mab, c);
            push(mab, b, c);
        } else if (mbc != kNone) {
            push(a, b, mbc);
            push(a, mbc, c);
        } else {
            push(a, b, mca);
            push(mca, b, c);
        }
    } else {
        // Turned so that xy and yz are split, zx is not.
        uint32_t x = a, y = b, z = c, mxy = mab, myz = mbc;
        if (mab == kNone) {
            x = b, y = c, z = a, mxy = mbc, myz = mca;
        } else if (mbc == kNone) {
            x = c, y = a, z = b, mxy = mca, myz = mab;
        }
        push(mxy, y, myz);
        // The rest, a quad x mxy myz z, by its shorter diagonal; of two as
        // long -- a mirror image, which the sides of a crack round apart
        // differently -- the one from the corner first in x, y, z.
        const float d1 = length(pos[x] - pos[myz]), d2 = length(pos[mxy] - pos[z]);
        bool fromX = d1 < d2;
        if (std::fabs(d1 - d2) <= 1e-4f * (d1 + d2)) {
            const Vec3* firstOf = &pos[x];
            for (const uint32_t q : {mxy, myz, z}) {
                if (lexLess(pos[q], *firstOf)) firstOf = &pos[q];
            }
            fromX = firstOf == &pos[x] || firstOf == &pos[myz];
        }
        if (fromX) {
            push(x, mxy, myz);
            push(x, myz, z);
        } else {
            push(x, mxy, z);
            push(mxy, myz, z);
        }
    }
}

/// `piece` with its rough faces made rough; the plain positions in proxy.
std::shared_ptr<Geometry> roughen(const Geometry& piece, const std::string& inside, const Rough& r, const Anchors& anchors) {
    const Group* in = piece.findGroup(inside);
    const Group* flat = piece.findGroup(kSpall);
    auto isRough = [&](size_t prim) {
        return in && in->contains(prim) && !(flat && flat->contains(prim)) && piece.primitiveClosed(prim) &&
               piece.primitiveVertexCount(prim) >= 3;
    };
    bool any = false;
    for (size_t prim = 0; prim < piece.primitiveCount() && !any; ++prim) any = isRough(prim);
    if (!any || r.amount <= 0.0f) {
        auto same = std::make_shared<Geometry>(piece);
        auto proxy = same->points().create("proxy", AttrType::Vec3).write<Vec3>();
        const auto P = same->positions();
        std::copy(P.begin(), P.end(), proxy.begin());
        return same;
    }
    const auto P = piece.positions();
    std::vector<Vec3> pos(P.begin(), P.end());
    std::vector<std::vector<std::pair<uint32_t, float>>> terms(pos.size());
    for (uint32_t i = 0; i < terms.size(); ++i) terms[i] = {{i, 1.0f}};
    std::vector<Tri> tris;
    std::vector<uint32_t> open;  // polylines: as they were
    for (size_t prim = 0; prim < piece.primitiveCount(); ++prim) {
        const auto c = piece.primitivePoints(prim);
        if (!piece.primitiveClosed(prim) || c.size() < 3) {
            open.push_back(static_cast<uint32_t>(prim));
            continue;
        }
        triangulate(piece, c, static_cast<uint32_t>(prim), isRough(prim), tris);
    }
    // The edges of rough triangles longer than Detail split in two, round
    // after round: the other triangles on them split along, so no crack
    // opens between a rough face and the faces round it.
    const float detail = std::max(r.detail, 1e-4f);
    for (int round = 0; round < 20; ++round) {
        std::unordered_map<uint64_t, uint32_t> mid;
        for (const Tri& t : tris) {
            if (!t.rough) continue;
            for (int e = 0; e < 3; ++e) {
                const uint32_t a = t.v[static_cast<size_t>(e)], b = t.v[static_cast<size_t>((e + 1) % 3)];
                if (length(pos[a] - pos[b]) > detail) mid.try_emplace(edgeKey(a, b), kNone);
            }
        }
        if (mid.empty()) break;
        std::vector<uint64_t> keys;
        keys.reserve(mid.size());
        for (const auto& [k, v] : mid) keys.push_back(k);
        std::sort(keys.begin(), keys.end());
        for (const uint64_t k : keys) {
            const uint32_t a = static_cast<uint32_t>(k >> 32), b = static_cast<uint32_t>(k & 0xFFFFFFFFu);
            mid[k] = static_cast<uint32_t>(pos.size());
            pos.push_back((pos[a] + pos[b]) * 0.5f);
            std::vector<std::pair<uint32_t, float>> both;
            for (const auto& [i, w] : terms[a]) both.emplace_back(i, 0.5f * w);
            for (const auto& [i, w] : terms[b]) both.emplace_back(i, 0.5f * w);
            std::sort(both.begin(), both.end());
            std::vector<std::pair<uint32_t, float>> merged;
            for (const auto& [i, w] : both) {
                if (!merged.empty() && merged.back().first == i) merged.back().second += w;
                else merged.emplace_back(i, w);
            }
            terms.push_back(std::move(merged));
        }
        std::vector<Tri> next;
        next.reserve(tris.size() * 2);
        for (const Tri& t : tris) split(t, mid, pos, next);
        tris.swap(next);
    }
    // What moves: the points of rough faces alone -- by the same noise on
    // both sides of a crack, fading out towards the faces that stay.
    std::vector<uint8_t> stays(pos.size(), 1);
    for (const Tri& t : tris) {
        if (t.rough) {
            for (const uint32_t v : t.v) stays[v] = stays[v] == 1 ? 2 : stays[v];
        }
    }
    for (const Tri& t : tris) {
        if (!t.rough) {
            for (const uint32_t v : t.v) stays[v] = 1;
        }
    }
    std::vector<Vec3> moved = pos;
    const float reach = 2.0f * r.amount;  // no further than a point is from the outside
    for (size_t i = 0; i < pos.size(); ++i) {
        if (stays[i] != 2) continue;
        const float fade = smooth01(anchors.distance(pos[i]) / reach);
        if (fade <= 0.0f) continue;
        moved[i] = pos[i] + bumps(pos[i], r.scale, r.seed) * (r.amount * fade);
    }

    // The piece again: its attributes blended onto the new points, the
    // primitives' onto their triangles.
    Blends points;
    for (auto& t : terms) {
        std::vector<std::pair<uint32_t, float>> copy = t;
        points.add(copy);
    }
    std::vector<std::vector<uint32_t>> faces;
    std::vector<uint8_t> closed;
    std::vector<uint32_t> source;
    Blends vertices;
    auto vertexOf = [&](uint32_t prim, uint32_t point) -> int64_t {
        const auto c = piece.primitivePoints(prim);
        for (size_t k = 0; k < c.size(); ++k) {
            if (c[k] == point) return static_cast<int64_t>(piece.primitiveVertexStart(prim) + k);
        }
        return -1;
    };
    for (const Tri& t : tris) {
        faces.push_back({t.v[0], t.v[1], t.v[2]});
        closed.push_back(1);
        source.push_back(t.prim);
        for (const uint32_t v : t.v) {
            std::vector<std::pair<uint32_t, float>> corner;
            for (const auto& [pt, w] : terms[v]) {
                const int64_t vx = vertexOf(t.prim, pt);
                if (vx >= 0) corner.emplace_back(static_cast<uint32_t>(vx), w);
            }
            vertices.add(corner);
        }
    }
    for (const uint32_t prim : open) {
        const auto c = piece.primitivePoints(prim);
        faces.emplace_back(c.begin(), c.end());
        closed.push_back(0);
        source.push_back(prim);
        for (size_t k = 0; k < c.size(); ++k) vertices.one(static_cast<uint32_t>(piece.primitiveVertexStart(prim) + k));
    }
    std::shared_ptr<Geometry> out = rebuild(piece, points, faces, closed, vertices, source);
    auto proxy = out->points().create("proxy", AttrType::Vec3).write<Vec3>();
    std::copy(pos.begin(), pos.end(), proxy.begin());
    auto outP = out->positionsForWrite();
    std::copy(moved.begin(), moved.end(), outP.begin());
    out->points().erase("N");  // the rough faces' own: the renderer's
    return out;
}

class ConcreteFractureNode : public Node {
public:
    explicit ConcreteFractureNode(std::string name) : Node("concretefracture", std::move(name)) {
        setInputCount(2);
        params_.setInt("count", 60);
        params_.setInt("seed", 1);
        params_.setFloat("uneven", 0.5f);
        params_.setVec3("impact", Vec3(0.0f, 1.0f, 0.0f));
        params_.setFloat("focus", 0.0f);
        params_.setFloat("reach", 1.0f);
        params_.setFloat("chips", 0.2f);
        params_.setFloat("chipsize", 0.06f);
        params_.setFloat("rough", 0.02f);
        params_.setFloat("roughscale", 0.3f);
        params_.setFloat("detail", 0.03f);
        params_.setString("attribute", "piece");
        params_.setString("insidegroup", "inside");
    }

    GeometryPtr cookNode(const CookContext& ctx, std::span<const GeometryPtr> in) override {
        if (in.empty() || !in[0] || in[0]->primitiveCount() == 0) return std::make_shared<Geometry>();
        const GeometryPtr mesh = in[0];
        const uint64_t seed = static_cast<uint64_t>(std::max(params_.evalInt("seed", ctx, 1), 0));
        std::vector<Vec3> seeds;
        if (in.size() > 1 && in[1] && in[1]->pointCount() > 0) {
            const auto P = in[1]->positions();
            seeds.assign(P.begin(), P.end());
        } else {
            seeds = seedsInside(*mesh, std::clamp(params_.evalInt("count", ctx, 60), 0, 10000),
                                params_.evalFloat("uneven", ctx, 0.5f), params_.evalVec3("impact", ctx, Vec3(0.0f, 1.0f, 0.0f)),
                                params_.evalFloat("focus", ctx, 0.0f), params_.evalFloat("reach", ctx, 1.0f), seed);
        }
        const std::string attribute = params_.getString("attribute", "piece");
        const std::string inside = params_.getString("insidegroup", "inside");
        const float chips = std::clamp(params_.evalFloat("chips", ctx, 0.2f), 0.0f, 1.0f);
        const float chipSize = std::max(params_.evalFloat("chipsize", ctx, 0.06f), 0.0f);
        Rough rough;
        rough.amount = std::max(params_.evalFloat("rough", ctx, 0.02f), 0.0f);
        rough.scale = std::max(params_.evalFloat("roughscale", ctx, 0.3f), 1e-3f);
        rough.detail = std::max(params_.evalFloat("detail", ctx, 0.03f), 1e-3f);
        rough.seed = static_cast<uint32_t>(seed * 0x9E3779B1ull + 7);
        const std::string group = inside.empty() ? std::string("inside") : inside;

        // The cells -- each by itself, in parallel.
        std::vector<std::shared_ptr<Geometry>> cells(seeds.size());
        parallelFor(seeds.size(), 1, [&](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i) {
                if (ctx.interrupted()) return;
                std::shared_ptr<Geometry> cell = voronoiCell(mesh, seeds, i, group);
                if (cell && cell->primitiveCount() > 0) cells[i] = std::move(cell);
            }
        });
        if (ctx.interrupted()) return nullptr;
        // Their cut faces rough -- how far from the outside counted over all
        // of them, so both sides of a crack agree -- and then their corners
        // chipped off.
        const Anchors anchors(cells, group, 2.0f * rough.amount);
        std::vector<std::vector<Piece>> byCell(cells.size());
        parallelFor(cells.size(), 1, [&](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i) {
                if (ctx.interrupted() || !cells[i]) continue;
                const std::vector<Corner> corners = cornersOf(*cells[i]);
                byCell[i] = chipped(roughen(*cells[i], group, rough, anchors), corners, chips, chipSize,
                                    seed * 0x94D049BB133111EBull + (i + 1) * 0x9E3779B97F4A7C15ull);
            }
        });
        if (ctx.interrupted()) return nullptr;
        std::vector<Piece> pieces;
        for (auto& cell : byCell) {
            for (Piece& p : cell) pieces.push_back(std::move(p));
        }
        auto out = std::make_shared<Geometry>();
        int32_t number = 0;
        for (size_t i = 0; i < pieces.size(); ++i) {
            Geometry& g = *pieces[i].geo;
            if (g.primitiveCount() == 0) continue;
            if (!attribute.empty()) {
                auto pp = g.primitives().create(attribute, AttrType::Int).write<int32_t>();
                std::fill(pp.begin(), pp.end(), number);
                auto pt = g.points().create(attribute, AttrType::Int).write<int32_t>();
                std::fill(pt.begin(), pt.end(), number);
            }
            auto chip = g.primitives().create("chip", AttrType::Int).write<int32_t>();
            std::fill(chip.begin(), chip.end(), pieces[i].chip ? 1 : 0);
            // The spalls' faces are broken inside too.
            Group& cut = g.createGroup(group, AttrClass::Primitive);
            if (const Group* flat = g.findGroup(kSpall)) {
                for (size_t prim = 0; prim < g.primitiveCount(); ++prim) {
                    if (flat->contains(prim)) cut.set(prim, true);
                }
            }
            g.eraseGroup(kSpall);
            ++number;
            out->append(g);
        }
        if (!out->findGroup(group)) out->createGroup(group, AttrClass::Primitive);
        return out;
    }
};

}  // namespace

void registerConcreteNodes() {
    NodeRegistry::instance().add("concretefracture", [](const std::string& n) { return std::make_unique<ConcreteFractureNode>(n); });
}

}  // namespace pg
