// The rough faces of a crack (Rough.h): concrete's bumps, wood's splinters.
#include "pg/nodes/Rough.h"

#include "pg/core/Parallel.h"
#include "pg/nodes/Nodes.h"
#include "pg/nodes/Rebuild.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace pg {
namespace {

constexpr uint32_t kNone = std::numeric_limits<uint32_t>::max();

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

float smooth01(float x) {
    const float t = std::clamp(x, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

bool lexLess(const Vec3& a, const Vec3& b) {
    if (a.x != b.x) return a.x < b.x;
    if (a.y != b.y) return a.y < b.y;
    return a.z < b.z;
}


/// The point of triangle abc nearest p (Ericson, Real-Time Collision Detection 5.1.5).
Vec3 closest(const Vec3& p, const Vec3& a, const Vec3& b, const Vec3& c) {
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


}  // namespace

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

Vec3 roughBumps(const Vec3& p, float scale, uint32_t seed) {
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

RoughAnchors::RoughAnchors(const std::vector<std::shared_ptr<Geometry>>& pieces, const std::string& rough,
                           const std::string& flatGroup, float reach)
    : reach_(std::max(reach, 1e-5f)) {
    Vec3 lo(1e30f, 1e30f, 1e30f), hi(-1e30f, -1e30f, -1e30f);
    for (const std::shared_ptr<Geometry>& piece : pieces) {
        if (!piece) continue;
        const Geometry& g = *piece;
        const Group* in = g.findGroup(rough);
        const Group* flat = flatGroup.empty() ? nullptr : g.findGroup(flatGroup);
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

float RoughAnchors::distance(const Vec3& p) const {
    const auto it = cells_.find(key(at(p.x), at(p.y), at(p.z)));
    if (it == cells_.end()) return reach_;
    float best = reach_;
    for (const uint32_t t : it->second) {
        const auto& tri = tris_[t];
        best = std::min(best, length(p - closest(p, tri[0], tri[1], tri[2])));
    }
    return best;
}

int RoughAnchors::at(float x) const { return static_cast<int>(std::floor(std::clamp(x / cell_, -1e6f, 1e6f))); }

uint64_t RoughAnchors::key(int i, int j, int k) {
    auto part = [](int v) { return static_cast<uint64_t>(static_cast<uint32_t>(v + (1 << 20)) & 0x1FFFFFu); };
    return (part(i) << 42) | (part(j) << 21) | part(k);
}

namespace {

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

// A groove along the fibres is this many times as long as it is wide.
constexpr float kFibre = 8.0f;

/// How far a point of a wood crack moves along the fibres `g`: on a face
/// across them (`end` 1) a spike of the fibres -- bundles splinterSize
/// across torn at lengths up to splinter, steep from one to the next; the
/// same where the other side of the crack has it, so the spikes of one
/// side go into the hollows of the other.
Vec3 splinterOffset(const Vec3& p, const Vec3& g, float end, const RoughCut& r) {
    if (end <= 0.0f || r.splinter <= 0.0f) return Vec3();
    const Vec3 across = p - g * dot(p, g);
    const float s = 1.0f / std::max(r.splinterSize, 1e-4f);
    const float n = gradientNoise(across * s, r.seed + 7u) + 0.5f * gradientNoise(across * (2.3f * s), r.seed + 11u) +
                    0.35f * gradientNoise(across * (0.4f * s), r.seed + 13u);
    return g * (r.splinter * end * std::tanh(2.5f * n));
}

}  // namespace

std::shared_ptr<Geometry> roughenCuts(const Geometry& piece, const std::string& rough, const std::string& flatGroup,
                                      const RoughCut& r, const RoughAnchors& anchors) {
    const Group* in = piece.findGroup(rough);
    const Group* flat = flatGroup.empty() ? nullptr : piece.findGroup(flatGroup);
    // A piece cut again keeps where its points were before they were rough.
    const bool hadProxy = piece.points().find("proxy") != nullptr;
    auto isRough = [&](size_t prim) {
        return in && in->contains(prim) && !(flat && flat->contains(prim)) && piece.primitiveClosed(prim) &&
               piece.primitiveVertexCount(prim) >= 3;
    };
    bool any = false;
    for (size_t prim = 0; prim < piece.primitiveCount() && !any; ++prim) any = isRough(prim);
    const bool wood = dot(r.grain, r.grain) > 0.25f && r.splinter > 0.0f;
    if (!any || (r.amount <= 0.0f && !wood)) {
        auto same = std::make_shared<Geometry>(piece);
        if (hadProxy) return same;
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
    // How long an edge is, for how fine a face is cut: along the fibres of
    // wood kFibre times shorter -- its grooves run that far, and the
    // splinters are as fine as they are across them.
    const Vec3 fibre = wood ? normalize(r.grain) : Vec3();
    auto edgeLength = [&](const Vec3& e) {
        if (!wood) return length(e);
        const float along = dot(e, fibre);
        const Vec3 across = e - fibre * along;
        return std::sqrt(dot(across, across) + along * along / (kFibre * kFibre));
    };
    for (int round = 0; round < 20; ++round) {
        std::unordered_map<uint64_t, uint32_t> mid;
        for (const Tri& t : tris) {
            if (!t.rough) continue;
            for (int e = 0; e < 3; ++e) {
                const uint32_t a = t.v[static_cast<size_t>(e)], b = t.v[static_cast<size_t>((e + 1) % 3)];
                if (edgeLength(pos[b] - pos[a]) > detail) mid.try_emplace(edgeKey(a, b), kNone);
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
    if (!wood) {
        for (size_t i = 0; i < pos.size(); ++i) {
            if (stays[i] != 2) continue;
            const float fade = smooth01(anchors.distance(pos[i]) / reach);
            if (fade <= 0.0f) continue;
            moved[i] = pos[i] + roughBumps(pos[i], r.scale, r.seed) * (r.amount * fade);
        }
    } else {
        // Wood: how much each point is of a face across the fibres -- its
        // rough triangles' way, the same either side of the crack but for
        // the sign -- torn into spikes along them; of a face along them,
        // grooved as they run.
        std::vector<Vec3> across(pos.size());
        for (const Tri& t : tris) {
            if (!t.rough) continue;
            const Vec3 n = cross(pos[t.v[1]] - pos[t.v[0]], pos[t.v[2]] - pos[t.v[0]]);
            for (const uint32_t v : t.v) across[v] += dot(n, across[v]) < 0.0f ? n * -1.0f : n;
        }
        const Vec3 g = normalize(r.grain);
        for (size_t i = 0; i < pos.size(); ++i) {
            if (stays[i] != 2) continue;
            const float l = length(across[i]);
            // A cut of wood is never square across: the cells, squeezed
            // along the fibres, are cut by planes that lean along them. A
            // face leaning a tenth of the way across them already tears.
            const float end = l > 0.0f ? smooth01((std::fabs(dot(across[i], g)) / l - 0.06f) / 0.24f) : 0.0f;
            moved[i] = pos[i] + splinterOffset(pos[i], g, end, r);
            if (r.amount > 0.0f && end < 1.0f) {
                const float fade = smooth01(anchors.distance(pos[i]) / reach);
                const float along = dot(pos[i], g);
                const Vec3 stretched = pos[i] - g * (along * (1.0f - 1.0f / kFibre));
                moved[i] += roughBumps(stretched, r.scale, r.seed) * (r.amount * fade * (1.0f - end));
            }
        }
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
    if (!hadProxy) {
        auto proxy = out->points().create("proxy", AttrType::Vec3).write<Vec3>();
        std::copy(pos.begin(), pos.end(), proxy.begin());
    }
    auto outP = out->positionsForWrite();
    std::copy(moved.begin(), moved.end(), outP.begin());
    out->points().erase("N");  // the rough faces' own: the renderer's
    return out;
}


std::vector<std::shared_ptr<Geometry>> grainCells(const std::shared_ptr<const Geometry>& mesh, const std::vector<Vec3>& seeds,
                                                  const Vec3& grain, float stretch, const std::string& inside) {
    std::vector<std::shared_ptr<Geometry>> out;
    if (!mesh || seeds.empty()) return out;
    const bool along = dot(grain, grain) > 0.25f && stretch > 1.0f;
    const Vec3 g = along ? normalize(grain) : Vec3();
    // Squeezed along the grain about the middle of the mesh's box -- an
    // affine map, so a plane stays a plane and a cut a cut -- and back.
    Vec3 lo(1e30f, 1e30f, 1e30f), hi(-1e30f, -1e30f, -1e30f);
    for (const Vec3& p : mesh->positions()) {
        for (int a = 0; a < 3; ++a) {
            lo[a] = std::min(lo[a], p[a]);
            hi[a] = std::max(hi[a], p[a]);
        }
    }
    const Vec3 middle = (lo + hi) * 0.5f;
    auto squeeze = [&](const Vec3& p) { return p + g * (dot(p - middle, g) * (1.0f / stretch - 1.0f)); };
    auto unsqueeze = [&](const Vec3& p) { return p + g * (dot(p - middle, g) * (stretch - 1.0f)); };
    std::shared_ptr<Geometry> squeezed = std::make_shared<Geometry>(*mesh);
    std::vector<Vec3> at(seeds);
    if (along) {
        for (Vec3& p : squeezed->positionsForWrite()) p = squeeze(p);
        for (Vec3& p : at) p = squeeze(p);
    }
    std::vector<std::shared_ptr<Geometry>> cells(at.size());
    const VoronoiCells cutter(squeezed, at);
    parallelFor(at.size(), 1, [&](size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i) {
            std::shared_ptr<Geometry> cell = cutter.cell(i, inside);
            if (!cell || cell->primitiveCount() == 0) continue;
            if (along) {
                for (Vec3& p : cell->positionsForWrite()) p = unsqueeze(p);
                // Their way, squeezed, is not theirs: the renderer finds them again.
                cell->points().erase("N");
                cell->vertices().erase("N");
            }
            cells[i] = std::move(cell);
        }
    });
    for (std::shared_ptr<Geometry>& c : cells) {
        if (c) out.push_back(std::move(c));
    }
    return out;
}

}  // namespace pg
