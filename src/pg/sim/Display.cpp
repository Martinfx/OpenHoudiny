#include "pg/sim/Display.h"

#include "pg/core/Instances.h"
#include "pg/core/Parallel.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <utility>

namespace pg::sim {
namespace {

const AttributeArray* pointVectors(const Geometry& geo, const char* name) {
    const AttributeArray* a = geo.points().find(name);
    return a && a->type() == AttrType::Vec3 && a->size() == geo.pointCount() ? a : nullptr;
}

/// The points' N, where every corner of the primitives has one of some
/// length -- else none, and the faces bend the normals: a ground merged
/// with what brought N along has N of 0, and is drawn as it was.
const AttributeArray* usableNormals(const Geometry& geo) {
    const AttributeArray* N = pointVectors(geo, "N");
    if (!N) return nullptr;
    const auto n = N->read<Vec3>();
    for (const uint32_t p : geo.vertexPoints()) {
        if (p < n.size() && dot(n[p], n[p]) <= 1e-24f) return nullptr;
    }
    return N;
}

/// 1 for each point of `geo` that stands for a prototype, 0 for the rest.
std::vector<uint8_t> instancePoints(const Geometry& geo) {
    std::vector<uint8_t> out(geo.pointCount(), 0);
    for (const auto& points : instancesByPrototype(geo)) {
        for (const uint32_t p : points) out[p] = 1;
    }
    return out;
}

constexpr float kPi = 3.14159265358979323846f;

/// Element `i` of a Cd attribute, if there is one: a colour, or a grey.
bool colorAt(const AttributeArray* a, size_t i, Vec3& out) {
    if (!a || i >= a->size()) return false;
    switch (a->type()) {
        case AttrType::Vec3: out = a->read<Vec3>()[i]; return true;
        case AttrType::Vec4: {
            const Vec4 c = a->read<Vec4>()[i];
            out = Vec3(c.x, c.y, c.z);
            return true;
        }
        case AttrType::Float: out = Vec3(a->read<float>()[i]); return true;
        default: return false;
    }
}

/// Cd of one element of an attribute set, if it has one.
bool colorOf(const AttributeSet& set, size_t i, Vec3& out) { return colorAt(set.find("Cd"), i, out); }

void grow(DisplayGeometry& d, const Vec3& p) {
    for (int a = 0; a < 3; ++a) {
        d.lo[a] = std::min(d.lo[a], p[a]);
        d.hi[a] = std::max(d.hi[a], p[a]);
    }
}

void put(std::vector<float>& to, const Vec3& a, const Vec3& b) { to.insert(to.end(), {a.x, a.y, a.z, b.x, b.y, b.z}); }

/// The triangles round each point, packed: point p's are around[start[p],
/// start[p + 1]), in order.
void trianglesRound(size_t points, const std::vector<std::array<uint32_t, 3>>& triangles, std::vector<uint32_t>& start,
                    std::vector<uint32_t>& around) {
    start.assign(points + 1, 0);
    around.resize(triangles.size() * 3);
    for (const auto& t : triangles) {
        for (const uint32_t v : t) ++start[v + 1];
    }
    for (size_t v = 0; v < points; ++v) start[v + 1] += start[v];
    std::vector<uint32_t> fill(start.begin(), start.end() - 1);
    for (size_t f = 0; f < triangles.size(); ++f) {
        for (const uint32_t v : triangles[f]) around[fill[v]++] = static_cast<uint32_t>(f);
    }
}

/// Each face's normal, as long as twice its area -- bigger faces count
/// more -- and that length, found once, not at each corner round it.
void faceNormals(std::span<const Vec3> P, const std::vector<std::array<uint32_t, 3>>& triangles, std::vector<Vec3>& normal,
                 std::vector<float>& size) {
    normal.resize(triangles.size());
    size.resize(triangles.size());
    parallelFor(triangles.size(), 4096, [&](size_t begin, size_t end) {
        for (size_t f = begin; f < end; ++f) {
            const auto& t = triangles[f];
            normal[f] = cross(P[t[1]] - P[t[0]], P[t[2]] - P[t[0]]);
            size[f] = length(normal[f]);
        }
    });
}

/// The normal of a corner: the faces round its point that bend less than
/// the crease from its own face, summed.
struct Creases {
    std::span<const Vec3> normal;
    std::span<const float> size;
    std::span<const uint32_t> start, around;
    float cosine = 0.5f;

    /// Face f's own normal, of unit length; up for a face of no area.
    Vec3 own(size_t f) const { return size[f] > 0.0f ? normal[f] * (1.0f / size[f]) : Vec3(0.0f, 1.0f, 0.0f); }
    /// The corner at point v of a face whose own normal is `face`.
    Vec3 corner(const Vec3& face, uint32_t v) const {
        Vec3 sum;
        for (uint32_t k = start[v]; k < start[v + 1]; ++k) {
            const Vec3& other = normal[around[k]];
            const float l = size[around[k]];
            if (l > 0.0f && dot(other, face) >= cosine * l) sum += other;
        }
        const float l = length(sum);
        return l > 0.0f ? sum * (1.0f / l) : face;
    }
};

/// The viewport's crease: how far, in degrees, a face may bend from a
/// corner's own and still round it.
constexpr float kCrease = 60.0f;

/// The colour of a corner: the first of its vertex's Cd, its point's, its
/// primitive's, the detail's, a light grey -- the attributes found once, not
/// by name at each of millions.
struct Colors {
    const AttributeArray* vertex;
    const AttributeArray* point;
    const AttributeArray* primitive;
    Vec3 detail{0.72f, 0.72f, 0.74f};

    explicit Colors(const Geometry& geo)
        : vertex(geo.vertices().find("Cd")), point(geo.points().find("Cd")), primitive(geo.primitives().find("Cd")) {
        colorOf(geo.detail(), 0, detail);
    }
    Vec3 at(size_t prim, size_t corner, uint32_t p) const {
        Vec3 c;
        if (colorAt(vertex, corner, c) || colorAt(point, p, c) || colorAt(primitive, prim, c)) return c;
        return detail;
    }
};

/// Glass: the primitives whose attribute glass is 1 or more -- 2 a crack.
struct Glass {
    const AttributeArray* attr;

    explicit Glass(const Geometry& geo) : attr(geo.primitives().find("glass")) {}
    float of(size_t prim) const {
        if (!attr || prim >= attr->size()) return 0.0f;
        if (attr->type() == AttrType::Int) return static_cast<float>(attr->read<int32_t>()[prim]);
        if (attr->type() == AttrType::Float) return attr->read<float>()[prim];
        return 0.0f;
    }
};

}  // namespace

std::vector<Vec3> cornerNormals(std::span<const Vec3> positions, const std::vector<std::array<uint32_t, 3>>& triangles,
                                float crease) {
    std::vector<Vec3> normal;
    std::vector<float> size;
    faceNormals(positions, triangles, normal, size);
    std::vector<uint32_t> start, around;
    trianglesRound(positions.size(), triangles, start, around);
    const Creases creases{normal, size, start, around, std::cos(crease * kPi / 180.0f)};
    std::vector<Vec3> out(triangles.size() * 3);
    parallelFor(triangles.size(), 2048, [&](size_t begin, size_t end) {
        for (size_t f = begin; f < end; ++f) {
            const Vec3 own = creases.own(f);
            for (size_t c = 0; c < 3; ++c) out[f * 3 + c] = creases.corner(own, triangles[f][c]);
        }
    });
    return out;
}

ShadedTriangles shadedTriangles(const Geometry& geo) {
    ShadedTriangles out;
    const auto P = geo.positions();
    const size_t points = geo.pointCount();
    const Colors colors(geo);
    const Glass glass(geo);
    std::vector<std::array<uint32_t, 3>> tris;
    std::vector<std::array<size_t, 3>> corners;  // the vertex of each corner
    for (size_t prim = 0; prim < geo.primitiveCount(); ++prim) {
        if (!geo.primitiveClosed(prim)) continue;
        const auto pts = geo.primitivePoints(prim);
        const size_t first = geo.primitiveVertexStart(prim);
        for (size_t k = 1; k + 1 < pts.size(); ++k) {
            if (pts[0] >= points || pts[k] >= points || pts[k + 1] >= points) continue;
            tris.push_back({pts[0], pts[k], pts[k + 1]});
            corners.push_back({first, first + k, first + k + 1});
            out.prims.push_back(static_cast<uint32_t>(prim));
        }
    }
    if (tris.empty()) return out;
    const AttributeArray* N = usableNormals(geo);
    std::vector<Vec3> made;
    if (!N) made = cornerNormals(P, tris, kCrease);
    const std::span<const Vec3> pointN = N ? N->read<Vec3>() : std::span<const Vec3>();
    const AttributeArray* restAttr = geo.points().find("rest");
    const std::span<const Vec3> rest =
        restAttr && restAttr->type() == AttrType::Vec3 ? restAttr->read<Vec3>() : std::span<const Vec3>();
    const AttributeArray* vAttr = geo.points().find("v");
    const std::span<const Vec3> v =
        vAttr && vAttr->type() == AttrType::Vec3 ? vAttr->read<Vec3>() : std::span<const Vec3>();
    // Texture coordinates: the corners', else the points' -- two parts or
    // three (u, v, 0).
    auto uvOf = [](const AttributeArray* a) {
        return a && (a->type() == AttrType::Vec2 || a->type() == AttrType::Vec3) ? a : nullptr;
    };
    const AttributeArray* vertexUv = uvOf(geo.vertices().find("uv"));
    const AttributeArray* pointUv = vertexUv ? nullptr : uvOf(geo.points().find("uv"));
    auto uvAt = [&](const AttributeArray& a, size_t i) {
        return a.type() == AttrType::Vec2 ? a.read<Vec2>()[i] : Vec2(a.read<Vec3>()[i]);
    };
    const size_t n = tris.size();
    out.positions.resize(3 * n);
    out.normals.resize(3 * n);
    out.colors.resize(3 * n);
    if (!rest.empty()) out.rest.resize(3 * n);
    if (!v.empty()) out.velocities.resize(3 * n);
    if (vertexUv || pointUv) out.uvs.resize(3 * n);
    out.glass.resize(n);
    parallelFor(n, 4096, [&](size_t begin, size_t end) {
        for (size_t t = begin; t < end; ++t) {
            const float kind = glass.of(out.prims[t]);
            out.glass[t] = kind >= 1.5f ? 2 : kind >= 0.5f ? 1 : 0;
            const Vec3& a = P[tris[t][0]];
            const Vec3 flat = normalize(cross(P[tris[t][1]] - a, P[tris[t][2]] - a));
            for (size_t c = 0; c < 3; ++c) {
                const uint32_t p = tris[t][c];
                out.positions[3 * t + c] = P[p];
                if (!rest.empty()) out.rest[3 * t + c] = rest[p];
                if (!v.empty()) out.velocities[3 * t + c] = v[p];
                if (vertexUv) out.uvs[3 * t + c] = uvAt(*vertexUv, corners[t][c]);
                else if (pointUv) out.uvs[3 * t + c] = uvAt(*pointUv, p);
                Vec3 nrm = N ? normalize(pointN[p]) : made[3 * t + c];
                if (out.glass[t] != 0 && length(flat) > 0.5f) nrm = flat;  // glass is flat, as the viewport has it
                out.normals[3 * t + c] = nrm;
                out.colors[3 * t + c] = colors.at(out.prims[t], corners[t][c], p);
            }
        }
    });
    return out;
}

DisplayGeometry displayOf(const Geometry& geo, size_t maxDots, bool faces) {
    DisplayGeometry d;
    const auto P = geo.positions();
    const size_t points = geo.pointCount();
    const Colors colors(geo);
    const Vec3 detailColor = colors.detail;
    auto cornerColor = [&](size_t prim, size_t vertex, uint32_t point) { return colors.at(prim, vertex, point); };
    const Glass glass(geo);
    auto glassOf = [&](size_t prim) { return glass.of(prim); };

    // Polygons: a fan across each, the corners remembered for their colour.
    std::vector<std::array<uint32_t, 3>> tris;
    std::vector<std::array<size_t, 3>> corners;  // the vertex of each corner
    std::vector<size_t> owner;                   // the primitive of each triangle
    std::vector<uint8_t> used = instancePoints(geo);  // a point that stands for a prototype is no dot
    for (size_t prim = 0; prim < geo.primitiveCount(); ++prim) {
        const auto pts = geo.primitivePoints(prim);
        const size_t first = geo.primitiveVertexStart(prim);
        for (const uint32_t p : pts) {
            if (p < points) used[p] = 1;
        }
        if (!geo.primitiveClosed(prim)) {
            // A polyline: its segments.
            for (size_t k = 0; k + 1 < pts.size(); ++k) {
                if (pts[k] >= points || pts[k + 1] >= points) continue;
                const Vec3 a = cornerColor(prim, first + k, pts[k]);
                const Vec3 b = cornerColor(prim, first + k + 1, pts[k + 1]);
                d.lines.insert(d.lines.end(), {P[pts[k]].x, P[pts[k]].y, P[pts[k]].z, a.x, a.y, a.z, 1.0f});
                d.lines.insert(d.lines.end(),
                               {P[pts[k + 1]].x, P[pts[k + 1]].y, P[pts[k + 1]].z, b.x, b.y, b.z, 1.0f});
                grow(d, P[pts[k]]);
                grow(d, P[pts[k + 1]]);
            }
            continue;
        }
        for (size_t k = 1; k + 1 < pts.size(); ++k) {
            if (pts[0] >= points || pts[k] >= points || pts[k + 1] >= points) continue;
            tris.push_back({pts[0], pts[k], pts[k + 1]});
            corners.push_back({first, first + k, first + k + 1});
            owner.push_back(prim);
        }
    }
    if (!tris.empty()) {
        // Without the faces, their normals only for the glass among them.
        bool wanted = faces;
        for (size_t t = 0; t < tris.size() && !wanted; ++t) wanted = glassOf(owner[t]) >= 0.5f;
        std::vector<Vec3> normals;
        const AttributeArray* N = usableNormals(geo);
        const bool pointNormals = N != nullptr;
        if (!pointNormals && wanted) normals = cornerNormals(P, tris, kCrease);
        const std::span<const Vec3> pointN = pointNormals ? N->read<Vec3>() : std::span<const Vec3>();
        const AttributeArray* v = geo.points().find("v");
        const bool moving = v && v->type() == AttrType::Vec3 && v->size() == points;
        const std::span<const Vec3> pointV = moving ? v->read<Vec3>() : std::span<const Vec3>();
        if (faces) d.triangles.reserve(tris.size() * 27);
        if (moving && faces) d.velocities.reserve(tris.size() * 9);
        for (size_t t = 0; t < tris.size(); ++t) {
            const float kind = glassOf(owner[t]);
            if (!faces && kind < 0.5f) {
                // Drawn as a DisplayMesh: only the box goes round it.
                for (const uint32_t p : tris[t]) grow(d, P[p]);
                continue;
            }
            for (int c = 0; c < 3; ++c) {
                const uint32_t p = tris[t][static_cast<size_t>(c)];
                Vec3 n = pointNormals ? pointN[p] : normals[t * 3 + static_cast<size_t>(c)];
                const Vec3 col = cornerColor(owner[t], corners[t][static_cast<size_t>(c)], p);
                grow(d, P[p]);
                if (kind >= 0.5f) {
                    // Glass is flat: each face its own normal, as its corners
                    // go round -- which way it faces tells where a ray comes
                    // into a piece of it.
                    const Vec3& a = P[tris[t][0]];
                    const Vec3 f = normalize(cross(P[tris[t][1]] - a, P[tris[t][2]] - a));
                    if (length(f) > 0.5f) n = f;
                    d.glass.insert(d.glass.end(), {P[p].x, P[p].y, P[p].z, n.x, n.y, n.z, col.x, col.y, col.z,
                                                   kind >= 1.5f ? 2.0f : 1.0f});
                    continue;
                }
                d.triangles.insert(d.triangles.end(), {P[p].x, P[p].y, P[p].z, n.x, n.y, n.z, col.x, col.y, col.z});
                if (moving) {
                    const Vec3 w = pointV[p];
                    d.velocities.insert(d.velocities.end(), {w.x, w.y, w.z});
                }
            }
        }
    }

    // Loose points: dots -- glass chips as wide, their radius below 0.
    const AttributeArray* pscale = geo.points().find("pscale");
    const bool sized = pscale && pscale->type() == AttrType::Float && pscale->size() == points;
    const AttributeArray* chips = geo.points().find("glass");
    const bool glassy = chips && chips->type() == AttrType::Int && chips->size() == points;
    for (size_t p = 0; p < points; ++p) {
        if (used[p]) continue;
        Vec3 c;
        if (!colorAt(colors.point, p, c)) c = detailColor;
        float r = sized ? std::max(pscale->read<float>()[p], 0.0f) : 0.0f;
        if (glassy && chips->read<int32_t>()[p] > 0 && r > 0.0f) r = -r;
        put(d.dots, P[p], c);
        d.dots.push_back(r);
        grow(d, P[p]);
    }

    // Volumes: a dot in each voxel that is not empty, and its box.
    for (const Volume& v : geo.volumes()) {
        if (!v.values || v.count() == 0) continue;
        const Vec3 lo = v.origin, hi = v.origin + v.size();
        const float frame[7] = {0.55f, 0.62f, 0.75f, 0.8f, 0.0f, 0.0f, 0.0f};
        auto corner = [&](int i) { return Vec3(i & 1 ? hi.x : lo.x, i & 2 ? hi.y : lo.y, i & 4 ? hi.z : lo.z); };
        for (int i = 0; i < 8; ++i) {
            for (const int bit : {1, 2, 4}) {
                if (i & bit) continue;
                for (const Vec3& e : {corner(i), corner(i | bit)}) {
                    d.lines.insert(d.lines.end(), {e.x, e.y, e.z, frame[0], frame[1], frame[2], frame[3]});
                }
            }
        }
        grow(d, lo);
        grow(d, hi);
        const std::vector<float>& values = *v.values;
        float top = 0.0f;
        size_t full = 0;
        for (const float x : values) {
            top = std::max(top, std::fabs(x));
        }
        if (top <= 0.0f) continue;
        const float floor = 0.02f * top;
        for (const float x : values) full += std::fabs(x) > floor ? 1 : 0;
        const size_t room = maxDots > d.dotCount() ? maxDots - d.dotCount() : 0;
        // Every stride-th voxel along each axis, so that they fit.
        int stride = 1;
        while (full / (static_cast<size_t>(stride) * stride * stride) > room && stride < 64) ++stride;
        size_t kept = 0;
        for (int k = 0; k < v.res[2]; k += stride) {
            for (int j = 0; j < v.res[1]; j += stride) {
                for (int i = 0; i < v.res[0]; i += stride) {
                    const float x = v.at(i, j, k);
                    if (std::fabs(x) <= floor) continue;
                    if (kept >= room) {
                        ++d.dotsLeftOut;
                        continue;
                    }
                    const float t = std::clamp(std::fabs(x) / top, 0.0f, 1.0f);
                    // Blue, through magenta, to yellow.
                    const Vec3 c = t < 0.5f ? Vec3(0.15f, 0.25f, 0.9f) * (1.0f - 2.0f * t) + Vec3(0.85f, 0.25f, 0.55f) * (2.0f * t)
                                            : Vec3(0.85f, 0.25f, 0.55f) * (2.0f - 2.0f * t) + Vec3(1.0f, 0.88f, 0.35f) * (2.0f * t - 1.0f);
                    const Vec3 at = v.origin + Vec3(static_cast<float>(i) + 0.5f, static_cast<float>(j) + 0.5f,
                                                    static_cast<float>(k) + 0.5f) * v.voxel;
                    put(d.dots, at, c);
                    d.dots.push_back(0.3f * v.voxel * static_cast<float>(stride));
                    ++kept;
                }
            }
        }
    }
    return d;
}


// --- the displayed node's polygons, indexed ---------------------------------------------

namespace {

static_assert(sizeof(Vec3) == 3 * sizeof(float), "a Vec3 is three floats: compared bit for bit");

/// The same three floats, bit for bit -- +0 and -0 are two, as the
/// triangles have them.
bool sameBits(const float* a, const Vec3& b) { return std::memcmp(a, &b, sizeof(Vec3)) == 0; }

/// The buffer an attribute's elements are in, and their type; nothing for
/// no attribute.
std::pair<const void*, int> identity(const AttributeArray* a) {
    if (!a) return {nullptr, -1};
    return {a->bufferId(), static_cast<int>(a->type())};
}

/// A point attribute of three floats for every point, if there is one.
void growMesh(DisplayMesh& mesh, const Vec3& p) {
    for (int a = 0; a < 3; ++a) {
        mesh.lo[a] = std::min(mesh.lo[a], p[a]);
        mesh.hi[a] = std::max(mesh.hi[a], p[a]);
    }
}

}  // namespace

DisplayMesher::Made DisplayMesher::make(const GeometryPtr& geo, DisplayMesh& mesh) {
    if (geo && sameMaking(*geo) && move(*geo, mesh)) {
        made_ = geo;
        return Made::Moved;
    }
    if (geo) build(*geo, mesh);
    else build(Geometry(), mesh);
    made_ = geo;
    return Made::Anew;
}

bool DisplayMesher::sameMaking(const Geometry& geo) const {
    if (!made_) return false;
    const Geometry& was = *made_;
    // The buffers of what was made are held: one of the same place is the same.
    return geo.pointCount() == was.pointCount() && geo.primitiveCount() == was.primitiveCount() &&
           geo.vertexCount() == was.vertexCount() && geo.vertexPoints().data() == was.vertexPoints().data() &&
           identity(geo.vertices().find("Cd")) == identity(was.vertices().find("Cd")) &&
           identity(geo.points().find("Cd")) == identity(was.points().find("Cd")) &&
           identity(geo.primitives().find("Cd")) == identity(was.primitives().find("Cd")) &&
           identity(geo.detail().find("Cd")) == identity(was.detail().find("Cd")) &&
           identity(geo.primitives().find("glass")) == identity(was.primitives().find("glass")) &&
           (usableNormals(geo) != nullptr) == pointNormals_ && (pointVectors(geo, "v") != nullptr) == moving_ &&
           geo.volumes().empty() == was.volumes().empty() &&
           identity(geo.points().find("instance")) == identity(was.points().find("instance")) &&
           geo.prototypeCount() == was.prototypeCount();
}

void DisplayMesher::build(const Geometry& geo, DisplayMesh& mesh) {
    mesh = DisplayMesh();
    tris_.clear();
    drawn_.clear();
    start_.clear();
    around_.clear();
    vertexPoint_.clear();
    vertexCorner_.clear();
    const size_t points = geo.pointCount();
    const auto P = geo.positions();
    const Colors colors(geo);
    const Glass glass(geo);
    const AttributeArray* N = usableNormals(geo);
    const AttributeArray* v = pointVectors(geo, "v");
    const AttributeArray* through = geo.primitives().find("translucency");
    if (through && through->type() != AttrType::Float) through = nullptr;
    const auto throughOf = through ? through->read<float>() : std::span<const float>();
    pointNormals_ = N != nullptr;
    moving_ = v != nullptr;
    rest_ = !geo.volumes().empty();

    // The fans across the closed polygons, as displayOf makes them; lines
    // and loose points are the rest.
    std::vector<std::array<uint32_t, 3>> corners;  // the vertex of each corner, for its colour
    std::vector<uint32_t> owner;
    std::vector<uint8_t> used = instancePoints(geo);  // a point that stands for a prototype is no loose point
    tris_.reserve(geo.vertexCount());
    corners.reserve(geo.vertexCount());
    owner.reserve(geo.vertexCount());
    for (size_t prim = 0; prim < geo.primitiveCount(); ++prim) {
        const auto pts = geo.primitivePoints(prim);
        const size_t first = geo.primitiveVertexStart(prim);
        for (const uint32_t p : pts) {
            if (p < points) used[p] = 1;
        }
        if (!geo.primitiveClosed(prim)) {
            rest_ = rest_ || pts.size() > 1;
            continue;
        }
        for (size_t k = 1; k + 1 < pts.size(); ++k) {
            if (pts[0] >= points || pts[k] >= points || pts[k + 1] >= points) continue;
            tris_.push_back({pts[0], pts[k], pts[k + 1]});
            corners.push_back({static_cast<uint32_t>(first), static_cast<uint32_t>(first + k), static_cast<uint32_t>(first + k + 1)});
            owner.push_back(static_cast<uint32_t>(prim));
        }
    }
    rest_ = rest_ || std::find(used.begin(), used.end(), uint8_t(0)) != used.end();
    for (size_t t = 0; t < tris_.size(); ++t) {
        if (glass.of(owner[t]) >= 0.5f) rest_ = true;
        else drawn_.push_back(static_cast<uint32_t>(t));
    }
    if (drawn_.empty()) return;

    // Each corner's normal: the points' N, else the faces round it that
    // bend less than the crease -- those found for all at once, in parallel.
    const auto pointN = N ? N->read<Vec3>() : std::span<const Vec3>();
    const size_t count = drawn_.size() * 3;
    std::vector<Vec3> normal;
    if (!pointNormals_) {
        std::vector<Vec3> faceNormal;
        std::vector<float> faceSize;
        faceNormals(P, tris_, faceNormal, faceSize);
        trianglesRound(points, tris_, start_, around_);
        const Creases creases{faceNormal, faceSize, start_, around_, std::cos(kCrease * kPi / 180.0f)};
        normal.resize(count);
        parallelFor(drawn_.size(), 2048, [&](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i) {
                const uint32_t t = drawn_[i];
                const Vec3 own = creases.own(t);
                for (size_t c = 0; c < 3; ++c) normal[i * 3 + c] = creases.corner(own, tris_[t][c]);
            }
        });
    }

    // One vertex for the corners of a point that have the same normal and
    // colour: a point's vertices chained, the first found first.
    const auto pointV = v ? v->read<Vec3>() : std::span<const Vec3>();
    std::vector<int32_t> firstVertex(points, -1), nextVertex;
    nextVertex.reserve(points);
    vertexPoint_.reserve(points);
    vertexCorner_.reserve(points);
    mesh.places.reserve(points * 6);
    mesh.colors.reserve(points * 3);
    if (moving_) mesh.velocities.reserve(points * 3);
    if (through) mesh.translucency.reserve(points);
    mesh.indices.resize(count);
    for (size_t k = 0; k < count; ++k) {
        const uint32_t t = drawn_[k / 3];
        const uint32_t p = tris_[t][k % 3];
        const Vec3 n = pointNormals_ ? pointN[p] : normal[k];
        const Vec3 c = colors.at(owner[t], corners[t][k % 3], p);
        const float lets = through ? std::clamp(throughOf[owner[t]], 0.0f, 1.0f) : 0.0f;
        int32_t found = -1, last = -1;
        for (int32_t w = firstVertex[p]; w >= 0; w = nextVertex[static_cast<size_t>(w)]) {
            const size_t at = static_cast<size_t>(w);
            if (sameBits(&mesh.places[at * 6 + 3], n) && sameBits(&mesh.colors[at * 3], c) &&
                (!through || mesh.translucency[at] == lets)) {
                found = w;
                break;
            }
            last = w;
        }
        if (found < 0) {
            found = static_cast<int32_t>(vertexPoint_.size());
            vertexPoint_.push_back(p);
            vertexCorner_.push_back(static_cast<uint32_t>(k));
            nextVertex.push_back(-1);
            if (last >= 0) nextVertex[static_cast<size_t>(last)] = found;
            else firstVertex[p] = found;
            const Vec3& at = P[p];
            mesh.places.insert(mesh.places.end(), {at.x, at.y, at.z, n.x, n.y, n.z});
            mesh.colors.insert(mesh.colors.end(), {c.x, c.y, c.z});
            if (through) mesh.translucency.push_back(lets);
            if (moving_) mesh.velocities.insert(mesh.velocities.end(), {pointV[p].x, pointV[p].y, pointV[p].z});
            growMesh(mesh, at);
        }
        mesh.indices[k] = static_cast<uint32_t>(found);
    }
}

bool DisplayMesher::move(const Geometry& geo, DisplayMesh& mesh) {
    const size_t count = vertexPoint_.size();
    // Not the mesh it made: made anew.
    if (mesh.vertexCount() != count || mesh.places.size() != count * 6 || mesh.indices.size() != drawn_.size() * 3) {
        return false;
    }
    const auto P = geo.positions();
    const AttributeArray* N = usableNormals(geo);
    const auto pointN = N ? N->read<Vec3>() : std::span<const Vec3>();
    std::vector<Vec3> faceNormal;
    std::vector<float> faceSize;
    if (!pointNormals_) faceNormals(P, tris_, faceNormal, faceSize);
    const Creases creases{faceNormal, faceSize, start_, around_, std::cos(kCrease * kPi / 180.0f)};
    auto cornerNormal = [&](size_t k) {
        const uint32_t t = drawn_[k / 3];
        const uint32_t p = tris_[t][k % 3];
        return pointNormals_ ? pointN[p] : creases.corner(creases.own(t), p);
    };
    // Each vertex: its point's place, its first corner's normal.
    parallelFor(count, 4096, [&](size_t begin, size_t end) {
        for (size_t w = begin; w < end; ++w) {
            const Vec3& at = P[vertexPoint_[w]];
            const Vec3 n = cornerNormal(vertexCorner_[w]);
            float* out = &mesh.places[w * 6];
            out[0] = at.x;
            out[1] = at.y;
            out[2] = at.z;
            out[3] = n.x;
            out[4] = n.y;
            out[5] = n.z;
        }
    });
    // Every other corner of a vertex the same normal -- else a fold has
    // parted them, and it is made anew. (The points' N are one a point.)
    if (!pointNormals_) {
        const auto chunks = chunkRanges(mesh.indices.size(), 8192);
        std::vector<uint8_t> parted(chunks.size(), 0);
        TaskPool::instance().run(chunks.size(), [&](size_t c) {
            for (size_t k = chunks[c].first; k < chunks[c].second; ++k) {
                if (!sameBits(&mesh.places[static_cast<size_t>(mesh.indices[k]) * 6 + 3], cornerNormal(k))) {
                    parted[c] = 1;
                    return;
                }
            }
        });
        if (std::find(parted.begin(), parted.end(), uint8_t(1)) != parted.end()) return false;
    }
    if (moving_) {
        const auto pointV = pointVectors(geo, "v")->read<Vec3>();
        for (size_t w = 0; w < count; ++w) {
            const Vec3& u = pointV[vertexPoint_[w]];
            mesh.velocities[w * 3] = u.x;
            mesh.velocities[w * 3 + 1] = u.y;
            mesh.velocities[w * 3 + 2] = u.z;
        }
    }
    mesh.lo = Vec3(1e30f, 1e30f, 1e30f);
    mesh.hi = Vec3(-1e30f, -1e30f, -1e30f);
    for (size_t w = 0; w < count; ++w) growMesh(mesh, P[vertexPoint_[w]]);
    return true;
}

DisplayInstances instancesOf(const Geometry& geo) {
    DisplayInstances out;
    const auto byPrototype = instancesByPrototype(geo);
    if (byPrototype.empty()) return out;
    const std::vector<Placement> places = placementsOf(geo);
    const AttributeArray* tint = geo.points().find("tint");
    if (tint && tint->type() != AttrType::Vec3) tint = nullptr;
    for (size_t k = 0; k < byPrototype.size(); ++k) {
        const GeometryPtr& prototype = geo.prototypes()[k];
        if (byPrototype[k].empty() || !prototype || prototype->pointCount() == 0) continue;
        // The box round the prototype: where each copy of it reaches, its
        // corners placed.
        Vec3 lo(1e30f, 1e30f, 1e30f), hi(-1e30f, -1e30f, -1e30f);
        for (const Vec3& p : prototype->positions()) {
            for (int a = 0; a < 3; ++a) {
                lo[a] = std::min(lo[a], p[a]);
                hi[a] = std::max(hi[a], p[a]);
            }
        }
        const auto& points = byPrototype[k];
        std::vector<float> f(points.size() * DisplayInstances::kFloats);
        for (size_t i = 0; i < points.size(); ++i) {
            const uint32_t p = points[i];
            const Placement& pl = places[p];
            const Vec3 t = tint ? tint->read<Vec3>()[p] : Vec3(1.0f, 1.0f, 1.0f);
            float* o = &f[i * DisplayInstances::kFloats];
            o[0] = pl.at.x, o[1] = pl.at.y, o[2] = pl.at.z, o[3] = pl.scale;
            o[4] = pl.orient.x, o[5] = pl.orient.y, o[6] = pl.orient.z, o[7] = pl.orient.w;
            o[8] = t.x, o[9] = t.y, o[10] = t.z, o[11] = 1.0f;
            for (int c = 0; c < 8; ++c) {
                const Vec3 corner = pl.point(Vec3(c & 1 ? hi.x : lo.x, c & 2 ? hi.y : lo.y, c & 4 ? hi.z : lo.z));
                for (int a = 0; a < 3; ++a) {
                    out.lo[a] = std::min(out.lo[a], corner[a]);
                    out.hi[a] = std::max(out.hi[a], corner[a]);
                }
            }
        }
        out.prototypes.push_back(prototype);
        out.placements.push_back(std::move(f));
        out.centers.push_back((lo + hi) * 0.5f);
        out.radii.push_back(0.5f * length(hi - lo));
    }
    return out;
}

std::array<std::vector<float>, 3> placementsByDetail(std::span<const float> placements, const Vec3& center, float radius,
                                                     const Vec3& eye) {
    std::array<std::vector<float>, 3> out;
    constexpr size_t n = DisplayInstances::kFloats;
    for (size_t i = 0; i + n <= placements.size(); i += n) {
        const float* o = &placements[i];
        const float scale = o[3];
        const Vec3 middle = Vec3(o[0], o[1], o[2]) + quatRotate(Vec4(o[4], o[5], o[6], o[7]), center * scale);
        // How big it looks: its radius over how far it is.
        const float looks = radius * std::fabs(scale) / std::max(length(middle - eye), 1e-6f);
        for (size_t level = 0; level < 3; ++level) {
            if (looks >= kDetailSize[level]) {
                out[level].insert(out[level].end(), o, o + n);
                break;
            }
        }
    }
    return out;
}

}  // namespace pg::sim
