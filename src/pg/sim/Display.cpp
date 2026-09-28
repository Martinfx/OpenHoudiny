#include "pg/sim/Display.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace pg::sim {
namespace {

constexpr float kPi = 3.14159265358979323846f;

/// Cd of one element of an attribute set, if it has one: a colour, or a grey.
bool colorOf(const AttributeSet& set, size_t i, Vec3& out) {
    const AttributeArray* a = set.find("Cd");
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

void grow(DisplayGeometry& d, const Vec3& p) {
    for (int a = 0; a < 3; ++a) {
        d.lo[a] = std::min(d.lo[a], p[a]);
        d.hi[a] = std::max(d.hi[a], p[a]);
    }
}

void put(std::vector<float>& to, const Vec3& a, const Vec3& b) { to.insert(to.end(), {a.x, a.y, a.z, b.x, b.y, b.z}); }

}  // namespace

std::vector<Vec3> cornerNormals(const std::vector<Vec3>& positions, const std::vector<std::array<uint32_t, 3>>& triangles,
                                float crease) {
    const size_t faces = triangles.size(), verts = positions.size();
    std::vector<Vec3> faceNormal(faces);  // as long as twice the area: bigger faces count more
    for (size_t f = 0; f < faces; ++f) {
        const auto& t = triangles[f];
        faceNormal[f] = cross(positions[t[1]] - positions[t[0]], positions[t[2]] - positions[t[0]]);
    }
    // The faces round each point, packed.
    std::vector<uint32_t> start(verts + 1, 0), around(faces * 3);
    for (const auto& t : triangles) {
        for (const uint32_t v : t) ++start[v + 1];
    }
    for (size_t v = 0; v < verts; ++v) start[v + 1] += start[v];
    std::vector<uint32_t> fill(start.begin(), start.end() - 1);
    for (size_t f = 0; f < faces; ++f) {
        for (const uint32_t v : triangles[f]) around[fill[v]++] = static_cast<uint32_t>(f);
    }
    const float cosine = std::cos(crease * kPi / 180.0f);
    std::vector<Vec3> out;
    out.reserve(faces * 3);
    for (size_t f = 0; f < faces; ++f) {
        const float area = length(faceNormal[f]);
        const Vec3 own = area > 0.0f ? faceNormal[f] * (1.0f / area) : Vec3(0.0f, 1.0f, 0.0f);
        for (const uint32_t v : triangles[f]) {
            Vec3 sum;
            for (uint32_t k = start[v]; k < start[v + 1]; ++k) {
                const Vec3& other = faceNormal[around[k]];
                const float l = length(other);
                if (l > 0.0f && dot(other, own) >= cosine * l) sum += other;
            }
            const float l = length(sum);
            out.push_back(l > 0.0f ? sum * (1.0f / l) : own);
        }
    }
    return out;
}

DisplayGeometry displayOf(const Geometry& geo, size_t maxDots) {
    DisplayGeometry d;
    const auto P = geo.positions();
    const size_t points = geo.pointCount();
    const Vec3 grey(0.72f, 0.72f, 0.74f);
    Vec3 detailColor = grey;
    colorOf(geo.detail(), 0, detailColor);
    // The colour of a corner: the first of vertex, point, primitive, detail.
    auto cornerColor = [&](size_t prim, size_t vertex, uint32_t point) {
        Vec3 c;
        if (colorOf(geo.vertices(), vertex, c) || colorOf(geo.points(), point, c) || colorOf(geo.primitives(), prim, c)) {
            return c;
        }
        return detailColor;
    };

    // Glass: the primitives whose attribute glass is 1 or more.
    const AttributeArray* glassAttr = geo.primitives().find("glass");
    auto glassOf = [&](size_t prim) -> float {
        if (!glassAttr || prim >= glassAttr->size()) return 0.0f;
        if (glassAttr->type() == AttrType::Int) return static_cast<float>(glassAttr->read<int32_t>()[prim]);
        if (glassAttr->type() == AttrType::Float) return glassAttr->read<float>()[prim];
        return 0.0f;
    };

    // Polygons: a fan across each, the corners remembered for their colour.
    std::vector<std::array<uint32_t, 3>> tris;
    std::vector<std::array<size_t, 3>> corners;  // the vertex of each corner
    std::vector<size_t> owner;                   // the primitive of each triangle
    std::vector<uint8_t> used(points, 0);
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
        std::vector<Vec3> normals;
        const AttributeArray* N = geo.points().find("N");
        const bool pointNormals = N && N->type() == AttrType::Vec3 && N->size() == points;
        if (!pointNormals) normals = cornerNormals(std::vector<Vec3>(P.begin(), P.end()), tris);
        const AttributeArray* v = geo.points().find("v");
        const bool moving = v && v->type() == AttrType::Vec3 && v->size() == points;
        d.triangles.reserve(tris.size() * 27);
        if (moving) d.velocities.reserve(tris.size() * 9);
        for (size_t t = 0; t < tris.size(); ++t) {
            const float kind = glassOf(owner[t]);
            for (int c = 0; c < 3; ++c) {
                const uint32_t p = tris[t][static_cast<size_t>(c)];
                Vec3 n = pointNormals ? N->read<Vec3>()[p] : normals[t * 3 + static_cast<size_t>(c)];
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
                    const Vec3 w = v->read<Vec3>()[p];
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
        if (!colorOf(geo.points(), p, c)) c = detailColor;
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

}  // namespace pg::sim
