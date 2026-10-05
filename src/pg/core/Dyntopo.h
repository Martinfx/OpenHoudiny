#pragma once
//
// Dynamic topology for sculpting, as Blender's dyntopo: a mesh made finer
// under the brush as it goes, so a stroke has the points it needs where it
// needs them -- a crease in a coarse ball, a horn pulled out of a plane.
//
// The closed polygons are cut into triangles, fans as the viewport draws
// them. Before each dab the triangles it comes to have their edges made no
// shorter than 0.4 of the detail -- the shortest first, its two ends made
// one point halfway -- and then no longer than the detail: the longest
// split in half, again and again (longest-edge bisection, so the triangles
// keep their shape). The detail is a share of the dab's radius, or a
// length. Then the dab moves the points, as it would without.
//
// Two ends are made one only where the mesh stays a surface: they share no
// neighbours but the corners across the edge (the link condition), it is
// not the last of a closed piece, no triangle turns over. A border stays
// where it is: of a border point and an inner one, the border point stays;
// two border points only along the border, a corner of it staying put.
// The points of open lines and of what else is not cut are moved by the
// dabs, never taken out.
//
// Points and corners carry the attributes: numbers blended -- a point made
// halfway along an edge has half of each end's, and so has a point two were
// made into -- integers and strings from the end with the lower number,
// groups where both ends are in them. A triangle has the attributes and
// groups of the polygon it was cut from; a corner moved onto another point
// keeps what it had. Detail attributes, volumes and instances stay.
//
// The same strokes make the same mesh, to the bit: edges are taken longest
// (or shortest) first, equal ones by the numbers of their points, and
// nothing depends on how the work was split between threads.
//
#include "pg/core/Geometry.h"

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace pg {

/// How the mesh is made finer and coarser under each dab.
struct Dyntopo {
    bool subdivide = false;  ///< edges longer than the detail split in half
    bool collapse = false;   ///< edges shorter than kDyntopoShortest of it made one point
    bool constant = false;   ///< `detail` a length, m; else a share of each dab's radius
    float detail = 0.25f;

    bool on() const { return subdivide || collapse; }
    /// The longest an edge under a dab of radius `r` may be.
    float longest(float r) const { return constant ? detail : detail * r; }
    bool operator==(const Dyntopo&) const = default;
};

/// How short an edge under a dab may be, as a share of the longest -- as
/// Blender has it: an edge split in half is not short.
inline constexpr float kDyntopoShortest = 0.4f;
/// The most edges one dab splits -- a tiny detail under a big brush would
/// fill the memory -- and the most triangles there may be.
inline constexpr size_t kDyntopoMostSplits = 250000;
inline constexpr size_t kDyntopoMostTriangles = size_t(1) << 25;

/// A mesh of triangles as it is sculpted with dyntopo, and the points and
/// corners of the geometry it was made of. Points and triangles taken out
/// keep their numbers, empty, until it is a geometry again.
class SculptMesh {
public:
    /// `geo` with its closed polygons cut into triangles.
    explicit SculptMesh(const Geometry& geo);
    /// A copy, without the grids: they are made again when asked for.
    SculptMesh(const SculptMesh& other);
    SculptMesh& operator=(const SculptMesh&) = delete;
    ~SculptMesh();

    /// Where the points are, by number -- NaN where one was taken out.
    /// Valid until the next `refine`.
    std::span<Vec3> positions() { return P_; }
    std::span<const Vec3> positions() const { return P_; }
    size_t pointCount() const { return points_; }
    size_t triangleCount() const { return triangles_; }

    /// The edges of the triangles nearer `c` than `r` made no shorter than
    /// kDyntopoShortest of `d.longest(r)` and no longer than it, as `d` says.
    void refine(const Vec3& c, float r, const Dyntopo& d);
    /// The points nearer `c` than `r`, in no set order.
    void near(const Vec3& c, float r, std::vector<uint32_t>& out) const;
    /// Point `i` has moved.
    void moved(uint32_t i);
    /// Where point `i` is smoothed towards: the middle of its neighbours
    /// along the edges; on a border, of its two along the border, where it
    /// runs on straight -- a corner stays. False where it stays.
    bool target(std::span<const Vec3> P, uint32_t i, Vec3& out) const;

    /// Grids of cells `cell` big to find the points and triangles near a
    /// dab in -- for many dabs; without them every one is asked.
    void index(float cell);
    void unindex();

    /// The mesh as a geometry: the points left, in order; each primitive of
    /// the geometry it was made of in its place -- a polygon as the
    /// triangles it became, in the order they were made; the rest as they
    /// were.
    std::shared_ptr<Geometry> geometry() const;

private:
    /// The attributes of the points or of the corners as they change:
    /// numbers kept here, blended; integers and strings as the element of
    /// the geometry each is from; groups as who is in them.
    struct Columns {
        struct Numbers {
            std::string name;
            int width = 0;  ///< floats an element
            std::vector<float> values;
        };
        struct Members {
            std::string name;
            std::vector<uint8_t> in;
        };
        std::vector<Numbers> numbers;
        uint32_t base = 0;           ///< the geometry's elements, each its own
        std::vector<uint32_t> more;  ///< ... and which of them each one after is from
        std::vector<Members> groups;

        uint32_t sourceOf(uint32_t e) const { return e < base ? e : more[e - base]; }

        /// `geo`'s attributes and groups of class `cls` -- but P -- for
        /// `count` elements, each from itself.
        void take(const Geometry& geo, AttrClass cls, size_t count);
        /// A new element, half `a` and half `b`; integers and strings `a`'s.
        uint32_t mix(uint32_t a, uint32_t b);
        /// Element `a` made half itself and half `b`.
        void mixInto(uint32_t a, uint32_t b);
        const Numbers* find(const std::string& name) const;
        const Members* group(const std::string& name) const;
    };
    /// The triangles round a point, sorted: a run of the pool.
    struct Ring {
        uint32_t start = 0, count = 0, cap = 0;
    };
    struct Grids;

    std::span<const uint32_t> ring(uint32_t p) const {
        return std::span<const uint32_t>(pool_.data() + ring_[p].start, ring_[p].count);
    }
    void ringAdd(uint32_t p, uint32_t t);
    void ringRemove(uint32_t p, uint32_t t);
    void ringClear(uint32_t p);
    void ringGrow(uint32_t p, uint32_t cap);
    void packPool();

    bool has(uint32_t t, uint32_t p) const {
        return tri_[t][0] == p || tri_[t][1] == p || tri_[t][2] == p;
    }
    bool meets(uint32_t t, const Vec3& c, float r2) const;
    void trianglesNear(const Vec3& c, float r, std::vector<uint32_t>& out) const;
    void neighbours(uint32_t p, std::vector<uint32_t>& out) const;
    int borders(uint32_t p, uint32_t ends[2]) const;
    bool hasTriangle(uint32_t a, uint32_t b, uint32_t c) const;
    void put(uint32_t t);
    void removeTriangle(uint32_t t);

    /// Edge ab split at its middle: the new point, or none where there is no
    /// such edge.
    uint32_t split(uint32_t a, uint32_t b);
    /// Edge ab made one point, where the mesh stays a surface: the point
    /// kept, in `kept`.
    bool collapse(uint32_t a, uint32_t b, uint32_t& kept);

    Geometry source_;  ///< what it was made of, its buffers shared
    std::vector<Vec3> P_;
    std::vector<uint8_t> flags_;
    std::vector<std::array<uint32_t, 3>> tri_;     ///< points; taken out: all none
    std::vector<std::array<uint32_t, 3>> corner_;  ///< corners, of cornerAttrs_
    std::vector<uint32_t> prim_;                   ///< the polygon each was cut from
    std::vector<uint8_t> cut_;                     ///< each primitive: cut into triangles, or kept
    std::vector<uint32_t> lineStart_, lineNext_;   ///< the neighbours along lines of the geometry's points
    Columns pointAttrs_, cornerAttrs_;
    std::vector<Ring> ring_;
    std::vector<uint32_t> pool_;
    size_t pooled_ = 0;  ///< how much of the pool the rings hold
    size_t points_ = 0, triangles_ = 0;
    std::unique_ptr<Grids> grids_;
    std::vector<uint32_t> work_[3];  ///< scratch for split and collapse
};

}  // namespace pg
