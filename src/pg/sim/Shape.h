#pragma once
//
// Shapes placed in the world: the solids of a scene -- a ball, a box, a
// column, a cone, a ring -- and the regions sources emit from.
//
// Each is a canonical shape in a unit space of its own, placed by three
// things, as in any 3D program:
//
//   center     where it is, world units;
//   rotation   how it is turned: degrees about x, then about y, then about z
//              (the axes of the world, so the matrix is Rz Ry Rx);
//   size       how big it is along its own axes, turned with it: a box's
//              edges, a ball's diameters, a column's width, height, depth.
//
// Along its own axes a shape fills [-size/2, size/2]. A cylinder and a cone
// stand along their own y; a torus lies in their own xz plane, as thick as
// size.y. Sizes that differ stretch the shape: a ball becomes an ellipsoid.
//
// ShapeInstance precomputes what the hot loops need -- the turn, the half
// size -- and answers: is a point inside, how far is it from the surface,
// how much of a source is at it, where does a ray meet it. A distance is
// exact for a shape of equal sizes and a close estimate for a stretched one;
// inside or outside is exact for all.
//
#include "pg/core/Types.h"

#include <cstdint>

namespace pg::sim {

enum class Shape : uint8_t { Sphere, Box, Cylinder, Cone, Torus };

/// Names as files and the command line write them: "sphere", "box", ...
const char* shapeName(Shape shape);

/// A rotation as the images of the three axes: the columns of its matrix.
struct Rotation {
    Vec3 x{1.0f, 0.0f, 0.0f}, y{0.0f, 1.0f, 0.0f}, z{0.0f, 0.0f, 1.0f};

    /// The rotation applied to v.
    Vec3 apply(const Vec3& v) const { return x * v.x + y * v.y + z * v.z; }
    /// Undone: world to the rotated frame.
    Vec3 inverse(const Vec3& v) const { return {dot(x, v), dot(y, v), dot(z, v)}; }
    /// `this` after `first`.
    Rotation then(const Rotation& first) const;
    /// Axis `a` (0 x, 1 y, 2 z) of the rotated frame.
    const Vec3& axis(int a) const { return a == 0 ? x : a == 1 ? y : z; }

    /// Degrees about x, then about y, then about z -- world axes: Rz Ry Rx.
    static Rotation fromEuler(const Vec3& degrees);
    /// The angles back, each in (-180, 180]; of the two sets of angles that
    /// give the same rotation, the one nearer `near` (in the unwrapped sense:
    /// multiples of 360 added freely), so that an angle turned past 180
    /// degrees keeps growing rather than jumping.
    Vec3 toEuler(const Vec3& near = Vec3()) const;
    /// `degrees` about `axis` (need not be of unit length).
    static Rotation about(const Vec3& axis, float degrees);
};

/// A shape placed in the world, ready for the questions asked of it cell by
/// cell and ray by ray.
class ShapeInstance {
public:
    ShapeInstance() = default;
    ShapeInstance(Shape shape, const Vec3& center, const Vec3& rotationDegrees, const Vec3& size);

    Shape shape() const { return shape_; }
    const Vec3& center() const { return center_; }
    const Rotation& turn() const { return turn_; }
    /// Half the size: the shape fills [-half, half] along its own axes.
    const Vec3& half() const { return half_; }
    /// A torus's tube radius and the radius of its centre line, world units
    /// along its own x (z is stretched by half.z / half.x).
    float tube() const { return tube_; }
    float ring() const { return ring_; }

    /// World to the shape's own axes, world units: centred, turned back.
    Vec3 toLocal(const Vec3& p) const { return turn_.inverse(p - center_); }
    Vec3 toWorld(const Vec3& local) const { return center_ + turn_.apply(local); }

    /// Signed distance, world units: below 0 inside.
    float distance(const Vec3& p) const;
    bool contains(const Vec3& p) const;
    /// The outward normal of the surface nearest p.
    Vec3 normal(const Vec3& p) const;
    /// How much of a source is at p: 1 well inside, easing to 0 at the
    /// surface, 0 outside. A ball eases over its outer 40 %, a box over the
    /// outer quarter of each half edge.
    float falloff(const Vec3& p) const;

    /// Where a ray from `origin` along `dir` (any length) first meets the
    /// surface at t >= tMin, in units of `dir`, with the outward normal
    /// there. False if it does not.
    bool intersect(const Vec3& origin, const Vec3& dir, float tMin, float& t, Vec3& normal) const;

    /// The corners of a box round it, world units, not the tightest.
    void bounds(Vec3& lo, Vec3& hi) const;

private:
    /// Distance in the shape's own axes, for a point already there.
    float localDistance(const Vec3& q) const;

    Shape shape_ = Shape::Sphere;
    Vec3 center_;
    Rotation turn_;
    Vec3 half_{0.5f, 0.5f, 0.5f};
    float tube_ = 0.0f, ring_ = 0.0f;
};

}  // namespace pg::sim
