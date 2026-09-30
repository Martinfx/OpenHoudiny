#pragma once
//
// Elements named by a pattern, as the group fields of Houdini's nodes take
// them: numbers and ranges of numbers ("0-9 12 20-30"), the names of
// groups, "*" for every one; "^" before an item takes it away again. Items
// are apart by spaces or commas and count in order: "* ^0-9" is every
// element but the first ten. A group of the other class counts by its
// points: the points of a primitive group, the primitives all of whose
// points are in a point group. What names nothing -- a number past the
// last, a group there is not -- selects nothing.
//
// Edges are named by the points they join, as in Houdini: "p3-4" is the
// edge from point 3 to point 4, "p0-1-2-3" the three edges of a path.
// For points, an edge names its two points; for primitives, those it is a
// side of. Only the edges the geometry has count.
//
// The viewport writes a selection so (patternOf, edgePatternOf), and the
// Group, Edit, Blast and Attribute Paint nodes read it.
//
#include "pg/core/Attribute.h"
#include "pg/core/Geometry.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pg {

/// An edge: the two points it joins, the lower number first.
using Edge = std::pair<uint32_t, uint32_t>;

/// For each element of class `cls` (Point or Primitive) of `geo`, 1 where
/// `pattern` names it. `named`, when given: whether any item of it is
/// something there is -- numbers, "*", a group of that name, empty or not.
std::vector<uint8_t> selectElements(const Geometry& geo, AttrClass cls, std::string_view pattern,
                                    bool* named = nullptr);

/// The elements `mask` holds, as the shortest pattern of numbers and ranges:
/// "0-9 12 20-30"; "" for none.
std::string patternOf(std::span<const uint8_t> mask);

/// For each point of `geo`, 1 where a primitive `prims` holds is on it.
std::vector<uint8_t> pointsOfPrimitives(const Geometry& geo, std::span<const uint8_t> prims);

/// For each primitive of `geo`, 1 where every point of it `points` holds.
std::vector<uint8_t> primitivesOfPoints(const Geometry& geo, std::span<const uint8_t> points);

/// Every edge of `geo` once, sorted: the sides of its polygons -- the last
/// corner back to the first of a closed one -- and the segments of its
/// polylines.
std::vector<Edge> edgesOf(const Geometry& geo);

/// The edges `pattern` names -- its edge items, "*" for all of them --
/// sorted: those of `edges` (edgesOf), where "^" takes them away again.
std::vector<Edge> selectEdges(std::span<const Edge> edges, std::string_view pattern);

/// `edges` (sorted) as edge items, a path of them as one: "p0-1-2 p7-8";
/// "" for none.
std::string edgePatternOf(std::span<const Edge> edges);

}  // namespace pg
