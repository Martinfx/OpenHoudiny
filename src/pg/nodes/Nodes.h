#pragma once
#include "pg/core/Node.h"

#include <string>

namespace pg {

void registerGeneratorNodes();
void registerModifierNodes();
void registerWrangleNodes();
void registerPrimitiveNodes();
void registerSurfaceNodes();
void registerTopologyNodes();
void registerFractureNodes();
void registerVolumeNodes();

/// Newell's normal of the polygon through `corners`: pointing the way they
/// turn anticlockwise, twice the polygon's area long.
Vec3 polygonNormal(const Geometry& geo, std::span<const uint32_t> corners);

/// What of `src` is on the side of the plane through `origin` that `dir`
/// points to; faces cut are cut along it and, with `cap`, closed with faces
/// on it -- in the primitive group `capGroup`, if named.
std::shared_ptr<Geometry> clipGeometry(const Geometry& src, const Vec3& origin, const Vec3& dir, bool cap,
                                       const std::string& capGroup);
/// `iterations` steps of Catmull-Clark subdivision.
std::shared_ptr<Geometry> subdivideGeometry(const Geometry& src, int iterations);

/// The surface of `volume` where its values cross `iso`, as a mesh of quads
/// turned outward: the inside is below iso (a distance, a level set) with
/// `insideBelow`, else above it (a density). Beyond the volume is outside,
/// so what it holds is closed where it ends. The points carry N.
std::shared_ptr<Geometry> volumeToMesh(const Volume& volume, float iso, bool insideBelow);

/// Parse/run error of a `pointwrangle` node; empty if it is fine.
std::string wrangleError(const Node& node);

}  // namespace pg
