#include "pg/core/Selection.h"

#include <algorithm>
#include <charconv>
#include <map>

namespace pg {

namespace {

bool apart(char c) { return c == ' ' || c == ',' || c == '\t' || c == '\n'; }

/// Each item of a pattern in turn, with 1 -- or 0 for one after "^".
template <typename F>
void forEachItem(std::string_view pattern, F&& f) {
    size_t at = 0;
    while (at < pattern.size()) {
        while (at < pattern.size() && apart(pattern[at])) ++at;
        size_t end = at;
        while (end < pattern.size() && !apart(pattern[end])) ++end;
        std::string_view item = pattern.substr(at, end - at);
        at = end;
        if (item.empty()) continue;
        uint8_t set = 1;
        if (item.front() == '^') {
            set = 0;
            item.remove_prefix(1);
            if (item.empty()) continue;
        }
        f(item, set);
    }
}

Edge edgeOf(uint32_t a, uint32_t b) { return a < b ? Edge(a, b) : Edge(b, a); }

/// "p3-4", "p0-1-2-3": the points of an edge item, in order. False when
/// the item is not one.
bool edgeItem(std::string_view item, std::vector<uint32_t>& path) {
    path.clear();
    if (item.size() < 4 || item.front() != 'p') return false;
    const char* s = item.data() + 1;
    const char* end = item.data() + item.size();
    for (;;) {
        uint32_t v = 0;
        const auto r = std::from_chars(s, end, v);
        if (r.ec != std::errc() || r.ptr == s) return false;
        path.push_back(v);
        s = r.ptr;
        if (s == end) break;
        if (*s != '-') return false;
        ++s;
    }
    return path.size() >= 2;
}

/// The edges of a path that are among `all` (sorted), sorted.
std::vector<Edge> edgesAlong(const std::vector<uint32_t>& path, std::span<const Edge> all) {
    std::vector<Edge> out;
    for (size_t k = 0; k + 1 < path.size(); ++k) {
        const Edge e = edgeOf(path[k], path[k + 1]);
        if (std::binary_search(all.begin(), all.end(), e)) out.push_back(e);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

/// "5v2" or "5v0-2": a primitive and the first and last of its corners,
/// inclusive. False when the item is not one.
bool vertexItem(std::string_view item, size_t& prim, size_t& first, size_t& last) {
    const size_t v = item.find('v');
    if (v == std::string_view::npos || v == 0 || v + 1 >= item.size()) return false;
    const auto r = std::from_chars(item.data(), item.data() + v, prim);
    if (r.ec != std::errc() || r.ptr != item.data() + v) return false;
    std::string_view rest = item.substr(v + 1);
    const size_t dash = rest.find('-');
    auto number = [](std::string_view s, size_t& out) {
        if (s.empty()) return false;
        const auto n = std::from_chars(s.data(), s.data() + s.size(), out);
        return n.ec == std::errc() && n.ptr == s.data() + s.size();
    };
    if (dash == std::string_view::npos) {
        if (!number(rest, first)) return false;
        last = first;
        return true;
    }
    if (!number(rest.substr(0, dash), first) || !number(rest.substr(dash + 1), last)) return false;
    if (last < first) std::swap(first, last);
    return true;
}

/// "12" or "3-40": the first and last numbers, inclusive. False when the
/// item is not numbers.
bool rangeOf(std::string_view item, size_t& first, size_t& last) {
    const size_t dash = item.find('-');
    auto number = [](std::string_view s, size_t& out) {
        if (s.empty()) return false;
        const auto r = std::from_chars(s.data(), s.data() + s.size(), out);
        return r.ec == std::errc() && r.ptr == s.data() + s.size();
    };
    if (dash == std::string_view::npos) {
        if (!number(item, first)) return false;
        last = first;
        return true;
    }
    if (!number(item.substr(0, dash), first) || !number(item.substr(dash + 1), last)) return false;
    if (last < first) std::swap(first, last);
    return true;
}

}  // namespace

std::vector<uint8_t> pointsOfPrimitives(const Geometry& geo, std::span<const uint8_t> prims) {
    std::vector<uint8_t> points(geo.pointCount(), 0);
    for (size_t p = 0; p < geo.primitiveCount() && p < prims.size(); ++p) {
        if (!prims[p]) continue;
        for (const uint32_t q : geo.primitivePoints(p)) {
            if (q < points.size()) points[q] = 1;
        }
    }
    return points;
}

std::vector<uint8_t> primitivesOfPoints(const Geometry& geo, std::span<const uint8_t> points) {
    std::vector<uint8_t> prims(geo.primitiveCount(), 0);
    for (size_t p = 0; p < prims.size(); ++p) {
        const auto corners = geo.primitivePoints(p);
        if (corners.empty()) continue;
        bool all = true;
        for (const uint32_t q : corners) all = all && q < points.size() && points[q];
        prims[p] = all ? 1 : 0;
    }
    return prims;
}

std::vector<uint8_t> verticesOfPoints(const Geometry& geo, std::span<const uint8_t> points) {
    const auto corners = geo.vertexPoints();
    std::vector<uint8_t> out(corners.size(), 0);
    for (size_t v = 0; v < corners.size(); ++v) out[v] = corners[v] < points.size() && points[corners[v]] ? 1 : 0;
    return out;
}

std::vector<uint8_t> verticesOfPrimitives(const Geometry& geo, std::span<const uint8_t> prims) {
    std::vector<uint8_t> out(geo.vertexCount(), 0);
    for (size_t p = 0; p < geo.primitiveCount() && p < prims.size(); ++p) {
        if (!prims[p]) continue;
        const size_t first = geo.primitiveVertexStart(p);
        for (size_t k = 0; k < geo.primitiveVertexCount(p); ++k) out[first + k] = 1;
    }
    return out;
}

std::vector<uint8_t> pointsOfVertices(const Geometry& geo, std::span<const uint8_t> vertices) {
    const auto corners = geo.vertexPoints();
    std::vector<uint8_t> out(geo.pointCount(), 0);
    for (size_t v = 0; v < corners.size() && v < vertices.size(); ++v) {
        if (vertices[v] && corners[v] < out.size()) out[corners[v]] = 1;
    }
    return out;
}

std::vector<uint8_t> primitivesOfVertices(const Geometry& geo, std::span<const uint8_t> vertices) {
    std::vector<uint8_t> out(geo.primitiveCount(), 0);
    for (size_t p = 0; p < out.size(); ++p) {
        const size_t first = geo.primitiveVertexStart(p), count = geo.primitiveVertexCount(p);
        bool all = count > 0;
        for (size_t k = 0; k < count && all; ++k) all = first + k < vertices.size() && vertices[first + k];
        out[p] = all ? 1 : 0;
    }
    return out;
}

std::vector<uint8_t> selectElements(const Geometry& geo, AttrClass cls, std::string_view pattern, bool* named) {
    const size_t n = geo.elementCount(cls == AttrClass::Primitive || cls == AttrClass::Vertex ? cls : AttrClass::Point);
    std::vector<uint8_t> mask(n, 0);
    if (named) *named = false;
    std::vector<Edge> edges;  // the geometry's, once an edge item asks
    bool edgesMade = false;
    std::vector<uint32_t> path;
    forEachItem(pattern, [&](std::string_view item, uint8_t set) {
        size_t first = 0, last = 0;
        if (item == "*") {
            std::fill(mask.begin(), mask.end(), set);
            if (named) *named = true;
        } else if (rangeOf(item, first, last)) {
            for (size_t i = first; i <= last && i < n; ++i) mask[i] = set;
            if (named) *named = true;
        } else if (size_t prim = 0; vertexItem(item, prim, first, last)) {
            if (named) *named = true;
            if (prim >= geo.primitiveCount()) return;
            const size_t start = geo.primitiveVertexStart(prim), count = geo.primitiveVertexCount(prim);
            if (cls == AttrClass::Primitive) {
                if (first < count) mask[prim] = set;
                return;
            }
            const auto corners = geo.vertexPoints();
            for (size_t k = first; k <= last && k < count; ++k) {
                const size_t i = cls == AttrClass::Vertex ? start + k : corners[start + k];
                if (i < n) mask[i] = set;
            }
        } else if (edgeItem(item, path)) {
            if (named) *named = true;
            if (!edgesMade) {
                edges = edgesOf(geo);
                edgesMade = true;
            }
            const std::vector<Edge> wanted = edgesAlong(path, edges);
            if (wanted.empty()) return;
            if (cls == AttrClass::Point) {
                for (const Edge& e : wanted) {
                    if (e.first < n) mask[e.first] = set;
                    if (e.second < n) mask[e.second] = set;
                }
                return;
            }
            // The primitives they are sides of -- or those primitives'
            // corners at their ends.
            for (size_t p = 0; p < geo.primitiveCount(); ++p) {
                const auto pts = geo.primitivePoints(p);
                const size_t m = pts.size();
                if (m < 2) continue;
                const size_t sides = geo.primitiveClosed(p) ? m : m - 1;
                const size_t start = geo.primitiveVertexStart(p);
                for (size_t k = 0; k < sides; ++k) {
                    if (!std::binary_search(wanted.begin(), wanted.end(), edgeOf(pts[k], pts[(k + 1) % m]))) continue;
                    if (cls == AttrClass::Primitive) {
                        mask[p] = set;
                        break;
                    }
                    mask[start + k] = set;
                    mask[start + (k + 1) % m] = set;
                }
            }
        } else if (const Group* g = geo.findGroup(std::string(item))) {
            if (named) *named = true;
            const AttrClass of = g->classOf();
            if (of != AttrClass::Point && of != AttrClass::Primitive && of != AttrClass::Vertex) return;
            std::vector<uint8_t> members(geo.elementCount(of), 0);
            for (size_t i = 0; i < members.size(); ++i) members[i] = g->contains(i) ? 1 : 0;
            if (of != cls) {
                // Through the points: what another class of group holds.
                if (of == AttrClass::Vertex) {
                    members = cls == AttrClass::Point ? pointsOfVertices(geo, members) : primitivesOfVertices(geo, members);
                } else if (cls == AttrClass::Vertex) {
                    members = of == AttrClass::Point ? verticesOfPoints(geo, members) : verticesOfPrimitives(geo, members);
                } else if (of == AttrClass::Primitive) {
                    members = pointsOfPrimitives(geo, members);
                } else {
                    members = primitivesOfPoints(geo, members);
                }
            }
            for (size_t i = 0; i < n && i < members.size(); ++i) {
                if (members[i]) mask[i] = set;
            }
        }
    });
    return mask;
}

std::string patternOf(std::span<const uint8_t> mask) {
    std::string out;
    size_t i = 0;
    while (i < mask.size()) {
        if (!mask[i]) {
            ++i;
            continue;
        }
        size_t j = i;
        while (j + 1 < mask.size() && mask[j + 1]) ++j;
        if (!out.empty()) out += ' ';
        out += std::to_string(i);
        if (j > i) out += '-' + std::to_string(j);
        i = j + 1;
    }
    return out;
}

std::vector<Edge> edgesOf(const Geometry& geo) {
    std::vector<Edge> out;
    out.reserve(geo.vertexCount());
    for (size_t p = 0; p < geo.primitiveCount(); ++p) {
        const auto pts = geo.primitivePoints(p);
        const size_t m = pts.size();
        if (m < 2) continue;
        const size_t sides = geo.primitiveClosed(p) ? m : m - 1;
        for (size_t k = 0; k < sides; ++k) {
            const uint32_t a = pts[k], b = pts[(k + 1) % m];
            if (a != b) out.push_back(edgeOf(a, b));
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::vector<Edge> selectEdges(std::span<const Edge> edges, std::string_view pattern) {
    std::vector<uint8_t> mask(edges.size(), 0);
    std::vector<uint32_t> path;
    forEachItem(pattern, [&](std::string_view item, uint8_t set) {
        if (item == "*") {
            std::fill(mask.begin(), mask.end(), set);
        } else if (edgeItem(item, path)) {
            for (const Edge& e : edgesAlong(path, edges)) {
                mask[static_cast<size_t>(std::lower_bound(edges.begin(), edges.end(), e) - edges.begin())] = set;
            }
        }
    });
    std::vector<Edge> out;
    for (size_t i = 0; i < edges.size(); ++i) {
        if (mask[i]) out.push_back(edges[i]);
    }
    return out;
}

std::string edgePatternOf(std::span<const Edge> edges) {
    // Paths: from each edge not yet written, on along the next one at its
    // end -- the one to the lowest point -- while there is one.
    std::map<uint32_t, std::vector<size_t>> at;  // the edges at each point
    for (size_t i = 0; i < edges.size(); ++i) {
        at[edges[i].first].push_back(i);
        at[edges[i].second].push_back(i);
    }
    std::vector<uint8_t> written(edges.size(), 0);
    std::string out;
    for (size_t i = 0; i < edges.size(); ++i) {
        if (written[i]) continue;
        written[i] = 1;
        if (!out.empty()) out += ' ';
        out += 'p' + std::to_string(edges[i].first) + '-' + std::to_string(edges[i].second);
        uint32_t end = edges[i].second;
        for (;;) {
            size_t next = edges.size();
            uint32_t to = 0;
            for (const size_t j : at[end]) {
                if (written[j]) continue;
                const uint32_t other = edges[j].first == end ? edges[j].second : edges[j].first;
                if (next == edges.size() || other < to) {
                    next = j;
                    to = other;
                }
            }
            if (next == edges.size()) break;
            written[next] = 1;
            out += '-' + std::to_string(to);
            end = to;
        }
    }
    return out;
}

}  // namespace pg
