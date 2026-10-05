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
// And over triangles (TriangleTree): the nearest point of them to a place,
// whether a segment passes through one -- what a growing branch keeps
// clear of.
//
// And over places that change (MovingGrid, BoxGrid): the points and the
// triangles under a sculpting brush's dab, as one dab after another moves
// them, makes them and takes them out.
//
#include "pg/core/Types.h"

#include <array>
#include <cstdint>
#include <span>
#include <unordered_map>
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

/// Triangles to keep clear of: the nearest point of them to a place, and
/// whether a segment passes through one. A tree of boxes round them --
/// split at the middle of the longest side their middles span, at most four
/// a leaf -- built once over a geometry's closed polygons (fans of
/// triangles), then asked from any number of threads.
class TriangleTree {
public:
    TriangleTree() = default;
    explicit TriangleTree(const class Geometry& geo) { build(geo); }

    void build(const class Geometry& geo);
    bool empty() const { return triangles_.empty(); }
    size_t size() const { return triangles_.size(); }
    /// The nearest point of the triangles to `p`, no further than `radius`:
    /// false, `at` as it was, when none is as near.
    bool nearest(const Vec3& p, float radius, Vec3& at) const;
    /// Whether the segment from `a` to `b` passes through a triangle.
    bool crosses(const Vec3& a, const Vec3& b) const;

private:
    struct Node {
        Vec3 lo, hi;
        uint32_t first = 0, count = 0;  ///< a leaf: its triangles, order_[first, first + count)
        uint32_t left = 0, right = 0;
    };
    uint32_t make(uint32_t first, uint32_t count);

    std::vector<std::array<Vec3, 3>> triangles_;
    std::vector<uint32_t> order_;
    std::vector<Node> nodes_;
};

/// The point of triangle abc nearest `p` (Ericson, Real-Time Collision
/// Detection, 5.1.5).
Vec3 nearestOnTriangle(const Vec3& p, const Vec3& a, const Vec3& b, const Vec3& c);

/// The points in the cells of a grid, moved from cell to cell as they move:
/// who is near a place, while the places change. Points may be added and
/// taken out; one not finite is in no cell.
class MovingGrid {
public:
    MovingGrid(std::span<const Vec3> P, float cell);

    /// Point `i` is at `p` now.
    void moved(uint32_t i, const Vec3& p);
    /// A new point, numbered after all there are.
    void add(uint32_t i, const Vec3& p);
    /// Point `i` taken out.
    void erase(uint32_t i);
    /// The points of `P` nearer `c` than `r`, each once, in no set order.
    void near(std::span<const Vec3> P, const Vec3& c, float r, std::vector<uint32_t>& out) const;

private:
    int64_t index(float v) const;
    uint64_t key(const Vec3& p) const;
    void insert(uint32_t i, uint64_t k);
    void remove(uint32_t i);

    double inv_;
    std::unordered_map<uint64_t, std::vector<uint32_t>> cells_;
    std::vector<uint64_t> cellOf_;
    std::vector<uint32_t> slot_;
};

/// The points of `P` nearer `c` than `r`, every point asked -- on more
/// threads when there are many: quicker than making a grid for a question
/// or two. In number order.
void scanNear(std::span<const Vec3> P, const Vec3& c, float r, std::vector<uint32_t>& out);

/// Boxes of any size -- round the triangles of a mesh as it changes -- in
/// grids of cells doubling in size: each box in the one cell its middle is
/// in, of the finest grid whose cells are no smaller than it. So a big
/// triangle is in one cell as a small one is; who might meet a place is
/// found by asking each grid the cells within half a cell of it.
class BoxGrid {
public:
    explicit BoxGrid(float cell);

    /// Box `i` is lo..hi (now).
    void put(uint32_t i, const Vec3& lo, const Vec3& hi);
    void erase(uint32_t i);
    /// The boxes that may meet lo..hi -- all that do, and some that do not
    /// -- each once, in no set order.
    void near(const Vec3& lo, const Vec3& hi, std::vector<uint32_t>& out) const;

private:
    static constexpr int kLevels = 48;
    struct Where {
        int8_t level = -1;  ///< -1: in no grid
        uint32_t slot = 0;
        uint64_t key = 0;
    };
    void remove(uint32_t i);

    double cell_;
    std::vector<Where> where_;
    std::vector<std::unordered_map<uint64_t, std::vector<uint32_t>>> levels_;
    std::vector<size_t> counts_;
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
