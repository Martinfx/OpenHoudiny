#pragma once
//
// Soft selection: how much of a move each point round a selection takes
// along -- all of it at the selection, none at the radius, between as the
// falloff's shape says. The distance is straight through space, or along
// the surface: through its edges, so that a sheet lying over another, or
// a piece near but not joined to the selection, stays where it is.
//
// The Edit node moves the points by these shares; the viewport shows them.
//
#include "pg/core/Geometry.h"

#include <cstdint>
#include <span>
#include <vector>

namespace pg {

/// How the share goes from all of the move at the selection to none at
/// the radius; x is the distance over the radius, 0 to 1.
enum class Falloff : uint8_t {
    Smooth,    ///< (1 - x^2)^2: flat at both ends -- a hill
    Linear,    ///< 1 - x: a cone
    Sharp,     ///< (1 - x)^2: a spike
    Sphere,    ///< sqrt(1 - x^2): a dome, steep at its edge
    Constant,  ///< 1: all of it, as far as the radius
};
float falloff(Falloff shape, float x);

/// How far a point is from the selection.
enum class SoftDistance : uint8_t {
    Space,    ///< straight, to the nearest point picked
    Surface,  ///< along the surface, through the edges, from a point picked
};

/// For each point of `geo`, the share of a move of the points `picked`
/// (1 each) that it takes: 1 for those, falloff(d / radius) for those
/// nearer than the radius, 0 for the rest -- all of them for a radius of
/// 0. Points past the end of `picked` are not picked.
///
/// Along the surface, the distance runs out from the points picked
/// through the edges: straight from the point it came from while the way
/// runs on away from it -- so that it is round on a flat sheet, not a
/// diamond of steps along the edges -- and the length of the edges where
/// the surface bends back towards it. What no edges join to the selection
/// takes none of the move.
std::vector<float> softWeights(const Geometry& geo, std::span<const uint8_t> picked, float radius,
                               SoftDistance distance = SoftDistance::Space, Falloff shape = Falloff::Smooth);

}  // namespace pg
