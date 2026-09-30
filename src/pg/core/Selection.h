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
// The viewport writes a selection so (patternOf), and the Group, Edit,
// Blast and Attribute Paint nodes read it.
//
#include "pg/core/Attribute.h"
#include "pg/core/Geometry.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace pg {

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

}  // namespace pg
