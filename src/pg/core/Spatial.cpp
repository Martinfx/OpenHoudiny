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

}  // namespace pg
