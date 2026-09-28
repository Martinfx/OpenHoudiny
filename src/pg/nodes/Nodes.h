#pragma once
#include "pg/core/Node.h"

#include <array>

#include <string>
#include <vector>

namespace pg {

void registerGeneratorNodes();
void registerModifierNodes();
void registerWrangleNodes();
void registerPrimitiveNodes();
void registerSurfaceNodes();
void registerTopologyNodes();
void registerFractureNodes();
void registerConcreteNodes();
void registerClusterNodes();
void registerRebarNodes();
void registerGlassNodes();
void registerBrickNodes();
void registerVolumeNodes();
void registerUsdNodes();

/// Newell's normal of the polygon through `corners`: pointing the way they
/// turn anticlockwise, twice the polygon's area long.
Vec3 polygonNormal(const Geometry& geo, std::span<const uint32_t> corners);

/// What of `src` is on the side of the plane through `origin` that `dir`
/// points to; faces cut are cut along it and, with `cap`, closed with faces
/// on it -- in the primitive group `capGroup`, if named.
std::shared_ptr<Geometry> clipGeometry(const Geometry& src, const Vec3& origin, const Vec3& dir, bool cap,
                                       const std::string& capGroup);
/// Whether `p` is inside the closed polygons of `geo`: how often a ray from
/// it crosses them, odd inside.
bool insideMesh(const Geometry& geo, const Vec3& p);

/// A box round points, turned as it lies: its three axes -- unit, square to
/// each other -- and how far along each the points go.
struct OrientedBox {
    std::array<Vec3, 3> axis{Vec3(1.0f, 0.0f, 0.0f), Vec3(0.0f, 1.0f, 0.0f), Vec3(0.0f, 0.0f, 1.0f)};
    std::array<double, 3> lo{0.0, 0.0, 0.0}, hi{0.0, 0.0, 0.0};

    double size(int a) const { return hi[static_cast<size_t>(a)] - lo[static_cast<size_t>(a)]; }
    double volume() const { return size(0) * size(1) * size(2); }
    /// The point `along` each axis from the box's middle.
    Vec3 at(const std::array<double, 3>& along) const;
};
/// The box round the points `P` of `geo`, turned as it lies (its polygons
/// say how): square to its biggest flat side -- the faces that face one way
/// or the other, the most area together -- and in the plane of that side
/// the rectangle round the points that fits them closest (rotating calipers
/// round their hull); along the world's axes when that box is as small.
OrientedBox fitBox(const Geometry& geo, const std::vector<Vec3>& P);
/// The points of `geo` where its proxy has them (Concrete Fracture's plain
/// cut, under the rough one), where it carries one.
std::vector<Vec3> proxyPositions(const Geometry& geo);

/// Seed `i`'s Voronoi cell of the closed mesh `mesh`: what of it is nearer
/// that seed than any other -- clipped by the plane half way to each other
/// seed, nearest first -- closed where it was cut, the cut faces in the
/// primitive group `insideGroup`.
std::shared_ptr<Geometry> voronoiCell(const GeometryPtr& mesh, const std::vector<Vec3>& seeds, size_t i,
                                      const std::string& insideGroup);

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
