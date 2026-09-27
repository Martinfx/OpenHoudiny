#pragma once
#include "pg/core/Node.h"

#include <string>

namespace pg {

void registerGeneratorNodes();
void registerModifierNodes();
void registerWrangleNodes();
void registerPrimitiveNodes();
void registerSurfaceNodes();

/// Newell's normal of the polygon through `corners`: pointing the way they
/// turn anticlockwise, twice the polygon's area long.
Vec3 polygonNormal(const Geometry& geo, std::span<const uint32_t> corners);

/// Parse/run error of a `pointwrangle` node; empty if it is fine.
std::string wrangleError(const Node& node);

}  // namespace pg
