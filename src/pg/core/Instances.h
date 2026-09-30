#pragma once
//
// Instances: geometry that points stand for, held once however many points
// stand for it -- a forest of a few trees, a meadow of a few clumps of
// grass. A point whose integer attribute `instance` is k (0 or more) stands
// for its geometry's prototype k (Geometry::prototypes()), placed as Copy
// to Points places a copy:
//
//   P        where the prototype's origin goes;
//   orient   how it is turned, a quaternion x, y, z, w -- else its +y is
//            turned to N, else it is not turned;
//   pscale   how big, 1 without it;
//   tint     what the prototype's colours are multiplied by: one clump of
//            grass a little yellower than the next.
//
// Its Cd does not colour it: points scattered over a coloured ground carry
// the ground's colour, and grass on them stays green.
//
// The renderer draws them instanced: the prototype once on the GPU, placed
// for each point there. Writers and the Unpack node make copies of them.
//
#include "pg/core/Geometry.h"

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace pg {

// --- quaternions x, y, z, w: turns by the right hand ------------------------
//
// As the `orient` attribute keeps them, computed as GLM's (quatOf, vec4Of:
// core/Types.h).

/// `a` after `b`: glm's product.
Vec4 quatMultiply(const Vec4& a, const Vec4& b);
/// `v` turned by the unit quaternion `q`.
Vec3 quatRotate(const Vec4& q, const Vec3& v);
/// What turns x, y and z to the unit vectors `X`, `Y`, `Z`, square to each
/// other, a right-handed frame: glm::quat_cast of the matrix of them.
Vec4 quatFromAxes(const Vec3& X, const Vec3& Y, const Vec3& Z);
/// The shortest turn of +y to the unit vector `n`.
Vec4 quatUpTo(const Vec3& n);
/// `v` turned as the shortest turn of +y to the unit vector `n` turns it --
/// how Copy to Points turns a copy to its point's normal.
Vec3 turnUpTo(const Vec3& n, const Vec3& v);

// --- instances ---------------------------------------------------------------

/// The prototype point `p` of `geo` stands for; -1 if none.
int32_t instanceOf(const Geometry& geo, size_t p);
/// The points of `geo` that stand for each of its prototypes, in their order.
std::vector<std::vector<uint32_t>> instancesByPrototype(const Geometry& geo);
/// How many points of `geo` stand for a prototype.
size_t instanceCount(const Geometry& geo);

/// Where a point places what stands on it: its P, its turn -- orient, else
/// the shortest turn of +y to N, else none -- and its size, pscale.
struct Placement {
    Vec3 at;
    Vec4 orient{0.0f, 0.0f, 0.0f, 1.0f};
    float scale = 1.0f;

    /// A point of what stands there, placed.
    Vec3 point(const Vec3& p) const { return at + quatRotate(orient, p * scale); }
};
/// The placement of each point of `geo`, `scale` times as big; its N is left
/// out without `align`.
std::vector<Placement> placementsOf(const Geometry& geo, float scale = 1.0f, bool align = true);

/// A copy of `tpl` on each point `which` of `pts` -- in their order: sized by
/// the point's pscale times `scale`, turned by its orient, else (with
/// `align`) its +y to its N, moved to its P. The copies keep `tpl`'s
/// attributes and groups; the points' other attributes go onto their copy
/// -- all but P, N, pscale, orient, instance and tint, and Cd without
/// `colours` -- their Cd in place of the copy's colours, their tint
/// multiplying them. `tpl`'s own instances are made copies first.
std::shared_ptr<Geometry> copiesOnPoints(const Geometry& tpl, const Geometry& pts, std::span<const uint32_t> which,
                                         float scale = 1.0f, bool align = true, bool colours = true);

/// `geo` without its instances: the points that stand for a prototype,
/// the prototypes and `instance` gone; the rest as it is.
std::shared_ptr<Geometry> withoutInstances(const Geometry& geo);

/// The box round what the instances of `geo` place: each prototype's box
/// -- as it is drawn, its own instances too -- turned, sized and moved as
/// its points place it. Empty (lo above hi) when there are none.
void instancesBox(const Geometry& geo, Vec3& lo, Vec3& hi);
/// The box round `geo` as it is drawn: its points but those that stand for
/// a prototype, and what those place. Empty (lo above hi) when there is
/// nothing.
void drawnBox(const Geometry& geo, Vec3& lo, Vec3& hi);

/// `geo` with each point that stands for a prototype made a copy of it --
/// as the renderer draws it: its tint, not its Cd -- what is no instance
/// first, as it is, then the copies of each prototype in turn, on its
/// points in their order. No prototypes are left.
std::shared_ptr<Geometry> unpackInstances(const Geometry& geo);

}  // namespace pg
