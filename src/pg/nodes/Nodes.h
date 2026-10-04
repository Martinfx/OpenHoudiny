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
void registerWoodNodes();
void registerClusterNodes();
void registerRebarNodes();
void registerGlassNodes();
void registerBrickNodes();
void registerVolumeNodes();
void registerUsdNodes();
void registerEditNodes();
void registerTreeNodes();
void registerGrassNodes();

/// How Scatter places its points.
struct ScatterRules {
    size_t count = 1000;           ///< how many to try
    double density = -1.0;         ///< 0 or more: as many to a square metre instead
    uint32_t seed = 0;
    std::string densityAttribute;  ///< a point attribute of the surface, 0 to 1: the share kept there
    float maxSlope = 180.0f;       ///< degrees from level: none on steeper faces
    float minDistance = 0.0f;      ///< m: none nearer to one kept before it
};
/// Points over the closed polygons of `src` by the rules, the same for the
/// same seed -- each with the normal of its face (unless the corners have
/// N) and the point attributes of its corners, blended (whole numbers and
/// strings: the nearest corner's).
std::shared_ptr<Geometry> scatterPoints(const Geometry& src, const ScatterRules& rules);

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

/// A volume kept in tiles of 8 x 8 x 8 voxels, only some of them -- as a
/// sparse simulation keeps its fields: a voxel of a tile not kept holds
/// `background`. Voxels, origin and size as Volume's.
struct TiledVolume {
    Vec3 origin;
    float voxel = 1.0f;
    int res[3] = {0, 0, 0};
    std::vector<uint32_t> tiles;  ///< the tiles kept, by number (x fastest), in order
    std::vector<float> values;    ///< their 512 voxels each in turn, x fastest within a tile
    float background = 0.0f;
    /// Tiles whose every voxel holds `fill`, by number, in order, kept
    /// without their values -- none of them in `tiles`, on a side of the
    /// volume or beside a tile of the background: deep in what it holds.
    std::vector<uint32_t> filled;
    float fill = 0.0f;
};

/// volumeToMesh() of the volume the tiles make, looking only round them:
/// the same polygons, in the same order, as over every voxel.
std::shared_ptr<Geometry> volumeToMesh(const TiledVolume& volume, float iso, bool insideBelow);

/// Parse/run error of a `pointwrangle` node; empty if it is fine.
std::string wrangleError(const Node& node);

}  // namespace pg
