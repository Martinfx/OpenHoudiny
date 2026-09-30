#pragma once
//
// Bounding volume hierarchies for rays: boxes within boxes round what a ray
// may meet -- the triangles of a mesh, or the meshes and shapes of a scene --
// so that a ray looks only at what lies along it. Built by the surface area
// heuristic over bins, the same for the same boxes.
//
#include "pg/core/Types.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

namespace pg::render {

struct Box {
    Vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};

    void grow(const Vec3& p) {
        lo = Vec3(std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z));
        hi = Vec3(std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z));
    }
    void grow(const Box& b) {
        if (b.empty()) return;
        grow(b.lo);
        grow(b.hi);
    }
    bool empty() const { return lo.x > hi.x; }
    Vec3 center() const { return (lo + hi) * 0.5f; }
    /// Half the surface: what the heuristic weighs a box by.
    float area() const {
        if (empty()) return 0.0f;
        const Vec3 e = hi - lo;
        return e.x * e.y + e.y * e.z + e.z * e.x;
    }
};

/// A box of the hierarchy. An inner one's children are nodes `first` and
/// `first + 1`; a leaf holds `count` items, from `first` on in Bvh::items.
struct BvhNode {
    Vec3 lo;
    uint32_t first = 0;
    Vec3 hi;
    uint32_t count = 0;  ///< 0: an inner node
};

struct Bvh {
    std::vector<BvhNode> nodes;   ///< the root first
    std::vector<uint32_t> items;  ///< the items in the order the leaves take them
    bool empty() const { return nodes.empty(); }
};

/// The hierarchy over items with boxes `boxes`: at most `leafSize` items a
/// leaf, unless many share one place; never deeper than kMaxDepth.
Bvh buildBvh(std::span<const Box> boxes, int leafSize = 4);
constexpr int kMaxDepth = 60;

/// Where a ray from `origin` -- `inverse` one over each part of its
/// direction -- enters the box, if it meets it within [tMin, tMax].
inline bool hitBox(const Vec3& lo, const Vec3& hi, const Vec3& origin, const Vec3& inverse, float tMin, float tMax,
                   float& tNear) {
    const float tx0 = (lo.x - origin.x) * inverse.x, tx1 = (hi.x - origin.x) * inverse.x;
    const float ty0 = (lo.y - origin.y) * inverse.y, ty1 = (hi.y - origin.y) * inverse.y;
    const float tz0 = (lo.z - origin.z) * inverse.z, tz1 = (hi.z - origin.z) * inverse.z;
    const float near = std::max(std::max(std::min(tx0, tx1), std::min(ty0, ty1)), std::max(std::min(tz0, tz1), tMin));
    const float far = std::min(std::min(std::max(tx0, tx1), std::max(ty0, ty1)), std::min(std::max(tz0, tz1), tMax));
    tNear = near;
    return near <= far;
}

/// One over each part of `dir`, a very large number for a part that is 0.
inline Vec3 inverseOf(const Vec3& dir) {
    auto inv = [](float d) { return std::fabs(d) > 1e-30f ? 1.0f / d : (d >= 0.0f ? 1e30f : -1e30f); };
    return {inv(dir.x), inv(dir.y), inv(dir.z)};
}

/// The leaves a ray's way meets, the nearer child first: `leaf(first,
/// count)` is called with each leaf's items and returns false to stop (a
/// shadow ray that met something). `tMax` -- which a leaf may shorten as it
/// finds nearer hits -- bounds the boxes looked at.
template <class Leaf>
void traverse(const Bvh& bvh, const Vec3& origin, const Vec3& dir, float tMin, const float& tMax, Leaf&& leaf) {
    if (bvh.nodes.empty()) return;
    const Vec3 inverse = inverseOf(dir);
    // The farther children put by, with where the ray enters them: passed
    // over when a nearer hit has been found before them.
    uint32_t stack[64];
    float entry[64];
    int top = 0;
    float t = 0.0f;
    if (!hitBox(bvh.nodes[0].lo, bvh.nodes[0].hi, origin, inverse, tMin, tMax, t)) return;
    uint32_t at = 0;
    for (;;) {
        const BvhNode& node = bvh.nodes[at];
        if (node.count > 0) {
            if (!leaf(node.first, node.count)) return;
        } else {
            const BvhNode& a = bvh.nodes[node.first];
            const BvhNode& b = bvh.nodes[node.first + 1];
            float ta = 0.0f, tb = 0.0f;
            const bool ha = hitBox(a.lo, a.hi, origin, inverse, tMin, tMax, ta);
            const bool hb = hitBox(b.lo, b.hi, origin, inverse, tMin, tMax, tb);
            if (ha && hb) {
                const bool aFirst = ta <= tb;
                if (top < 64) {
                    stack[top] = aFirst ? node.first + 1 : node.first;
                    entry[top++] = aFirst ? tb : ta;
                }
                at = aFirst ? node.first : node.first + 1;
                continue;
            }
            if (ha || hb) {
                at = ha ? node.first : node.first + 1;
                continue;
            }
        }
        do {
            if (top == 0) return;
            at = stack[--top];
        } while (entry[top] > tMax);
    }
}

}  // namespace pg::render
