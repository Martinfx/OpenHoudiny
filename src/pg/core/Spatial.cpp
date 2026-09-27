#include "pg/core/Spatial.h"

#include "pg/core/Geometry.h"

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

}  // namespace

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

void Adjacency::build(const Geometry& geo) {
    const size_t np = geo.pointCount(), nprim = geo.primitiveCount();
    std::vector<std::vector<int32_t>> n(np), p(np), v(np);
    for (size_t prim = 0; prim < nprim; ++prim) {
        const auto pts = geo.primitivePoints(prim);
        const size_t count = pts.size();
        const size_t start = geo.primitiveVertexStart(prim);
        const bool closed = geo.primitiveClosed(prim) && count > 2;
        for (size_t i = 0; i < count; ++i) {
            const uint32_t a = pts[i];
            if (a >= np) continue;
            p[a].push_back(static_cast<int32_t>(prim));
            v[a].push_back(static_cast<int32_t>(start + i));
            if (i + 1 < count || closed) {
                const uint32_t b = pts[(i + 1) % count];
                if (b < np && b != a) {
                    n[a].push_back(static_cast<int32_t>(b));
                    n[b].push_back(static_cast<int32_t>(a));
                }
            }
        }
    }
    auto pack = [&](std::vector<std::vector<int32_t>>& lists, std::vector<uint32_t>& start, std::vector<int32_t>& flat) {
        start.assign(np + 1, 0);
        flat.clear();
        for (size_t i = 0; i < np; ++i) {
            auto& l = lists[i];
            std::sort(l.begin(), l.end());
            l.erase(std::unique(l.begin(), l.end()), l.end());
            start[i] = static_cast<uint32_t>(flat.size());
            flat.insert(flat.end(), l.begin(), l.end());
        }
        start[np] = static_cast<uint32_t>(flat.size());
    };
    pack(n, nStart_, nList_);
    pack(p, pStart_, pList_);
    pack(v, vStart_, vList_);
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

}  // namespace pg
