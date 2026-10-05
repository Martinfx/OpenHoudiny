#pragma once
//
// Dissolving edges, as Dissolve does in Houdini and Blender: an edge taken
// out of the mesh, the two polygons it was a side of made one. The polygons
// a set of edges joins become one each -- the sides between two of them
// taken out with it -- walked round as one loop. Where that is not one
// loop -- a ring of polygons round a hole, a boundary that touches itself,
// sides that do not wind alike -- the polygons stay as they were. A border
// edge, a side of one polygon only, is not dissolved; nor a side of three.
//
// The new polygon has the attributes of the lowest-numbered polygon it was
// made of, and each corner those of the corner it was. Points the dissolved
// sides alone used go; so do points left inline on a side -- on no other
// polygon, their two sides straight on within Inline Angle.
//
#include "pg/core/Geometry.h"
#include "pg/core/Selection.h"

#include <cstdint>
#include <span>
#include <vector>

namespace pg {

struct DissolveSettings {
    bool inlinePoints = true;  ///< points left on a straight side go
    float inlineAngle = 1.0f;  ///< degrees the side may bend through such a point
};

/// What dissolving did.
struct DissolveCount {
    size_t polygons = 0;  ///< made of two or more
    size_t merged = 0;    ///< polygons they were made of
    size_t kept = 0;      ///< edges not dissolved: borders, of three, round a hole
};

/// `geo` with `edges` (pairs of points, lower first) dissolved: the polygons
/// made one come after those that stay, in the order of their lowest one.
Geometry dissolveEdges(const Geometry& geo, std::span<const Edge> edges, const DissolveSettings& s = {},
                       DissolveCount* count = nullptr);

/// The sides that two of the polygons `prims` holds share: dissolved, the
/// faces picked become one.
std::vector<Edge> innerEdges(const Geometry& geo, std::span<const uint8_t> prims);

}  // namespace pg
