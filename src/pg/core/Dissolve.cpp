#include "pg/core/Dissolve.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <numeric>
#include <unordered_map>

namespace pg {
namespace {

uint64_t keyOf(uint32_t a, uint32_t b) {
    if (a > b) std::swap(a, b);
    return (static_cast<uint64_t>(a) << 32) | b;
}

uint64_t wayOf(uint32_t from, uint32_t to) { return (static_cast<uint64_t>(from) << 32) | to; }

/// A side of a polygon: which, and its corner where it starts.
struct Side {
    uint32_t prim = 0, corner = 0;
};

/// Sets joined by the edges between them; the lowest number stands for each.
struct Sets {
    std::vector<uint32_t> up;
    explicit Sets(size_t n) : up(n) { std::iota(up.begin(), up.end(), 0u); }
    uint32_t find(uint32_t x) {
        while (up[x] != x) x = up[x] = up[up[x]];
        return x;
    }
    void join(uint32_t a, uint32_t b) {
        a = find(a), b = find(b);
        if (a != b) up[std::max(a, b)] = std::min(a, b);
    }
};

/// Each side of each closed polygon, by its edge.
std::unordered_map<uint64_t, std::vector<Side>> sidesOf(const Geometry& geo) {
    std::unordered_map<uint64_t, std::vector<Side>> sides;
    for (size_t p = 0; p < geo.primitiveCount(); ++p) {
        if (!geo.primitiveClosed(p)) continue;
        const auto pts = geo.primitivePoints(p);
        const size_t m = pts.size(), first = geo.primitiveVertexStart(p);
        if (m < 3) continue;
        for (size_t k = 0; k < m; ++k) {
            const uint32_t a = pts[k], b = pts[(k + 1) % m];
            if (a != b) sides[keyOf(a, b)].push_back({static_cast<uint32_t>(p), static_cast<uint32_t>(first + k)});
        }
    }
    return sides;
}

/// Copies element `from` of each attribute of `set` to element `to`.
void copyElement(AttributeSet& set, size_t from, size_t to) {
    for (auto& [name, array] : set) {
        const size_t size = attrSize(array.type());
        std::byte* bytes = array.rawWrite();
        std::memmove(bytes + to * size, bytes + from * size, size);
    }
}

}  // namespace

std::vector<Edge> innerEdges(const Geometry& geo, std::span<const uint8_t> prims) {
    std::map<Edge, uint32_t> count;
    for (size_t p = 0; p < geo.primitiveCount() && p < prims.size(); ++p) {
        if (!prims[p] || !geo.primitiveClosed(p)) continue;
        const auto pts = geo.primitivePoints(p);
        if (pts.size() < 3) continue;
        for (size_t k = 0; k < pts.size(); ++k) {
            const uint32_t a = pts[k], b = pts[(k + 1) % pts.size()];
            if (a != b) ++count[Edge(std::min(a, b), std::max(a, b))];
        }
    }
    std::vector<Edge> out;
    for (const auto& [e, n] : count) {
        if (n >= 2) out.push_back(e);
    }
    return out;
}

Geometry dissolveEdges(const Geometry& geo, std::span<const Edge> edges, const DissolveSettings& s, DissolveCount* count) {
    DissolveCount c;
    const size_t prims = geo.primitiveCount();
    const auto sides = sidesOf(geo);
    // The polygons each edge joins: two, and not one twice.
    Sets sets(prims);
    std::vector<uint8_t> joined(prims, 0);
    for (const Edge& e : edges) {
        const auto it = sides.find(keyOf(e.first, e.second));
        if (it == sides.end() || it->second.size() != 2 || it->second[0].prim == it->second[1].prim) {
            ++c.kept;
            continue;
        }
        sets.join(it->second[0].prim, it->second[1].prim);
        joined[it->second[0].prim] = joined[it->second[1].prim] = 1;
    }
    std::map<uint32_t, std::vector<uint32_t>> groups;  // by their lowest polygon, each in order
    for (uint32_t p = 0; p < prims; ++p) {
        if (joined[p]) groups[sets.find(p)].push_back(p);
    }

    // Each group walked round as one loop of the corners its outer sides
    // start at; the sides two of its polygons share go.
    std::vector<uint8_t> keep(prims, 1);
    std::vector<std::vector<uint32_t>> loops;  // corners of the input
    std::vector<uint32_t> from;                // each one's lowest polygon
    const auto vertexPoints = geo.vertexPoints();
    for (const auto& [root, members] : groups) {
        if (members.size() < 2) continue;
        std::vector<std::array<uint32_t, 3>> directed;  // from, to, corner
        for (const uint32_t p : members) {
            const auto pts = geo.primitivePoints(p);
            const size_t first = geo.primitiveVertexStart(p), m = pts.size();
            for (size_t k = 0; k < m; ++k) {
                if (pts[k] != pts[(k + 1) % m]) directed.push_back({pts[k], pts[(k + 1) % m], static_cast<uint32_t>(first + k)});
            }
        }
        std::unordered_map<uint64_t, uint32_t> ways;
        for (const auto& d : directed) ++ways[wayOf(d[0], d[1])];
        bool ok = true;
        std::vector<std::array<uint32_t, 3>> outer;
        for (const auto& d : directed) {
            if (ways.count(wayOf(d[1], d[0]))) continue;  // between two of them
            if (ways[wayOf(d[0], d[1])] > 1) ok = false;  // twice the same way: not wound alike
            outer.push_back(d);
        }
        std::unordered_map<uint32_t, uint32_t> next;  // a point -> the outer side from it
        for (uint32_t i = 0; i < outer.size() && ok; ++i) {
            if (!next.emplace(outer[i][0], i).second) ok = false;  // the border touches itself
        }
        if (!ok || outer.size() < 3) {
            ++c.kept;
            continue;
        }
        // From the lowest corner round: all the outer sides, or a hole.
        uint32_t start = 0;
        for (uint32_t i = 1; i < outer.size(); ++i) {
            if (outer[i][2] < outer[start][2]) start = i;
        }
        std::vector<uint32_t> loop;
        uint32_t at = start;
        do {
            loop.push_back(outer[at][2]);
            const auto it = next.find(outer[at][1]);
            if (it == next.end()) {
                ok = false;
                break;
            }
            at = it->second;
        } while (at != start && loop.size() <= outer.size());
        if (!ok || loop.size() != outer.size()) {
            ++c.kept;
            continue;
        }
        for (const uint32_t p : members) keep[p] = 0;
        loops.push_back(std::move(loop));
        from.push_back(members.front());
        ++c.polygons;
        c.merged += members.size();
    }

    // Points left inline on a side, on no other polygon: out of it.
    if (s.inlinePoints && !loops.empty()) {
        std::vector<uint32_t> uses(geo.pointCount(), 0);
        for (size_t p = 0; p < prims; ++p) {
            if (!keep[p]) continue;
            for (const uint32_t q : geo.primitivePoints(p)) {
                if (q < uses.size()) ++uses[q];
            }
        }
        for (const auto& loop : loops) {
            for (const uint32_t corner : loop) ++uses[vertexPoints[corner]];
        }
        const float straight = std::cos(std::clamp(s.inlineAngle, 0.0f, 89.0f) * 3.14159265358979f / 180.0f);
        const auto P = geo.positions();
        for (auto& loop : loops) {
            for (bool dropped = true; dropped && loop.size() > 3;) {
                dropped = false;
                for (size_t k = 0; k < loop.size() && loop.size() > 3; ++k) {
                    const uint32_t a = vertexPoints[loop[(k + loop.size() - 1) % loop.size()]];
                    const uint32_t q = vertexPoints[loop[k]];
                    const uint32_t b = vertexPoints[loop[(k + 1) % loop.size()]];
                    if (uses[q] != 1) continue;
                    const Vec3 u = P[q] - P[a], v = P[b] - P[q];
                    const float lu = length(u), lv = length(v);
                    if (lu < 1e-12f || lv < 1e-12f || dot(u, v) < straight * lu * lv) continue;
                    loop.erase(loop.begin() + static_cast<std::ptrdiff_t>(k));
                    uses[q] = 0;
                    dropped = true;
                    break;
                }
            }
        }
    }

    if (count) *count = c;
    if (loops.empty()) return geo;
    // The input with the new polygons after it -- each corner's attributes
    // those of the corner it was, each polygon's those of its lowest -- the
    // ones made one taken out, and the points only they used.
    Geometry out = geo;
    std::vector<uint32_t> points, counts;
    for (const auto& loop : loops) {
        for (const uint32_t corner : loop) points.push_back(vertexPoints[corner]);
        counts.push_back(static_cast<uint32_t>(loop.size()));
    }
    const std::vector<uint8_t> closed(counts.size(), 1);
    const size_t firstPrim = out.addPrimitives(points, counts, closed);
    AttributeSet& vertices = out.attributes(AttrClass::Vertex);
    AttributeSet& primitives = out.attributes(AttrClass::Primitive);
    size_t corner = out.primitiveVertexStart(firstPrim);
    for (size_t i = 0; i < loops.size(); ++i) {
        for (const uint32_t was : loops[i]) copyElement(vertices, was, corner++);
        copyElement(primitives, from[i], firstPrim + i);
    }
    for (const std::string& name : out.groupNames()) {
        Group* g = out.findGroup(name);
        if (g->classOf() == AttrClass::Primitive) {
            g->resize(out.primitiveCount());
            for (size_t i = 0; i < loops.size(); ++i) g->set(firstPrim + i, g->contains(from[i]));
        } else if (g->classOf() == AttrClass::Vertex) {
            g->resize(out.vertexCount());
            size_t v = out.primitiveVertexStart(firstPrim);
            for (const auto& loop : loops) {
                for (const uint32_t was : loop) g->set(v++, g->contains(was));
            }
        }
    }
    keep.resize(out.primitiveCount(), 1);
    out.deletePrimitives(keep, true);
    return out;
}

}  // namespace pg
