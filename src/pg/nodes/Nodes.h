#pragma once
#include "pg/core/Node.h"

#include <array>

#include <string>
#include <utility>
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
void registerEditNodes();

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

/// The Voronoi cells of `seeds` in the closed `mesh`, each what voronoiCell
/// makes of it -- made without the rest of the mesh and the rest of the
/// seeds: a cell is cut out of the parts of the mesh (primitives that share
/// points) that reach its box, by the planes of the seeds near enough to cut
/// them, nearest first, as they come out of a grid. Set up once for all the
/// cells; cell() may be called from several threads at once.
class VoronoiCells {
public:
    VoronoiCells(GeometryPtr mesh, std::vector<Vec3> seeds);
    std::shared_ptr<Geometry> cell(size_t i, const std::string& insideGroup) const;

private:
    struct Part {
        std::vector<uint32_t> prims;
        Vec3 lo, hi;
    };
    /// `piece` cut by the plane half way to each seed near enough, nearest first.
    std::shared_ptr<Geometry> cut(size_t i, std::shared_ptr<Geometry> piece, const std::string& group) const;
    /// The seeds but i further from seed i than `from` and no further than
    /// `to`, as (distance, seed), nearest first -- the first of equals first.
    void shell(size_t i, float from, float to, std::vector<std::pair<float, uint32_t>>& out) const;

    GeometryPtr mesh_;
    std::vector<Vec3> seeds_;
    std::vector<Part> parts_;
    std::shared_ptr<const Geometry> box_;  ///< the box round the mesh, a little bigger
    float margin_ = 0.0f;                  ///< what float rounding cannot reach
    float reachAll_ = 0.0f;                ///< no seed is further from another
    Vec3 gridLo_;
    float gridCell_ = 1.0f;
    int dims_[3] = {1, 1, 1};
    std::vector<uint32_t> gridStart_, gridSeeds_;
};

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
