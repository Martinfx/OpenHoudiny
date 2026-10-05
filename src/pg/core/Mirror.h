#pragma once
//
// Symmetry across a plane through the origin, square to an axis: what the
// viewport's M sets, what Edit moves by -- the points on the other side of
// the plane moved as their mirror images are -- and what the brushes
// mirror their dabs by (Sculpt.h). Each point's mirror image is the point
// nearest where it is reflected, within a tolerance: a geometry made
// symmetric finds its pairs; one that is not, finds fewer.
//
#include "pg/core/Geometry.h"

#include <cstdint>
#include <span>
#include <vector>

namespace pg {

enum class Mirror : uint8_t { None, X, Y, Z };

/// The axis square to the plane: 0, 1, 2; -1 for none.
inline int mirrorAxis(Mirror m) { return m == Mirror::None ? -1 : static_cast<int>(m) - 1; }

/// `v` reflected in the plane -- a place, or a way (the plane goes through
/// the origin).
inline Vec3 mirrored(const Vec3& v, Mirror m) {
    Vec3 out = v;
    const int a = mirrorAxis(m);
    if (a >= 0) out[a] = -out[a];
    return out;
}

/// How near two places must be to be each other's image: 1e-4 of the size
/// of the box round `geo`'s points, 1e-6 at least.
float mirrorTolerance(const Geometry& geo);

/// Each point's mirror image among `geo`'s points -- the nearest to where it
/// is reflected, within `tolerance` -- or -1. A point on the plane is its own.
std::vector<int32_t> mirrorPoints(const Geometry& geo, Mirror m, float tolerance);

/// The points `mask` holds, and their mirror images.
std::vector<uint8_t> withMirror(const Geometry& geo, std::span<const uint8_t> mask, Mirror m);

}  // namespace pg
