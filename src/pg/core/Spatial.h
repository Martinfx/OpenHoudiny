#pragma once
//
// Spatial queries over points: the nearest ones to a place, those within a
// radius. A static k-d tree -- built once over a geometry's points, then
// asked from any number of threads.
//
// Answers are sorted by distance and then by point number, so they do not
// depend on how the tree happened to split: equal distances come out in the
// same order on every machine.
//
#include "pg/core/Types.h"

#include <cstdint>
#include <span>
#include <vector>

namespace pg {

class PointTree {
public:
    PointTree() = default;
    explicit PointTree(std::span<const Vec3> points) { build(points); }

    void build(std::span<const Vec3> points);
    size_t size() const { return points_.size(); }

    /// The points within `radius` of `p` (all of them for a negative radius),
    /// nearest first; at most `max` of them, 0 for no limit.
    void near(const Vec3& p, float radius, size_t max, std::vector<int32_t>& out) const;
    /// The nearest point within `radius` (anywhere for a negative one); -1
    /// if there is none.
    int32_t nearest(const Vec3& p, float radius = -1.0f) const;

private:
    struct Node {
        float split = 0.0f;
        uint32_t lo = 0, hi = 0;  ///< the points it holds: order_[lo, hi)
        int8_t axis = -1;         ///< -1: a leaf
        uint32_t left = 0, right = 0;
    };
    uint32_t make(uint32_t lo, uint32_t hi, int depth);
    void search(uint32_t node, const Vec3& p, float r2, size_t max, std::vector<std::pair<float, int32_t>>& best) const;

    std::vector<Vec3> points_;
    std::vector<int32_t> order_;
    std::vector<Node> nodes_;
};

/// Who touches whom in a geometry: the points an edge joins to each point,
/// and the primitives each point is a corner of. Lists are sorted.
class Adjacency {
public:
    Adjacency() = default;

    void build(const class Geometry& geo);
    std::span<const int32_t> neighbours(size_t point) const;
    std::span<const int32_t> primitives(size_t point) const;
    std::span<const int32_t> vertices(size_t point) const;

private:
    std::vector<uint32_t> nStart_, pStart_, vStart_;
    std::vector<int32_t> nList_, pList_, vList_;
};

}  // namespace pg
