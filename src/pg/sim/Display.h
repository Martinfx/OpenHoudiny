#pragma once
//
// Geometry as the viewport draws it -- the network's displayed node:
//
//   polygons   triangles (a fan across each closed polygon), lit, in the
//              colour Cd of each corner -- the vertex's, else the point's,
//              the primitive's, the detail's, or a light grey -- bent by the
//              points' N where there is one, else by the faces round each
//              corner that bend less than 60 degrees from its own: round
//              things come out round, a box keeps its edges;
//   glass      the polygons of primitives whose attribute glass is 1 or more:
//              triangles apart from the rest, the colour their tint -- 2 for
//              the faces of a crack, what light runs along;
//   polylines  the open primitives, as line segments in their colour;
//   points     those no primitive uses, as dots: pscale wide where they have
//              one, else a few pixels -- a glass chip (a point attribute
//              glass of 1) glints instead; a point that stands for a
//              prototype (Instances.h) is not a dot: its prototype's
//              polygons are drawn there, instanced (DisplayInstances);
//   volumes    a dot in each voxel that is not empty -- blue to yellow as
//              the value grows -- and the box round the volume.
//
// Worked out on the CPU, handed to the renderer as flat arrays of floats.
// The displayed node's polygons go as a DisplayMesh instead -- indexed, and
// made again quickly when only the points move (a sculpting brush, a
// handle dragged).
//
#include "pg/core/Geometry.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace pg::sim {

struct DisplayGeometry {
    /// Nine floats a corner, three corners a triangle: position, normal, colour.
    std::vector<float> triangles;
    /// Three floats a corner of `triangles`: its point's velocity v, world
    /// units a second -- what motion blur needs. Empty when the points have
    /// no v.
    std::vector<float> velocities;
    /// Ten floats a corner, three corners a triangle: position, normal --
    /// out of the solid, as its corners turn -- tint, and 1 for a face of
    /// the pane, 2 for a face of a crack.
    std::vector<float> glass;
    /// Seven floats a dot: position, colour, radius -- world units; 0 for a
    /// dot a few pixels wide; below 0 a chip of glass as wide.
    std::vector<float> dots;
    /// Seven floats an end, two ends a segment: position, colour, alpha.
    std::vector<float> lines;
    /// The box round it all; lo > hi when there is nothing.
    Vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
    /// Dots left out of volumes that had more than there is room for.
    size_t dotsLeftOut = 0;

    size_t triangleCount() const { return triangles.size() / 27; }
    size_t glassCount() const { return glass.size() / 30; }
    size_t dotCount() const { return dots.size() / 7; }
    size_t segmentCount() const { return lines.size() / 14; }
    bool empty() const { return triangles.empty() && glass.empty() && dots.empty() && lines.empty(); }
};

/// What `geo` looks like in the viewport. At most `maxDots` dots come from
/// its volumes: past that, every second voxel, every third... Without
/// `faces`, the polygons that are not glass are left out of `triangles`
/// (a DisplayMesh draws them) -- the box still goes round them.
DisplayGeometry displayOf(const Geometry& geo, size_t maxDots = 400000, bool faces = true);

/// The closed polygons of `geo` as the viewport shades them, for a renderer
/// of its own: fans of triangles, three corners each -- place, normal (the
/// points' N where every point has a usable one, else the faces' round the
/// corner within the viewport's crease; glass flat) and colour (Cd as
/// displayOf finds it) -- the primitive each came from, and its glass: 0
/// none, 1 a pane, 2 a crack. Where the points have rest (where they were
/// before they moved), that of each corner too; where they have a velocity
/// v, how fast each corner goes.
struct ShadedTriangles {
    std::vector<Vec3> positions, normals, colors;  ///< three a triangle
    std::vector<Vec3> rest;                        ///< three a triangle, or none
    std::vector<Vec3> velocities;                  ///< three a triangle, or none: m/s
    /// Three a triangle where the corners or the points have uv -- what a
    /// picture is laid on by -- else none.
    std::vector<Vec2> uvs;
    std::vector<uint32_t> prims;                   ///< one a triangle
    std::vector<uint8_t> glass;                    ///< one a triangle
    size_t count() const { return prims.size(); }
};
ShadedTriangles shadedTriangles(const Geometry& geo);

/// What stands on the points of a geometry (Instances.h), as the renderer
/// draws it: each prototype some point stands for -- its polygons once on
/// the GPU -- and where it goes, twelve floats an instance: its place and
/// size, its turn (a quaternion x, y, z, w), its tint and 1.
struct DisplayInstances {
    static constexpr size_t kFloats = 12;
    std::vector<GeometryPtr> prototypes;
    std::vector<std::vector<float>> placements;  ///< for each of `prototypes`
    /// The middle of each prototype's box and how far its corners are from
    /// it: how big a copy looks (placementsByDetail).
    std::vector<Vec3> centers;
    std::vector<float> radii;
    Vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};  ///< the box round them all

    size_t count() const {
        size_t n = 0;
        for (const auto& p : placements) n += p.size() / kFloats;
        return n;
    }
};
DisplayInstances instancesOf(const Geometry& geo);

/// The levels of detail a plant is drawn at far away (core/Lod.h:
/// plantDetail): the share of its leaves and blades kept at each -- the
/// last a billboard, a picture of the plant from the side it is seen from
/// (its shadow cast by the level before's plant).
inline constexpr size_t kDetailLevels = 4;
inline constexpr std::array<float, kDetailLevels> kDetailKeep = {1.0f, 0.35f, 0.12f, 0.12f};
/// How big a copy must look -- its prototype's radius times its size, over
/// how far its middle is from the eye -- to be drawn at each level: below
/// the last, not at all.
inline constexpr std::array<float, kDetailLevels> kDetailSize = {0.04f, 0.012f, 0.005f, 0.0015f};
/// How far either side of each of those sizes a copy is both levels at
/// once, a share of the size: fading from one to the other.
inline constexpr float kDetailFade = 0.2f;

/// The placements of a prototype's copies (DisplayInstances::kFloats each),
/// shared out among the levels of detail by how big each looks from `eye`:
/// its prototype's box round `center`, `radius` from it -- the smallest
/// left out. One near where two levels meet is at both, its last float --
/// its fade -- saying which pixels of it each draws: below 1, that share
/// of them; above 1, the rest (the viewport dithers them).
std::array<std::vector<float>, kDetailLevels> placementsByDetail(std::span<const float> placements, const Vec3& center,
                                                                 float radius, const Vec3& eye);

/// The normal of each corner of `triangles` (three point indices each), in
/// order: the faces round its point that bend less than `crease` degrees
/// from its own, averaged by area.
std::vector<Vec3> cornerNormals(std::span<const Vec3> positions, const std::vector<std::array<uint32_t, 3>>& triangles,
                                float crease = 60.0f);

/// The polygons of a geometry that are not glass, as the GPU draws them
/// best: the corners that share a point, a normal and a colour are one
/// vertex, the triangles indices of the vertices -- a smooth surface has
/// about as many vertices as points, a sixth of its corners. Corner for
/// corner the triangles of displayOf, in the same order, to the bit.
/// A set of pictures the viewport lays on a surface (render/Textures.h):
/// its colour (with the alpha that cuts it out, if it has one) and its
/// normal map; whether Cd tints it, round its mean.
struct DisplayPicture {
    std::string color, alpha, normal;
    bool alphaChannel = false, normalDirectX = false, tint = true;
    Vec3 mean{0.5f, 0.5f, 0.5f};
    bool operator==(const DisplayPicture&) const = default;
};

struct DisplayMesh {
    std::vector<float> places;      ///< six floats a vertex: position, normal
    std::vector<float> colors;      ///< three floats a vertex
    std::vector<float> velocities;  ///< three floats a vertex, its point's v; empty when the points have none
    /// One float a vertex: how much light its face lets through (primitive
    /// translucency -- leaves, blades of grass); empty when it has none.
    std::vector<float> translucency;
    /// Five floats a vertex where some faces have pictures laid on: where on
    /// them -- by uv (u, v, 0, 0), or from three sides by its rest, else its
    /// place (x, y, z, metres a picture; below 0 along the face) -- and
    /// which of `pictures` (-1 none). Empty when none have.
    std::vector<float> textures;
    std::vector<DisplayPicture> pictures;
    std::vector<uint32_t> indices;  ///< three vertices a triangle
    Vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};  ///< the box round the vertices

    size_t vertexCount() const { return colors.size() / 3; }
    size_t triangleCount() const { return indices.size() / 3; }
};

/// Makes the DisplayMesh of a geometry, and makes it again quickly when
/// only the points moved -- under a sculpting brush, a handle: the same
/// topology, colours and glass keep the triangles and which corners are one
/// vertex, and only the vertices' places, normals and velocities are found
/// again. A fold turned sharp enough to part the corners of a vertex -- or
/// anything else changed -- makes it anew.
class DisplayMesher {
public:
    enum class Made {
        Anew,   ///< all of it: the indices and colours too
        Moved,  ///< the places, normals and velocities only
    };
    Made make(const GeometryPtr& geo, DisplayMesh& mesh);
    /// Whether the geometry last made has more to draw than these polygons
    /// -- glass, lines, loose points, volumes: what displayOf draws.
    bool hasRest() const { return rest_; }

private:
    void build(const Geometry& geo, DisplayMesh& mesh);
    bool move(const Geometry& geo, DisplayMesh& mesh);
    bool sameMaking(const Geometry& geo) const;

    GeometryPtr made_;  ///< the geometry last made -- held, so no buffer of it is taken for a new one
    bool pointNormals_ = false, cornerNormals_ = false, moving_ = false, rest_ = false;
    bool creased_ = false;  ///< some corners without a normal of their own -- corner's or point's: creased
    std::vector<std::array<uint32_t, 3>> tris_;  ///< the fan of every closed polygon, glass too: its faces bend the normals
    std::vector<uint32_t> drawn_;                ///< the triangles drawn -- not glass -- in order
    std::vector<uint32_t> start_, around_;       ///< the triangles round each point: around_[start_[p], start_[p + 1])
    std::vector<uint32_t> vertexPoint_;          ///< each vertex's point
    std::vector<uint32_t> vertexCorner_;         ///< ... and its first corner: 3 x its triangle in drawn_ + which
    std::vector<std::array<uint32_t, 3>> corners_;  ///< the geometry's vertex of each corner of tris_
};

}  // namespace pg::sim
