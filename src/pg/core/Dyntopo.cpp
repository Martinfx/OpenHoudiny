#include "pg/core/Dyntopo.h"

#include "pg/core/Parallel.h"
#include "pg/core/Spatial.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <iterator>
#include <limits>
#include <queue>
#include <unordered_set>
#include <utility>

namespace pg {
namespace {

constexpr uint32_t kNone = ~0u;

// What a point is.
constexpr uint8_t kGone = 1;    ///< taken out
constexpr uint8_t kKept = 2;    ///< of a primitive not cut -- a line: never taken out
constexpr uint8_t kCorner = 4;  ///< a corner of a border, or where borders meet: stays

// What became of a primitive.
constexpr uint8_t kAsItWas = 0;  ///< kept, not cut
constexpr uint8_t kCut = 1;      ///< cut into triangles
constexpr uint8_t kLeftOut = 2;  ///< it named a point not there

bool finite(const Vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

uint64_t keyOf(uint32_t a, uint32_t b) {
    if (a > b) std::swap(a, b);
    return (static_cast<uint64_t>(a) << 32) | b;
}

float distance2(const Vec3& a, const Vec3& b) {
    const Vec3 d = a - b;
    return dot(d, d);
}

/// How many floats an attribute of this type holds; 0 for integers and strings.
int widthOf(AttrType t) {
    switch (t) {
        case AttrType::Float: return 1;
        case AttrType::Vec2: return 2;
        case AttrType::Vec3: return 3;
        case AttrType::Vec4: return 4;
        default: return 0;
    }
}

/// Beside a long edge, an edge this much longer (squared) and longer than
/// the limit goes too; the limit grows by kEvenGrowth a step away from the
/// dab -- Blender's EVEN_EDGELEN_THRESHOLD and EVEN_GENERATION_SCALE.
constexpr float kEvenLonger = 1.2f;
constexpr float kEvenGrowth = 1.6f;

/// An edge waiting its turn: its length squared, its two points.
using Queued = std::pair<float, uint64_t>;

/// The longest first; of equal ones, those of the lower points.
struct Shorter {
    bool operator()(const Queued& x, const Queued& y) const {
        return x.first < y.first || (x.first == y.first && x.second > y.second);
    }
};

}  // namespace

struct SculptMesh::Grids {
    MovingGrid points;
    BoxGrid triangles;
    Grids(std::span<const Vec3> P, float cell) : points(P, cell), triangles(cell) {}
};

// --- the attributes -----------------------------------------------------------------------

void SculptMesh::Columns::take(const Geometry& geo, AttrClass cls, size_t count) {
    const AttributeSet& set = geo.attributes(cls);
    for (const std::string& name : set.names()) {
        if (cls == AttrClass::Point && name == "P") continue;
        const AttributeArray& a = *set.find(name);
        const int w = widthOf(a.type());
        if (w == 0) continue;  // integers and strings: by `from`
        Numbers n{name, w, std::vector<float>(count * static_cast<size_t>(w), 0.0f)};
        if (const float* src = reinterpret_cast<const float*>(a.rawRead())) {
            std::copy_n(src, std::min(count, a.size()) * static_cast<size_t>(w), n.values.begin());
        }
        numbers.push_back(std::move(n));
    }
    base = static_cast<uint32_t>(count);
    for (const std::string& name : geo.groupNames()) {
        const Group* g = geo.findGroup(name);
        if (g->classOf() != cls) continue;
        Members m{name, std::vector<uint8_t>(count, 0)};
        for (size_t i = 0; i < count; ++i) m.in[i] = g->contains(i) ? 1 : 0;
        groups.push_back(std::move(m));
    }
}

uint32_t SculptMesh::Columns::mix(uint32_t a, uint32_t b) {
    const size_t e = base + more.size();
    for (Numbers& n : numbers) {
        const size_t w = static_cast<size_t>(n.width);
        n.values.resize(n.values.size() + w);
        float* v = n.values.data();
        for (size_t k = 0; k < w; ++k) v[e * w + k] = (v[a * w + k] + v[b * w + k]) * 0.5f;
    }
    more.push_back(sourceOf(a));
    for (Members& m : groups) {
        const uint8_t in = m.in[a] && m.in[b] ? 1 : 0;
        m.in.push_back(in);
    }
    return static_cast<uint32_t>(e);
}

void SculptMesh::Columns::mixInto(uint32_t a, uint32_t b) {
    for (Numbers& n : numbers) {
        const size_t w = static_cast<size_t>(n.width);
        float* v = n.values.data();
        for (size_t k = 0; k < w; ++k) v[a * w + k] = (v[a * w + k] + v[b * w + k]) * 0.5f;
    }
    for (Members& m : groups) m.in[a] = m.in[a] && m.in[b] ? 1 : 0;
}

const SculptMesh::Columns::Numbers* SculptMesh::Columns::find(const std::string& name) const {
    for (const Numbers& n : numbers) {
        if (n.name == name) return &n;
    }
    return nullptr;
}

const SculptMesh::Columns::Members* SculptMesh::Columns::group(const std::string& name) const {
    for (const Members& m : groups) {
        if (m.name == name) return &m;
    }
    return nullptr;
}

// --- made, copied ---------------------------------------------------------------------------

SculptMesh::SculptMesh(const Geometry& geo) : source_(geo) {
    const auto P = geo.positions();
    const size_t n = P.size();
    P_.assign(P.begin(), P.end());
    flags_.assign(n, 0);
    points_ = n;
    pointAttrs_.take(geo, AttrClass::Point, n);
    cornerAttrs_.take(geo, AttrClass::Vertex, geo.vertexCount());

    // Closed polygons cut into fans; the rest kept as they are, their
    // points never taken out -- and a line's points smoothed along it.
    const size_t prims = geo.primitiveCount();
    cut_.assign(prims, kAsItWas);
    std::vector<std::pair<uint32_t, uint32_t>> along;
    for (size_t p = 0; p < prims; ++p) {
        const auto pts = geo.primitivePoints(p);
        const size_t m = pts.size();
        const uint32_t first = static_cast<uint32_t>(geo.primitiveVertexStart(p));
        if (std::any_of(pts.begin(), pts.end(), [&](uint32_t q) { return q >= n; })) {
            cut_[p] = kLeftOut;
            continue;
        }
        if (geo.primitiveClosed(p) && m >= 3) {
            cut_[p] = kCut;
            for (size_t k = 1; k + 1 < m; ++k) {
                const uint32_t a = pts[0], b = pts[k], c = pts[k + 1];
                if (a == b || b == c || a == c) continue;  // no area: left out
                tri_.push_back({a, b, c});
                corner_.push_back({first, static_cast<uint32_t>(first + k), static_cast<uint32_t>(first + k + 1)});
                prim_.push_back(static_cast<uint32_t>(p));
            }
            continue;
        }
        for (size_t k = 0; k < m; ++k) {
            flags_[pts[k]] |= kKept;
            if (k + 1 < m && pts[k] != pts[k + 1]) {
                along.push_back({pts[k], pts[k + 1]});
                along.push_back({pts[k + 1], pts[k]});
            }
        }
    }
    std::sort(along.begin(), along.end());
    along.erase(std::unique(along.begin(), along.end()), along.end());
    lineStart_.assign(n + 1, 0);
    for (const auto& side : along) ++lineStart_[side.first + 1];
    for (size_t i = 0; i < n; ++i) lineStart_[i + 1] += lineStart_[i];
    lineNext_.resize(along.size());
    for (size_t k = 0; k < along.size(); ++k) lineNext_[k] = along[k].second;

    // The triangles round each point, with room for a couple more.
    triangles_ = tri_.size();
    ring_.assign(n, Ring{});
    std::vector<uint32_t> count(n, 0);
    for (const auto& t : tri_) {
        for (const uint32_t q : t) ++count[q];
    }
    size_t at = 0;
    for (size_t i = 0; i < n; ++i) {
        ring_[i].start = static_cast<uint32_t>(at);
        ring_[i].cap = count[i] + 2;
        at += ring_[i].cap;
    }
    pool_.assign(at, 0);
    pooled_ = at;
    for (uint32_t t = 0; t < tri_.size(); ++t) {
        for (const uint32_t q : tri_[t]) pool_[ring_[q].start + ring_[q].count++] = t;
    }

    // A border point on two border edges going on straight within 30
    // degrees moves along them; any other border point is a corner: what
    // was one stays one.
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t ends[2] = {0, 0};
        const int b = borders(i, ends);
        if (b == 0) continue;
        bool straight = false;
        if (b == 2) {
            const Vec3 u = P_[ends[0]] - P_[i], v = P_[ends[1]] - P_[i];
            const float lu = length(u), lv = length(v);
            straight = lu > 1e-12f && lv > 1e-12f && dot(u, v) <= -0.866f * lu * lv;
        }
        if (!straight) flags_[i] |= kCorner;
    }
}

SculptMesh::SculptMesh(const SculptMesh& o)
    : source_(o.source_),
      P_(o.P_),
      flags_(o.flags_),
      tri_(o.tri_),
      corner_(o.corner_),
      prim_(o.prim_),
      cut_(o.cut_),
      lineStart_(o.lineStart_),
      lineNext_(o.lineNext_),
      pointAttrs_(o.pointAttrs_),
      cornerAttrs_(o.cornerAttrs_),
      ring_(o.ring_),
      pool_(o.pool_),
      pooled_(o.pooled_),
      points_(o.points_),
      triangles_(o.triangles_) {}

SculptMesh::~SculptMesh() = default;

// --- the triangles round a point -------------------------------------------------------------

void SculptMesh::ringAdd(uint32_t p, uint32_t t) {
    if (ring_[p].count == ring_[p].cap) ringGrow(p, std::max<uint32_t>(8, ring_[p].cap * 2));
    Ring& r = ring_[p];
    uint32_t* s = pool_.data() + r.start;
    uint32_t k = r.count;
    for (; k > 0 && s[k - 1] > t; --k) s[k] = s[k - 1];
    s[k] = t;
    ++r.count;
}

void SculptMesh::ringRemove(uint32_t p, uint32_t t) {
    Ring& r = ring_[p];
    uint32_t* s = pool_.data() + r.start;
    uint32_t k = 0;
    while (k < r.count && s[k] != t) ++k;
    if (k == r.count) return;
    for (; k + 1 < r.count; ++k) s[k] = s[k + 1];
    --r.count;
}

void SculptMesh::ringClear(uint32_t p) {
    pooled_ -= ring_[p].cap;
    ring_[p] = Ring{};
}

void SculptMesh::ringGrow(uint32_t p, uint32_t cap) {
    // Moved to the end of the pool -- packed first, when most of it is room
    // the rings have moved out of.
    if (pool_.size() > 2 * pooled_ + 4096) packPool();
    Ring& r = ring_[p];
    const size_t start = pool_.size();
    pool_.resize(start + cap);
    std::copy_n(pool_.begin() + r.start, r.count, pool_.begin() + static_cast<std::ptrdiff_t>(start));
    pooled_ += cap - r.cap;
    r.start = static_cast<uint32_t>(start);
    r.cap = cap;
}

void SculptMesh::packPool() {
    std::vector<uint32_t> pool;
    pool.reserve(pooled_);
    for (Ring& r : ring_) {
        const size_t start = pool.size();
        pool.insert(pool.end(), pool_.begin() + r.start, pool_.begin() + r.start + r.count);
        pool.resize(start + r.cap);
        r.start = static_cast<uint32_t>(start);
    }
    pool_ = std::move(pool);
}

// --- asking ---------------------------------------------------------------------------------

bool SculptMesh::meets(uint32_t t, const Vec3& c, float r2) const {
    const auto& v = tri_[t];
    if (v[0] == kNone) return false;
    const Vec3& a = P_[v[0]];
    const Vec3& b = P_[v[1]];
    const Vec3& d = P_[v[2]];
    const Vec3 lo = glm::min(glm::min(a, b), d), hi = glm::max(glm::max(a, b), d);
    const Vec3 off = glm::max(glm::max(lo - c, c - hi), Vec3(0.0f));
    if (!(dot(off, off) < r2)) return false;
    return distance2(nearestOnTriangle(c, a, b, d), c) < r2;
}

void SculptMesh::trianglesNear(const Vec3& c, float r, std::vector<uint32_t>& out) const {
    out.clear();
    const float r2 = r * r;
    if (grids_) {
        std::vector<uint32_t> maybe;
        grids_->triangles.near(c - Vec3(r), c + Vec3(r), maybe);
        for (const uint32_t t : maybe) {
            if (meets(t, c, r2)) out.push_back(t);
        }
        return;
    }
    // Every triangle asked -- on more threads, when there are many.
    const auto chunks = chunkRanges(tri_.size(), size_t(1) << 14);
    if (chunks.size() <= 1) {
        for (uint32_t t = 0; t < tri_.size(); ++t) {
            if (meets(t, c, r2)) out.push_back(t);
        }
        return;
    }
    std::vector<std::vector<uint32_t>> found(chunks.size());
    TaskPool::instance().run(chunks.size(), [&](size_t k) {
        for (size_t t = chunks[k].first; t < chunks[k].second; ++t) {
            if (meets(static_cast<uint32_t>(t), c, r2)) found[k].push_back(static_cast<uint32_t>(t));
        }
    });
    for (const auto& f : found) out.insert(out.end(), f.begin(), f.end());
}

void SculptMesh::neighbours(uint32_t p, std::vector<uint32_t>& out) const {
    out.clear();
    for (const uint32_t t : ring(p)) {
        for (const uint32_t q : tri_[t]) {
            if (q != p) out.push_back(q);
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
}

int SculptMesh::borders(uint32_t p, uint32_t ends[2]) const {
    // The other corners of the triangles round it: one on a single
    // triangle is the other end of a border edge.
    thread_local std::vector<uint32_t> others;
    others.clear();
    for (const uint32_t t : ring(p)) {
        for (const uint32_t q : tri_[t]) {
            if (q != p) others.push_back(q);
        }
    }
    std::sort(others.begin(), others.end());
    int count = 0;
    for (size_t k = 0; k < others.size();) {
        size_t j = k + 1;
        while (j < others.size() && others[j] == others[k]) ++j;
        if (j - k == 1) {
            if (count < 2) ends[count] = others[k];
            ++count;
        }
        k = j;
    }
    return count;
}

bool SculptMesh::hasTriangle(uint32_t a, uint32_t b, uint32_t c) const {
    for (const uint32_t t : ring(a)) {
        if (has(t, b) && has(t, c)) return true;
    }
    return false;
}

void SculptMesh::near(const Vec3& c, float r, std::vector<uint32_t>& out) const {
    if (grids_) grids_->points.near(P_, c, r, out);
    else scanNear(P_, c, r, out);
}

bool SculptMesh::target(std::span<const Vec3> P, uint32_t i, Vec3& out) const {
    thread_local std::vector<uint32_t> others;
    others.clear();
    for (const uint32_t t : ring(i)) {
        for (const uint32_t q : tri_[t]) {
            if (q != i) others.push_back(q);
        }
    }
    std::sort(others.begin(), others.end());
    // Each neighbour once; those on a single triangle are along a border.
    uint32_t ends[2] = {0, 0};
    int border = 0;
    size_t unique = 0;
    for (size_t k = 0; k < others.size();) {
        size_t j = k + 1;
        while (j < others.size() && others[j] == others[k]) ++j;
        if (j - k == 1) {
            if (border < 2) ends[border] = others[k];
            ++border;
        }
        others[unique++] = others[k];
        k = j;
    }
    others.resize(unique);
    if (border > 0) {
        if (border != 2 || (flags_[i] & kCorner)) return false;
        out = (P[ends[0]] + P[ends[1]]) * 0.5f;
        return true;
    }
    if (i + 1 < lineStart_.size() && lineStart_[i + 1] > lineStart_[i]) {
        others.insert(others.end(), lineNext_.begin() + lineStart_[i], lineNext_.begin() + lineStart_[i + 1]);
        std::sort(others.begin(), others.end());
        others.erase(std::unique(others.begin(), others.end()), others.end());
    }
    if (others.size() < 2) return false;
    Vec3 sum;
    for (const uint32_t j : others) sum += P[j];
    out = sum * (1.0f / static_cast<float>(others.size()));
    return true;
}

// --- grids ----------------------------------------------------------------------------------

void SculptMesh::index(float cell) {
    grids_ = std::make_unique<Grids>(P_, std::max(cell, 1e-6f));
    for (uint32_t t = 0; t < tri_.size(); ++t) {
        if (tri_[t][0] != kNone) put(t);
    }
}

void SculptMesh::unindex() { grids_.reset(); }

void SculptMesh::put(uint32_t t) {
    if (!grids_) return;
    const auto& v = tri_[t];
    const Vec3& a = P_[v[0]];
    const Vec3& b = P_[v[1]];
    const Vec3& c = P_[v[2]];
    grids_->triangles.put(t, glm::min(glm::min(a, b), c), glm::max(glm::max(a, b), c));
}

void SculptMesh::moved(uint32_t i) {
    if (!grids_) return;
    grids_->points.moved(i, P_[i]);
    for (const uint32_t t : ring(i)) put(t);
}

void SculptMesh::removeTriangle(uint32_t t) {
    for (const uint32_t q : tri_[t]) ringRemove(q, t);
    tri_[t] = {kNone, kNone, kNone};
    --triangles_;
    if (grids_) grids_->triangles.erase(t);
}

// --- made finer, made coarser ------------------------------------------------------------------

uint32_t SculptMesh::split(uint32_t a, uint32_t b) {
    if (a > b) std::swap(a, b);
    std::vector<uint32_t>& on = work_[0];
    on.clear();
    for (const uint32_t t : ring(a)) {
        if (has(t, b)) on.push_back(t);
    }
    if (on.empty()) return kNone;
    // The new point: halfway, half of each end. On a border edge it is a
    // border point, going on straight.
    const uint32_t m = static_cast<uint32_t>(P_.size());
    P_.push_back((P_[a] + P_[b]) * 0.5f);
    flags_.push_back(0);
    ring_.push_back(Ring{});
    pointAttrs_.mix(a, b);
    ++points_;
    for (const uint32_t t : on) {
        const std::array<uint32_t, 3> v = tri_[t], cv = corner_[t];
        int k = 0;
        while (k < 3 && !((v[k] == a && v[(k + 1) % 3] == b) || (v[k] == b && v[(k + 1) % 3] == a))) ++k;
        if (k == 3) continue;
        const int k1 = (k + 1) % 3, k2 = (k + 2) % 3;
        const uint32_t x = v[k1], w = v[k2];
        // Its corner at the new point: half of those at the ends, integers
        // from the one at the lower point.
        const uint32_t cm = v[k] == a ? cornerAttrs_.mix(cv[k], cv[k1]) : cornerAttrs_.mix(cv[k1], cv[k]);
        // The triangle keeps the half from its first corner of the two; a
        // new one, wound the same way, is the other half.
        tri_[t][static_cast<size_t>(k1)] = m;
        corner_[t][static_cast<size_t>(k1)] = cm;
        ringRemove(x, t);
        ringAdd(m, t);
        const uint32_t t2 = static_cast<uint32_t>(tri_.size());
        const uint32_t polygon = prim_[t];
        tri_.push_back({m, x, w});
        corner_.push_back({cm, cv[static_cast<size_t>(k1)], cv[static_cast<size_t>(k2)]});
        prim_.push_back(polygon);
        ++triangles_;
        ringAdd(m, t2);
        ringAdd(x, t2);
        ringAdd(w, t2);
        put(t);
        put(t2);
    }
    if (grids_) grids_->points.add(m, P_[m]);
    return m;
}

bool SculptMesh::collapse(uint32_t a, uint32_t b, uint32_t& kept) {
    if (a == b || ((flags_[a] | flags_[b]) & (kGone | kKept))) return false;
    // The triangles on the edge: one, along a border, or two.
    uint32_t on[2] = {kNone, kNone};
    int count = 0;
    for (const uint32_t t : ring(a)) {
        if (!has(t, b)) continue;
        if (count == 2) return false;
        on[count++] = t;
    }
    if (count == 0) return false;
    // Which stays, and where.
    uint32_t ends[2] = {0, 0};
    const int ba = borders(a, ends), bb = borders(b, ends);
    uint32_t s = std::min(a, b), g = std::max(a, b);
    bool halfway = true;
    if (count == 1) {
        // Along the border: two ordinary points of it; a corner stays put.
        if (ba != 2 || bb != 2) return false;
        const bool ca = (flags_[a] & kCorner) != 0, cb = (flags_[b] & kCorner) != 0;
        if (ca && cb) return false;
        if (ca || cb) {
            s = ca ? a : b;
            g = ca ? b : a;
            halfway = false;
        }
    } else {
        // Across the inside: not between two border points -- that would
        // pinch the surface; a border point stays put.
        if (ba > 0 && bb > 0) return false;
        if (ba > 0 || bb > 0) {
            s = ba > 0 ? a : b;
            g = ba > 0 ? b : a;
            halfway = false;
        }
    }
    const Vec3 to = halfway ? (P_[a] + P_[b]) * 0.5f : P_[s];

    // The link condition: the two ends share no neighbours but the corners
    // across the edge.
    uint32_t across[2] = {kNone, kNone};
    for (int k = 0; k < count; ++k) {
        for (const uint32_t q : tri_[on[k]]) {
            if (q != a && q != b) across[k] = q;
        }
    }
    if (count == 2 && across[0] == across[1]) return false;
    if (count == 2 && across[0] > across[1]) std::swap(across[0], across[1]);
    std::vector<uint32_t>& ns = work_[0];
    std::vector<uint32_t>& ng = work_[1];
    std::vector<uint32_t>& both = work_[2];
    neighbours(s, ns);
    neighbours(g, ng);
    both.clear();
    std::set_intersection(ns.begin(), ns.end(), ng.begin(), ng.end(), std::back_inserter(both));
    if (both.size() != static_cast<size_t>(count)) return false;
    for (int k = 0; k < count; ++k) {
        if (both[static_cast<size_t>(k)] != across[k]) return false;
    }
    // Not four triangles closed round a point made two, back to back; not
    // the last triangle of a piece.
    if (count == 2 && hasTriangle(s, across[0], across[1]) && hasTriangle(g, across[0], across[1])) return false;
    if (ring(s).size() + ring(g).size() <= static_cast<size_t>(2 * count)) return false;
    // No triangle turned over.
    for (const uint32_t p : {s, g}) {
        if (p == s && !halfway) continue;  // stays where it is: so do its triangles
        for (const uint32_t t : ring(p)) {
            if (t == on[0] || t == on[1]) continue;
            const auto& v = tri_[t];
            const Vec3 n0 = cross(P_[v[1]] - P_[v[0]], P_[v[2]] - P_[v[0]]);
            Vec3 q[3];
            for (int k = 0; k < 3; ++k) q[k] = v[static_cast<size_t>(k)] == s || v[static_cast<size_t>(k)] == g ? to : P_[v[static_cast<size_t>(k)]];
            const Vec3 n1 = cross(q[1] - q[0], q[2] - q[0]);
            if (dot(n0, n0) > 0.0f && !(dot(n0, n1) > 0.0f)) return false;
        }
    }

    // The triangles on the edge go; the other end's are the one kept's.
    for (int k = 0; k < count; ++k) removeTriangle(on[k]);
    std::vector<uint32_t>& moving = work_[0];
    const auto rg = ring(g);
    moving.assign(rg.begin(), rg.end());
    for (const uint32_t t : moving) {
        for (uint32_t& q : tri_[t]) {
            if (q == g) q = s;
        }
        ringAdd(s, t);
    }
    ringClear(g);
    if (halfway) pointAttrs_.mixInto(s, g);
    P_[s] = to;
    flags_[g] |= kGone;
    P_[g] = Vec3(std::numeric_limits<float>::quiet_NaN());
    --points_;
    if (grids_) {
        grids_->points.erase(g);
        grids_->points.moved(s, P_[s]);
    }
    for (const uint32_t t : ring(s)) put(t);
    kept = s;
    return true;
}

void SculptMesh::refine(const Vec3& c, float r, const Dyntopo& d) {
    if (!d.on() || !(r > 0.0f) || !finite(c)) return;
    const float longest = d.longest(r);
    if (!(longest > 0.0f) || !std::isfinite(longest)) return;
    const float r2 = r * r;
    std::vector<uint32_t> near;
    if (d.collapse) {
        // The shortest first; an edge whose ends moved since it was put in
        // waits again, by its length now.
        const float least = kDyntopoShortest * longest, least2 = least * least;
        std::priority_queue<Queued, std::vector<Queued>, std::greater<Queued>> queue;
        const auto offer = [&](uint32_t t) {
            const auto& v = tri_[t];
            for (int k = 0; k < 3; ++k) {
                const uint32_t p = v[static_cast<size_t>(k)], q = v[static_cast<size_t>((k + 1) % 3)];
                const float l2 = distance2(P_[p], P_[q]);
                if (l2 < least2) queue.push({l2, keyOf(p, q)});
            }
        };
        trianglesNear(c, r, near);
        for (const uint32_t t : near) offer(t);
        while (!queue.empty()) {
            const Queued e = queue.top();
            queue.pop();
            const uint32_t a = static_cast<uint32_t>(e.second >> 32), b = static_cast<uint32_t>(e.second);
            if ((flags_[a] | flags_[b]) & kGone) continue;
            const float l2 = distance2(P_[a], P_[b]);
            if (!(l2 < least2)) continue;
            if (l2 != e.first) {
                queue.push({l2, e.second});
                continue;
            }
            uint32_t kept = kNone;
            if (!collapse(a, b, kept)) continue;
            for (const uint32_t t : ring(kept)) {
                if (meets(t, c, r2)) offer(t);
            }
        }
    }
    if (d.subdivide && triangles_ < kDyntopoMostTriangles) {
        // The longest first, split in half; the halves, and the triangles
        // made, wait their turn if they are long still. With a long edge go
        // the edges beside it longer still -- by more the further from the
        // dab, as Blender has it -- so the triangles get bigger evenly away
        // from the brush, not as long thin ones.
        const float most2 = longest * longest;
        std::priority_queue<Queued, std::vector<Queued>, Shorter> queue;
        std::unordered_set<uint64_t> queued;
        struct Offered {
            uint32_t a, b;
            float l2, limit;
        };
        std::vector<Offered> stack;
        const auto offerEdge = [&](uint32_t a, uint32_t b, float l2) {
            stack.push_back({a, b, l2, longest});
            while (!stack.empty()) {
                const Offered e = stack.back();
                stack.pop_back();
                const uint64_t key = keyOf(e.a, e.b);
                if (!queued.insert(key).second) continue;
                queue.push({e.l2, key});
                const float limit = e.limit * kEvenGrowth;
                const float than = std::max(e.l2 * kEvenLonger, limit * limit);
                for (const uint32_t t : ring(e.a)) {
                    if (!has(t, e.b)) continue;
                    const auto& v = tri_[t];
                    for (size_t k = 0; k < 3; ++k) {
                        const uint32_t p = v[k], q = v[(k + 1) % 3];
                        if (keyOf(p, q) == key) continue;
                        const float l = distance2(P_[p], P_[q]);
                        if (l > than && std::isfinite(l)) stack.push_back({p, q, l, limit});
                    }
                }
            }
        };
        const auto offer = [&](uint32_t t) {
            const auto& v = tri_[t];
            for (size_t k = 0; k < 3; ++k) {
                const uint32_t p = v[k], q = v[(k + 1) % 3];
                const float l2 = distance2(P_[p], P_[q]);
                if (l2 > most2 && std::isfinite(l2)) offerEdge(p, q, l2);
            }
        };
        trianglesNear(c, r, near);
        std::sort(near.begin(), near.end());  // offered in an order that does not depend on how they were found
        for (const uint32_t t : near) offer(t);
        size_t splits = 0;
        while (!queue.empty() && splits < kDyntopoMostSplits && triangles_ < kDyntopoMostTriangles) {
            const Queued e = queue.top();
            queue.pop();
            const uint32_t m = split(static_cast<uint32_t>(e.second >> 32), static_cast<uint32_t>(e.second));
            if (m == kNone) continue;  // split already
            ++splits;
            for (const uint32_t t : ring(m)) {
                if (meets(t, c, r2)) offer(t);
            }
        }
    }
}

// --- a geometry again -----------------------------------------------------------------------

std::shared_ptr<Geometry> SculptMesh::geometry() const {
    const Geometry& src = source_;
    auto out = std::make_shared<Geometry>();

    // The points left, in order.
    std::vector<uint32_t> number(P_.size(), kNone), kept(points_);
    {
        size_t k = 0;
        for (uint32_t i = 0; i < P_.size(); ++i) {
            if (flags_[i] & kGone) continue;
            number[i] = static_cast<uint32_t>(k);
            kept[k++] = i;
        }
    }
    out->addPoints(kept.size());
    {
        const auto Q = out->positionsForWrite();
        for (size_t k = 0; k < kept.size(); ++k) Q[k] = P_[kept[k]];
    }

    // Each primitive in its place: a polygon as its triangles, in the
    // order they were made; the rest as they were. Where each one's
    // primitives and corners begin, then each triangle put in its place.
    const size_t prims = src.primitiveCount();
    std::vector<uint32_t> primAt(prims + 1, 0), cornerAt(prims + 1, 0);
    for (uint32_t t = 0; t < tri_.size(); ++t) {
        if (tri_[t][0] != kNone) ++primAt[prim_[t] + 1];
    }
    for (size_t p = 0; p < prims; ++p) {
        const uint32_t made = primAt[p + 1];
        uint32_t corners = 3 * made;
        if (cut_[p] == kAsItWas) {
            primAt[p + 1] = 1;
            corners = static_cast<uint32_t>(src.primitiveVertexCount(p));
        }
        primAt[p + 1] += primAt[p];
        cornerAt[p + 1] = cornerAt[p] + corners;
    }
    const size_t outPrims = primAt[prims], outCorners = cornerAt[prims];
    // Which corner and which primitive of the geometry each is: only where
    // there are attributes or groups to carry.
    bool needCorners = src.vertices().count() > 0, needFrom = src.primitives().count() > 0;
    for (const std::string& name : src.groupNames()) {
        needCorners = needCorners || src.findGroup(name)->classOf() == AttrClass::Vertex;
        needFrom = needFrom || src.findGroup(name)->classOf() == AttrClass::Primitive;
    }
    std::vector<uint32_t> points(outCorners), counts(outPrims, 3), corners(needCorners ? outCorners : 0),
        from(needFrom ? outPrims : 0);
    std::vector<uint8_t> closed(outPrims, 1);
    std::vector<uint32_t> next(primAt.begin(), primAt.end() - 1);
    for (uint32_t t = 0; t < tri_.size(); ++t) {
        if (tri_[t][0] == kNone) continue;
        const uint32_t p = prim_[t];
        const uint32_t o = next[p]++;
        const size_t c0 = cornerAt[p] + 3 * static_cast<size_t>(o - primAt[p]);
        for (size_t c = 0; c < 3; ++c) points[c0 + c] = number[tri_[t][c]];
        if (needCorners) {
            for (size_t c = 0; c < 3; ++c) corners[c0 + c] = corner_[t][c];
        }
        if (needFrom) from[o] = p;
    }
    for (size_t p = 0; p < prims; ++p) {
        if (cut_[p] != kAsItWas) continue;
        const uint32_t o = primAt[p];
        const auto pts = src.primitivePoints(p);
        const uint32_t v0 = static_cast<uint32_t>(src.primitiveVertexStart(p));
        for (size_t k = 0; k < pts.size(); ++k) {
            points[cornerAt[p] + k] = number[pts[k]];
            if (needCorners) corners[cornerAt[p] + k] = v0 + static_cast<uint32_t>(k);
        }
        counts[o] = static_cast<uint32_t>(pts.size());
        closed[o] = src.primitiveClosed(p) ? 1 : 0;
        if (needFrom) from[o] = static_cast<uint32_t>(p);
    }
    out->addPrimitives(points, counts, closed);

    // Attributes: numbers as they are here; integers and strings from the
    // element of the geometry each is from.
    const auto carry = [](const AttributeSet& in, AttributeSet& to, const Columns& cols,
                          std::span<const uint32_t> elements, bool positions) {
        std::vector<uint32_t> source;
        for (const std::string& name : in.names()) {
            if (positions && name == "P") continue;
            const AttributeArray& a = *in.find(name);
            if (const Columns::Numbers* n = cols.find(name)) {
                AttributeArray& o = to.create(name, a.type());
                float* dst = reinterpret_cast<float*>(o.rawWrite());
                const size_t w = static_cast<size_t>(n->width);
                for (size_t k = 0; k < elements.size(); ++k) std::copy_n(n->values.data() + elements[k] * w, w, dst + k * w);
                continue;
            }
            if (source.empty() && !elements.empty()) {
                source.resize(elements.size());
                for (size_t k = 0; k < elements.size(); ++k) source[k] = cols.sourceOf(elements[k]);
            }
            to.assign(name, a.gather(source));
        }
    };
    carry(src.points(), out->points(), pointAttrs_, kept, true);
    if (needCorners) carry(src.vertices(), out->vertices(), cornerAttrs_, corners, false);
    for (const std::string& name : src.primitives().names()) out->primitives().assign(name, src.primitives().find(name)->gather(from));

    for (const std::string& name : src.groupNames()) {
        const Group* g = src.findGroup(name);
        Group& o = out->createGroup(name, g->classOf());
        uint8_t* mask = o.writableMask();
        if (g->classOf() == AttrClass::Point) {
            const Columns::Members* m = pointAttrs_.group(name);
            for (size_t k = 0; k < kept.size() && m; ++k) mask[k] = m->in[kept[k]];
        } else if (g->classOf() == AttrClass::Vertex) {
            const Columns::Members* m = cornerAttrs_.group(name);
            for (size_t k = 0; k < corners.size() && m; ++k) mask[k] = m->in[corners[k]];
        } else if (g->classOf() == AttrClass::Primitive) {
            for (size_t k = 0; k < from.size(); ++k) mask[k] = g->contains(from[k]) ? 1 : 0;
        } else if (g->contains(0)) {
            o.set(0, true);
        }
    }
    out->detail() = src.detail();
    for (const Volume& v : src.volumes()) out->addVolume(v);
    for (const auto& prototype : src.prototypes()) out->addPrototype(prototype);
    return out;
}

}  // namespace pg
