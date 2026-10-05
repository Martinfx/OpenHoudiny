#include "pg/core/Spatial.h"

#include "pg/core/Geometry.h"
#include "pg/core/Parallel.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace pg {

namespace {

constexpr uint32_t kLeaf = 8;

float dist2(const Vec3& a, const Vec3& b) {
    const Vec3 d = a - b;
    return dot(d, d);
}

bool closer(const std::pair<float, int32_t>& a, const std::pair<float, int32_t>& b) {
    return a.first < b.first || (a.first == b.first && a.second < b.second);
}


/// How far `p` is from the box, squared; 0 inside.
float boxDistance2(const Vec3& p, const Vec3& lo, const Vec3& hi) {
    const Vec3 d = glm::max(glm::max(lo - p, p - hi), Vec3(0.0f));
    return dot(d, d);
}

/// Whether the segment from `a` to `b` meets the box (by its slabs).
bool segmentMeetsBox(const Vec3& a, const Vec3& b, const Vec3& lo, const Vec3& hi) {
    const Vec3 d = b - a;
    float t0 = 0.0f, t1 = 1.0f;
    for (int i = 0; i < 3; ++i) {
        if (std::fabs(d[i]) < 1e-12f) {
            if (a[i] < lo[i] || a[i] > hi[i]) return false;
            continue;
        }
        float u = (lo[i] - a[i]) / d[i], v = (hi[i] - a[i]) / d[i];
        if (u > v) std::swap(u, v);
        t0 = std::max(t0, u);
        t1 = std::min(t1, v);
        if (t0 > t1) return false;
    }
    return true;
}

/// Whether the segment from `a` to `b` passes through the triangle (Moller
/// and Trumbore): where it meets its plane inside it, between the ends.
bool segmentMeetsTriangle(const Vec3& a, const Vec3& b, const std::array<Vec3, 3>& tri) {
    const Vec3 d = b - a, e1 = tri[1] - tri[0], e2 = tri[2] - tri[0];
    const Vec3 h = cross(d, e2);
    const float det = dot(e1, h);
    if (std::fabs(det) < 1e-12f) return false;  // along its plane
    const float inv = 1.0f / det;
    const Vec3 s = a - tri[0];
    const float u = dot(s, h) * inv;
    if (u < 0.0f || u > 1.0f) return false;
    const Vec3 q = cross(s, e1);
    const float v = dot(d, q) * inv;
    if (v < 0.0f || u + v > 1.0f) return false;
    const float t = dot(e2, q) * inv;
    return t >= 0.0f && t <= 1.0f;
}

}  // namespace

// By the region of the triangle's plane p is over.
Vec3 nearestOnTriangle(const Vec3& p, const Vec3& a, const Vec3& b, const Vec3& c) {
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
    const float denom = 1.0f / (va + vb + vc);
    return a + ab * (vb * denom) + ac * (vc * denom);
}

void PointTree::build(std::span<const Vec3> points) {
    points_.assign(points.begin(), points.end());
    order_.resize(points_.size());
    for (size_t i = 0; i < order_.size(); ++i) order_[i] = static_cast<int32_t>(i);
    nodes_.clear();
    if (!points_.empty()) {
        nodes_.reserve(2 * points_.size() / kLeaf + 2);
        make(0, static_cast<uint32_t>(points_.size()), 0);
    }
}

uint32_t PointTree::make(uint32_t lo, uint32_t hi, int depth) {
    const auto index = static_cast<uint32_t>(nodes_.size());
    nodes_.push_back({});
    nodes_[index].lo = lo;
    nodes_[index].hi = hi;
    if (hi - lo <= kLeaf || depth > 60) return index;
    // Split along the longest side of the points' box, at the median.
    Vec3 mn(std::numeric_limits<float>::max()), mx(-std::numeric_limits<float>::max());
    for (uint32_t i = lo; i < hi; ++i) {
        const Vec3& p = points_[static_cast<size_t>(order_[i])];
        for (int a = 0; a < 3; ++a) {
            mn[a] = std::min(mn[a], p[a]);
            mx[a] = std::max(mx[a], p[a]);
        }
    }
    int axis = 0;
    for (int a = 1; a < 3; ++a) {
        if (mx[a] - mn[a] > mx[axis] - mn[axis]) axis = a;
    }
    if (!(mx[axis] - mn[axis] > 0.0f)) return index;  // all in one place: a leaf
    const uint32_t mid = lo + (hi - lo) / 2;
    std::nth_element(order_.begin() + lo, order_.begin() + mid, order_.begin() + hi, [&](int32_t a, int32_t b) {
        const float pa = points_[static_cast<size_t>(a)][axis], pb = points_[static_cast<size_t>(b)][axis];
        return pa < pb || (pa == pb && a < b);
    });
    const float split = points_[static_cast<size_t>(order_[mid])][axis];
    const uint32_t left = make(lo, mid, depth + 1);
    const uint32_t right = make(mid, hi, depth + 1);
    nodes_[index].axis = static_cast<int8_t>(axis);
    nodes_[index].split = split;
    nodes_[index].left = left;
    nodes_[index].right = right;
    return index;
}

void PointTree::search(uint32_t n, const Vec3& p, float r2, size_t max,
                       std::vector<std::pair<float, int32_t>>& best) const {
    const Node& node = nodes_[n];
    if (node.axis < 0) {
        for (uint32_t i = node.lo; i < node.hi; ++i) {
            const int32_t idx = order_[i];
            const float d = dist2(points_[static_cast<size_t>(idx)], p);
            if (d > r2) continue;
            const std::pair<float, int32_t> c{d, idx};
            if (max == 0 || best.size() < max) {
                best.push_back(c);
                if (max) std::push_heap(best.begin(), best.end(), closer);
            } else if (closer(c, best.front())) {
                std::pop_heap(best.begin(), best.end(), closer);
                best.back() = c;
                std::push_heap(best.begin(), best.end(), closer);
            }
        }
        return;
    }
    const float delta = p[node.axis] - node.split;
    const uint32_t first = delta < 0.0f ? node.left : node.right;
    const uint32_t second = delta < 0.0f ? node.right : node.left;
    search(first, p, r2, max, best);
    // The other side, if it can hold anything nearer.
    float bound = r2;
    if (max && best.size() == max) bound = std::min(bound, best.front().first);
    if (delta * delta <= bound) search(second, p, r2, max, best);
}

void PointTree::near(const Vec3& p, float radius, size_t max, std::vector<int32_t>& out) const {
    out.clear();
    if (nodes_.empty()) return;
    const float r2 = radius < 0.0f ? std::numeric_limits<float>::infinity() : radius * radius;
    std::vector<std::pair<float, int32_t>> best;
    search(0, p, r2, max, best);
    std::sort(best.begin(), best.end(), closer);
    out.reserve(best.size());
    for (const auto& b : best) out.push_back(b.second);
}

int32_t PointTree::nearest(const Vec3& p, float radius) const {
    std::vector<int32_t> one;
    near(p, radius, 1, one);
    return one.empty() ? -1 : one.front();
}

// --- adjacency -------------------------------------------------------------------------------

void TriangleTree::build(const Geometry& geo) {
    triangles_.clear();
    order_.clear();
    nodes_.clear();
    const auto P = geo.positions();
    for (size_t prim = 0; prim < geo.primitiveCount(); ++prim) {
        if (!geo.primitiveClosed(prim)) continue;
        const auto pts = geo.primitivePoints(prim);
        for (size_t k = 1; k + 1 < pts.size(); ++k) {
            if (pts[0] < P.size() && pts[k] < P.size() && pts[k + 1] < P.size()) {
                triangles_.push_back({P[pts[0]], P[pts[k]], P[pts[k + 1]]});
            }
        }
    }
    if (triangles_.empty()) return;
    order_.resize(triangles_.size());
    for (size_t i = 0; i < order_.size(); ++i) order_[i] = static_cast<uint32_t>(i);
    nodes_.reserve(triangles_.size() / 2 + 1);
    make(0, static_cast<uint32_t>(triangles_.size()));
}

uint32_t TriangleTree::make(uint32_t first, uint32_t count) {
    const auto index = static_cast<uint32_t>(nodes_.size());
    nodes_.push_back({});
    Vec3 lo(std::numeric_limits<float>::max()), hi(-std::numeric_limits<float>::max());
    Vec3 mlo = lo, mhi = hi;  // round their middles
    auto middle = [&](uint32_t t) { return (triangles_[t][0] + triangles_[t][1] + triangles_[t][2]) / 3.0f; };
    for (uint32_t i = first; i < first + count; ++i) {
        for (const Vec3& v : triangles_[order_[i]]) lo = glm::min(lo, v), hi = glm::max(hi, v);
        const Vec3 m = middle(order_[i]);
        mlo = glm::min(mlo, m), mhi = glm::max(mhi, m);
    }
    nodes_[index].lo = lo;
    nodes_[index].hi = hi;
    if (count <= 4) {
        nodes_[index].first = first;
        nodes_[index].count = count;
        return index;
    }
    // Halved along the longest side their middles span; equal ones by number.
    const Vec3 span = mhi - mlo;
    const int axis = span.x >= span.y && span.x >= span.z ? 0 : span.y >= span.z ? 1 : 2;
    std::sort(order_.begin() + first, order_.begin() + first + count, [&](uint32_t a, uint32_t b) {
        const float ma = middle(a)[axis], mb = middle(b)[axis];
        return ma < mb || (ma == mb && a < b);
    });
    const uint32_t half = count / 2;
    const uint32_t left = make(first, half);
    const uint32_t right = make(first + half, count - half);
    nodes_[index].left = left;
    nodes_[index].right = right;
    return index;
}

bool TriangleTree::nearest(const Vec3& p, float radius, Vec3& at) const {
    if (nodes_.empty() || !(radius >= 0.0f)) return false;
    float best = radius * radius;
    bool found = false;
    uint32_t stack[96];
    int top = 0;
    stack[top++] = 0;
    while (top > 0) {
        const Node& n = nodes_[stack[--top]];
        if (boxDistance2(p, n.lo, n.hi) > best) continue;
        if (n.count > 0) {
            for (uint32_t i = n.first; i < n.first + n.count; ++i) {
                const auto& t = triangles_[order_[i]];
                const Vec3 q = nearestOnTriangle(p, t[0], t[1], t[2]);
                const float d2 = dist2(p, q);
                if (d2 < best || (!found && d2 <= best)) best = d2, at = q, found = true;
            }
            continue;
        }
        // The nearer child first: off the stack first.
        const float dl = boxDistance2(p, nodes_[n.left].lo, nodes_[n.left].hi);
        const float dr = boxDistance2(p, nodes_[n.right].lo, nodes_[n.right].hi);
        if (top + 2 > 96) continue;
        if (dl <= dr) {
            stack[top++] = n.right;
            stack[top++] = n.left;
        } else {
            stack[top++] = n.left;
            stack[top++] = n.right;
        }
    }
    return found;
}

bool TriangleTree::crosses(const Vec3& a, const Vec3& b) const {
    if (nodes_.empty()) return false;
    uint32_t stack[96];
    int top = 0;
    stack[top++] = 0;
    while (top > 0) {
        const Node& n = nodes_[stack[--top]];
        if (!segmentMeetsBox(a, b, n.lo, n.hi)) continue;
        if (n.count > 0) {
            for (uint32_t i = n.first; i < n.first + n.count; ++i) {
                if (segmentMeetsTriangle(a, b, triangles_[order_[i]])) return true;
            }
            continue;
        }
        if (top + 2 > 96) continue;
        stack[top++] = n.left;
        stack[top++] = n.right;
    }
    return false;
}

void Adjacency::build(const Geometry& geo) {
    // Two passes over the primitives -- how many entries each point gets,
    // then the entries -- into one array per list rather than a vector per
    // point; then each point's entries sorted, made unique and packed. A
    // million points take tens of milliseconds, not half a second.
    const size_t np = geo.pointCount(), nprim = geo.primitiveCount();
    const auto visit = [&](auto&& side, auto&& corner) {
        for (size_t prim = 0; prim < nprim; ++prim) {
            const auto pts = geo.primitivePoints(prim);
            const size_t count = pts.size();
            const size_t start = geo.primitiveVertexStart(prim);
            const bool closed = geo.primitiveClosed(prim) && count > 2;
            for (size_t i = 0; i < count; ++i) {
                const uint32_t a = pts[i];
                if (a >= np) continue;
                corner(a, prim, start + i);
                if (i + 1 < count || closed) {
                    const uint32_t b = pts[(i + 1) % count];
                    if (b < np && b != a) side(a, b);
                }
            }
        }
    };
    // Where each point's entries begin, as many as it gets with repeats.
    std::vector<size_t> nAt(np + 1, 0), pAt(np + 1, 0), vAt(np + 1, 0);
    visit(
        [&](uint32_t a, uint32_t b) {
            ++nAt[a + 1];
            ++nAt[b + 1];
        },
        [&](uint32_t a, size_t, size_t) {
            ++pAt[a + 1];
            ++vAt[a + 1];
        });
    for (size_t i = 0; i < np; ++i) {
        nAt[i + 1] += nAt[i];
        pAt[i + 1] += pAt[i];
        vAt[i + 1] += vAt[i];
    }
    std::vector<int32_t> nAll(nAt[np]), pAll(pAt[np]), vAll(vAt[np]);
    {
        std::vector<size_t> nNext(nAt.begin(), nAt.end() - 1), pNext(pAt.begin(), pAt.end() - 1),
            vNext(vAt.begin(), vAt.end() - 1);
        visit(
            [&](uint32_t a, uint32_t b) {
                nAll[nNext[a]++] = static_cast<int32_t>(b);
                nAll[nNext[b]++] = static_cast<int32_t>(a);
            },
            [&](uint32_t a, size_t prim, size_t vertex) {
                pAll[pNext[a]++] = static_cast<int32_t>(prim);
                vAll[vNext[a]++] = static_cast<int32_t>(vertex);
            });
    }
    const auto pack = [&](const std::vector<size_t>& at, std::vector<int32_t>& all, std::vector<uint32_t>& start,
                          std::vector<int32_t>& flat) {
        start.assign(np + 1, 0);
        size_t kept = 0;
        for (size_t i = 0; i < np; ++i) {
            // Packed in place: what is kept never lies after what is read.
            int32_t* first = all.data() + at[i];
            int32_t* last = all.data() + at[i + 1];
            // Mostly a few entries, and those of the primitives and the
            // vertices come in order already.
            if (last - first > 16) {
                std::sort(first, last);
            } else {
                for (int32_t* k = first + 1; k < last; ++k) {
                    const int32_t v = *k;
                    int32_t* j = k;
                    for (; j > first && *(j - 1) > v; --j) *j = *(j - 1);
                    *j = v;
                }
            }
            last = std::unique(first, last);
            start[i] = static_cast<uint32_t>(kept);
            if (all.data() + kept != first) std::copy(first, last, all.data() + kept);
            kept += static_cast<size_t>(last - first);
        }
        start[np] = static_cast<uint32_t>(kept);
        all.resize(kept);
        all.shrink_to_fit();
        flat = std::move(all);
    };
    pack(nAt, nAll, nStart_, nList_);
    pack(pAt, pAll, pStart_, pList_);
    pack(vAt, vAll, vStart_, vList_);
}

std::span<const int32_t> Adjacency::neighbours(size_t point) const {
    if (point + 1 >= nStart_.size()) return {};
    return std::span<const int32_t>(nList_.data() + nStart_[point], nStart_[point + 1] - nStart_[point]);
}

std::span<const int32_t> Adjacency::primitives(size_t point) const {
    if (point + 1 >= pStart_.size()) return {};
    return std::span<const int32_t>(pList_.data() + pStart_[point], pStart_[point + 1] - pStart_[point]);
}

std::span<const int32_t> Adjacency::vertices(size_t point) const {
    if (point + 1 >= vStart_.size()) return {};
    return std::span<const int32_t>(vList_.data() + vStart_[point], vStart_[point + 1] - vStart_[point]);
}

// --- places that change ---------------------------------------------------------------

namespace {

constexpr uint64_t kNoCell = ~uint64_t(0);

bool finitePlace(const Vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

/// The cell `v` is in, of cells 1 / `inv` big.
int64_t cellIndex(double v, double inv) {
    const double x = std::floor(v * inv);
    return std::isfinite(x) ? static_cast<int64_t>(std::clamp(x, -1e15, 1e15)) : 0;
}

/// A cell's key: 21 bits a side, so cells 2^21 apart share one.
uint64_t cellKey(int64_t x, int64_t y, int64_t z) {
    constexpr uint64_t m = (1ull << 21) - 1;
    return (static_cast<uint64_t>(x) & m) | ((static_cast<uint64_t>(y) & m) << 21) | ((static_cast<uint64_t>(z) & m) << 42);
}

}  // namespace

MovingGrid::MovingGrid(std::span<const Vec3> P, float cell) : inv_(1.0 / static_cast<double>(cell)) {
    cellOf_.assign(P.size(), kNoCell);
    slot_.resize(P.size());
    for (size_t i = 0; i < P.size(); ++i) {
        if (finitePlace(P[i])) insert(static_cast<uint32_t>(i), key(P[i]));
    }
}

void MovingGrid::moved(uint32_t i, const Vec3& p) {
    const uint64_t k = finitePlace(p) ? key(p) : kNoCell;
    if (k == cellOf_[i]) return;
    if (cellOf_[i] != kNoCell) remove(i);
    if (k != kNoCell) insert(i, k);
}

void MovingGrid::add(uint32_t i, const Vec3& p) {
    if (i >= cellOf_.size()) {
        cellOf_.resize(i + 1, kNoCell);
        slot_.resize(i + 1);
    }
    moved(i, p);
}

void MovingGrid::erase(uint32_t i) {
    if (i < cellOf_.size() && cellOf_[i] != kNoCell) remove(i);
}

void MovingGrid::near(std::span<const Vec3> P, const Vec3& c, float r, std::vector<uint32_t>& out) const {
    out.clear();
    const float r2 = r * r;
    int64_t lo[3], hi[3];
    double cells = 1.0;
    bool shared = false;
    for (int k = 0; k < 3; ++k) {
        // A little further than the radius: no point the test below
        // lets in is missed by how the cell's number rounds.
        const float reach = r + 1e-5f * (std::fabs(c[k]) + r);
        lo[k] = index(c[k] - reach);
        hi[k] = index(c[k] + reach);
        cells *= static_cast<double>(hi[k] - lo[k] + 1);
        shared = shared || hi[k] - lo[k] + 1 >= (int64_t(1) << 21);
    }
    if (cells > 4.0 * static_cast<double>(P.size()) + 64.0) {
        // More cells than points: every point, once.
        for (size_t i = 0; i < P.size(); ++i) {
            const Vec3 d = P[i] - c;
            if (dot(d, d) < r2) out.push_back(static_cast<uint32_t>(i));
        }
        return;
    }
    for (int64_t x = lo[0]; x <= hi[0]; ++x) {
        for (int64_t y = lo[1]; y <= hi[1]; ++y) {
            for (int64_t z = lo[2]; z <= hi[2]; ++z) {
                const auto it = cells_.find(cellKey(x, y, z));
                if (it == cells_.end()) continue;
                for (const uint32_t i : it->second) {
                    const Vec3 d = P[i] - c;
                    if (dot(d, d) < r2) out.push_back(i);
                }
            }
        }
    }
    if (shared) {
        // Cells 2^21 apart share a key, and were both asked: each point once.
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
    }
}

int64_t MovingGrid::index(float v) const { return cellIndex(static_cast<double>(v), inv_); }

uint64_t MovingGrid::key(const Vec3& p) const { return cellKey(index(p.x), index(p.y), index(p.z)); }

void MovingGrid::insert(uint32_t i, uint64_t k) {
    std::vector<uint32_t>& v = cells_[k];
    slot_[i] = static_cast<uint32_t>(v.size());
    v.push_back(i);
    cellOf_[i] = k;
}

void MovingGrid::remove(uint32_t i) {
    std::vector<uint32_t>& v = cells_[cellOf_[i]];
    const uint32_t last = v.back();
    v[slot_[i]] = last;
    slot_[last] = slot_[i];
    v.pop_back();
    cellOf_[i] = kNoCell;
}

void scanNear(std::span<const Vec3> P, const Vec3& c, float r, std::vector<uint32_t>& out) {
    out.clear();
    const float r2 = r * r;
    const auto chunks = chunkRanges(P.size(), size_t(1) << 15);
    if (chunks.size() <= 1) {
        for (size_t i = 0; i < P.size(); ++i) {
            const Vec3 d = P[i] - c;
            if (dot(d, d) < r2) out.push_back(static_cast<uint32_t>(i));
        }
        return;
    }
    std::vector<std::vector<uint32_t>> found(chunks.size());
    TaskPool::instance().run(chunks.size(), [&](size_t k) {
        for (size_t i = chunks[k].first; i < chunks[k].second; ++i) {
            const Vec3 d = P[i] - c;
            if (dot(d, d) < r2) found[k].push_back(static_cast<uint32_t>(i));
        }
    });
    for (const auto& f : found) out.insert(out.end(), f.begin(), f.end());
}

BoxGrid::BoxGrid(float cell)
    : cell_(std::max(static_cast<double>(cell), 1e-9)), levels_(kLevels), counts_(kLevels, 0) {}

void BoxGrid::put(uint32_t i, const Vec3& lo, const Vec3& hi) {
    if (i >= where_.size()) where_.resize(i + 1);
    // The finest grid whose cells are no smaller than the box.
    const double size = std::max({static_cast<double>(hi.x) - lo.x, static_cast<double>(hi.y) - lo.y,
                                  static_cast<double>(hi.z) - lo.z, 0.0});
    int level = 0;
    double s = cell_;
    while (level + 1 < kLevels && !(s >= size)) {
        s *= 2.0;
        ++level;
    }
    const double inv = 1.0 / s;
    const uint64_t key = cellKey(cellIndex(0.5 * (static_cast<double>(lo.x) + hi.x), inv),
                                 cellIndex(0.5 * (static_cast<double>(lo.y) + hi.y), inv),
                                 cellIndex(0.5 * (static_cast<double>(lo.z) + hi.z), inv));
    if (where_[i].level == level && where_[i].key == key) return;
    if (where_[i].level >= 0) remove(i);
    std::vector<uint32_t>& v = levels_[static_cast<size_t>(level)][key];
    where_[i] = {static_cast<int8_t>(level), static_cast<uint32_t>(v.size()), key};
    v.push_back(i);
    ++counts_[static_cast<size_t>(level)];
}

void BoxGrid::erase(uint32_t i) {
    if (i < where_.size() && where_[i].level >= 0) remove(i);
}

void BoxGrid::remove(uint32_t i) {
    const Where w = where_[i];
    auto& level = levels_[static_cast<size_t>(w.level)];
    const auto it = level.find(w.key);
    std::vector<uint32_t>& v = it->second;
    const uint32_t last = v.back();
    v[w.slot] = last;
    where_[last].slot = w.slot;
    v.pop_back();
    if (v.empty()) level.erase(it);
    --counts_[static_cast<size_t>(w.level)];
    where_[i].level = -1;
}

void BoxGrid::near(const Vec3& lo, const Vec3& hi, std::vector<uint32_t>& out) const {
    out.clear();
    bool shared = false;
    double s = cell_;
    for (int l = 0; l < kLevels; ++l, s *= 2.0) {
        if (counts_[static_cast<size_t>(l)] == 0) continue;
        // A box no bigger than a cell has its middle within half a cell of
        // anything it meets (and a hair more, for the rounding).
        const double inv = 1.0 / s, reach = 0.5 * s * (1.0 + 1e-9) + 1e-12;
        int64_t a[3], b[3];
        double cells = 1.0;
        for (int k = 0; k < 3; ++k) {
            a[k] = cellIndex(static_cast<double>(lo[k]) - reach, inv);
            b[k] = cellIndex(static_cast<double>(hi[k]) + reach, inv);
            cells *= static_cast<double>(b[k] - a[k] + 1);
            shared = shared || b[k] - a[k] + 1 >= (int64_t(1) << 21);
        }
        const auto& level = levels_[static_cast<size_t>(l)];
        if (cells > 2.0 * static_cast<double>(level.size()) + 8.0) {
            // More cells to ask than there are: every box of this grid.
            for (const auto& [key, v] : level) out.insert(out.end(), v.begin(), v.end());
            continue;
        }
        for (int64_t x = a[0]; x <= b[0]; ++x) {
            for (int64_t y = a[1]; y <= b[1]; ++y) {
                for (int64_t z = a[2]; z <= b[2]; ++z) {
                    const auto it = level.find(cellKey(x, y, z));
                    if (it != level.end()) out.insert(out.end(), it->second.begin(), it->second.end());
                }
            }
        }
    }
    if (shared) {
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
    }
}

}  // namespace pg
