#pragma once
//
// What the viewport's mouse is over in a geometry -- a point, an edge, a
// primitive -- and what a part of the screen takes in -- a box, a lasso, the
// path of a brush -- seen through the view's camera: nothing the
// geometry's own surface hides, unless asked. The surface is its polygons,
// a fan across each; a ray finds the nearest of them through a tree of
// boxes round them (a bounding volume hierarchy), built once for a
// geometry and then asked from any number of threads.
//
// Answers do not depend on how the tree happened to split: of two faces a
// ray meets as far away, the one made first counts.
//
#include "pg/core/Geometry.h"
#include "pg/core/Selection.h"

#include <cstdint>
#include <vector>

namespace pg {

/// A camera as the screen sees it: where the eye is, its axes, how wide
/// it sees, and the rectangle of the screen its picture fills -- pixels, y
/// down.
struct PickView {
    Vec3 eye;
    Vec3 forward{0.0f, 0.0f, -1.0f}, right{1.0f, 0.0f, 0.0f}, up{0.0f, 1.0f, 0.0f};
    float tanHalfFov = 0.3f;  ///< of the vertical field of view
    float x = 0.0f, y = 0.0f, width = 1.0f, height = 1.0f;

    /// Where `p` lands on the screen; false when it is not in front of the eye.
    bool project(const Vec3& p, float& sx, float& sy) const;
    /// The ray from the eye through a point of the screen; `dir` of unit length.
    void ray(float sx, float sy, Vec3& origin, Vec3& dir) const;
};

/// A part of the screen a selection takes in: a box; a lasso -- what a line
/// drawn round it goes round, closed from its end back to its start; or
/// what a brush, a circle so many pixels across, goes over on its way from
/// one place to the next. Pixels, y down.
class ScreenRegion {
public:
    enum class Kind { Box, Lasso, Brush };

    ScreenRegion() = default;
    static ScreenRegion box(float x0, float y0, float x1, float y1);
    /// Inside: where a line from a place out to one side crosses the lasso
    /// an odd number of times -- a loop it makes round itself is out again.
    static ScreenRegion lasso(std::vector<Vec2> points);
    /// A brush of `radius` pixels moved from (x0, y0) to (x1, y1); where
    /// they are the same, a dab.
    static ScreenRegion brush(float x0, float y0, float x1, float y1, float radius);

    Kind kind() const { return kind_; }
    /// A lasso's line.
    const std::vector<Vec2>& points() const { return points_; }
    /// A brush's way, and how far round it it reaches.
    Vec2 from() const { return {x0_, y0_}; }
    Vec2 to() const { return {x1_, y1_}; }
    float radius() const { return radius_; }
    /// Whether (x, y) is in it.
    bool contains(float x, float y) const;
    /// Whether the segment a-b on the screen comes into the brush's path,
    /// and where along it (0 at a, 1 at b) it comes nearest its middle.
    /// A box or a lasso: whether both of its ends are in it.
    bool touches(float ax, float ay, float bx, float by, float& along) const;
    /// The box round it.
    void bounds(float& x0, float& y0, float& x1, float& y1) const;

private:
    Kind kind_ = Kind::Box;
    float x0_ = 0.0f, y0_ = 0.0f, x1_ = 0.0f, y1_ = 0.0f;  ///< a box's corners; a brush's way
    float radius_ = 0.0f;
    std::vector<Vec2> points_;
    /// A lasso's sides sorted into bands of the screen, by height: a place
    /// asks only the sides of its own band.
    float bandTop_ = 0.0f, bandHeight_ = 1.0f;
    std::vector<uint32_t> bandStart_, bandSides_;
};

class ElementPicker {
public:
    ElementPicker() = default;
    explicit ElementPicker(GeometryPtr geo) { build(std::move(geo)); }

    void build(GeometryPtr geo);
    const GeometryPtr& geometry() const { return geo_; }
    /// Whether `geo` has the points and the topology this was built for --
    /// the same buffers: a geometry painted, not moved, needs no new one.
    bool fits(const Geometry& geo) const;
    /// The geometry asked about from now on: one that fits.
    void adopt(GeometryPtr geo) { geo_ = std::move(geo); }

    /// Its edges (edgesOf): what edge() and edgesIn() count.
    const std::vector<Edge>& edges() const { return edges_; }
    /// The middle of primitive `prim`: where its points are on average.
    Vec3 middle(size_t prim) const;

    /// The nearest face along a ray: its primitive, how far it is and its
    /// normal (of unit length, towards the ray's origin); -1 when the ray
    /// meets none.
    int32_t raycast(const Vec3& origin, const Vec3& dir, float& t, Vec3* normal = nullptr) const;
    /// Whether `p` is seen from `eye`: no face in front of it.
    bool visible(const Vec3& eye, const Vec3& p) const;

    /// The point that lands nearest the screen point (sx, sy), within
    /// `reach` pixels, of those not hidden -- of all, with `hidden`; -1 for
    /// none.
    int32_t point(const PickView& view, float sx, float sy, float reach, bool hidden = false) const;
    /// The edge nearest it on the screen within `reach`, not hidden there
    /// (unless `hidden`): its number in edges(); -1 for none.
    int32_t edge(const PickView& view, float sx, float sy, float reach, bool hidden = false) const;
    /// The primitive under it: the face the ray through it meets first --
    /// or a polyline within `reach` pixels in front of that face, or with
    /// `hidden` behind it too; -1 for none.
    int32_t primitive(const PickView& view, float sx, float sy, float reach, bool hidden = false) const;

    /// What a part of the screen takes in, 1 each -- not what is hidden,
    /// unless `hidden`. Of a box or a lasso: the points that land in it, the
    /// edges both of whose ends do, the primitives whose middle does. Of a
    /// brush: the points it goes over, the edges it touches, the primitives
    /// whose middle it goes over, the faces under its middle and the
    /// polylines it touches.
    std::vector<uint8_t> pointsIn(const PickView& view, const ScreenRegion& region, bool hidden = false) const;
    std::vector<uint8_t> edgesIn(const PickView& view, const ScreenRegion& region, bool hidden = false) const;
    std::vector<uint8_t> primitivesIn(const PickView& view, const ScreenRegion& region, bool hidden = false) const;
    /// What the box from (x0, y0) to (x1, y1) holds.
    std::vector<uint8_t> pointsIn(const PickView& view, float x0, float y0, float x1, float y1, bool hidden = false) const {
        return pointsIn(view, ScreenRegion::box(x0, y0, x1, y1), hidden);
    }
    std::vector<uint8_t> edgesIn(const PickView& view, float x0, float y0, float x1, float y1, bool hidden = false) const {
        return edgesIn(view, ScreenRegion::box(x0, y0, x1, y1), hidden);
    }
    std::vector<uint8_t> primitivesIn(const PickView& view, float x0, float y0, float x1, float y1,
                                      bool hidden = false) const {
        return primitivesIn(view, ScreenRegion::box(x0, y0, x1, y1), hidden);
    }

private:
    struct Triangle {
        uint32_t a = 0, b = 0, c = 0;  ///< points
        uint32_t prim = 0;
        uint32_t order = 0;            ///< made so many before it
        Vec3 center;
    };
    struct Node {
        Vec3 lo, hi;
        uint32_t first = 0, count = 0;  ///< a leaf's triangles: tris_[first, first + count)
        uint32_t left = 0, right = 0;   ///< a branch's children
    };
    uint32_t make(uint32_t first, uint32_t count);
    /// Whether the ray meets triangle `tri` nearer than `best`: then `best`.
    bool hit(const Triangle& tri, const Vec3& origin, const Vec3& dir, float& best) const;
    bool faceSeen(size_t prim, const Vec3& eye, const Vec3& p) const;

    GeometryPtr geo_;
    const void* positions_ = nullptr;  ///< the buffers it was built for
    const void* corners_ = nullptr;
    size_t pointCount_ = 0, primitiveCount_ = 0;
    std::vector<Triangle> tris_;
    std::vector<Node> nodes_;
    std::vector<Edge> edges_;
};

}  // namespace pg
