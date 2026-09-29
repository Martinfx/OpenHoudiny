#include "pg/sim/Rigid.h"

#include "pg/core/Spatial.h"
#include "pg/nodes/Rebuild.h"
#include "pg/sim/Mesh.h"
#include "pg/sim/Shape.h"
#include "pg/sim/Shared.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <mutex>
#include <numeric>
#include <tuple>
#include <type_traits>

#ifdef PG_HAVE_JOLT
#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayerInterfaceTable.h>
#include <Jolt/Physics/Collision/BroadPhase/ObjectVsBroadPhaseLayerFilterTable.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollidePointResult.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/ObjectLayerPairFilterTable.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/CylinderShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/Body/BodyLockMulti.h>
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <Jolt/Physics/Constraints/SixDOFConstraint.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>
#endif

namespace pg::sim {

RigidScene RigidScene::sanitized() const {
    RigidScene r = *this;
    RigidSettings& s = r.solver;
    const RigidSettings d;
    auto finite = [](float v, float fallback) { return std::isfinite(v) ? v : fallback; };
    s.density = std::clamp(finite(s.density, d.density), 1.0f, 1e6f);
    s.friction = std::clamp(finite(s.friction, d.friction), 0.0f, 10.0f);
    s.bounce = std::clamp(finite(s.bounce, d.bounce), 0.0f, 1.0f);
    s.glue = std::clamp(finite(s.glue, d.glue), 0.0f, 1e15f);
    s.spread = std::clamp(finite(s.spread, d.spread), 0.0f, 1.0f);
    s.rings = std::clamp(s.rings, 0, 1 << 20);
    s.rebarStrength = std::clamp(finite(s.rebarStrength, d.rebarStrength), 1e6f, 1e11f);
    s.bond = std::clamp(finite(s.bond, d.bond), 0.0f, 1e10f);
    s.stretch = std::clamp(finite(s.stretch, d.stretch), 0.0f, 100.0f);
    for (int a = 0; a < 3; ++a) s.gravity[a] = std::clamp(finite(s.gravity[a], 0.0f), -1000.0f, 1000.0f);
    s.substeps = std::clamp(s.substeps, 1, 16);
    s.dust = std::clamp(finite(s.dust, d.dust), 0.0f, 1000.0f);
    s.impactDust = std::clamp(finite(s.impactDust, d.impactDust), 0.0f, 1000.0f);
    s.dustSize = std::clamp(finite(s.dustSize, d.dustSize), 0.01f, 100.0f);
    s.debris = std::clamp(finite(s.debris, d.debris), 0.0f, 100.0f);
    s.trail = std::clamp(finite(s.trail, d.trail), 0.0f, 100.0f);
    s.air = std::clamp(finite(s.air, d.air), 0.0f, 100.0f);
    s.guideStrength = std::clamp(finite(s.guideStrength, d.guideStrength), 0.0f, 1.0f);
    s.guideUntil = std::clamp(finite(s.guideUntil, d.guideUntil), 0.0f, 1e6f);
    s.guideReach = std::clamp(finite(s.guideReach, d.guideReach), 0.0f, 1e6f);
    s.timeStep = std::clamp(finite(s.timeStep, d.timeStep), 1e-4f, 1.0f);
    detail::sanitize(r.colliders);
    return r;
}

Vec3 RigidPose::apply(const Vec3& p) const {
    // q p q*, for a unit quaternion: p + 2 w (u x p) + 2 u x (u x p).
    const Vec3 u(rotation.x, rotation.y, rotation.z);
    const Vec3 t = cross(u, p) * 2.0f;
    return position + p + t * rotation.w + cross(u, t);
}

namespace {

struct UnionFind {
    std::vector<uint32_t> parent;
    explicit UnionFind(size_t n) : parent(n) { std::iota(parent.begin(), parent.end(), 0u); }
    uint32_t find(uint32_t x) {
        while (parent[x] != x) x = parent[x] = parent[parent[x]];
        return x;
    }
    void unite(uint32_t a, uint32_t b) {
        a = find(a);
        b = find(b);
        if (a != b) parent[std::max(a, b)] = std::min(a, b);
    }
};

/// Quaternions x, y, z, w: `a` after `b`.
Vec4 turnAfter(const Vec4& a, const Vec4& b) {
    return Vec4(a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z);
}

/// `v` turned by the unit quaternion `q`: v + 2 w (u x v) + 2 u x (u x v).
Vec3 turned(const Vec4& q, const Vec3& v) {
    const Vec3 u(q.x, q.y, q.z);
    const Vec3 t = cross(u, v) * 2.0f;
    return v + t * q.w + cross(u, t);
}

Vec4 unitTurn(const Vec4& q) {
    const float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    return n > 1e-20f ? Vec4(q.x / n, q.y / n, q.z / n, q.w / n) : Vec4(0.0f, 0.0f, 0.0f, 1.0f);
}

/// The turn that takes points at rest nearest to where they have been
/// moved, as a rigid body can: `spread` sums, over the points, where each
/// is moved to from the middle of those times where it rests from theirs
/// -- (moved - middle)(rest - middle)^T, column by column. From `from` on,
/// turned the way the spread still asks until it asks nothing more (Müller,
/// Bender, Chentanez and Macklin 2016: the rotational part of a
/// deformation).
Vec4 bestTurn(const Vec3 spread[3], Vec4 from) {
    Vec4 q = unitTurn(from);
    for (int i = 0; i < 64; ++i) {
        const Vec3 axes[3] = {turned(q, Vec3(1.0f, 0.0f, 0.0f)), turned(q, Vec3(0.0f, 1.0f, 0.0f)),
                              turned(q, Vec3(0.0f, 0.0f, 1.0f))};
        Vec3 spin;
        float along = 0.0f;
        for (int c = 0; c < 3; ++c) {
            spin += cross(axes[c], spread[c]);
            along += dot(axes[c], spread[c]);
        }
        spin = spin * (1.0f / (std::fabs(along) + 1e-20f));
        const float angle = length(spin);
        if (!(angle > 1e-7f)) break;
        const Vec3 axis = spin * (std::sin(0.5f * angle) / angle);
        q = unitTurn(turnAfter(Vec4(axis.x, axis.y, axis.z, std::cos(0.5f * angle)), q));
    }
    return q;
}

}  // namespace

std::vector<int32_t> pieceOfPrimitives(const Geometry& pieces, const std::string& attribute, int& count) {
    const size_t nprims = pieces.primitiveCount();
    std::vector<int32_t> out(nprims, 0);
    count = 0;
    // The values of the attribute, numbered in their order.
    auto number = [&](const std::vector<int32_t>& values) {
        std::vector<int32_t> sorted = values;
        std::sort(sorted.begin(), sorted.end());
        sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
        for (size_t p = 0; p < nprims; ++p) {
            out[p] = static_cast<int32_t>(std::lower_bound(sorted.begin(), sorted.end(), values[p]) - sorted.begin());
        }
        count = static_cast<int>(sorted.size());
    };
    if (const AttributeArray* a = pieces.primitives().find(attribute); a && a->type() == AttrType::Int) {
        const auto v = a->read<int32_t>();
        number(std::vector<int32_t>(v.begin(), v.end()));
        return out;
    }
    if (const AttributeArray* a = pieces.points().find(attribute); a && a->type() == AttrType::Int) {
        const auto v = a->read<int32_t>();
        std::vector<int32_t> values(nprims, 0);
        for (size_t p = 0; p < nprims; ++p) {
            const auto c = pieces.primitivePoints(p);
            if (!c.empty()) values[p] = v[c[0]];
        }
        number(values);
        return out;
    }
    // None: what touches what -- primitives that share points are one piece.
    UnionFind uf(pieces.pointCount());
    for (size_t p = 0; p < nprims; ++p) {
        const auto c = pieces.primitivePoints(p);
        for (size_t i = 1; i < c.size(); ++i) uf.unite(c[0], c[i]);
    }
    std::vector<int32_t> roots(nprims, 0);
    for (size_t p = 0; p < nprims; ++p) {
        const auto c = pieces.primitivePoints(p);
        roots[p] = c.empty() ? -1 - static_cast<int32_t>(p) : static_cast<int32_t>(uf.find(c[0]));
    }
    number(roots);
    return out;
}

RigidGuideRest rigidGuideRest(const Geometry& pieces, const std::string& attribute) {
    RigidGuideRest r;
    int count = 0;
    const std::vector<int32_t> piece = pieceOfPrimitives(pieces, attribute, count);
    // Each point's piece: that of the first primitive that has it.
    r.pieceOfPoint.assign(pieces.pointCount(), -1);
    for (size_t p = 0; p < pieces.primitiveCount(); ++p) {
        for (const uint32_t i : pieces.primitivePoints(p)) {
            if (r.pieceOfPoint[i] < 0) r.pieceOfPoint[i] = piece[p];
        }
    }
    r.points = pieces.pointCount();
    const auto at = pieces.positions();
    std::vector<double> sums(static_cast<size_t>(count) * 4, 0.0);
    for (size_t i = 0; i < r.pieceOfPoint.size(); ++i) {
        if (r.pieceOfPoint[i] < 0) continue;
        double* s = sums.data() + 4 * static_cast<size_t>(r.pieceOfPoint[i]);
        for (int a = 0; a < 3; ++a) s[a] += at[i][a];
        s[3] += 1.0;
    }
    r.middle.resize(static_cast<size_t>(count));
    for (size_t k = 0; k < r.middle.size(); ++k) {
        const double* s = sums.data() + 4 * k;
        const double w = s[3] > 0.0 ? 1.0 / s[3] : 0.0;
        r.middle[k] = Vec3(static_cast<float>(s[0] * w), static_cast<float>(s[1] * w), static_cast<float>(s[2] * w));
    }
    return r;
}

std::shared_ptr<const RigidGuide> rigidGuide(const Geometry& pieces, const RigidGuideRest& rest, const Geometry& guide) {
    if (guide.pointCount() != pieces.pointCount() || pieces.pointCount() == 0 || rest.points != pieces.pointCount()) {
        return nullptr;
    }
    const size_t count = rest.middle.size();
    const auto at = pieces.positions(), moved = guide.positions();
    // Where each piece's points are moved to on average; how they spread
    // there against how they spread at rest.
    std::vector<double> sums(count * 4, 0.0);
    for (size_t i = 0; i < rest.pieceOfPoint.size(); ++i) {
        if (rest.pieceOfPoint[i] < 0) continue;
        double* s = sums.data() + 4 * static_cast<size_t>(rest.pieceOfPoint[i]);
        for (int a = 0; a < 3; ++a) s[a] += moved[i][a];
        s[3] += 1.0;
    }
    std::vector<Vec3> movedMiddle(count);
    for (size_t k = 0; k < count; ++k) {
        const double* s = sums.data() + 4 * k;
        const double w = s[3] > 0.0 ? 1.0 / s[3] : 0.0;
        movedMiddle[k] = Vec3(static_cast<float>(s[0] * w), static_cast<float>(s[1] * w), static_cast<float>(s[2] * w));
    }
    std::vector<double> spread(count * 9, 0.0);
    for (size_t i = 0; i < rest.pieceOfPoint.size(); ++i) {
        if (rest.pieceOfPoint[i] < 0) continue;
        const size_t k = static_cast<size_t>(rest.pieceOfPoint[i]);
        const Vec3 a = moved[i] - movedMiddle[k], b = at[i] - rest.middle[k];
        double* m = spread.data() + 9 * k;
        for (int col = 0; col < 3; ++col) {
            for (int row = 0; row < 3; ++row) m[3 * col + row] += static_cast<double>(a[row]) * b[col];
        }
    }
    auto out = std::make_shared<RigidGuide>();
    out->pieces.resize(count);
    for (size_t k = 0; k < count; ++k) {
        RigidPose& pose = out->pieces[k];
        if (sums[4 * k + 3] <= 0.0) continue;  // nothing to it: where it rests
        const double* m = spread.data() + 9 * k;
        Vec3 columns[3];
        for (int col = 0; col < 3; ++col) {
            columns[col] = Vec3(static_cast<float>(m[3 * col]), static_cast<float>(m[3 * col + 1]),
                                static_cast<float>(m[3 * col + 2]));
        }
        pose.rotation = bestTurn(columns, Vec4(0.0f, 0.0f, 0.0f, 1.0f));
        pose.position = movedMiddle[k] - turned(pose.rotation, rest.middle[k]);
    }
    return out;
}

std::shared_ptr<const RigidGuide> rigidGuide(const Geometry& pieces, const Geometry& guide, const std::string& attribute) {
    if (guide.pointCount() != pieces.pointCount() || pieces.pointCount() == 0) return nullptr;
    return rigidGuide(pieces, rigidGuideRest(pieces, attribute), guide);
}

std::vector<Vec3> rigidPositions(const Geometry& pieces) {
    const auto P = pieces.positions();
    std::vector<Vec3> out(P.begin(), P.end());
    const AttributeArray* proxy = pieces.points().find("proxy");
    if (!proxy || proxy->type() != AttrType::Vec3 || proxy->size() != out.size()) return out;
    const auto Q = proxy->read<Vec3>();
    for (size_t i = 0; i < out.size(); ++i) {
        // Merged with pieces that had none, a point has a proxy of 0: its own place.
        if (Q[i] == Vec3() && length(out[i]) > 1e-3f) continue;
        out[i] = Q[i];
    }
    return out;
}

namespace {

/// A number of an attribute of body `k`: on its first primitive, else that
/// one's first point; `fallback` when neither has it.
float numberOf(const Geometry& geo, const RigidLayout& L, int k, const char* name, float fallback) {
    const uint32_t prim = L.prims[static_cast<size_t>(k)].front();
    auto read = [](const AttributeArray* a, size_t i, float& out) {
        if (!a || i >= a->size()) return false;
        if (a->type() == AttrType::Float) out = a->read<float>()[i];
        else if (a->type() == AttrType::Int) out = static_cast<float>(a->read<int32_t>()[i]);
        else return false;
        return true;
    };
    float out = fallback;
    if (read(geo.primitives().find(name), prim, out)) return out;
    const auto c = geo.primitivePoints(prim);
    if (!c.empty() && read(geo.points().find(name), c[0], out)) return out;
    return fallback;
}

}  // namespace

// --- The layout: bodies, and where they touch ----------------------------------------

namespace {

using P2 = std::array<double, 2>;

double cross2(const P2& a, const P2& b, const P2& c) {
    return (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
}

double signedArea(const std::vector<P2>& q) {
    double s = 0.0;
    for (size_t i = 0; i < q.size(); ++i) {
        const P2& a = q[i];
        const P2& b = q[(i + 1) % q.size()];
        s += a[0] * b[1] - b[0] * a[1];
    }
    return 0.5 * s;
}

/// A polygon counter-clockwise as convex pieces: itself when it is convex,
/// else the triangles of its ears.
std::vector<std::vector<P2>> convexPieces(std::vector<P2> q) {
    if (signedArea(q) < 0.0) std::reverse(q.begin(), q.end());
    bool convex = true;
    for (size_t i = 0; i < q.size() && convex; ++i) {
        if (cross2(q[i], q[(i + 1) % q.size()], q[(i + 2) % q.size()]) < -1e-12) convex = false;
    }
    if (convex) return {q};
    std::vector<std::vector<P2>> out;
    std::vector<size_t> left(q.size());
    std::iota(left.begin(), left.end(), size_t{0});
    auto inside = [](const P2& p, const P2& a, const P2& b, const P2& c) {
        return cross2(a, b, p) > 0.0 && cross2(b, c, p) > 0.0 && cross2(c, a, p) > 0.0;
    };
    size_t guard = 0;
    while (left.size() > 3 && guard++ < q.size() * q.size()) {
        bool cut = false;
        for (size_t i = 0; i < left.size(); ++i) {
            const P2& a = q[left[(i + left.size() - 1) % left.size()]];
            const P2& b = q[left[i]];
            const P2& c = q[left[(i + 1) % left.size()]];
            if (cross2(a, b, c) <= 0.0) continue;
            bool empty = true;
            for (const size_t k : left) {
                if (&q[k] != &a && &q[k] != &b && &q[k] != &c && inside(q[k], a, b, c)) {
                    empty = false;
                    break;
                }
            }
            if (!empty) continue;
            out.push_back({a, b, c});
            left.erase(left.begin() + static_cast<long>(i));
            cut = true;
            break;
        }
        if (!cut) break;
    }
    if (left.size() == 3) out.push_back({q[left[0]], q[left[1]], q[left[2]]});
    return out;
}

/// `subject` cut to the convex, counter-clockwise `window` (Sutherland-Hodgman).
std::vector<P2> clipConvex(std::vector<P2> subject, const std::vector<P2>& window) {
    for (size_t e = 0; e < window.size() && !subject.empty(); ++e) {
        const P2& a = window[e];
        const P2& b = window[(e + 1) % window.size()];
        std::vector<P2> next;
        for (size_t i = 0; i < subject.size(); ++i) {
            const P2& p = subject[i];
            const P2& q = subject[(i + 1) % subject.size()];
            const double dp = cross2(a, b, p), dq = cross2(a, b, q);
            if (dp >= 0.0) next.push_back(p);
            if ((dp >= 0.0) != (dq >= 0.0)) {
                const double t = dp / (dp - dq);
                next.push_back({p[0] + (q[0] - p[0]) * t, p[1] + (q[1] - p[1]) * t});
            }
        }
        subject = std::move(next);
    }
    return subject;
}

/// A face of a part: its corners, its plane, the box round it.
struct Face {
    uint32_t part = 0;
    std::vector<Vec3> corners;
    Vec3 normal;  ///< unit, as its corners turn
    double d = 0.0;
    Vec3 lo, hi;
};

/// Where faces `f` and `g`, in one plane, overlap: the area, and the middle
/// (added to `sum`, weighted by the area).
double overlapArea(const Face& f, const Face& g, Vec3& sum) {
    const Vec3 n = f.normal;
    const Vec3 axis = std::fabs(n.x) < 0.6f ? Vec3(1.0f, 0.0f, 0.0f) : Vec3(0.0f, 1.0f, 0.0f);
    const Vec3 u = normalize(cross(n, axis)), v = cross(n, u);
    const Vec3 o = f.corners[0];
    auto flat = [&](const std::vector<Vec3>& c) {
        std::vector<P2> q;
        q.reserve(c.size());
        for (const Vec3& p : c) q.push_back({static_cast<double>(dot(p - o, u)), static_cast<double>(dot(p - o, v))});
        return q;
    };
    std::vector<P2> a = flat(f.corners);
    if (signedArea(a) < 0.0) std::reverse(a.begin(), a.end());
    double area = 0.0, cx = 0.0, cy = 0.0;
    for (const std::vector<P2>& window : convexPieces(flat(g.corners))) {
        for (const std::vector<P2>& piece : convexPieces(a)) {
            const std::vector<P2> r = clipConvex(piece, window);
            if (r.size() < 3) continue;
            // Area and centroid (shoelace).
            double s = 0.0, x = 0.0, y = 0.0;
            for (size_t i = 0; i < r.size(); ++i) {
                const P2& p = r[i];
                const P2& q = r[(i + 1) % r.size()];
                const double c = p[0] * q[1] - q[0] * p[1];
                s += c;
                x += (p[0] + q[0]) * c;
                y += (p[1] + q[1]) * c;
            }
            if (std::fabs(s) < 1e-18) continue;
            const double ar = 0.5 * s;
            area += std::fabs(ar);
            cx += x / (6.0 * ar) * std::fabs(ar);
            cy += y / (6.0 * ar) * std::fabs(ar);
        }
    }
    if (area > 0.0) sum += o * static_cast<float>(area) + u * static_cast<float>(cx) + v * static_cast<float>(cy);
    return area;
}

/// The closed polygons `prims` of a part, those that lie in one plane side
/// by side made one: the ring of points round them, the points where it
/// only runs straight on left out. Where they do not make one ring -- a
/// hole, two islands -- each stays as it is.
std::vector<std::vector<uint32_t>> planeOutlines(const Geometry& geo, const std::vector<Vec3>& P,
                                                 const std::vector<uint32_t>& prims, float eps) {
    struct Plane {
        Vec3 n;
        double d = 0.0;
        std::vector<uint32_t> prims;
    };
    std::vector<Plane> planes;
    for (const uint32_t p : prims) {
        const auto c = geo.primitivePoints(p);
        if (c.size() < 3 || !geo.primitiveClosed(p)) continue;
        Vec3 n, mid;
        for (size_t i = 0; i < c.size(); ++i) {
            const Vec3& a = P[c[i]];
            const Vec3& b = P[c[(i + 1) % c.size()]];
            n += Vec3((a.y - b.y) * (a.z + b.z), (a.z - b.z) * (a.x + b.x), (a.x - b.x) * (a.y + b.y));
            mid += a;
        }
        if (length(n) * 0.5f < 1e-12f) continue;
        n = normalize(n);
        const double d = static_cast<double>(dot(n, mid * (1.0f / static_cast<float>(c.size()))));
        Plane* found = nullptr;
        for (Plane& q : planes) {
            if (dot(q.n, n) > 0.99995f && std::fabs(q.d - d) <= static_cast<double>(eps)) {
                found = &q;
                break;
            }
        }
        if (!found) {
            planes.push_back({n, d, {}});
            found = &planes.back();
        }
        found->prims.push_back(p);
    }
    std::vector<std::vector<uint32_t>> out;
    auto alone = [&](const Plane& q) {
        for (const uint32_t p : q.prims) {
            const auto c = geo.primitivePoints(p);
            out.emplace_back(c.begin(), c.end());
        }
    };
    for (const Plane& q : planes) {
        if (q.prims.size() == 1) {
            alone(q);
            continue;
        }
        // The edges no two of them share run round them.
        std::map<std::pair<uint32_t, uint32_t>, int> edges;
        for (const uint32_t p : q.prims) {
            const auto c = geo.primitivePoints(p);
            for (size_t i = 0; i < c.size(); ++i) ++edges[{c[i], c[(i + 1) % c.size()]}];
        }
        std::map<uint32_t, uint32_t> next;
        bool simple = true;
        size_t rim = 0;
        for (const auto& [e, count] : edges) {
            if (edges.count({e.second, e.first})) continue;
            if (count != 1 || !next.emplace(e.first, e.second).second) simple = false;
            ++rim;
        }
        std::vector<uint32_t> ring;
        if (simple && rim >= 3) {
            const uint32_t start = next.begin()->first;
            uint32_t at = start;
            do {
                ring.push_back(at);
                const auto it = next.find(at);
                if (it == next.end()) break;
                at = it->second;
            } while (at != start && ring.size() <= rim);
            simple = at == start && ring.size() == rim;
        }
        if (!simple) {
            alone(q);
            continue;
        }
        // Points where the ring runs straight on are no corners.
        std::vector<uint32_t> corners;
        for (size_t i = 0; i < ring.size(); ++i) {
            const Vec3& a = P[ring[(i + ring.size() - 1) % ring.size()]];
            const Vec3& b = P[ring[i]];
            const Vec3& c = P[ring[(i + 1) % ring.size()]];
            const Vec3 u = b - a, v = c - b;
            if (length(cross(u, v)) > 1e-5f * length(u) * length(v)) corners.push_back(ring[i]);
        }
        if (corners.size() < 3) {
            alone(q);
            continue;
        }
        out.push_back(std::move(corners));
    }
    return out;
}

}  // namespace

std::shared_ptr<const RigidLayout> rigidLayout(const Geometry& geo, const std::string& attribute) {
    auto out = std::make_shared<RigidLayout>();
    RigidLayout& L = *out;
    const size_t nprims = geo.primitiveCount();
    L.bodyOf.assign(nprims, -1);
    if (nprims == 0) return out;
    int pieceCount = 0;
    const std::vector<int32_t> piece = pieceOfPrimitives(geo, attribute, pieceCount);
    const std::vector<Vec3> P = rigidPositions(geo);

    // The parts: the primitives of a piece that share points.
    UnionFind prims(nprims);
    {
        std::vector<std::tuple<uint32_t, int32_t, uint32_t>> uses;  // point, piece, primitive
        uses.reserve(geo.vertexCount());
        for (size_t p = 0; p < nprims; ++p) {
            for (const uint32_t pt : geo.primitivePoints(p)) uses.emplace_back(pt, piece[p], static_cast<uint32_t>(p));
        }
        std::sort(uses.begin(), uses.end());
        for (size_t i = 1; i < uses.size(); ++i) {
            if (std::get<0>(uses[i]) == std::get<0>(uses[i - 1]) && std::get<1>(uses[i]) == std::get<1>(uses[i - 1])) {
                prims.unite(std::get<2>(uses[i]), std::get<2>(uses[i - 1]));
            }
        }
    }
    std::vector<int32_t> partOfPrim(nprims, -1);
    struct Part {
        std::vector<uint32_t> prims, points;
        int32_t piece = 0;
        Vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f}, center;
    };
    std::vector<Part> parts;
    for (size_t p = 0; p < nprims; ++p) {
        const uint32_t root = prims.find(static_cast<uint32_t>(p));
        if (partOfPrim[root] < 0) {
            partOfPrim[root] = static_cast<int32_t>(parts.size());
            parts.emplace_back();
            parts.back().piece = piece[p];
        }
        partOfPrim[p] = partOfPrim[root];
        parts[static_cast<size_t>(partOfPrim[p])].prims.push_back(static_cast<uint32_t>(p));
    }
    Vec3 lo(1e30f, 1e30f, 1e30f), hi(-1e30f, -1e30f, -1e30f);
    for (Part& part : parts) {
        for (const uint32_t p : part.prims) {
            for (const uint32_t pt : geo.primitivePoints(p)) part.points.push_back(pt);
        }
        std::sort(part.points.begin(), part.points.end());
        part.points.erase(std::unique(part.points.begin(), part.points.end()), part.points.end());
        Vec3 sum;
        for (const uint32_t pt : part.points) {
            sum += P[pt];
            for (int a = 0; a < 3; ++a) {
                part.lo[a] = std::min(part.lo[a], P[pt][a]);
                part.hi[a] = std::max(part.hi[a], P[pt][a]);
            }
        }
        part.center = part.points.empty() ? Vec3() : sum * (1.0f / static_cast<float>(part.points.size()));
        for (int a = 0; a < 3; ++a) {
            lo[a] = std::min(lo[a], part.lo[a]);
            hi[a] = std::max(hi[a], part.hi[a]);
        }
    }
    const float diag = std::max(length(hi - lo), 1e-3f);
    const float eps = 1e-4f * diag;
    // Glue needs a face, not a sliver of one: a square a ten thousandth of
    // the whole across.
    const double minArea = static_cast<double>(1e-4f * diag) * static_cast<double>(1e-4f * diag);

    // The faces, part by part: those that lie side by side in one plane --
    // a cut face of rough concrete, as its proxy has it, is hundreds of
    // triangles -- as the one polygon round them.
    std::vector<Face> faces;
    std::vector<uint32_t> firstFace(parts.size() + 1, 0);
    for (size_t k = 0; k < parts.size(); ++k) {
        firstFace[k] = static_cast<uint32_t>(faces.size());
        for (const std::vector<uint32_t>& ring : planeOutlines(geo, P, parts[k].prims, eps)) {
            Face f;
            f.part = static_cast<uint32_t>(k);
            Vec3 n, mid;
            f.lo = Vec3(1e30f, 1e30f, 1e30f);
            f.hi = Vec3(-1e30f, -1e30f, -1e30f);
            for (size_t i = 0; i < ring.size(); ++i) {
                const Vec3& a = P[ring[i]];
                const Vec3& b = P[ring[(i + 1) % ring.size()]];
                n += Vec3((a.y - b.y) * (a.z + b.z), (a.z - b.z) * (a.x + b.x), (a.x - b.x) * (a.y + b.y));  // Newell
                mid += a;
                f.corners.push_back(a);
                for (int ax = 0; ax < 3; ++ax) {
                    f.lo[ax] = std::min(f.lo[ax], a[ax]);
                    f.hi[ax] = std::max(f.hi[ax], a[ax]);
                }
            }
            if (length(n) * 0.5f < 1e-12f) continue;
            f.normal = normalize(n);
            f.d = static_cast<double>(dot(f.normal, mid * (1.0f / static_cast<float>(ring.size()))));
            faces.push_back(std::move(f));
        }
    }
    firstFace[parts.size()] = static_cast<uint32_t>(faces.size());

    // Parts whose boxes meet, found by sweeping along x; their faces that
    // lie in one plane facing each other, and overlap.
    struct Touch {
        double area = 0.0;
        Vec3 sum;    ///< the middles, weighted by area
        Vec3 normal; ///< from the first part to the second, weighted by area
    };
    std::map<std::pair<uint32_t, uint32_t>, Touch> touches;
    std::vector<uint32_t> order(parts.size());
    std::iota(order.begin(), order.end(), 0u);
    std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
        return parts[a].lo.x < parts[b].lo.x || (parts[a].lo.x == parts[b].lo.x && a < b);
    });
    auto boxesMeet = [&](const Vec3& alo, const Vec3& ahi, const Vec3& blo, const Vec3& bhi) {
        for (int a = 0; a < 3; ++a) {
            if (alo[a] > bhi[a] + eps || blo[a] > ahi[a] + eps) return false;
        }
        return true;
    };
    // Two parts' faces that may meet: those near where the parts' boxes
    // overlap, swept along its longest side. A rough face whose triangles
    // make no one polygon is thousands of faces; every one against every
    // one of the other part's took minutes.
    struct Near {
        float lo, hi;
        uint32_t face;
        bool second;
    };
    std::vector<Near> sweep;
    struct Hit {
        uint32_t fa, fb;
        double area;
        Vec3 sum;
    };
    std::vector<Hit> hits;
    for (size_t oi = 0; oi < order.size(); ++oi) {
        const uint32_t i = order[oi];
        for (size_t oj = oi + 1; oj < order.size(); ++oj) {
            const uint32_t j = order[oj];
            if (parts[j].lo.x > parts[i].hi.x + eps) break;
            if (!boxesMeet(parts[i].lo, parts[i].hi, parts[j].lo, parts[j].hi)) continue;
            const uint32_t a = std::min(i, j), b = std::max(i, j);
            Vec3 olo, ohi;
            for (int ax = 0; ax < 3; ++ax) {
                olo[ax] = std::max(parts[a].lo[ax], parts[b].lo[ax]) - 2.0f * eps;
                ohi[ax] = std::min(parts[a].hi[ax], parts[b].hi[ax]) + 2.0f * eps;
            }
            int axis = 0;
            for (int ax = 1; ax < 3; ++ax) {
                if (ohi[ax] - olo[ax] > ohi[axis] - olo[axis]) axis = ax;
            }
            sweep.clear();
            for (const uint32_t part : {a, b}) {
                for (uint32_t k = firstFace[part]; k < firstFace[part + 1]; ++k) {
                    const Face& f = faces[k];
                    if (boxesMeet(f.lo, f.hi, olo, ohi)) sweep.push_back({f.lo[axis], f.hi[axis], k, part == b});
                }
            }
            std::sort(sweep.begin(), sweep.end(), [](const Near& x, const Near& y) {
                return x.lo < y.lo || (x.lo == y.lo && x.face < y.face);
            });
            hits.clear();
            for (size_t x = 0; x < sweep.size(); ++x) {
                for (size_t y = x + 1; y < sweep.size() && sweep[y].lo <= sweep[x].hi + eps; ++y) {
                    if (sweep[x].second == sweep[y].second) continue;
                    const uint32_t fa = sweep[x].second ? sweep[y].face : sweep[x].face;
                    const uint32_t fb = sweep[x].second ? sweep[x].face : sweep[y].face;
                    const Face& f = faces[fa];
                    const Face& g = faces[fb];
                    if (dot(f.normal, g.normal) > -0.9995f) continue;
                    if (std::fabs(f.d + g.d) > static_cast<double>(eps)) continue;
                    if (!boxesMeet(f.lo, f.hi, g.lo, g.hi)) continue;
                    Vec3 sum;
                    const double area = overlapArea(f, g, sum);
                    if (area <= 0.0) continue;
                    hits.push_back({fa, fb, area, sum});
                }
            }
            // Summed in the order of the faces, as face against face would.
            std::sort(hits.begin(), hits.end(), [](const Hit& x, const Hit& y) {
                return x.fa < y.fa || (x.fa == y.fa && x.fb < y.fb);
            });
            Touch t;
            for (const Hit& h : hits) {
                t.area += h.area;
                t.sum += h.sum;
                t.normal += faces[h.fa].normal * static_cast<float>(h.area);
            }
            if (t.area > 0.0) touches[{a, b}] = t;
        }
    }

    // The bodies: the parts of a piece that touch are one.
    UnionFind bodies(parts.size());
    for (const auto& [pair, t] : touches) {
        if (parts[pair.first].piece == parts[pair.second].piece) bodies.unite(pair.first, pair.second);
    }
    std::vector<int32_t> bodyOfPart(parts.size(), -1), bodyOfRoot(parts.size(), -1);
    for (size_t p = 0; p < nprims; ++p) {
        const uint32_t part = static_cast<uint32_t>(partOfPrim[p]);
        const uint32_t root = bodies.find(part);
        if (bodyOfRoot[root] < 0) bodyOfRoot[root] = L.bodies++;
        bodyOfPart[part] = bodyOfRoot[root];
        L.bodyOf[p] = bodyOfPart[part];
    }
    L.parts.resize(static_cast<size_t>(L.bodies));
    L.prims.resize(static_cast<size_t>(L.bodies));
    for (size_t k = 0; k < parts.size(); ++k) {
        L.parts[static_cast<size_t>(bodyOfPart[k])].push_back(parts[k].points);
    }
    for (size_t p = 0; p < nprims; ++p) L.prims[static_cast<size_t>(L.bodyOf[p])].push_back(static_cast<uint32_t>(p));

    // Where bodies touch: the glue.
    std::map<std::pair<int, int>, Touch> joined;
    for (const auto& [pair, t] : touches) {
        int a = bodyOfPart[pair.first], b = bodyOfPart[pair.second];
        if (a == b || t.area <= minArea) continue;
        // The normal from part to part: the way from one's middle to the other's.
        Vec3 n = normalize(t.normal);
        if (dot(n, parts[pair.second].center - parts[pair.first].center) < 0.0f) n = -n;
        if (a > b) {
            std::swap(a, b);
            n = -n;
        }
        Touch& j = joined[{a, b}];
        j.area += t.area;
        j.sum += t.sum;
        j.normal += n * static_cast<float>(t.area);
    }
    for (const auto& [pair, t] : joined) {
        RigidLayout::Contact c;
        c.a = pair.first;
        c.b = pair.second;
        c.area = static_cast<float>(t.area);
        c.at = t.sum * static_cast<float>(1.0 / t.area);
        c.normal = normalize(t.normal);
        L.contacts.push_back(c);
    }
    return out;
}

// --- The glue: joints between the bodies, and the network of them ----------------------

namespace {

/// A number of attribute `a` of element `i`, Int or Float; `fallback` where
/// it has none.
float numberAt(const AttributeArray* a, size_t i, float fallback) {
    if (!a || i >= a->size()) return fallback;
    if (a->type() == AttrType::Float) return a->read<float>()[i];
    if (a->type() == AttrType::Int) return static_cast<float>(a->read<int32_t>()[i]);
    return fallback;
}

/// How a joint of `strength` (a share of the Glue) is drawn: green as the
/// Glue holds, yellow weaker -- a sixteenth or less all yellow -- blue
/// stronger, grey where it holds nothing.
Vec3 jointColor(float strength) {
    if (!(strength > 0.0f)) return Vec3(0.45f, 0.45f, 0.45f);
    const float t = std::clamp(std::log2(strength) / 4.0f, -1.0f, 1.0f);
    const Vec3 held(0.3f, 0.85f, 0.35f), weak(1.0f, 0.85f, 0.15f), strong(0.25f, 0.55f, 1.0f);
    return t < 0.0f ? held + (weak - held) * (-t) : held + (strong - held) * t;
}

constexpr Vec3 kBrokenJoint(1.0f, 0.15f, 0.1f);

}  // namespace

std::shared_ptr<const RigidGlue> rigidGlue(const Geometry& pieces, const RigidLayout& L, const std::string& attribute,
                                           const Geometry* network) {
    auto out = std::make_shared<RigidGlue>();
    RigidGlue& g = *out;
    const size_t bodies = static_cast<size_t>(std::max(L.bodies, 0));
    if (bodies == 0 || L.prims.size() < bodies || L.parts.size() < bodies) return out;

    // Each body: the middle of its box; the piece it is -- the value of the
    // attribute, Int, on the primitives or else the points, as
    // pieceOfPrimitives has it, else the piece's number -- and which of
    // that piece's bodies.
    const std::vector<Vec3> P = rigidPositions(pieces);
    int count = 0;
    const std::vector<int32_t> number = pieceOfPrimitives(pieces, attribute, count);
    const AttributeArray* onPrims = pieces.primitives().find(attribute);
    const AttributeArray* onPoints = pieces.points().find(attribute);
    if (onPrims && onPrims->type() != AttrType::Int) onPrims = nullptr;
    if (onPoints && onPoints->type() != AttrType::Int) onPoints = nullptr;
    g.centres.resize(bodies);
    g.piece.resize(bodies);
    g.part.resize(bodies);
    std::map<int32_t, int32_t> parts;
    for (size_t k = 0; k < bodies; ++k) {
        Vec3 lo(1e30f, 1e30f, 1e30f), hi(-1e30f, -1e30f, -1e30f);
        bool any = false;
        for (const std::vector<uint32_t>& part : L.parts[k]) {
            for (const uint32_t i : part) {
                for (int a = 0; a < 3; ++a) {
                    lo[a] = std::min(lo[a], P[i][a]);
                    hi[a] = std::max(hi[a], P[i][a]);
                }
                any = true;
            }
        }
        g.centres[k] = any ? (lo + hi) * 0.5f : Vec3();
        const uint32_t prim = L.prims[k].front();
        int32_t value = number[prim];
        if (onPrims) {
            value = onPrims->read<int32_t>()[prim];
        } else if (onPoints) {
            const auto c = pieces.primitivePoints(prim);
            if (!c.empty()) value = onPoints->read<int32_t>()[c[0]];
        }
        g.piece[k] = value;
        g.part[k] = parts[value]++;
    }

    // Without a network: where bodies touch, as strong as their attributes say.
    if (!network) {
        std::vector<float> glue(bodies), clusterGlue(bodies);
        std::vector<int> cluster(bodies);
        for (size_t k = 0; k < bodies; ++k) {
            const int body = static_cast<int>(k);
            glue[k] = std::max(numberOf(pieces, L, body, "glue", 1.0f), 0.0f);
            cluster[k] = static_cast<int>(std::lround(numberOf(pieces, L, body, "cluster", 0.0f)));
            clusterGlue[k] = std::max(numberOf(pieces, L, body, "clusterglue", 1.0f), 0.0f);
        }
        g.joints.reserve(L.contacts.size());
        for (const RigidLayout::Contact& c : L.contacts) {
            const size_t a = static_cast<size_t>(c.a), b = static_cast<size_t>(c.b);
            RigidJoint j;
            j.a = c.a;
            j.b = c.b;
            j.at = c.at;
            j.normal = c.normal;
            j.area = c.area;
            j.strength = std::min(glue[a], glue[b]);
            // Inside one chunk (RBD Cluster): as many times as strong as the
            // weaker of the two says.
            if (cluster[a] > 0 && cluster[a] == cluster[b]) j.strength *= std::min(clusterGlue[a], clusterGlue[b]);
            g.joints.push_back(j);
        }
        return out;
    }

    // With one: its lines, from the body of the first point to that of the
    // last -- the one whose piece the point names, else the nearest.
    std::map<std::pair<int, int>, const RigidLayout::Contact*> touching;
    for (const RigidLayout::Contact& c : L.contacts) touching[{c.a, c.b}] = &c;
    const size_t np = network->pointCount();
    std::vector<int32_t> bodyOfPoint(np, -1);
    const AttributeArray* names = network->points().find(attribute);
    if (names && names->type() != AttrType::Int && names->type() != AttrType::Float) names = nullptr;
    if (names) {
        std::map<std::pair<int32_t, int32_t>, int32_t> bodyOf;
        for (size_t k = 0; k < bodies; ++k) bodyOf.emplace(std::make_pair(g.piece[k], g.part[k]), static_cast<int32_t>(k));
        const AttributeArray* part = network->points().find("part");
        for (size_t i = 0; i < np; ++i) {
            const auto key = std::make_pair(static_cast<int32_t>(std::lround(numberAt(names, i, -1.0f))),
                                            static_cast<int32_t>(std::lround(numberAt(part, i, 0.0f))));
            if (const auto it = bodyOf.find(key); it != bodyOf.end()) bodyOfPoint[i] = it->second;
        }
    } else {
        const PointTree tree(g.centres);
        const auto NP = network->positions();
        for (size_t i = 0; i < np; ++i) bodyOfPoint[i] = tree.nearest(NP[i]);
    }
    const AttributeArray* strength = network->primitives().find("strength");
    const AttributeArray* area = network->primitives().find("area");
    for (size_t prim = 0; prim < network->primitiveCount(); ++prim) {
        const auto c = network->primitivePoints(prim);
        int a = c.size() < 2 ? -1 : bodyOfPoint[c.front()], b = c.size() < 2 ? -1 : bodyOfPoint[c.back()];
        if (a < 0 || b < 0 || a == b) {
            ++g.skipped;
            continue;
        }
        if (a > b) std::swap(a, b);
        const auto touch = touching.find({a, b});
        RigidJoint j;
        j.a = a;
        j.b = b;
        const Vec3 ca = g.centres[static_cast<size_t>(a)], cb = g.centres[static_cast<size_t>(b)];
        j.at = touch != touching.end() ? touch->second->at : (ca + cb) * 0.5f;
        j.normal = touch != touching.end() ? touch->second->normal
                   : length(cb - ca) > 1e-9f ? normalize(cb - ca) : Vec3(0.0f, 1.0f, 0.0f);
        j.area = numberAt(area, prim, 0.0f);
        // None given -- or a merge's 0 -- the faces they share; a hand's
        // breadth square where they share none.
        if (!(j.area > 0.0f)) j.area = touch != touching.end() ? touch->second->area : 0.01f;
        j.strength = numberAt(strength, prim, 1.0f);
        g.joints.push_back(j);
    }
    return out;
}

std::shared_ptr<Geometry> rigidNetwork(const RigidGlue& g, const std::string& attribute) {
    auto geo = std::make_shared<Geometry>();
    const size_t bodies = g.centres.size();
    geo->addPoints(bodies);
    auto P = geo->positionsForWrite();
    std::copy(g.centres.begin(), g.centres.end(), P.begin());
    if (!attribute.empty() && g.piece.size() == bodies) {
        auto piece = geo->points().create(attribute, AttrType::Int).write<int32_t>();
        std::copy(g.piece.begin(), g.piece.end(), piece.begin());
    }
    if (g.part.size() == bodies && std::any_of(g.part.begin(), g.part.end(), [](int32_t p) { return p != 0; })) {
        auto part = geo->points().create("part", AttrType::Int).write<int32_t>();
        std::copy(g.part.begin(), g.part.end(), part.begin());
    }
    for (const RigidJoint& j : g.joints) {
        const uint32_t ends[2] = {static_cast<uint32_t>(j.a), static_cast<uint32_t>(j.b)};
        geo->addPrimitive(ends, false);
    }
    auto strength = geo->primitives().create("strength", AttrType::Float).write<float>();
    auto area = geo->primitives().create("area", AttrType::Float).write<float>();
    auto cd = geo->primitives().create("Cd", AttrType::Vec3).write<Vec3>();
    for (size_t i = 0; i < g.joints.size(); ++i) {
        strength[i] = g.joints[i].strength;
        area[i] = g.joints[i].area;
        cd[i] = jointColor(g.joints[i].strength);
    }
    return geo;
}

std::shared_ptr<Geometry> rigidNetwork(const RigidFrame& f) {
    if (!f.glue) return std::make_shared<Geometry>();
    const RigidGlue& g = *f.glue;
    std::shared_ptr<Geometry> geo = rigidNetwork(g, f.attribute);
    // The bodies where they are, moving as they do.
    const size_t bodies = g.centres.size();
    const bool posed = f.poses.size() == bodies;
    {
        auto P = geo->positionsForWrite();
        auto v = geo->points().create("v", AttrType::Vec3).write<Vec3>();
        for (size_t k = 0; k < bodies && posed; ++k) {
            P[k] = f.poses[k].apply(g.centres[k]);
            v[k] = f.poses[k].velocityAt(g.centres[k]);
        }
    }
    // The joints that held: what became of each, and where it was.
    const bool known = f.jointState.size() == g.joints.size() && f.jointTime.size() == g.joints.size();
    std::vector<uint8_t> keep(g.joints.size(), 1);
    {
        auto broken = geo->primitives().create("broken", AttrType::Int).write<int32_t>();
        auto time = geo->primitives().create("time", AttrType::Float).write<float>();
        auto at = geo->primitives().create("at", AttrType::Vec3).write<Vec3>();
        auto cd = geo->primitives().create("Cd", AttrType::Vec3).write<Vec3>();
        for (size_t i = 0; i < g.joints.size(); ++i) {
            const RigidJoint& j = g.joints[i];
            const uint8_t state = known ? f.jointState[i] : RigidFrame::kJointHolds;
            keep[i] = state != RigidFrame::kJointNone;
            broken[i] = state == RigidFrame::kJointBroken ? 1 : 0;
            time[i] = broken[i] ? f.jointTime[i] : -1.0f;
            at[i] = posed ? f.poses[static_cast<size_t>(j.a)].apply(j.at) : j.at;
            if (broken[i]) cd[i] = kBrokenJoint;
        }
    }
    if (std::find(keep.begin(), keep.end(), 0) != keep.end()) geo->deletePrimitives(keep, false);
    return geo;
}

// --- The bars: where each runs through which body ---------------------------------------

Vec3 RigidRebar::at(const Bar& bar, float s) {
    const size_t n = bar.points.size();
    if (n == 0) return {};
    if (n == 1) return bar.points[0];
    const auto it = std::upper_bound(bar.along.begin(), bar.along.end(), s);
    size_t i = it == bar.along.begin() ? 0 : static_cast<size_t>(it - bar.along.begin()) - 1;
    i = std::min(i, n - 2);
    const float len = bar.along[i + 1] - bar.along[i];
    const float t = len > 0.0f ? std::clamp((s - bar.along[i]) / len, 0.0f, 1.0f) : 0.0f;
    return bar.points[i] + (bar.points[i + 1] - bar.points[i]) * t;
}

Vec3 RigidRebar::tangent(const Bar& bar, float s) {
    const size_t n = bar.points.size();
    if (n < 2) return Vec3(1.0f, 0.0f, 0.0f);
    const auto it = std::upper_bound(bar.along.begin(), bar.along.end(), s);
    size_t i = it == bar.along.begin() ? 0 : static_cast<size_t>(it - bar.along.begin()) - 1;
    i = std::min(i, n - 2);
    return normalize(bar.points[i + 1] - bar.points[i]);
}

namespace {

/// A convex part of a body as the planes round it: inside is behind them all.
struct Hull {
    int32_t body = 0;
    std::vector<Vec3> n;
    std::vector<float> d;
    Vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
    Vec3 middle;
};

}  // namespace

std::shared_ptr<const RigidRebar> rigidRebar(const Geometry& pieces, const RigidLayout& L, const Geometry& bars) {
    auto out = std::make_shared<RigidRebar>();
    RigidRebar& R = *out;
    // The bars: each polyline -- a closed one round to its first point again.
    const auto B = bars.positions();
    const AttributeArray* primWidth = bars.primitives().find("width");
    const AttributeArray* pointWidth = bars.points().find("width");
    auto read = [](const AttributeArray* a, size_t i, float& o) {
        if (!a || i >= a->size()) return false;
        if (a->type() == AttrType::Float) o = a->read<float>()[i];
        else if (a->type() == AttrType::Int) o = static_cast<float>(a->read<int32_t>()[i]);
        else return false;
        return true;
    };
    for (size_t prim = 0; prim < bars.primitiveCount(); ++prim) {
        const auto c = bars.primitivePoints(prim);
        if (c.size() < 2) continue;
        RigidRebar::Bar bar;
        bool finite = true;
        for (const uint32_t q : c) {
            const Vec3 p = B[q];
            finite = finite && std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
            if (!bar.points.empty() && length(p - bar.points.back()) < 1e-6f) continue;
            bar.points.push_back(p);
        }
        if (!finite) continue;
        if (bars.primitiveClosed(prim) && bar.points.size() >= 3 && length(bar.points.front() - bar.points.back()) >= 1e-6f) {
            bar.points.push_back(bar.points.front());
        }
        if (bar.points.size() < 2) continue;
        bar.along.push_back(0.0f);
        for (size_t i = 1; i < bar.points.size(); ++i) bar.along.push_back(bar.along.back() + length(bar.points[i] - bar.points[i - 1]));
        float w = 0.012f;
        if (!read(primWidth, prim, w)) read(pointWidth, c[0], w);
        bar.width = std::isfinite(w) ? std::clamp(w, 1e-4f, 1.0f) : 0.012f;
        R.bars.push_back(std::move(bar));
    }
    if (R.bars.empty() || L.bodies == 0) return out;

    // The parts, as the solver collides them -- convex -- as planes: those of
    // their faces, as the proxy has them, facing away from their middles.
    const std::vector<Vec3> P = rigidPositions(pieces);
    std::vector<int32_t> partOf(pieces.pointCount(), -1);
    std::vector<Hull> hulls;
    Vec3 lo(1e30f, 1e30f, 1e30f), hi(-1e30f, -1e30f, -1e30f);
    for (int k = 0; k < L.bodies; ++k) {
        for (const std::vector<uint32_t>& part : L.parts[static_cast<size_t>(k)]) {
            if (part.empty()) continue;
            Hull h;
            h.body = k;
            for (const uint32_t pt : part) {
                partOf[pt] = static_cast<int32_t>(hulls.size());
                h.middle += P[pt];
                for (int a = 0; a < 3; ++a) {
                    h.lo[a] = std::min(h.lo[a], P[pt][a]);
                    h.hi[a] = std::max(h.hi[a], P[pt][a]);
                }
            }
            h.middle = h.middle * (1.0f / static_cast<float>(part.size()));
            for (int a = 0; a < 3; ++a) {
                lo[a] = std::min(lo[a], h.lo[a]);
                hi[a] = std::max(hi[a], h.hi[a]);
            }
            hulls.push_back(std::move(h));
        }
    }
    const float diag = std::max(length(hi - lo), 1e-3f);
    const float eps = 1e-5f * diag;
    for (size_t prim = 0; prim < pieces.primitiveCount(); ++prim) {
        const auto c = pieces.primitivePoints(prim);
        if (c.size() < 3 || !pieces.primitiveClosed(prim) || partOf[c[0]] < 0) continue;
        Hull& h = hulls[static_cast<size_t>(partOf[c[0]])];
        Vec3 n, mid;
        for (size_t i = 0; i < c.size(); ++i) {
            const Vec3& a = P[c[i]];
            const Vec3& b = P[c[(i + 1) % c.size()]];
            n += Vec3((a.y - b.y) * (a.z + b.z), (a.z - b.z) * (a.x + b.x), (a.x - b.x) * (a.y + b.y));
            mid += a;
        }
        // Faces of next to no area -- a rough face's triangles squashed on the
        // proxy -- say nothing of which way they face.
        if (0.5f * length(n) < 1e-8f * diag * diag) continue;
        n = normalize(n);
        float d = dot(n, mid * (1.0f / static_cast<float>(c.size())));
        if (dot(n, h.middle) - d > 0.0f) {
            n = -n;
            d = -d;
        }
        bool known = false;
        for (size_t j = 0; j < h.n.size() && !known; ++j) known = dot(n, h.n[j]) > 0.99999f && std::fabs(d - h.d[j]) < eps;
        if (known) continue;
        h.n.push_back(n);
        h.d.push_back(d);
    }

    // Each bar through them: the stretch of each segment inside each part
    // (clipped by its planes), in order along the bar, those of one body
    // one after the other made one.
    struct Span {
        float in, out;
        int32_t body;
    };
    for (RigidRebar::Bar& bar : R.bars) {
        std::vector<Span> spans;
        for (size_t i = 0; i + 1 < bar.points.size(); ++i) {
            const Vec3 p0 = bar.points[i], p1 = bar.points[i + 1];
            const float len = bar.along[i + 1] - bar.along[i];
            Vec3 slo, shi;
            for (int a = 0; a < 3; ++a) {
                slo[a] = std::min(p0[a], p1[a]) - eps;
                shi[a] = std::max(p0[a], p1[a]) + eps;
            }
            for (const Hull& h : hulls) {
                if (h.n.size() < 4) continue;  // not closed round
                bool apart = false;
                for (int a = 0; a < 3 && !apart; ++a) apart = h.lo[a] > shi[a] || h.hi[a] < slo[a];
                if (apart) continue;
                float t0 = 0.0f, t1 = 1.0f;
                for (size_t j = 0; j < h.n.size() && !apart; ++j) {
                    const float a = dot(h.n[j], p0) - h.d[j], b = dot(h.n[j], p1) - h.d[j];
                    if (a > eps && b > eps) apart = true;
                    else if (a > eps) t0 = std::max(t0, a / (a - b));
                    else if (b > eps) t1 = std::min(t1, a / (a - b));
                    apart = apart || t0 >= t1;
                }
                if (apart || (t1 - t0) * len <= 1e-6f) continue;
                spans.push_back({bar.along[i] + t0 * len, bar.along[i] + t1 * len, h.body});
            }
        }
        std::sort(spans.begin(), spans.end(), [](const Span& a, const Span& b) {
            return a.in < b.in || (a.in == b.in && (a.body < b.body || (a.body == b.body && a.out < b.out)));
        });
        std::vector<RigidRebar::Station> list;
        for (Span sp : spans) {
            if (!list.empty()) {
                RigidRebar::Station& last = list.back();
                if (sp.body == last.body && sp.in <= last.out + eps) {
                    last.out = std::max(last.out, sp.out);
                    continue;
                }
                // Overlapping another body -- a bar along a face both have:
                // from where that one ends.
                sp.in = std::max(sp.in, last.out);
                if (sp.out <= sp.in) continue;
            }
            list.push_back({sp.body, sp.in, sp.out});
        }
        bar.first = static_cast<uint32_t>(R.stations.size());
        for (const RigidRebar::Station& st : list) {
            if (st.out - st.in < 0.5f * bar.width) continue;  // a corner grazed
            if (!R.stations.empty() && R.stations.size() > bar.first && R.stations.back().body == st.body) {
                R.stations.back().out = st.out;
                continue;
            }
            R.stations.push_back(st);
        }
        bar.count = static_cast<uint32_t>(R.stations.size()) - bar.first;
    }
    return out;
}

// --- Posed, and drawn ------------------------------------------------------------------

namespace {

std::shared_ptr<const RigidLayout> layoutOf(const RigidFrame& f, const Geometry& geo) {
    if (f.layout && f.layout->bodyOf.size() == geo.primitiveCount()) return f.layout;
    return rigidLayout(geo, f.attribute);
}

/// wholePanes() with the bodies of `rest`, the frame's pieces, in `layout`.
std::vector<uint8_t> wholePanes(const RigidFrame& f, const Geometry& rest, const RigidLayout& layout) {
    const AttributeArray* attr = rest.primitives().find("glass");
    if (!attr || f.poses.empty()) return {};
    const size_t bodies = static_cast<size_t>(layout.bodies);
    std::vector<uint8_t> glass(bodies, 0);
    for (size_t p = 0; p < layout.bodyOf.size(); ++p) {
        const int32_t b = layout.bodyOf[p];
        if (b >= 0 && static_cast<size_t>(b) < bodies && glassOf(attr, p) >= 0.5f) glass[static_cast<size_t>(b)] = 1;
    }
    // The panes: the glass bodies that touch, one to the next.
    std::vector<size_t> root(bodies);
    std::iota(root.begin(), root.end(), size_t{0});
    auto find = [&](size_t b) {
        while (root[b] != b) b = root[b] = root[root[b]];
        return b;
    };
    auto both = [&](const RigidLayout::Contact& c) {
        return static_cast<size_t>(c.a) < bodies && static_cast<size_t>(c.b) < bodies && glass[static_cast<size_t>(c.a)] &&
               glass[static_cast<size_t>(c.b)];
    };
    // A pane of one piece has no cracks: the faces a fracture cut of it are
    // its edges.
    std::vector<uint8_t> pane(bodies, 0);
    for (const RigidLayout::Contact& c : layout.contacts) {
        if (!both(c)) continue;
        root[find(static_cast<size_t>(c.a))] = find(static_cast<size_t>(c.b));
        pane[static_cast<size_t>(c.a)] = pane[static_cast<size_t>(c.b)] = 1;
    }
    // Broken where two pieces that touched have come apart, or one is gone.
    std::vector<uint8_t> broken(bodies, 0);
    for (const RigidLayout::Contact& c : layout.contacts) {
        if (!both(c) || static_cast<size_t>(c.a) >= f.poses.size() || static_cast<size_t>(c.b) >= f.poses.size()) continue;
        const Vec3 a = f.poses[static_cast<size_t>(c.a)].apply(c.at), b = f.poses[static_cast<size_t>(c.b)].apply(c.at);
        if (length(a - b) > 1e-4f + 1e-5f * length(c.at)) broken[find(static_cast<size_t>(c.a))] = 1;
    }
    for (const std::vector<uint32_t>* list : {&f.vanished, &f.unglued}) {
        for (const uint32_t b : *list) {
            if (b < bodies && glass[b]) broken[find(b)] = 1;
        }
    }
    std::vector<uint8_t> whole(bodies, 0);
    for (size_t b = 0; b < bodies; ++b) whole[b] = pane[b] && !broken[find(b)];
    return whole;
}

}  // namespace

float glassOf(const AttributeArray* glass, size_t p) {
    if (!glass || p >= glass->size()) return 0.0f;
    if (glass->type() == AttrType::Int) return static_cast<float>(glass->read<int32_t>()[p]);
    if (glass->type() == AttrType::Float) return glass->read<float>()[p];
    return 0.0f;
}

std::vector<uint8_t> wholePanes(const RigidFrame& f) {
    if (!f.pieces || f.poses.empty()) return {};
    return wholePanes(f, *f.pieces, *layoutOf(f, *f.pieces));
}

std::shared_ptr<Geometry> posedPieces(const RigidFrame& f) {
    if (!f.pieces) return std::make_shared<Geometry>();
    auto geo = std::make_shared<Geometry>(*f.pieces);
    const std::shared_ptr<const RigidLayout> layout = layoutOf(f, *geo);
    // Each point with the body of the first primitive it is a corner of.
    std::vector<int32_t> pointBody(geo->pointCount(), -1);
    for (size_t p = 0; p < geo->primitiveCount(); ++p) {
        for (const uint32_t c : geo->primitivePoints(p)) {
            if (pointBody[c] < 0) pointBody[c] = layout->bodyOf[p];
        }
    }
    geo->points().erase("proxy");  // the rest's, not where the pieces are now
    auto v = geo->points().create("v", AttrType::Vec3).write<Vec3>();
    auto P = geo->positionsForWrite();
    AttributeArray* nAttr = geo->points().find("N");
    std::span<Vec3> N;
    if (nAttr && nAttr->type() == AttrType::Vec3) N = nAttr->write<Vec3>();
    for (size_t i = 0; i < P.size(); ++i) {
        const int32_t k = pointBody[i];
        if (k < 0 || static_cast<size_t>(k) >= f.poses.size()) {
            v[i] = Vec3();
            continue;
        }
        const RigidPose& pose = f.poses[static_cast<size_t>(k)];
        v[i] = pose.velocityAt(P[i]);
        P[i] = pose.apply(P[i]);
        if (!N.empty()) N[i] = pose.apply(N[i]) - pose.position;
    }
    // Blown to dust: gone. And glass is whole until it breaks: a pane none
    // of whose pieces has come away from the others has no cracks yet --
    // the faces of them, glass 2, are not there.
    const std::vector<uint8_t> whole = wholePanes(f, *f.pieces, *layout);
    if (!f.vanished.empty() || std::find(whole.begin(), whole.end(), 1) != whole.end()) {
        const AttributeArray* glass = geo->primitives().find("glass");
        std::vector<uint8_t> keep(geo->primitiveCount(), 1);
        for (size_t p = 0; p < keep.size(); ++p) {
            const auto body = static_cast<uint32_t>(layout->bodyOf[p]);
            const bool uncracked = body < whole.size() && whole[body] && glassOf(glass, p) >= 1.5f;
            keep[p] = !uncracked && !std::binary_search(f.vanished.begin(), f.vanished.end(), body);
        }
        geo->deletePrimitives(keep, true);
    }
    return geo;
}

namespace {

/// Element i of a colour attribute as a colour: a grey for a float.
Vec3 colorAt(const AttributeArray& a, size_t i) {
    switch (a.type()) {
        case AttrType::Vec3: return a.read<Vec3>()[i];
        case AttrType::Vec4: {
            const Vec4 c = a.read<Vec4>()[i];
            return Vec3(c.x, c.y, c.z);
        }
        case AttrType::Float: return Vec3(a.read<float>()[i]);
        default: return Vec3(1.0f);
    }
}

bool isColor(const AttributeArray* a) {
    return a && (a->type() == AttrType::Vec3 || a->type() == AttrType::Vec4 || a->type() == AttrType::Float) && a->size() > 0;
}

/// `geo` with the points the primitives of `cut` share with the others made
/// two: one for each side, the same in all else.
std::shared_ptr<Geometry> apart(const Geometry& geo, const Group& cut) {
    const size_t np = geo.pointCount(), nprims = geo.primitiveCount();
    std::vector<uint8_t> use(np, 0);
    for (size_t p = 0; p < nprims; ++p) {
        const uint8_t bit = cut.contains(p) ? 1 : 2;
        for (const uint32_t pt : geo.primitivePoints(p)) use[pt] |= bit;
    }
    Blends points;
    for (uint32_t i = 0; i < np; ++i) points.one(i);
    std::vector<uint32_t> twin(np, 0);
    for (uint32_t i = 0; i < np; ++i) {
        if (use[i] != 3) continue;
        twin[i] = static_cast<uint32_t>(points.size());
        points.one(i);
    }
    if (points.size() == np) return std::make_shared<Geometry>(geo);
    std::vector<std::vector<uint32_t>> faces(nprims);
    std::vector<uint8_t> closed(nprims);
    std::vector<uint32_t> source(nprims);
    Blends vertices;
    for (size_t p = 0; p < nprims; ++p) {
        const auto c = geo.primitivePoints(p);
        const bool isCut = cut.contains(p);
        faces[p].reserve(c.size());
        for (const uint32_t pt : c) faces[p].push_back(isCut && use[pt] == 3 ? twin[pt] : pt);
        closed[p] = geo.primitiveClosed(p) ? 1 : 0;
        source[p] = static_cast<uint32_t>(p);
        for (size_t k = 0; k < c.size(); ++k) vertices.one(static_cast<uint32_t>(geo.primitiveVertexStart(p) + k));
    }
    return rebuild(geo, points, faces, closed, vertices, source);
}

}  // namespace

std::shared_ptr<Geometry> rebarBars(const RigidFrame& f) {
    auto out = std::make_shared<Geometry>();
    if (!f.rebar) return out;
    const RigidRebar& R = *f.rebar;
    std::vector<Vec3> points, velocities;
    std::vector<float> widths;
    std::vector<std::vector<uint32_t>> lines;
    auto state = [&](size_t s) -> uint8_t { return s < f.rebarState.size() ? f.rebarState[s] : 0; };
    auto pose = [&](int32_t body) {
        return body >= 0 && static_cast<size_t>(body) < f.poses.size() ? f.poses[static_cast<size_t>(body)] : RigidPose{};
    };
    auto turned = [](const RigidPose& p, const Vec3& v) { return p.apply(v) - p.position; };
    auto holds = [&](size_t s) {
        return !(state(s) & RigidFrame::kRebarLoose) &&
               !std::binary_search(f.vanished.begin(), f.vanished.end(), static_cast<uint32_t>(R.stations[s].body));
    };
    for (const RigidRebar::Bar& bar : R.bars) {
        const float total = bar.along.empty() ? 0.0f : bar.along.back();
        // A torn end sticks out of the piece: the steel came loose of the
        // concrete round the crack and necked before it broke.
        const float stub = std::max(8.0f * bar.width, 0.05f);
        size_t start = bar.first;
        float from = 0.0f;
        while (start < bar.first + bar.count) {
            // A run: the stations up to a tear, and the bar round them.
            size_t stop = start;
            while (stop + 1 < bar.first + bar.count && !(state(stop) & RigidFrame::kRebarTorn)) ++stop;
            const bool torn = (state(stop) & RigidFrame::kRebarTorn) && stop + 1 < bar.first + bar.count;
            const float to = torn ? 0.5f * (R.stations[stop].out + R.stations[stop + 1].in) : total;
            std::vector<size_t> held;
            for (size_t s = start; s <= stop; ++s) {
                if (holds(s)) held.push_back(s);
            }
            if (!held.empty()) {
                std::vector<uint32_t> line;
                auto add = [&](const Vec3& p, const Vec3& v) {
                    if (!line.empty() && length(p - points[line.back()]) < 1e-5f) return;
                    line.push_back(static_cast<uint32_t>(points.size()));
                    points.push_back(p);
                    velocities.push_back(v);
                    widths.push_back(bar.width);
                };
                // The bar from `a` to `b` along it, as `body` holds it.
                auto rigid = [&](int32_t body, float a, float b) {
                    const RigidPose q = pose(body);
                    const Vec3 ra = RigidRebar::at(bar, a);
                    add(q.apply(ra), q.velocityAt(ra));
                    for (size_t i = 0; i < bar.points.size(); ++i) {
                        if (bar.along[i] > a && bar.along[i] < b) add(q.apply(bar.points[i]), q.velocityAt(bar.points[i]));
                    }
                    const Vec3 rb = RigidRebar::at(bar, b);
                    add(q.apply(rb), q.velocityAt(rb));
                };
                const RigidRebar::Station& first = R.stations[held.front()];
                const float head = start > bar.first ? std::max(from, 0.0f) : 0.0f;
                rigid(first.body, std::min(head, first.in), first.in);
                for (size_t h = 0; h < held.size(); ++h) {
                    const RigidRebar::Station& st = R.stations[held[h]];
                    rigid(st.body, st.in, st.out);
                    if (h + 1 == held.size()) break;
                    // Bare between two pieces: bent from the one to the other.
                    const RigidRebar::Station& next = R.stations[held[h + 1]];
                    const RigidPose qa = pose(st.body), qb = pose(next.body);
                    const Vec3 ra = RigidRebar::at(bar, st.out), rb = RigidRebar::at(bar, next.in);
                    const Vec3 pa = qa.apply(ra), pb = qb.apply(rb);
                    const float chord = length(pb - pa), span = next.in - st.out;
                    const float reach = std::max(span, chord);
                    const Vec3 ta = turned(qa, RigidRebar::tangent(bar, st.out)) * reach;
                    const Vec3 tb = turned(qb, RigidRebar::tangent(bar, next.in)) * reach;
                    const Vec3 va = qa.velocityAt(ra), vb = qb.velocityAt(rb);
                    const int n = std::clamp(static_cast<int>(std::ceil(reach / 0.04f)), 1, 24);
                    for (int i = 1; i < n; ++i) {
                        const float t = static_cast<float>(i) / static_cast<float>(n), t2 = t * t, t3 = t2 * t;
                        const Vec3 p = pa * (2.0f * t3 - 3.0f * t2 + 1.0f) + ta * (t3 - 2.0f * t2 + t) +
                                       pb * (-2.0f * t3 + 3.0f * t2) + tb * (t3 - t2);
                        add(p, va + (vb - va) * t);
                    }
                }
                const RigidRebar::Station& last = R.stations[held.back()];
                rigid(last.body, last.out, torn ? std::max(to, last.out + stub) : std::max(to, last.out));
                if (line.size() >= 2) lines.push_back(std::move(line));
            }
            // The next run starts at the tear, a stub before the piece it
            // goes into.
            from = torn ? std::min(to, R.stations[stop + 1].in - stub) : total;
            start = stop + 1;
        }
    }
    out->addPoints(points.size());
    auto P = out->positionsForWrite();
    std::copy(points.begin(), points.end(), P.begin());
    auto v = out->points().create("v", AttrType::Vec3).write<Vec3>();
    std::copy(velocities.begin(), velocities.end(), v.begin());
    auto w = out->points().create("width", AttrType::Float).write<float>();
    std::copy(widths.begin(), widths.end(), w.begin());
    for (const std::vector<uint32_t>& line : lines) out->addPrimitive(line, false);
    return out;
}

namespace {

/// The polylines of `bars` added to `geo` as tubes as thick as their width,
/// six-sided: their points and quads, the points moving as v says, the
/// corners' Cd `steel`.
void appendTubes(Geometry& geo, const Geometry& bars, const Vec3& steel) {
    constexpr int kSides = 6;
    const auto C = bars.positions();
    const AttributeArray* wa = bars.points().find("width");
    const AttributeArray* va = bars.points().find("v");
    std::vector<Vec3> ring, ringV, ringN;
    std::vector<std::array<uint32_t, 4>> quads;
    for (size_t prim = 0; prim < bars.primitiveCount(); ++prim) {
        const auto c = bars.primitivePoints(prim);
        if (c.size() < 2) continue;
        const uint32_t base = static_cast<uint32_t>(geo.pointCount() + ring.size());
        Vec3 normal;
        for (size_t i = 0; i < c.size(); ++i) {
            const Vec3 back = C[c[i > 0 ? i - 1 : 0]], ahead = C[c[i + 1 < c.size() ? i + 1 : i]];
            Vec3 t = normalize(ahead - back);
            if (length(t) < 0.5f) t = Vec3(1.0f, 0.0f, 0.0f);
            // Carried along the bar, square to it: the tube does not twist.
            if (i == 0) normal = cross(t, std::fabs(t.x) < 0.6f ? Vec3(1.0f, 0.0f, 0.0f) : Vec3(0.0f, 1.0f, 0.0f));
            normal = normalize(normal - t * dot(normal, t));
            if (length(normal) < 0.5f) normal = normalize(cross(t, std::fabs(t.x) < 0.6f ? Vec3(1.0f, 0.0f, 0.0f) : Vec3(0.0f, 1.0f, 0.0f)));
            const Vec3 side = cross(t, normal);
            const float r = 0.5f * (wa && wa->type() == AttrType::Float ? wa->read<float>()[c[i]] : 0.012f);
            const Vec3 vel = va && va->type() == AttrType::Vec3 ? va->read<Vec3>()[c[i]] : Vec3();
            for (int k = 0; k < kSides; ++k) {
                const float a = 6.2831853f * static_cast<float>(k) / static_cast<float>(kSides);
                const Vec3 out = normal * std::cos(a) + side * std::sin(a);
                ring.push_back(C[c[i]] + out * r);
                ringN.push_back(out);
                ringV.push_back(vel);
            }
            if (i == 0) continue;
            const uint32_t now = base + static_cast<uint32_t>(i * kSides), was = now - kSides;
            for (uint32_t k = 0; k < kSides; ++k) {
                const uint32_t k1 = (k + 1) % kSides;
                quads.push_back({was + k, was + k1, now + k1, now + k});
            }
        }
    }
    if (quads.empty()) return;
    const size_t first = geo.addPoints(ring.size());
    auto P = geo.positionsForWrite();
    std::copy(ring.begin(), ring.end(), P.begin() + static_cast<long>(first));
    if (AttributeArray* v = geo.points().find("v"); v && v->type() == AttrType::Vec3) {
        auto w = v->write<Vec3>();
        std::copy(ringV.begin(), ringV.end(), w.begin() + static_cast<long>(first));
    }
    if (AttributeArray* n = geo.points().find("N"); n && n->type() == AttrType::Vec3) {
        auto w = n->write<Vec3>();
        std::copy(ringN.begin(), ringN.end(), w.begin() + static_cast<long>(first));
    }
    const size_t corners = geo.vertexCount();
    for (const auto& q : quads) geo.addPrimitive(q, true);
    if (AttributeArray* cd = geo.vertices().find("Cd"); cd && cd->type() == AttrType::Vec3) {
        auto w = cd->write<Vec3>();
        std::fill(w.begin() + static_cast<long>(corners), w.end(), steel);
    }
}

/// The tint of a chip of glass.
constexpr Vec3 kGlassChip(0.86f, 0.94f, 0.92f);

}  // namespace

std::shared_ptr<Geometry> drawnPieces(const RigidFrame& f, const Vec3& color, const Vec3& inside,
                                      const std::string& insideGroup, const Vec3& steel) {
    std::shared_ptr<Geometry> geo = posedPieces(f);
    const Group* cut = insideGroup.empty() ? nullptr : geo->findGroup(insideGroup);
    if (cut && cut->classOf() != AttrClass::Primitive) cut = nullptr;
    // A crack at a shallow angle to the face it breaks would be shaded
    // smooth into it: the points where the cut faces meet the others, twice.
    if (cut) {
        geo = apart(*geo, *cut);
        cut = geo->findGroup(insideGroup);
    }
    const size_t nprims = geo->primitiveCount();
    // The colour of every corner -- the corner's own, the point's, the
    // face's, the whole's -- so that a cut face can differ from the face
    // beside it, and the grit's points can have a colour of their own.
    const AttributeArray* vertexCd = geo->vertices().find("Cd");
    const AttributeArray* pointCd = geo->points().find("Cd");
    const AttributeArray* primCd = geo->primitives().find("Cd");
    const AttributeArray* detailCd = geo->detail().find("Cd");
    // Glass keeps its tint on the faces of its cracks: they are glass too.
    const AttributeArray* glass = geo->primitives().find("glass");
    std::vector<Vec3> corners(geo->vertexCount(), color);
    for (size_t p = 0; p < nprims; ++p) {
        const auto c = geo->primitivePoints(p);
        const size_t start = geo->primitiveVertexStart(p);
        const bool isCut = cut && cut->contains(p) && glassOf(glass, p) < 0.5f;
        for (size_t i = 0; i < c.size(); ++i) {
            const size_t at = start + i;
            Vec3& out = corners[at];
            if (isCut) out = inside;
            else if (isColor(vertexCd)) out = colorAt(*vertexCd, at);
            else if (isColor(pointCd)) out = colorAt(*pointCd, c[i]);
            else if (isColor(primCd)) out = colorAt(*primCd, p);
            else if (isColor(detailCd)) out = colorAt(*detailCd, 0);
        }
    }
    geo->points().erase("Cd");
    geo->vertices().erase("Cd");
    geo->primitives().erase("Cd");
    geo->detail().erase("Cd");
    auto cd = geo->vertices().create("Cd", AttrType::Vec3).write<Vec3>();
    std::copy(corners.begin(), corners.end(), cd.begin());
    // The bars, where the pieces have taken them.
    if (f.rebar) appendTubes(*geo, *rebarBars(f), steel);
    // The grit: loose points, as big as it is, in the colour of a cut.
    const size_t first = appendGrit(*geo, f);
    if (geo->pointCount() > first) {
        auto pc = geo->points().create("Cd", AttrType::Vec3).write<Vec3>();
        for (size_t i = first; i < pc.size(); ++i) {
            pc[i] = f.debrisGlass.size() == pc.size() - first && f.debrisGlass[i - first] ? kGlassChip : inside * 0.9f;
        }
    }
    return geo;
}

size_t appendGrit(Geometry& geo, const RigidFrame& f) {
    const size_t grit = f.debris.size() / 4, first = geo.pointCount();
    if (grit == 0) return first;
    geo.addPoints(grit);
    auto P = geo.positionsForWrite();
    auto pscale = geo.points().create("pscale", AttrType::Float).write<float>();
    for (size_t i = 0; i < grit; ++i) {
        const float* g = f.debris.data() + 4 * i;
        P[first + i] = Vec3(g[0], g[1], g[2]);
        pscale[first + i] = 0.5f * g[3];
    }
    if (f.debrisVelocity.size() == 3 * grit) {
        auto v = geo.points().create("v", AttrType::Vec3).write<Vec3>();
        for (size_t i = 0; i < grit; ++i) {
            const float* g = f.debrisVelocity.data() + 3 * i;
            v[first + i] = Vec3(g[0], g[1], g[2]);
        }
    }
    if (f.debrisIds.size() == grit) {
        auto id = geo.points().create("id", AttrType::Int).write<int32_t>();
        for (size_t i = 0; i < grit; ++i) id[first + i] = static_cast<int32_t>(f.debrisIds[i]);
    }
    if (f.debrisOrient.size() == 4 * grit) {
        auto orient = geo.points().create("orient", AttrType::Vec4).write<Vec4>();
        for (size_t i = 0; i < grit; ++i) {
            const float* q = f.debrisOrient.data() + 4 * i;
            orient[first + i] = Vec4(q[0], q[1], q[2], q[3]);
        }
    }
    if (f.debrisGlass.size() == grit) {
        auto glass = geo.points().create("glass", AttrType::Int).write<int32_t>();
        for (size_t i = 0; i < grit; ++i) glass[first + i] = f.debrisGlass[i] ? 1 : 0;
    }
    return first;
}

#ifdef PG_HAVE_JOLT

bool rigidAvailable() { return true; }

namespace {

namespace Layers {
constexpr JPH::ObjectLayer still = 0;
constexpr JPH::ObjectLayer moving = 1;
constexpr JPH::uint count = 2;
}  // namespace Layers

/// How fast a piece may go: metres a second, and radians a second.
constexpr float kMaxSpeed = 40.0f;
constexpr float kMaxSpin = 30.0f;
/// Metres a second two things must come together at for a knock that puffs dust.
constexpr float kKnockSpeed = 1.5f;
/// How many frames a puff of dust goes on for.
constexpr int kPuffLife = 8;
constexpr size_t kMaxPuffs = 400;
constexpr size_t kMaxGrit = 40000;
constexpr float kMaxSwell = 20.0f;  // 1/s: the most a puff swells
/// How much of the speed it had before a knock a body keeps when the glue
/// under it broke: it was crushing what broke off, not standing on it.
constexpr float kCarry = 0.985f;
/// Marks the user data of a still piece's body, above its number.
constexpr uint64_t kStillTag = 1ull << 62;
/// Grit: how hard the air holds a bit back -- as a stone of 2400 kg/m^3,
/// its drag a pull of 1.5e-4 v^2 / r (m/s^2), a chip of glass three times
/// as hard -- how much of the way it went into what it knocks into it keeps
/// bouncing off, and of the way along it; how slowly it must come in to lie
/// still, and how fast what it lies on must go to throw it off. m/s.
constexpr float kGritDrag = 1.5e-4f;
constexpr float kGritBounce = 0.25f;
constexpr float kGritSlide = 0.55f;
constexpr float kGritRest = 0.35f;
constexpr float kGritRide = 1.0f;
constexpr float kGritSpin = 40.0f;  ///< radians a second: the fastest a bit tumbles
/// Dust trails: how long after a piece came loose, how fast it must fly,
/// and how many a step at most -- the fastest.
constexpr float kTrailTime = 1.5f;
constexpr float kTrailSpeed = 2.5f;
constexpr size_t kMaxTrails = 32;

/// `v` no longer than `limit`.
Vec3 clampLength(const Vec3& v, float limit) {
    const float l = length(v);
    return l > limit ? v * (limit / l) : v;
}

void trace(const char*, ...) {}

/// Jolt set up, once for the program.
void startJolt() {
    static std::once_flag once;
    std::call_once(once, [] {
        JPH::RegisterDefaultAllocator();
        JPH::Trace = trace;
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
    });
}

JPH::Vec3 jolt(const Vec3& v) { return JPH::Vec3(v.x, v.y, v.z); }
/// Jolt's vector as ours -- positions too: Jolt is built in single precision.
Vec3 ours(JPH::Vec3Arg v) { return Vec3(v.GetX(), v.GetY(), v.GetZ()); }
static_assert(std::is_same_v<JPH::RVec3, JPH::Vec3>, "Jolt built with double precision: convert positions");

/// A rotation of ours as Jolt's quaternion.
JPH::Quat quaternionOf(const Rotation& r) {
    const JPH::Mat44 m(JPH::Vec4(r.x.x, r.x.y, r.x.z, 0.0f), JPH::Vec4(r.y.x, r.y.y, r.y.z, 0.0f),
                       JPH::Vec4(r.z.x, r.z.y, r.z.z, 0.0f), JPH::Vec4(0.0f, 0.0f, 0.0f, 1.0f));
    return m.GetQuaternion().Normalized();
}

Rotation rotationOf(JPH::QuatArg q) {
    Rotation r;
    r.x = ours(q * JPH::Vec3::sAxisX());
    r.y = ours(q * JPH::Vec3::sAxisY());
    r.z = ours(q * JPH::Vec3::sAxisZ());
    return r;
}

/// The shape of a collider, for Jolt: a ball, a box, a column; anything
/// else the hull of its triangles or of its box.
JPH::ShapeRefC shapeOf(const Collider& c) {
    const Vec3 half = c.size * 0.5f;
    auto box = [&]() -> JPH::ShapeRefC {
        return new JPH::BoxShape(jolt(Vec3(std::max(half.x, 1e-3f), std::max(half.y, 1e-3f), std::max(half.z, 1e-3f))));
    };
    switch (c.shape) {
        case Shape::Sphere: return new JPH::SphereShape(std::max({half.x, half.y, half.z, 1e-3f}));
        case Shape::Box: return box();
        case Shape::Cylinder: return new JPH::CylinderShape(std::max(half.y, 1e-3f), std::max(std::max(half.x, half.z), 1e-3f));
        default: break;
    }
    JPH::Array<JPH::Vec3> points;
    if (c.shape == Shape::Mesh && c.mesh) {
        // The mesh's box onto the collider's size, round its middle.
        const Vec3 center = c.mesh->center(), h = c.mesh->half();
        for (const Vec3& p : c.mesh->mesh().positions) {
            const Vec3 local = p - center;
            points.push_back(jolt(Vec3(local.x / std::max(h.x, 1e-6f) * half.x, local.y / std::max(h.y, 1e-6f) * half.y,
                                       local.z / std::max(h.z, 1e-6f) * half.z)));
        }
    } else if (c.shape == Shape::Cone) {
        for (int i = 0; i < 24; ++i) {
            const float a = static_cast<float>(i) * 6.2831853f / 24.0f;
            points.push_back(jolt(Vec3(std::cos(a) * half.x, -half.y, std::sin(a) * half.z)));
        }
        points.push_back(jolt(Vec3(0.0f, half.y, 0.0f)));
    }
    if (points.size() < 4) return box();
    JPH::ConvexHullShapeSettings settings(points, 0.0f);
    const JPH::ShapeSettings::ShapeResult r = settings.Create();
    if (r.HasError()) return box();
    return r.Get();
}

uint64_t splitmix(uint64_t& state) {
    uint64_t z = (state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

Vec3 vectorOf(const Geometry& geo, const RigidLayout& L, int k, const char* name, const Vec3& fallback) {
    const uint32_t prim = L.prims[static_cast<size_t>(k)].front();
    auto read = [](const AttributeArray* a, size_t i, Vec3& out) {
        if (!a || i >= a->size() || a->type() != AttrType::Vec3) return false;
        out = a->read<Vec3>()[i];
        return true;
    };
    Vec3 out = fallback;
    if (read(geo.primitives().find(name), prim, out)) return out;
    const auto c = geo.primitivePoints(prim);
    if (!c.empty() && read(geo.points().find(name), c[0], out)) return out;
    return fallback;
}

}  // namespace

// The glue, the way Houdini's Bullet solver has it: pieces glued together
// are one body -- the hulls of all their parts in one compound shape -- so
// a glued stack stands perfectly still, and costs one body. A knock breaks
// the glue where it lands: the force it takes to stop what knocked, against
// each joint of the piece it hit; half of that goes on to the pieces next
// to it, and so on -- a hard knock tears a hole, a light one chips a
// corner. What broke off is a body of its own; what is left goes on moving,
// most of its speed kept -- it was crushing the part that broke, not
// standing on it. Charges break a piece's joints at a time. Pieces that do
// not move (active 0) are still bodies of their own; a cluster glued to one
// is held there by a joint as long as the glue between them lasts.
//
// The bars (rebar) join the bodies the glue no longer does: a joint of
// Jolt's with all six ways free and friction on each -- the steel's pull on
// the three ways it moves, its bending on the three it turns -- so that a
// bar holds all it can and gives beyond that, and stays as it was given:
// plastic. How far it has given is kept by where its ends are: pulled
// longer than it is, it slid out of a piece -- or stretched -- by as much.
struct RigidSolver::Impl : public JPH::ContactListener {
    JPH::TempAllocatorImplWithMallocFallback temp{64 * 1024 * 1024};
    JPH::JobSystemSingleThreaded jobs{JPH::cMaxPhysicsJobs};
    JPH::BroadPhaseLayerInterfaceTable broadPhase{Layers::count, 2};
    JPH::ObjectLayerPairFilterTable pairs{Layers::count};
    std::unique_ptr<JPH::ObjectVsBroadPhaseLayerFilterTable> objectVsBroadPhase;
    JPH::PhysicsSystem physics;
    RigidSettings settings;

    struct Piece {
        std::vector<JPH::ShapeRefC> hulls;      ///< its parts, where they rest
        float looseAt = -1.0f;                  ///< seconds: when a joint of it first broke; below 0 never
        bool moves = true;                      ///< active: else a still body of its own
        int cluster = -1;                       ///< the body it is in; -1: gone, or none
        std::shared_ptr<const MeshShape> mesh;  ///< at rest, for the water and the gas; null if not wanted
        Vec3 euler;                             ///< the last angles given out: the next near them
        float release = 0.0f;                   ///< seconds; 0 or less: never
        Vec3 kick;
        bool vanish = false;                    ///< blown to dust when released
        float crush = 0.0f;                     ///< crushed to dust by a knock this many times its glue; 0: never
        bool released = false, gone = false;
        bool glass = false;                     ///< of glass: little dust, and its grit glitters
        float size = 0.1f;                      ///< how big it is across, metres
        Vec3 centre;                            ///< the middle of its box, at rest
        float guideWeight = 1.0f;               ///< how much the guide leads it (attribute guide)
        bool guided = false;                    ///< the guide still leads it
        int32_t guidePiece = 0;                 ///< its piece, as a guide numbers them (pieceOfPrimitives)
        float points = 0.0f;                    ///< how many points it has
        Vec3 middle;                            ///< where they are on average, at rest
        Vec3 scatter[3];                        ///< how they spread about it: sum (p - middle)(p - middle)^T
    };
    std::vector<Piece> pieces;
    std::shared_ptr<const RigidGuide> guide;    ///< where the next step is to take them; null: nowhere
    float guideStrength = 1.0f;                 ///< how hard, this step
    std::vector<JPH::BodyID> still;             ///< each still piece's body
    std::vector<int> dirty;                     ///< pieces whose glue broke: their clusters come apart
    struct Edge {
        int a = 0, b = 0;      ///< the pieces it glues
        Vec3 at;               ///< where their faces touch, at rest
        Vec3 normal;           ///< across them, from a to b, at rest
        float area = 0.0f;
        float strength = 0.0f; ///< newtons it holds
        bool broken = false;
        size_t joint = 0;      ///< the joint of the glue it is (RigidGlue)
    };
    std::vector<Edge> edges;
    std::vector<std::vector<int>> edgesOf;  ///< each piece's
    size_t broken = 0;
    std::vector<uint8_t> jointState;        ///< what became of each joint of the glue (RigidFrame)
    std::vector<float> jointTime;           ///< ... and when those that broke did
    struct Cluster {
        JPH::BodyID id;
        std::vector<int> pieces;       ///< in order
        std::vector<int> subPiece;     ///< for each sub-shape of its compound, the piece
        std::vector<JPH::Ref<JPH::Constraint>> anchors;  ///< to the still pieces it is glued to
        std::vector<int> heldBy;       ///< ... those pieces, in order
        bool alive = false;
        float mass = 0.0f;             ///< kilograms
        Vec3 before, beforeSpin;       ///< its velocity before the step, at its centre of mass
        float lift = 0.0f;             ///< how much of gravity the guide held off this step
    };
    std::vector<Cluster> clusters;
    struct Object {
        JPH::BodyID body;
        int node = 0;
    };
    std::vector<Object> objects;
    struct Puff {
        Vec3 at, velocity;
        float size = 0.3f, amount = 1.0f;
        float air = 0.0f;  ///< m^3/s: the air it pushes out
        int age = 0;
    };
    std::vector<Puff> puffs;
    struct Grit {
        Vec3 p, v;
        float size = 0.03f;
        uint32_t id = 0;  ///< its own number, from the first thrown: the same as long as it is there
        bool resting = false;
        bool glass = false;  ///< a chip of glass
        bool free = false;   ///< out of the pieces it came from: it knocks into things
        JPH::Quat turn = JPH::Quat::sIdentity();  ///< how it is turned
        Vec3 spin;           ///< the axis it tumbles about, as long as radians a second
        /// What it lies on, when that can move -- it rides on it -- and where
        /// it lies, which way is up and how it is turned there, in its frame.
        JPH::BodyID on;
        Vec3 local, localUp;
        JPH::Quat localTurn = JPH::Quat::sIdentity();
    };
    std::vector<Grit> grit;
    uint32_t gritThrown = 0;  ///< how many bits so far: the next one's number
    struct Knock {
        Vec3 at, normal, velocity;
        float speed = 0.0f;
        float size = 1.0f;         ///< of the smaller piece, metres
        float impulse = 0.0f;      ///< newton seconds, to stop what knocked
        int cluster[2] = {-1, -1}; ///< the clusters, -1 for anything else
        int piece[2] = {-1, -1};   ///< the pieces hit
        uint32_t body[2] = {0, 0}; ///< the bodies, by Jolt's index
    };
    std::vector<Knock> knocks;
    // The bars: where each runs through which piece, and what became of each
    // stretch of one -- slid out of its piece, torn after it -- how far it
    // has slid out of its piece so far, how much longer the bar after it got.
    std::shared_ptr<const RigidRebar> rebar;
    std::vector<uint8_t> barState;
    std::vector<float> slid, longer;
    struct Link {
        uint32_t bar = 0, from = 0, to = 0;       ///< stations of the bar that hold it, one and the next
        JPH::Ref<JPH::SixDOFConstraint> joint;    ///< between their bodies; null while those are one
        JPH::BodyID a, b;                         ///< ... those bodies
        float span = 0.0f;                        ///< metres of the bar between them, at rest
    };
    std::vector<Link> links;
    JPH::BodyID floorBody;  ///< the floor's, if there is one
    uint64_t random = 0x2545F4914F6CDD1Dull;
    float time = 0.0f;
    float gritScale = 1.0f;  ///< grit as big as the scene's dust: a town's is stones, a model's sand

    Impl() {
        broadPhase.MapObjectToBroadPhaseLayer(Layers::still, JPH::BroadPhaseLayer(0));
        broadPhase.MapObjectToBroadPhaseLayer(Layers::moving, JPH::BroadPhaseLayer(1));
        pairs.EnableCollision(Layers::moving, Layers::moving);
        pairs.EnableCollision(Layers::moving, Layers::still);
        objectVsBroadPhase = std::make_unique<JPH::ObjectVsBroadPhaseLayerFilterTable>(broadPhase, 2, pairs, Layers::count);
    }

    /// Which piece a sub-shape of body `b` is: its cluster's, or a still piece's.
    void whoIs(const JPH::Body& b, const JPH::SubShapeID& sub, int& cluster, int& piece) const {
        cluster = piece = -1;
        const uint64_t tag = b.GetUserData();
        if (tag == 0) return;
        if (tag & kStillTag) {
            piece = static_cast<int>(tag & ~kStillTag) - 1;
            return;
        }
        cluster = static_cast<int>(tag) - 1;
        if (cluster < 0 || static_cast<size_t>(cluster) >= clusters.size()) {
            cluster = -1;
            return;
        }
        const Cluster& c = clusters[static_cast<size_t>(cluster)];
        uint32_t index = 0;
        if (c.subPiece.size() > 1 && b.GetShape()->GetSubType() == JPH::EShapeSubType::StaticCompound) {
            JPH::SubShapeID rest;
            index = static_cast<const JPH::StaticCompoundShape*>(b.GetShape())->GetSubShapeIndexFromID(sub, rest);
        }
        if (index < c.subPiece.size()) piece = c.subPiece[index];
    }

    // Knocks: two things that come together fast -- touching for the first
    // time, or pressing on at speed: what crushes. Called as the step finds
    // them -- on this thread, in the same order every time.
    void OnContactAdded(const JPH::Body& b1, const JPH::Body& b2, const JPH::ContactManifold& m,
                        JPH::ContactSettings& settings) override {
        knock(b1, b2, m, settings);
    }
    void OnContactPersisted(const JPH::Body& b1, const JPH::Body& b2, const JPH::ContactManifold& m,
                            JPH::ContactSettings& settings) override {
        knock(b1, b2, m, settings);
    }
    void knock(const JPH::Body& b1, const JPH::Body& b2, const JPH::ContactManifold& m, JPH::ContactSettings&) {
        if (m.mRelativeContactPointsOn1.empty()) return;
        const JPH::RVec3 p = m.GetWorldSpaceContactPointOn1(0);
        const JPH::Vec3 rel = b2.GetPointVelocity(p) - b1.GetPointVelocity(p);
        const float speed = -rel.Dot(m.mWorldSpaceNormal);
        if (speed < kKnockSpeed) return;
        Knock k;
        k.at = ours(p);
        k.normal = ours(m.mWorldSpaceNormal);
        k.velocity = ours((b1.GetPointVelocity(p) + b2.GetPointVelocity(p)) * 0.5f);
        k.speed = speed;
        // What it takes to stop them, one against the other.
        const float inverse = (b1.IsDynamic() ? b1.GetMotionPropertiesUnchecked()->GetInverseMass() : 0.0f) +
                              (b2.IsDynamic() ? b2.GetMotionPropertiesUnchecked()->GetInverseMass() : 0.0f);
        k.impulse = inverse > 0.0f ? (1.0f + settings.bounce) * speed / inverse : 0.0f;
        whoIs(b1, m.mSubShapeID1, k.cluster[0], k.piece[0]);
        whoIs(b2, m.mSubShapeID2, k.cluster[1], k.piece[1]);
        // A body pressed on the still piece it is glued to -- a keyed
        // object pushing the wall on its foundation -- knocks nothing: they
        // are one, as the pieces of a body are.
        for (int side = 0; side < 2; ++side) {
            const int c = k.cluster[side], still = k.piece[1 - side];
            if (c < 0 || k.cluster[1 - side] >= 0 || still < 0) continue;
            const std::vector<int>& held = clusters[static_cast<size_t>(c)].heldBy;
            if (std::binary_search(held.begin(), held.end(), still)) return;
        }
        k.body[0] = b1.GetID().GetIndex();
        k.body[1] = b2.GetID().GetIndex();
        // How big what knocked is: the smaller of two pieces -- a sliver puffs no dust.
        float size = 1e30f;
        for (const int q : k.piece) {
            if (q >= 0) size = std::min(size, pieces[static_cast<size_t>(q)].size);
        }
        k.size = size < 1e29f ? size : 1.0f;
        knocks.push_back(k);
    }

    float unit() { return static_cast<float>(splitmix(random) >> 40) / static_cast<float>(1ull << 24); }
    Vec3 inBall() {
        for (;;) {
            const Vec3 d(unit() * 2.0f - 1.0f, unit() * 2.0f - 1.0f, unit() * 2.0f - 1.0f);
            if (dot(d, d) <= 1.0f) return d;
        }
    }

    /// A puff of dust -- together with one just made close by.
    /// `air`: m^3/s of it squeezed out there -- by a piece about `size`
    /// across coming down at `speed` onto what stops it, it is that big
    /// across as fast.
    void puff(const Vec3& at, const Vec3& velocity, float size, float amount, float air = 0.0f) {
        if (amount <= 0.0f) return;
        for (Puff& p : puffs) {
            if (p.age == 0 && length(p.at - at) < 0.75f * std::max(p.size, size)) {
                const float total = p.amount + amount;
                p.at = (p.at * p.amount + at * amount) * (1.0f / total);
                p.velocity = (p.velocity * p.amount + velocity * amount) * (1.0f / total);
                p.amount = std::min(total, 4.0f);
                p.size = std::max(p.size, size);
                p.air += air;
                return;
            }
        }
        puffs.push_back({at, velocity, size, std::min(amount, 4.0f), air, 0});
    }

    /// The air a piece `size` across squeezes out, stopped at `speed`:
    /// its cross-section -- about a third of its diagonal squared -- as fast.
    float squeezed(float size, float speed) const { return settings.air * 0.35f * size * size * speed; }

    /// A bit of grit, a chip of glass for `glass`: its size -- a few
    /// centimetres, as big as the scene's dust says -- its number, how it is
    /// turned and how it tumbles, flying at `speed`.
    Grit newGrit(bool glass, float speed) {
        Grit g;
        g.size = (0.02f + 0.08f * unit() * unit()) * gritScale * (glass ? 0.5f : 1.0f);
        g.id = gritThrown++;
        g.glass = glass;
        // One draw after another: the order of a call's arguments is the
        // compiler's to choose.
        const float x = unit() - 0.5f, y = unit() - 0.5f, z = unit() - 0.5f, w = unit() - 0.5f + 1e-3f;
        g.turn = JPH::Quat(x, y, z, w).Normalized();
        g.spin = clampLength(inBall() * (speed / std::max(g.size, 0.01f)), kGritSpin);
        return g;
    }

    /// `count` bits of grit from `at`, flying off at about `speed` round `base`.
    /// ... chips of glass, smaller, for `glass`.
    void throwGrit(const Vec3& at, const Vec3& base, int count, float speed, float spread, bool glass = false) {
        for (int i = 0; i < count; ++i) {
            Grit g = newGrit(glass, speed);
            g.p = at + inBall() * spread;
            g.v = base + inBall() * speed;
            grit.push_back(g);
        }
    }

    /// `count` bits of grit out of a crack: from the rim of a face `radius`
    /// across round `at` -- `normal` across it -- flying out in its plane at
    /// about `speed` as the pieces part, a little across it, round `base`.
    /// `side` 1 or -1: only that way across it moves -- the other side
    /// stands still -- and the grit starts and goes that way; 0 either way.
    void throwFromFace(const Vec3& at, const Vec3& normal, float radius, const Vec3& base, int count, float speed,
                       bool glass, float side) {
        const Vec3 t1 = normalize(cross(normal, std::fabs(normal.x) < 0.9f ? Vec3(1.0f, 0.0f, 0.0f) : Vec3(0.0f, 1.0f, 0.0f)));
        const Vec3 t2 = cross(normal, t1);
        for (int i = 0; i < count; ++i) {
            Grit g = newGrit(glass, speed);
            // Within the rim of a disc as big as the face -- the most of a
            // square face's middle.
            const float a = 6.2831853f * unit(), rim = 0.6f + 0.3f * unit(), off = unit() - 0.5f;
            const float fast = 0.4f + 0.6f * unit(), across = unit() - 0.5f;
            const Vec3 out = t1 * std::cos(a) + t2 * std::sin(a), wobble = inBall();
            const float start = side != 0.0f ? 0.005f * side : 0.01f * off;
            const float away = side != 0.0f ? std::fabs(across) * side : across;
            g.p = at + out * (radius * rim) + normal * start;
            g.v = base + out * (speed * fast) + normal * (0.5f * speed * away) + wobble * (0.2f * speed);
            grit.push_back(g);
        }
    }

    /// What grit knocks into: where, which way the face goes out there, how
    /// fast what it is moves there, and whether it can move.
    struct GritHit {
        Vec3 at, normal, velocity;
        JPH::BodyID body;
        bool moves = false;
    };

    /// The first face grit at `p` moving by `step` goes into -- one it comes
    /// out of, from inside a piece, it goes through -- of what stands still
    /// alone (the still pieces, the floor) for `stillOnly`.
    bool gritHits(const Vec3& p, const Vec3& step, GritHit& out, bool stillOnly) const {
        if (dot(step, step) < 1e-14f) return false;
        const JPH::RRayCast ray{jolt(p), jolt(step)};
        JPH::RayCastSettings settings;
        settings.mTreatConvexAsSolid = false;
        settings.SetBackFaceMode(JPH::EBackFaceMode::IgnoreBackFaces);
        JPH::ClosestHitCollisionCollector<JPH::CastRayCollector> hit;
        const JPH::SpecifiedObjectLayerFilter still(Layers::still);
        const JPH::ObjectLayerFilter all;
        physics.GetNarrowPhaseQuery().CastRay(ray, settings, hit, {}, stillOnly ? static_cast<const JPH::ObjectLayerFilter&>(still) : all);
        if (!hit.HadHit()) return false;
        const JPH::BodyLockRead lock(physics.GetBodyLockInterfaceNoLock(), hit.mHit.mBodyID);
        if (!lock.Succeeded()) return false;
        const JPH::Body& body = lock.GetBody();
        const JPH::RVec3 at = ray.GetPointOnRay(hit.mHit.mFraction);
        out.at = ours(at);
        out.normal = ours(body.GetWorldSpaceSurfaceNormal(hit.mHit.mSubShapeID2, at));
        out.velocity = ours(body.GetPointVelocity(at));
        out.body = hit.mHit.mBodyID;
        out.moves = !body.IsStatic();
        return true;
    }

    /// Whether `p` is inside a piece or an object -- the floor aside.
    bool inside(const Vec3& p) const {
        JPH::AnyHitCollisionCollector<JPH::CollidePointCollector> hit;
        const JPH::IgnoreSingleBodyFilter notFloor(floorBody);
        physics.GetNarrowPhaseQuery().CollidePoint(jolt(p), hit, {}, {}, notFloor);
        return hit.HadHit();
    }

    /// A step of `dt` of a bit of grit: through the air, held back by it the
    /// more the smaller it is; off what it knocks into -- bouncing, taking
    /// on how that moves -- and at rest where it comes in slowly onto
    /// something under it, riding on it if that moves until it moves fast
    /// or tips. Tumbling as it flies and bounces.
    void moveGrit(Grit& q, float dt) {
        const float r = 0.5f * q.size;
        if (q.resting) {
            if (q.on.IsInvalid()) return;  // on what never moves
            const JPH::BodyLockRead lock(physics.GetBodyLockInterfaceNoLock(), q.on);
            bool off = !lock.Succeeded() || !lock.GetBody().IsInBroadPhase();
            if (!off) {
                const JPH::Body& b = lock.GetBody();
                const JPH::Quat turn = b.GetRotation();
                q.p = ours(b.GetPosition() + turn * jolt(q.local));
                q.v = ours(b.GetPointVelocity(jolt(q.p)));
                q.turn = (turn * q.localTurn).Normalized();
                off = length(q.v) > kGritRide || (turn * jolt(q.localUp)).GetY() < 0.5f;
            }
            if (off) {
                q.resting = false;
                q.on = JPH::BodyID();
            }
            return;
        }
        q.v += settings.gravity * dt;
        const float drag = kGritDrag * (q.glass ? 3.0f : 1.0f) * length(q.v) / std::max(r, 1e-3f);
        q.v = q.v * (1.0f / (1.0f + drag * dt));
        const Vec3 step = q.v * dt;
        // Still in the pieces it came out of, it goes through what moves --
        // those pieces among it -- until it is out; not through what stands.
        GritHit hit;
        if (gritHits(q.p, step, hit, !q.free) && dot(q.v - hit.velocity, hit.normal) < 0.0f) {
            // Off it: a quarter of the way it came in, a half of the way along.
            const Vec3 rel = q.v - hit.velocity;
            const Vec3 in = hit.normal * dot(rel, hit.normal), along = rel - in;
            q.v = hit.velocity - in * kGritBounce + along * kGritSlide;
            q.p = hit.at + hit.normal * std::max(r, 1e-3f);
            q.spin = clampLength(cross(hit.normal, along) * (kGritSlide / std::max(r, 1e-3f)), kGritSpin);
            if (length(q.v - hit.velocity) < kGritRest && hit.normal.y > 0.5f) {
                q.resting = true;
                q.spin = Vec3();
                q.v = hit.velocity;
                q.on = JPH::BodyID();
                const JPH::BodyLockRead lock(physics.GetBodyLockInterfaceNoLock(), hit.body);
                if (hit.moves && lock.Succeeded()) {
                    const JPH::Body& b = lock.GetBody();
                    const JPH::Quat back = b.GetRotation().Conjugated();
                    q.on = hit.body;
                    q.local = ours(back * (jolt(q.p) - b.GetPosition()));
                    q.localUp = ours(back * jolt(hit.normal));
                    q.localTurn = back * q.turn;
                }
            }
        } else {
            q.p += step;
        }
        if (!q.free) q.free = !inside(q.p);
        // Come through the floor -- out of a crack at its foot -- onto it.
        if (settings.floor && q.p.y < r) {
            q.p.y = r;
            if (q.v.y < 0.0f) q.v.y = -kGritBounce * q.v.y;
            q.v.x *= kGritSlide;
            q.v.z *= kGritSlide;
            q.free = true;
            if (length(q.v) < kGritRest) {
                q.v = Vec3();
                q.spin = Vec3();
                q.resting = true;
                q.on = JPH::BodyID();
            }
        }
        const float angle = length(q.spin) * dt;
        if (angle > 1e-6f) q.turn = (JPH::Quat::sRotation(jolt(normalize(q.spin)), angle) * q.turn).Normalized();
    }

    /// Where the guide has a piece's points on average.
    Vec3 guidedMiddle(const Piece& p) const {
        const size_t k = static_cast<size_t>(p.guidePiece);
        return guide && k < guide->pieces.size() ? guide->pieces[k].apply(p.middle) : p.middle;
    }

    /// Where the guide has the body `c` at the end of the step: the turn and
    /// the place taking its pieces' points at rest nearest to the guide's,
    /// the body's own turn now where the search starts. False without a
    /// guide.
    bool guidedPose(const Cluster& c, JPH::Quat& turn, Vec3& place) const {
        if (!guide) return false;
        // Its pieces' points -- by their count, middles and spreads -- at
        // rest and where the guide turns and moves each piece.
        double n = 0.0, from[3] = {0.0, 0.0, 0.0}, to[3] = {0.0, 0.0, 0.0};
        for (const int k : c.pieces) {
            const Piece& p = pieces[static_cast<size_t>(k)];
            const Vec3 there = guidedMiddle(p);
            for (int a = 0; a < 3; ++a) {
                from[a] += static_cast<double>(p.points) * p.middle[a];
                to[a] += static_cast<double>(p.points) * there[a];
            }
            n += p.points;
        }
        if (n <= 0.0) return false;
        const Vec3 restMiddle(static_cast<float>(from[0] / n), static_cast<float>(from[1] / n),
                              static_cast<float>(from[2] / n));
        const Vec3 guideMiddle(static_cast<float>(to[0] / n), static_cast<float>(to[1] / n),
                               static_cast<float>(to[2] / n));
        // The spread of the moved against the rest: each piece's own, turned
        // as the guide turns it, and that of its middle about the body's.
        Vec3 columns[3];
        for (const int k : c.pieces) {
            const Piece& p = pieces[static_cast<size_t>(k)];
            if (p.points <= 0.0f) continue;
            const size_t g = static_cast<size_t>(p.guidePiece);
            const Vec4 q = g < guide->pieces.size() ? guide->pieces[g].rotation : Vec4(0.0f, 0.0f, 0.0f, 1.0f);
            const Vec3 a = guidedMiddle(p) - guideMiddle, b = p.middle - restMiddle;
            for (int col = 0; col < 3; ++col) columns[col] += turned(q, p.scatter[col]) + a * (p.points * b[col]);
        }
        const JPH::Quat now = physics.GetBodyInterfaceNoLock().GetRotation(c.id);
        const Vec4 best = bestTurn(columns, Vec4(now.GetX(), now.GetY(), now.GetZ(), now.GetW()));
        turn = JPH::Quat(best.x, best.y, best.z, best.w);
        place = guideMiddle - ours(turn * jolt(restMiddle));
        return true;
    }

    /// Steers the bodies the guide leads to where it has them at the end of
    /// the step: their velocity and spin -- as much of the way there as its
    /// strength and their pieces' weights say -- are those the step takes
    /// them there with, and as much of gravity is held off them. They still
    /// knock into things. A body held by a still piece is left where it is.
    void steer(float dt) {
        for (Cluster& c : clusters) c.lift = 0.0f;
        if (!guide || guideStrength <= 0.0f) return;
        JPH::BodyInterface& bi = physics.GetBodyInterfaceNoLock();
        for (Cluster& c : clusters) {
            if (!c.alive || !c.anchors.empty() || c.pieces.empty()) continue;
            float weight = 0.0f;
            for (const int k : c.pieces) {
                const Piece& p = pieces[static_cast<size_t>(k)];
                if (p.guided) weight += p.guideWeight;
            }
            const float pull = guideStrength * weight / static_cast<float>(c.pieces.size());
            if (pull <= 0.0f) continue;
            JPH::Quat turn;
            Vec3 place;
            if (!guidedPose(c, turn, place)) continue;
            // Its centre of mass now, and where the guide has it.
            const JPH::Quat now = bi.GetRotation(c.id);
            const Vec3 com = ours(bi.GetCenterOfMassPosition(c.id));
            const Vec3 local = ours(now.Conjugated() * jolt(com - ours(bi.GetPosition(c.id))));
            const Vec3 there = place + ours(turn * jolt(local));
            const Vec3 go = (there - com) * (1.0f / dt);
            // The turn from now to there, the short way, over the step.
            JPH::Quat d = (turn * now.Conjugated()).Normalized();
            if (d.GetW() < 0.0f) d = -d;
            JPH::Vec3 axis;
            float angle = 0.0f;
            d.GetAxisAngle(axis, angle);
            const Vec3 spin = angle > 1e-7f ? ours(axis) * (angle / dt) : Vec3();
            const Vec3 v = ours(bi.GetLinearVelocity(c.id)), w = ours(bi.GetAngularVelocity(c.id));
            bi.SetLinearVelocity(c.id, jolt(clampLength(v + (go - v) * pull, kMaxSpeed)));
            bi.SetAngularVelocity(c.id, jolt(clampLength(w + (spin - w) * pull, kMaxSpin)));
            // Held up as much as it is led: over the step, it keeps the
            // velocity it was given.
            bi.AddForce(c.id, jolt(settings.gravity * (-c.mass * pull)));
            bi.ActivateBody(c.id);
            c.lift = pull;
        }
    }

    /// Lets go the pieces the guide no longer leads: all of them after its
    /// time; a piece whose glue broke, when it lets those go; the pieces of a
    /// body further from where the guide has them than its reach.
    void letGo() {
        const RigidSettings& s = settings;
        const bool over = s.guideUntil > 0.0f && time >= s.guideUntil - 1e-6f;
        for (Piece& p : pieces) {
            if (p.guided && (over || p.gone || (s.guideLetGo && p.looseAt >= 0.0f))) p.guided = false;
        }
        if (!guide || s.guideReach <= 0.0f) return;
        for (const Cluster& c : clusters) {
            if (!c.alive) continue;
            bool far = false;
            for (const int k : c.pieces) {
                const Piece& p = pieces[static_cast<size_t>(k)];
                if (!p.guided || p.points <= 0.0f) continue;
                if (length(whereNow(k, p.middle) - guidedMiddle(p)) > s.guideReach) {
                    far = true;
                    break;
                }
            }
            if (!far) continue;
            for (const int k : c.pieces) pieces[static_cast<size_t>(k)].guided = false;
        }
    }

    /// Dust behind the pieces come loose not long ago, flying fast -- what the
    /// air takes off their broken faces -- the more the bigger and faster,
    /// fading as they fly on; the fastest few a step.
    void trailDust(float dt) {
        struct Trail {
            float amount;
            Vec3 at, velocity;
            float size;
        };
        std::vector<Trail> trails;
        const JPH::BodyInterface& bi = physics.GetBodyInterfaceNoLock();
        for (const Cluster& c : clusters) {
            if (!c.alive) continue;
            float since = 1e30f, size = 0.0f;
            for (const int k : c.pieces) {
                const Piece& p = pieces[static_cast<size_t>(k)];
                if (p.looseAt >= 0.0f) since = std::min(since, time - p.looseAt);
                size = std::max(size, p.size);
            }
            if (since > kTrailTime) continue;
            const Vec3 v = ours(bi.GetLinearVelocity(c.id));
            const float speed = length(v);
            if (speed < kTrailSpeed) continue;
            const float amount = settings.trail * 0.5f * std::clamp(speed / 8.0f, 0.3f, 1.5f) * (1.0f - since / kTrailTime) *
                                 std::clamp(size, 0.3f, 2.0f);
            // No smaller than the dust of a knock: a wisp thinner than the
            // gas's cells would barely reach them.
            trails.push_back({amount, ours(bi.GetCenterOfMassPosition(c.id)) - v * (0.5f * dt), v * 0.15f,
                              settings.dustSize * std::clamp(1.5f * size, 0.8f, 2.0f)});
        }
        std::sort(trails.begin(), trails.end(), [](const Trail& a, const Trail& b) {
            if (a.amount != b.amount) return a.amount > b.amount;
            if (a.at.x != b.at.x) return a.at.x < b.at.x;
            if (a.at.y != b.at.y) return a.at.y < b.at.y;
            return a.at.z < b.at.z;
        });
        if (trails.size() > kMaxTrails) trails.resize(kMaxTrails);
        for (const Trail& t : trails) puff(t.at, t.velocity, t.size, t.amount);
    }

    /// The body piece `k` is in, if it moves with one.
    JPH::BodyID bodyOf(int k) const {
        const Piece& p = pieces[static_cast<size_t>(k)];
        if (p.cluster < 0) return JPH::BodyID();
        return clusters[static_cast<size_t>(p.cluster)].id;
    }

    /// Where a point of piece `k` at rest is now.
    Vec3 whereNow(int k, const Vec3& rest) const {
        const JPH::BodyID id = bodyOf(k);
        if (id.IsInvalid()) return rest;
        const JPH::BodyInterface& bi = physics.GetBodyInterfaceNoLock();
        return ours(bi.GetPosition(id)) + ours(bi.GetRotation(id) * jolt(rest));
    }

    Vec3 velocityAt(int k, const Vec3& at) const {
        const JPH::BodyID id = bodyOf(k);
        if (id.IsInvalid()) return {};
        return ours(physics.GetBodyInterfaceNoLock().GetPointVelocity(id, jolt(at)));
    }

    /// A joint breaks: for good -- a puff of dust, a spray of grit.
    void snap(Edge& e, float dustScale) {
        if (e.broken) return;
        e.broken = true;
        ++broken;
        jointState[e.joint] = RigidFrame::kJointBroken;
        jointTime[e.joint] = time;
        dirty.push_back(e.a);
        dirty.push_back(e.b);
        const int k = pieces[static_cast<size_t>(e.a)].cluster >= 0 ? e.a : e.b;
        const Vec3 at = whereNow(k, e.at);
        const Vec3 v = velocityAt(k, at);
        for (const int piece : {e.a, e.b}) {
            float& loose = pieces[static_cast<size_t>(piece)].looseAt;
            if (loose < 0.0f) loose = time;
        }
        const RigidSettings& s = settings;
        // Glass breaks clean: a little glass dust, and glittering chips.
        const bool glass = pieces[static_cast<size_t>(e.a)].glass || pieces[static_cast<size_t>(e.b)].glass;
        const float size = s.dustSize * std::clamp(std::sqrt(e.area) * 1.5f, 0.6f, 2.5f);
        puff(at, v * 0.5f, size, s.dust * dustScale * std::clamp(e.area * 4.0f, 0.3f, 2.0f) * (glass ? 0.1f : 1.0f));
        // The grit out of the crack, from the rim of the face that broke.
        const int count = static_cast<int>(std::lround(s.debris * std::clamp(e.area * 24.0f, 2.0f, 10.0f)));
        const Vec3 normal = whereNow(k, e.at + e.normal) - at;
        // Off a piece that stands still -- the normal goes from a to b --
        // the grit comes out on the side that moves.
        const float side = !pieces[static_cast<size_t>(e.a)].moves ? 1.0f : !pieces[static_cast<size_t>(e.b)].moves ? -1.0f : 0.0f;
        throwFromFace(at, length(normal) > 1e-6f ? normalize(normal) : Vec3(0.0f, 1.0f, 0.0f),
                      std::sqrt(e.area / 3.14159265f), v, count, 2.5f, glass, side);
    }

    /// Piece `k` crushed to dust: gone, in a burst of it, and grit.
    void crumble(int k) {
        Piece& p = pieces[static_cast<size_t>(k)];
        const Vec3 middle = whereNow(k, p.centre);
        const Vec3 v = velocityAt(k, middle);
        p.gone = true;
        dirty.push_back(k);
        for (const int ei : edgesOf[static_cast<size_t>(k)]) snap(edges[static_cast<size_t>(ei)], 0.5f);
        const RigidSettings& s = settings;
        puff(middle, v * 0.5f, s.dustSize * std::clamp(p.size, 1.0f, 3.0f), 2.0f * s.impactDust * (p.glass ? 0.2f : 1.0f),
             squeezed(p.size, length(v)));
        throwGrit(middle, v, static_cast<int>(std::lround(std::clamp(16.0f * p.size, 4.0f, 30.0f) * s.debris)), 3.0f,
                  0.3f * p.size, p.glass);
    }

    /// A knock of `force` newtons on piece `k`: the joints it cannot hold
    /// break; some of it (spread) goes on through them to the pieces
    /// beyond, and so on -- no further than rings of pieces, when set.
    void spread(int k, float force, std::vector<float>& felt) {
        struct Felt {
            int piece;
            float force;
            int ring;  // how many joints from where it landed
        };
        std::vector<Felt> queue{{k, force, 0}};
        for (size_t at = 0; at < queue.size(); ++at) {
            const auto [q, f, ring] = queue[at];
            if (f <= felt[static_cast<size_t>(q)]) continue;
            felt[static_cast<size_t>(q)] = f;
            for (const int ei : edgesOf[static_cast<size_t>(q)]) {
                Edge& e = edges[static_cast<size_t>(ei)];
                if (e.broken) continue;  // broken before: nothing goes through
                const int other = e.a == q ? e.b : e.a;
                if (f > e.strength) {
                    snap(e, 1.0f);
                    // Knocked far harder than it holds: crushed to dust.
                    Piece& p = pieces[static_cast<size_t>(q)];
                    if (p.crush > 0.0f && p.moves && !p.gone && f > p.crush * e.strength) crumble(q);
                }
                // What broke it went on through it -- some of it.
                if (settings.rings > 0 && ring >= settings.rings) continue;
                const float on = f * settings.spread;
                if (on > felt[static_cast<size_t>(other)] && on > 1e-3f * e.strength) queue.push_back({other, on, ring + 1});
            }
        }
    }

    /// The shape of a cluster: the hulls of all its pieces' parts, as one.
    JPH::ShapeRefC shapeOf(Cluster& c) {
        c.subPiece.clear();
        std::vector<JPH::ShapeRefC> subs;
        for (const int k : c.pieces) {
            for (const JPH::ShapeRefC& h : pieces[static_cast<size_t>(k)].hulls) {
                subs.push_back(h);
                c.subPiece.push_back(k);
            }
        }
        if (subs.size() == 1) return subs.front();
        JPH::StaticCompoundShapeSettings cs;
        for (const JPH::ShapeRefC& h : subs) cs.AddShape(JPH::Vec3::sZero(), JPH::Quat::sIdentity(), h);
        const JPH::ShapeSettings::ShapeResult r = cs.Create();
        return r.HasError() ? subs.front() : r.Get();
    }

    /// A body for the pieces `members` -- glued, all of them moving -- at
    /// `position` and `rotation`, moving as `velocity` (at `centre`) and
    /// `spin` say.
    int makeCluster(std::vector<int> members, JPH::RVec3Arg position, JPH::QuatArg rotation, const Vec3& centre,
                    const Vec3& velocity, const Vec3& spin, const std::vector<JPH::BodyID>& stillBodies) {
        JPH::BodyInterface& bi = physics.GetBodyInterfaceNoLock();
        std::sort(members.begin(), members.end());
        Cluster c;
        c.pieces = members;
        const JPH::ShapeRefC shape = shapeOf(c);
        JPH::BodyCreationSettings bcs(shape, position, rotation, JPH::EMotionType::Dynamic, Layers::moving);
        bcs.mFriction = settings.friction;
        bcs.mRestitution = settings.bounce;
        // No faster than debris flies: a keyed object -- infinitely heavy --
        // that lands deep in the pieces would otherwise fire them off at the
        // speed of a bullet, and the water and the gas they go into with them.
        bcs.mMaxLinearVelocity = kMaxSpeed;
        bcs.mMaxAngularVelocity = kMaxSpin;
        // Every sub-shape its own knocks: which piece was hit.
        bcs.mUseManifoldReduction = false;
        const int index = static_cast<int>(clusters.size());
        bcs.mUserData = static_cast<uint64_t>(index) + 1;
        JPH::Body* body = bi.CreateBody(bcs);
        if (!body) return -1;
        // Moving as the body it came from: the velocity at its own middle.
        const Vec3 middle = ours(body->GetCenterOfMassPosition());
        body->SetLinearVelocityClamped(jolt(velocity + cross(spin, middle - centre)));
        body->SetAngularVelocityClamped(jolt(spin));
        c.id = body->GetID();
        c.alive = true;
        c.mass = 1.0f / std::max(body->GetMotionProperties()->GetInverseMass(), 1e-12f);
        bi.AddBody(c.id, JPH::EActivation::Activate);
        for (const int k : members) pieces[static_cast<size_t>(k)].cluster = index;
        // Held where it is glued to still pieces.
        std::vector<int> held;
        for (const int k : members) {
            for (const int ei : edgesOf[static_cast<size_t>(k)]) {
                const Edge& e = edges[static_cast<size_t>(ei)];
                if (e.broken) continue;
                const int other = e.a == k ? e.b : e.a;
                if (!pieces[static_cast<size_t>(other)].moves) held.push_back(other);
            }
        }
        std::sort(held.begin(), held.end());
        held.erase(std::unique(held.begin(), held.end()), held.end());
        c.heldBy = held;
        for (const int s : held) {
            const JPH::BodyID still = stillBodies[static_cast<size_t>(s)];
            if (still.IsInvalid()) continue;
            JPH::FixedConstraintSettings fs;
            fs.mAutoDetectPoint = true;
            // The lock keeps the pointer to the ids: they outlive it.
            const std::array<JPH::BodyID, 2> pair{c.id, still};
            JPH::BodyLockMultiWrite lock(physics.GetBodyLockInterfaceNoLock(), pair.data(), 2);
            JPH::Body* a = lock.GetBody(0);
            JPH::Body* b = lock.GetBody(1);
            if (!a || !b) continue;
            JPH::Ref<JPH::Constraint> anchor = fs.Create(*a, *b);
            physics.AddConstraint(anchor);
            c.anchors.push_back(anchor);
        }
        clusters.push_back(std::move(c));
        return index;
    }

    /// Takes cluster `ci` apart into what is still glued together: a body
    /// for each such group, moving as the cluster did. `hit` pieces were
    /// where it was knocked -- their groups move as the step left them;
    /// the others as they did before it, mostly: they were crushing, not
    /// standing.
    void split(int ci, const std::vector<uint8_t>& hit, const std::vector<JPH::BodyID>& stillBodies) {
        Cluster& old = clusters[static_cast<size_t>(ci)];
        if (!old.alive) return;
        JPH::BodyInterface& bi = physics.GetBodyInterfaceNoLock();
        // The groups: the pieces still there, joined by joints that hold.
        std::vector<int> members;
        for (const int k : old.pieces) {
            if (!pieces[static_cast<size_t>(k)].gone) members.push_back(k);
        }
        std::map<int, int> slot;
        for (size_t i = 0; i < members.size(); ++i) slot[members[i]] = static_cast<int>(i);
        UnionFind uf(members.size());
        for (const int k : members) {
            for (const int ei : edgesOf[static_cast<size_t>(k)]) {
                const Edge& e = edges[static_cast<size_t>(ei)];
                if (e.broken) continue;
                const int other = e.a == k ? e.b : e.a;
                const auto it = slot.find(other);
                if (it != slot.end()) uf.unite(static_cast<uint32_t>(slot[k]), static_cast<uint32_t>(it->second));
            }
        }
        std::map<uint32_t, std::vector<int>> groups;
        for (size_t i = 0; i < members.size(); ++i) groups[uf.find(static_cast<uint32_t>(i))].push_back(members[i]);
        // How it moved: before the step, and now.
        const JPH::RVec3 position = bi.GetPosition(old.id);
        const JPH::Quat rotation = bi.GetRotation(old.id);
        const Vec3 centre = ours(bi.GetCenterOfMassPosition(old.id));
        const Vec3 velocity = ours(bi.GetLinearVelocity(old.id)), spin = ours(bi.GetAngularVelocity(old.id));
        for (const auto& a : old.anchors) physics.RemoveConstraint(a);
        old.anchors.clear();
        unjoin(old.id);
        bi.RemoveBody(old.id);
        bi.DestroyBody(old.id);
        old.alive = false;
        const Vec3 before = old.before, beforeSpin = old.beforeSpin;
        for (const int k : old.pieces) pieces[static_cast<size_t>(k)].cluster = -1;
        for (const auto& [root, group] : groups) {
            // Still glued to a still piece: where it was built, not moving
            // -- whatever pushed it in the step (a keyed object, as
            // unstoppable as the still piece is unmovable) pushed in vain.
            if (held(group)) {
                makeCluster(group, JPH::RVec3::sZero(), JPH::Quat::sIdentity(), centre, Vec3(), Vec3(), stillBodies);
                continue;
            }
            // What was mostly struck stops as the step stopped it; the rest
            // goes on, most of its speed kept.
            size_t struck = 0;
            for (const int k : group) struck += hit[static_cast<size_t>(k)];
            const float keep = 2 * struck > group.size() ? 0.0f : kCarry;
            const Vec3 v = velocity + (before - velocity) * keep;
            const Vec3 w = spin + (beforeSpin - spin) * keep;
            makeCluster(group, position, rotation, centre, v, w, stillBodies);
        }
    }

    /// Whether any of `members` is glued to a still piece.
    bool held(const std::vector<int>& members) const {
        for (const int k : members) {
            for (const int ei : edgesOf[static_cast<size_t>(k)]) {
                const Edge& e = edges[static_cast<size_t>(ei)];
                if (!e.broken && !pieces[static_cast<size_t>(e.a == k ? e.b : e.a)].moves) return true;
            }
        }
        return false;
    }

    /// The bodies glued to still pieces back where they were built, still:
    /// the joint holds them, but a keyed object -- infinitely heavy --
    /// pushes against it as hard as it must, and a step can give a little.
    void settleHeld() {
        JPH::BodyInterface& bi = physics.GetBodyInterfaceNoLock();
        for (const Cluster& c : clusters) {
            if (!c.alive || c.anchors.empty()) continue;
            bi.SetPositionAndRotation(c.id, JPH::RVec3::sZero(), JPH::Quat::sIdentity(), JPH::EActivation::DontActivate);
            bi.SetLinearAndAngularVelocity(c.id, JPH::Vec3::sZero(), JPH::Vec3::sZero());
        }
    }

    // --- The bars ---

    /// Newtons a bar holds pulled -- the steel yields -- and newton metres bent.
    float steelPull(const RigidRebar::Bar& bar) const { return settings.rebarStrength * 0.7853982f * bar.width * bar.width; }
    float steelBend(const RigidRebar::Bar& bar) const {
        return settings.rebarStrength * bar.width * bar.width * bar.width / 6.0f;
    }
    /// Newtons the bond of station `s` holds its bar with: along what of
    /// the bar is still in its piece.
    float bondAt(uint32_t s, const RigidRebar::Bar& bar) const {
        const RigidRebar::Station& st = rebar->stations[s];
        return settings.bond * 3.1415927f * bar.width * std::max(st.out - st.in - slid[s], 0.0f);
    }
    /// Newtons the bar is held with from station `s` on away from a link --
    /// `dir` -1 back along the bar, +1 on -- by the bond of all the pieces
    /// that hold it there, up to a tear or its end: each passes the pull on
    /// to the next. Counted until it is `enough`.
    float anchorage(uint32_t s, int dir, const RigidRebar::Bar& bar, float enough) const {
        float total = 0.0f;
        for (int64_t i = s; i >= bar.first && i < static_cast<int64_t>(bar.first + bar.count); i += dir) {
            const uint32_t u = static_cast<uint32_t>(i);
            if (dir < 0 && u < s && (barState[u] & RigidFrame::kRebarTorn)) break;
            if (holds(u)) total += bondAt(u, bar);
            if (total >= enough) break;
            if (dir > 0 && (barState[u] & RigidFrame::kRebarTorn)) break;
        }
        return total;
    }
    /// Whether the bar is still in the piece of station `s`.
    bool holds(uint32_t s) const {
        return !(barState[s] & RigidFrame::kRebarLoose) && !pieces[static_cast<size_t>(rebar->stations[s].body)].gone;
    }
    /// The body piece `k` is in: its cluster's, or its own still one.
    JPH::BodyID bodyAt(int k) const {
        const Piece& p = pieces[static_cast<size_t>(k)];
        if (p.gone) return JPH::BodyID();
        return p.moves ? bodyOf(k) : still[static_cast<size_t>(k)];
    }
    /// Which way a direction of piece `k` at rest points now.
    Vec3 turnNow(int k, const Vec3& v) const {
        const JPH::BodyID id = bodyOf(k);
        if (id.IsInvalid()) return v;
        return ours(physics.GetBodyInterfaceNoLock().GetRotation(id) * jolt(v));
    }
    /// The gap between two stations of a link nearest its middle: where it tears.
    uint32_t middle(const Link& l) const {
        const std::vector<RigidRebar::Station>& st = rebar->stations;
        const float mid = 0.5f * (st[l.from].out + st[l.to].in);
        uint32_t best = l.from;
        float nearest = 1e30f;
        for (uint32_t s = l.from; s < l.to; ++s) {
            const float d = std::fabs(0.5f * (st[s].out + st[s + 1].in) - mid);
            if (d < nearest) {
                nearest = d;
                best = s;
            }
        }
        return best;
    }
    /// How hard link `l` holds: all it can taut -- the steel, or the bond
    /// that anchors the bar less, on one side or the other -- slack only
    /// what bends it straight.
    void setHold(Link& l, float needed, float free) {
        const RigidRebar::Bar& bar = rebar->bars[l.bar];
        const float steel = steelPull(bar), bend = steelBend(bar);
        const float hold = std::min({steel, anchorage(l.from, -1, bar, steel), anchorage(l.to, 1, bar, steel)});
        const bool slack = needed < free - 0.5f * bar.width;
        const float pull = slack ? std::min(hold, 2.0f * bend / std::max(free, bar.width)) : hold;
        const float turn = bend * std::min(1.0f, hold / std::max(steel, 1e-6f));
        using Axis = JPH::SixDOFConstraintSettings::EAxis;
        for (int a = 0; a < 3; ++a) {
            l.joint->SetMaxFriction(static_cast<Axis>(Axis::TranslationX + a), pull);
            l.joint->SetMaxFriction(static_cast<Axis>(Axis::RotationX + a), turn);
        }
    }
    /// Where link `l` leaves its first station's piece and goes into its
    /// last's, now; how much bar there is between those; how far apart they
    /// are along the bar; and how much bar it takes to join them. The bar
    /// yields over twenty times its diameter round a crack as well: moved
    /// apart along the bar, it takes as much more; moved aside, it bends
    /// into an S along that and takes less.
    void ends(const Link& l, Vec3& pa, Vec3& pb, float& free, float& along, float& needed) const {
        const RigidRebar::Bar& bar = rebar->bars[l.bar];
        const RigidRebar::Station &A = rebar->stations[l.from], &B = rebar->stations[l.to];
        pa = whereNow(A.body, RigidRebar::at(bar, A.out));
        pb = whereNow(B.body, RigidRebar::at(bar, B.in));
        free = l.span + slid[l.from] + slid[l.to];
        for (uint32_t s = l.from; s < l.to; ++s) free += longer[s];
        Vec3 axis = normalize(turnNow(A.body, RigidRebar::tangent(bar, A.out)) + turnNow(B.body, RigidRebar::tangent(bar, B.in)));
        if (length(axis) < 0.5f) axis = normalize(pb - pa);
        const Vec3 d = pb - pa;
        const float yields = 20.0f * bar.width;
        along = std::max(dot(d, axis), -0.5f * yields);
        const float aside = length(d - axis * dot(d, axis));
        needed = std::sqrt((yields + along) * (yields + along) + aside * aside) - yields;
    }
    /// A joint for link `l` between its bodies -- none while they are one,
    /// or neither moves.
    JPH::Ref<JPH::SixDOFConstraint> join(Link& l) {
        if (l.a.IsInvalid() || l.b.IsInvalid() || l.a == l.b) return nullptr;
        JPH::BodyInterface& bi = physics.GetBodyInterfaceNoLock();
        if (bi.GetMotionType(l.a) != JPH::EMotionType::Dynamic && bi.GetMotionType(l.b) != JPH::EMotionType::Dynamic) {
            return nullptr;
        }
        Vec3 pa, pb;
        float free = 0.0f, along = 0.0f, needed = 0.0f;
        ends(l, pa, pb, free, along, needed);
        Vec3 axis = pb - pa;
        if (length(axis) < 1e-4f) axis = turnNow(rebar->stations[l.from].body, RigidRebar::tangent(rebar->bars[l.bar], rebar->stations[l.from].out));
        axis = normalize(axis);
        if (length(axis) < 0.5f) axis = Vec3(1.0f, 0.0f, 0.0f);
        const Vec3 side = normalize(cross(axis, std::fabs(axis.x) < 0.6f ? Vec3(1.0f, 0.0f, 0.0f) : Vec3(0.0f, 1.0f, 0.0f)));
        JPH::SixDOFConstraintSettings js;  // all six ways free: friction alone holds them
        js.mSpace = JPH::EConstraintSpace::WorldSpace;
        js.mPosition1 = jolt(pa);
        js.mPosition2 = jolt(pb);
        js.mAxisX1 = js.mAxisX2 = jolt(axis);
        js.mAxisY1 = js.mAxisY2 = jolt(side);
        const std::array<JPH::BodyID, 2> pair{l.a, l.b};
        JPH::BodyLockMultiWrite lock(physics.GetBodyLockInterfaceNoLock(), pair.data(), 2);
        JPH::Body* a = lock.GetBody(0);
        JPH::Body* b = lock.GetBody(1);
        if (!a || !b) return nullptr;
        l.joint = static_cast<JPH::SixDOFConstraint*>(js.Create(*a, *b));
        setHold(l, needed, free);
        physics.AddConstraint(l.joint);
        return l.joint;
    }
    /// The joints of the bars on body `id` gone with it.
    void unjoin(const JPH::BodyID& id) {
        for (Link& l : links) {
            if (!l.joint || (l.a != id && l.b != id)) continue;
            physics.RemoveConstraint(l.joint);
            l.joint = nullptr;
        }
    }
    /// The links of the bars as they are now: between the stations of each
    /// that hold it, one and the next -- none across a tear -- with a joint
    /// where their pieces are apart; the joints that join the same bodies
    /// as before kept.
    void relinkBars() {
        const std::vector<RigidRebar::Station>& st = rebar->stations;
        // A piece gone -- crushed, blown to dust -- lets go of its bars.
        for (size_t s = 0; s < st.size(); ++s) {
            if (pieces[static_cast<size_t>(st[s].body)].gone) barState[s] |= RigidFrame::kRebarLoose;
        }
        std::map<std::pair<uint32_t, uint32_t>, size_t> had;
        for (size_t i = 0; i < links.size(); ++i) had[{links[i].from, links[i].to}] = i;
        std::vector<uint8_t> kept(links.size(), 0);
        std::vector<Link> next;
        for (uint32_t b = 0; b < rebar->bars.size(); ++b) {
            const RigidRebar::Bar& bar = rebar->bars[b];
            int64_t prev = -1;
            for (uint32_t s = bar.first; s < bar.first + bar.count; ++s) {
                if (holds(s)) {
                    if (prev >= 0) {
                        Link l;
                        l.bar = b;
                        l.from = static_cast<uint32_t>(prev);
                        l.to = s;
                        l.span = std::max(st[s].in - st[l.from].out, 0.0f);
                        next.push_back(l);
                    }
                    prev = s;
                }
                if (barState[s] & RigidFrame::kRebarTorn) prev = -1;
            }
        }
        for (Link& l : next) {
            l.a = bodyAt(st[l.from].body);
            l.b = bodyAt(st[l.to].body);
            if (const auto it = had.find({l.from, l.to}); it != had.end()) {
                const Link& old = links[it->second];
                if (old.joint && old.a == l.a && old.b == l.b) {
                    l.joint = old.joint;
                    kept[it->second] = 1;
                    continue;
                }
            }
            join(l);
        }
        for (size_t i = 0; i < links.size(); ++i) {
            if (links[i].joint && !kept[i]) physics.RemoveConstraint(links[i].joint);
        }
        links = std::move(next);
    }
    /// The bar is out of the piece of station `s`, at `at`: the concrete
    /// falls off it in a puff of dust.
    void comeOut(uint32_t s, const Vec3& at) {
        barState[s] |= RigidFrame::kRebarLoose;
        const RigidRebar::Station& st = rebar->stations[s];
        const float l = st.out - st.in;
        const Vec3 v = velocityAt(st.body, at);
        puff(at, v * 0.5f, settings.dustSize * 0.8f, settings.dust * std::clamp(l * 3.0f, 0.2f, 1.2f));
        throwGrit(at, v, static_cast<int>(std::lround(settings.debris * std::clamp(l * 20.0f, 2.0f, 8.0f))), 1.5f, 0.05f,
                  pieces[static_cast<size_t>(st.body)].glass);
    }
    /// How the bars took the step. One pulled longer than it is between two
    /// pieces gives where it holds least. Where the bond anchors it harder
    /// than the steel holds on both sides, the steel stretches; else, pulled
    /// along it, it slides out of the side anchored less -- out of the piece
    /// at the link once it has slid as far as it ran through it, then out of
    /// the next -- and pulled aside, it bends there. Stretched or bent
    /// Stretch over what of it yields, it tears -- at the face of the side
    /// that holds it less: that one goes with a stub of it, the other keeps
    /// the rest. Then how hard each holds from now on. True if a bar tore or
    /// came out of a piece.
    bool strain() {
        bool changed = false;
        const std::vector<RigidRebar::Station>& st = rebar->stations;
        for (Link& l : links) {
            if (!l.joint || !holds(l.from) || !holds(l.to)) continue;
            const RigidRebar::Bar& bar = rebar->bars[l.bar];
            Vec3 pa, pb;
            float free = 0.0f, along = 0.0f, needed = 0.0f;
            ends(l, pa, pb, free, along, needed);
            const float excess = needed - free;
            if (excess > 0.0f) {
                const float steel = steelPull(bar);
                const float anchorA = anchorage(l.from, -1, bar, 4.0f * steel), anchorB = anchorage(l.to, 1, bar, 4.0f * steel);
                const bool aWeaker = anchorA <= anchorB;
                const uint32_t face = aWeaker ? l.from : l.to - 1;  // the gap at the weaker side's face
                float bent = excess;
                if (std::min(anchorA, anchorB) < steel) {
                    const uint32_t weak = aWeaker ? l.from : l.to;
                    const float slide = std::min(std::max(along - free, 0.0f), excess);
                    bent -= slide;
                    slid[weak] += slide;
                    if (slid[weak] >= st[weak].out - st[weak].in) {
                        comeOut(weak, weak == l.from ? pa : pb);
                        changed = true;
                        continue;
                    }
                }
                if (bent > 0.0f) {
                    longer[face] += bent;
                    float stretched = 0.0f;
                    for (uint32_t s = l.from; s < l.to; ++s) stretched += longer[s];
                    if (stretched > settings.stretch * (l.span + 20.0f * bar.width)) {
                        barState[face] |= RigidFrame::kRebarTorn;
                        changed = true;
                        continue;
                    }
                }
                free = needed;
            }
            setHold(l, needed, free);
        }
        return changed;
    }
};

RigidSolver::RigidSolver(const RigidScene& scene) : scene_(scene) {
    startJolt();
    impl_ = std::make_unique<Impl>();
    Impl& m = *impl_;
    const RigidSettings& s = scene_.solver;
    m.settings = s;
    const Geometry* geo = scene_.pieces.get();
    layout_ = geo ? rigidLayout(*geo, scene_.attribute) : std::make_shared<RigidLayout>();
    glue_ = geo ? rigidGlue(*geo, *layout_, scene_.attribute, scene_.constraints.get()) : std::make_shared<RigidGlue>();
    const RigidLayout& L = *layout_;
    const size_t count = static_cast<size_t>(L.bodies);
    // Room for a body a piece -- as the glue breaks -- a still body a
    // still piece, the objects and the floor.
    const size_t all = 2 * count + scene_.colliders.size() + 16;
    m.physics.Init(static_cast<JPH::uint>(all), 0, static_cast<JPH::uint>(std::max<size_t>(4096, all * 32)),
                   static_cast<JPH::uint>(std::max<size_t>(4096, all * 32)), m.broadPhase, *m.objectVsBroadPhase, m.pairs);
    m.physics.SetGravity(jolt(s.gravity));
    m.physics.SetContactListener(&m);
    m.random ^= static_cast<uint64_t>(count) * 0x9E3779B97F4A7C15ull;
    m.gritScale = std::clamp(s.dustSize / 0.3f, 0.5f, 4.0f);
    if (!geo || count == 0) {
        error_ = "no pieces: link geometry into Pieces -- a Voronoi Fracture's";
        return;
    }
    JPH::BodyInterface& bi = m.physics.GetBodyInterfaceNoLock();

    // The pieces: the hull of each part, where it rests -- as their proxy
    // has it, when they carry one; what their attributes say.
    const std::vector<Vec3> P = rigidPositions(*geo);
    const std::span<const Vec3> at = geo->positions();
    int pieceCount = 0;
    const std::vector<int32_t> pieceOf = pieceOfPrimitives(*geo, scene_.attribute, pieceCount);
    const bool meshes = scene_.intoGas || scene_.intoWater || scene_.intoRain;
    m.pieces.resize(count);
    m.still.assign(count, JPH::BodyID());
    for (size_t k = 0; k < count; ++k) {
        const int body = static_cast<int>(k);
        Impl::Piece& piece = m.pieces[k];
        const float density = std::max(numberOf(*geo, L, body, "density", s.density), 1.0f);
        Vec3 lo(1e30f, 1e30f, 1e30f), hi(-1e30f, -1e30f, -1e30f);
        for (const std::vector<uint32_t>& part : L.parts[k]) {
            JPH::Array<JPH::Vec3> points;
            Vec3 plo(1e30f, 1e30f, 1e30f), phi(-1e30f, -1e30f, -1e30f);
            for (const uint32_t i : part) {
                points.push_back(jolt(P[i]));
                for (int a = 0; a < 3; ++a) {
                    plo[a] = std::min(plo[a], P[i][a]);
                    phi[a] = std::max(phi[a], P[i][a]);
                }
            }
            if (points.size() < 4) continue;
            const float least = std::min({phi.x - plo.x, phi.y - plo.y, phi.z - plo.z});
            JPH::ConvexHullShapeSettings hs(points, std::clamp(least * 0.05f, 0.0f, 0.02f));
            hs.SetDensity(density);
            const JPH::ShapeSettings::ShapeResult r = hs.Create();
            if (r.HasError()) continue;  // too flat to be a body
            piece.hulls.push_back(r.Get());
            for (int a = 0; a < 3; ++a) {
                lo[a] = std::min(lo[a], plo[a]);
                hi[a] = std::max(hi[a], phi[a]);
            }
        }
        piece.moves = numberOf(*geo, L, body, "active", 1.0f) != 0.0f;
        piece.release = numberOf(*geo, L, body, "release", 0.0f);
        piece.kick = vectorOf(*geo, L, body, "kick", Vec3());
        piece.vanish = numberOf(*geo, L, body, "vanish", 0.0f) != 0.0f;
        piece.crush = std::max(numberOf(*geo, L, body, "crush", 0.0f), 0.0f);
        piece.glass = numberOf(*geo, L, body, "glass", 0.0f) >= 0.5f;
        piece.size = piece.hulls.empty() ? 0.0f : std::max(length(hi - lo), 0.01f);
        piece.centre = piece.hulls.empty() ? Vec3() : (lo + hi) * 0.5f;
        if (piece.hulls.empty()) piece.gone = true;  // nothing to it
        // What a guide moves it by: its piece, and its points -- how many,
        // where on average, how they spread about that.
        piece.guideWeight = std::clamp(numberOf(*geo, L, body, "guide", 1.0f), 0.0f, 1.0f);
        piece.guidePiece = L.prims[k].empty() ? 0 : pieceOf[L.prims[k].front()];
        {
            std::vector<uint32_t> points;
            for (const std::vector<uint32_t>& part : L.parts[k]) points.insert(points.end(), part.begin(), part.end());
            std::sort(points.begin(), points.end());
            points.erase(std::unique(points.begin(), points.end()), points.end());
            Vec3 sum;
            for (const uint32_t i : points) sum += at[i];
            piece.points = static_cast<float>(points.size());
            piece.middle = points.empty() ? piece.centre : sum * (1.0f / piece.points);
            for (const uint32_t i : points) {
                const Vec3 d = at[i] - piece.middle;
                for (int col = 0; col < 3; ++col) piece.scatter[col] += d * d[col];
            }
        }
        if (meshes && !piece.hulls.empty()) {
            // Its triangles at rest, for the water and the gas.
            Geometry one;
            std::map<uint32_t, uint32_t> local;
            for (const uint32_t prim : L.prims[k]) {
                for (const uint32_t q : geo->primitivePoints(prim)) local.try_emplace(q, static_cast<uint32_t>(local.size()));
            }
            one.addPoints(local.size());
            auto op = one.positionsForWrite();
            for (const auto& [q, i] : local) op[i] = P[q];
            std::vector<uint32_t> c;
            for (const uint32_t prim : L.prims[k]) {
                c.clear();
                for (const uint32_t q : geo->primitivePoints(prim)) c.push_back(local[q]);
                one.addPrimitive(c, geo->primitiveClosed(prim));
            }
            piece.mesh = std::make_shared<MeshShape>(triangulate(one), 16);
        }
        // A piece that does not move: a still body of its own.
        if (!piece.moves && !piece.gone) {
            Impl::Cluster tmp;
            tmp.pieces = {body};
            JPH::BodyCreationSettings bcs(m.shapeOf(tmp), JPH::RVec3::sZero(), JPH::Quat::sIdentity(), JPH::EMotionType::Static,
                                          Layers::still);
            bcs.mFriction = s.friction;
            bcs.mRestitution = s.bounce;
            bcs.mUserData = kStillTag | (static_cast<uint64_t>(k) + 1);
            m.still[k] = bi.CreateAndAddBody(bcs, JPH::EActivation::DontActivate);
        }
    }

    // The glue: where pieces touch face to face, as strong as the faces are
    // big -- or the joints of the network linked in.
    m.edgesOf.resize(count);
    const RigidGlue& glue = *glue_;
    m.jointState.assign(glue.joints.size(), RigidFrame::kJointNone);
    m.jointTime.assign(glue.joints.size(), 0.0f);
    if (s.glue > 0.0f) {
        for (size_t i = 0; i < glue.joints.size(); ++i) {
            const RigidJoint& j = glue.joints[i];
            const Impl::Piece& a = m.pieces[static_cast<size_t>(j.a)];
            const Impl::Piece& b = m.pieces[static_cast<size_t>(j.b)];
            if (a.gone || b.gone || (!a.moves && !b.moves)) continue;
            const float strength = s.glue * j.area * j.strength;
            if (!(strength > 0.0f)) continue;
            Impl::Edge e;
            e.a = j.a;
            e.b = j.b;
            e.at = j.at;
            e.normal = j.normal;
            e.area = j.area;
            e.strength = strength;
            e.joint = i;
            m.edgesOf[static_cast<size_t>(j.a)].push_back(static_cast<int>(m.edges.size()));
            m.edgesOf[static_cast<size_t>(j.b)].push_back(static_cast<int>(m.edges.size()));
            m.edges.push_back(e);
            m.jointState[i] = RigidFrame::kJointHolds;
        }
    }

    // The floor.
    if (s.floor) {
        JPH::BodyCreationSettings floor(new JPH::BoxShape(JPH::Vec3(500.0f, 1.0f, 500.0f)), JPH::RVec3(0.0f, -1.0f, 0.0f),
                                        JPH::Quat::sIdentity(), JPH::EMotionType::Static, Layers::still);
        floor.mFriction = s.friction;
        floor.mRestitution = s.bounce;
        m.floorBody = bi.CreateAndAddBody(floor, JPH::EActivation::DontActivate);
    }
    // The objects: they move as they are keyed, and push what is in the way.
    for (const Collider& c : scene_.colliders) {
        JPH::BodyCreationSettings bcs(shapeOf(c), JPH::RVec3(c.center.x, c.center.y, c.center.z),
                                      quaternionOf(Rotation::fromEuler(c.rotation)), JPH::EMotionType::Kinematic,
                                      Layers::moving);
        bcs.mFriction = s.friction;
        bcs.mRestitution = s.bounce;
        const JPH::BodyID id = bi.CreateAndAddBody(bcs, JPH::EActivation::Activate);
        m.objects.push_back({id, c.node});
    }

    // The bodies: the moving pieces glued together, each group one body --
    // and what the pieces start with moving them.
    UnionFind uf(count);
    for (const Impl::Edge& e : m.edges) {
        if (m.pieces[static_cast<size_t>(e.a)].moves && m.pieces[static_cast<size_t>(e.b)].moves) {
            uf.unite(static_cast<uint32_t>(e.a), static_cast<uint32_t>(e.b));
        }
    }
    std::map<uint32_t, std::vector<int>> groups;
    for (size_t k = 0; k < count; ++k) {
        if (m.pieces[k].moves && !m.pieces[k].gone) groups[uf.find(static_cast<uint32_t>(k))].push_back(static_cast<int>(k));
    }
    for (const auto& [root, group] : groups) {
        // What the first piece says it starts with.
        const Vec3 v = vectorOf(*geo, L, group.front(), "v", Vec3()), w = vectorOf(*geo, L, group.front(), "w", Vec3());
        m.makeCluster(group, JPH::RVec3::sZero(), JPH::Quat::sIdentity(), m.pieces[static_cast<size_t>(group.front())].centre,
                      v, w, m.still);
    }
    // The bars: where each runs through which piece; joints where the pieces
    // they run through are apart from the start.
    if (scene_.rebar && scene_.rebar->primitiveCount() > 0) {
        m.rebar = rigidRebar(*geo, L, *scene_.rebar);
        m.barState.assign(m.rebar->stations.size(), 0);
        m.slid.assign(m.rebar->stations.size(), 0.0f);
        m.longer.assign(m.rebar->stations.size(), 0.0f);
        m.relinkBars();
    }
    // The guide: what it leads -- the pieces that move, as much as they say.
    setGuide(scene_.guide, s.guideStrength);
    for (Impl::Piece& p : m.pieces) p.guided = m.guide && p.moves && !p.gone && p.guideWeight > 0.0f;
    m.physics.OptimizeBroadPhase();
}

RigidSolver::~RigidSolver() = default;

size_t RigidSolver::pieceCount() const { return impl_ ? impl_->pieces.size() : 0; }

void RigidSolver::setColliders(const std::vector<Collider>& colliders) {
    Impl& m = *impl_;
    JPH::BodyInterface& bi = m.physics.GetBodyInterfaceNoLock();
    const float dt = std::max(scene_.solver.timeStep, 1e-5f);
    for (const Collider& c : colliders) {
        for (const Impl::Object& o : m.objects) {
            if (o.node != c.node) continue;
            bi.MoveKinematic(o.body, JPH::RVec3(c.center.x, c.center.y, c.center.z),
                             quaternionOf(Rotation::fromEuler(c.rotation)), dt);
        }
    }
}

void RigidSolver::setGuide(std::shared_ptr<const RigidGuide> guide, float strength) {
    Impl& m = *impl_;
    m.guide = std::move(guide);
    m.guideStrength = std::isfinite(strength) ? std::clamp(strength, 0.0f, 1.0f) : 0.0f;
}

void RigidSolver::step() {
    Impl& m = *impl_;
    if (m.pieces.empty()) return;
    const RigidSettings& s = scene_.solver;
    const int substeps = std::clamp(s.substeps, 1, 16);
    const float dt = std::max(s.timeStep, 1e-5f);
    const float subStep = dt / static_cast<float>(substeps);
    JPH::BodyInterface& bi = m.physics.GetBodyInterfaceNoLock();
    // What was puffed ages.
    for (Impl::Puff& p : m.puffs) ++p.age;
    m.puffs.erase(std::remove_if(m.puffs.begin(), m.puffs.end(), [](const Impl::Puff& p) { return p.age > kPuffLife; }),
                  m.puffs.end());
    std::vector<uint8_t> hit(m.pieces.size(), 0);
    // Takes apart the clusters whose glue broke.
    auto mend = [&]() {
        std::vector<int> torn;
        for (const int k : m.dirty) {
            const int c = m.pieces[static_cast<size_t>(k)].cluster;
            if (c >= 0) torn.push_back(c);
        }
        m.dirty.clear();
        std::sort(torn.begin(), torn.end());
        torn.erase(std::unique(torn.begin(), torn.end()), torn.end());
        for (const int c : torn) m.split(c, hit, m.still);
        if (m.rebar) m.relinkBars();
    };

    // The charges that go off now: their pieces' joints break; a piece blown
    // to dust is gone in a burst of it, the others are kicked.
    std::vector<int> kicked;
    for (size_t k = 0; k < m.pieces.size(); ++k) {
        Impl::Piece& p = m.pieces[k];
        if (p.released || p.gone || p.release <= 0.0f || m.time + 0.5f * dt < p.release) continue;
        p.released = true;
        for (const int ei : m.edgesOf[k]) m.snap(m.edges[static_cast<size_t>(ei)], 2.0f);
        m.dirty.push_back(static_cast<int>(k));
        if (!p.moves) continue;
        const Vec3 middle = m.whereNow(static_cast<int>(k), p.centre);
        const Vec3 v = m.velocityAt(static_cast<int>(k), middle) + p.kick;
        if (p.vanish) {
            p.gone = true;
            m.puff(middle, v * 0.6f, s.dustSize * std::clamp(p.size, 1.0f, 3.0f), 3.0f * s.dust,
                   m.squeezed(p.size, std::max(length(v), 5.0f)));
            m.throwGrit(middle, v, static_cast<int>(std::lround(std::clamp(30.0f * p.size, 8.0f, 60.0f) * s.debris)), 5.0f,
                        0.3f * p.size, p.glass);
        } else {
            kicked.push_back(static_cast<int>(k));
            m.puff(middle, p.kick * 0.5f, s.dustSize * std::clamp(p.size, 1.0f, 3.0f), 2.0f * s.dust);
            m.throwGrit(middle, p.kick, static_cast<int>(std::lround(12.0f * s.debris)), 4.0f, 0.25f * p.size, p.glass);
        }
    }
    mend();
    for (const int k : kicked) {
        const JPH::BodyID id = m.bodyOf(k);
        if (id.IsInvalid()) continue;
        bi.SetLinearVelocity(id, bi.GetLinearVelocity(id) + jolt(m.pieces[static_cast<size_t>(k)].kick));
        bi.ActivateBody(id);
    }
    // The guide steers what it leads -- before the step, so what the step
    // does to them is a knock, and its pull is not.
    m.steer(dt);

    // The step, and how each body moved before it.
    for (Impl::Cluster& c : m.clusters) {
        if (!c.alive) continue;
        c.before = ours(bi.GetLinearVelocity(c.id));
        c.beforeSpin = ours(bi.GetAngularVelocity(c.id));
    }
    m.knocks.clear();
    m.physics.Update(dt, substeps, &m.temp, &m.jobs);
    m.time += dt;
    // The bars take the step: those that gave slid out of their pieces, or
    // tore; their links again, and those again, until all hold.
    if (m.rebar) {
        for (int pass = 0; pass < 64 && m.strain(); ++pass) m.relinkBars();
    }

    // The knocks break the glue where they land. How hard: what it took to
    // change how the body moved -- its momentum now against before, gravity
    // aside -- shared by the places it was knocked at, as a force; at least
    // what it takes to stop the two things one against the other.
    std::vector<int> knocksOn(m.clusters.size(), 0);
    for (const Impl::Knock& k : m.knocks) {
        for (const int c : k.cluster) {
            if (c >= 0) ++knocksOn[static_cast<size_t>(c)];
        }
    }
    std::vector<float> took(m.clusters.size(), 0.0f);
    for (size_t c = 0; c < m.clusters.size(); ++c) {
        const Impl::Cluster& cl = m.clusters[c];
        // A body held by a still piece does not move: how the step moved
        // it -- a keyed object pushing it against its foundation -- is not a
        // knock to share among all that touches it.
        if (!cl.alive || knocksOn[c] == 0 || !cl.anchors.empty()) continue;
        const Vec3 change = ours(bi.GetLinearVelocity(cl.id)) - cl.before - s.gravity * (dt * (1.0f - cl.lift));
        took[c] = cl.mass * length(change) / static_cast<float>(knocksOn[c]);
    }
    std::vector<float> felt(m.pieces.size(), 0.0f);
    for (const Impl::Knock& k : m.knocks) {
        float impulse = k.impulse;
        for (const int c : k.cluster) {
            if (c >= 0) impulse = std::max(impulse, took[static_cast<size_t>(c)]);
        }
        const float force = impulse / subStep;
        for (const int q : k.piece) {
            if (q < 0) continue;
            hit[static_cast<size_t>(q)] = 1;
            m.spread(q, force, felt);
        }
    }
    mend();
    m.settleHeld();
    m.letGo();

    // The knocks: the hardest first, dust and grit where they were.
    std::sort(m.knocks.begin(), m.knocks.end(), [](const Impl::Knock& a, const Impl::Knock& b) {
        if (a.speed != b.speed) return a.speed > b.speed;
        if (a.at.x != b.at.x) return a.at.x < b.at.x;
        if (a.at.y != b.at.y) return a.at.y < b.at.y;
        return a.at.z < b.at.z;
    });
    const size_t knocks = std::min<size_t>(m.knocks.size(), 96);
    for (size_t i = 0; i < knocks; ++i) {
        const Impl::Knock& k = m.knocks[i];
        const float hard = std::clamp((k.speed - kKnockSpeed) / 6.0f, 0.0f, 1.5f);
        const float big = std::clamp(k.size / (2.0f * s.dustSize), 0.0f, 1.0f);  // a pebble puffs little
        if (big < 0.05f) continue;
        bool glass = false;
        for (const int q : k.piece) glass = glass || (q >= 0 && m.pieces[static_cast<size_t>(q)].glass);
        m.puff(k.at, k.velocity * 0.5f, s.dustSize * (0.8f + 0.6f * hard),
               s.impactDust * (0.3f + hard) * big * (glass ? 0.1f : 1.0f), m.squeezed(k.size, k.speed - kKnockSpeed));
        m.throwGrit(k.at, k.velocity * 0.5f + k.normal * (0.5f * k.speed * 0.3f),
                    static_cast<int>(std::lround(s.debris * (2.0f + 6.0f * hard) * big)), 0.4f * k.speed, 0.05f, glass);
    }
    // No more than so many puffs: the oldest go first.
    if (m.puffs.size() > kMaxPuffs) m.puffs.erase(m.puffs.begin(), m.puffs.end() - static_cast<long>(kMaxPuffs));

    // Dust behind what flies off.
    if (s.trail > 0.0f) {
        m.trailDust(dt);
        if (m.puffs.size() > kMaxPuffs) m.puffs.erase(m.puffs.begin(), m.puffs.end() - static_cast<long>(kMaxPuffs));
    }

    // The grit flies, knocks into things, bounces and lies still.
    for (Impl::Grit& q : m.grit) m.moveGrit(q, dt);
    m.grit.erase(std::remove_if(m.grit.begin(), m.grit.end(), [](const Impl::Grit& q) { return q.p.y < -100.0f; }),
                 m.grit.end());
    if (m.grit.size() > kMaxGrit) m.grit.erase(m.grit.begin(), m.grit.end() - static_cast<long>(kMaxGrit));
}

RigidFrame RigidSolver::capture() const {
    RigidFrame f;
    f.pieces = scene_.pieces;
    f.layout = layout_;
    f.attribute = scene_.attribute;
    if (!impl_) return f;
    const Impl& m = *impl_;
    const JPH::BodyInterface& bi = m.physics.GetBodyInterfaceNoLock();
    f.poses.reserve(m.pieces.size());
    for (size_t k = 0; k < m.pieces.size(); ++k) {
        if (m.pieces[k].gone && !m.pieces[k].hulls.empty()) f.vanished.push_back(static_cast<uint32_t>(k));
        RigidPose pose;
        const JPH::BodyID id = m.bodyOf(static_cast<int>(k));
        if (!id.IsInvalid()) {
            const JPH::Quat q = bi.GetRotation(id);
            pose.position = ours(bi.GetPosition(id));
            pose.rotation = Vec4(q.GetX(), q.GetY(), q.GetZ(), q.GetW());
            // The velocity of the centre of mass, carried to the point at `position`.
            pose.spin = ours(bi.GetAngularVelocity(id));
            pose.velocity = ours(bi.GetLinearVelocity(id)) + cross(pose.spin, pose.position - ours(bi.GetCenterOfMassPosition(id)));
        }
        f.poses.push_back(pose);
    }
    f.debris.reserve(m.grit.size() * 4);
    f.debrisVelocity.reserve(m.grit.size() * 3);
    f.debrisIds.reserve(m.grit.size());
    const bool glassy = std::any_of(m.grit.begin(), m.grit.end(), [](const Impl::Grit& q) { return q.glass; });
    f.debrisOrient.reserve(m.grit.size() * 4);
    for (const Impl::Grit& q : m.grit) {
        f.debris.insert(f.debris.end(), {q.p.x, q.p.y, q.p.z, q.size});
        f.debrisVelocity.insert(f.debrisVelocity.end(), {q.v.x, q.v.y, q.v.z});
        f.debrisIds.push_back(q.id);
        f.debrisOrient.insert(f.debrisOrient.end(), {q.turn.GetX(), q.turn.GetY(), q.turn.GetZ(), q.turn.GetW()});
        if (glassy) f.debrisGlass.push_back(q.glass ? 1 : 0);
    }
    std::vector<uint8_t> loose(m.pieces.size(), 0);
    for (const Impl::Edge& e : m.edges) {
        if (e.broken) loose[static_cast<size_t>(e.a)] = loose[static_cast<size_t>(e.b)] = 1;
    }
    for (size_t k = 0; k < loose.size(); ++k) {
        if (loose[k]) f.unglued.push_back(static_cast<uint32_t>(k));
    }
    f.joints = m.edges.size();
    f.broken = m.broken;
    f.glue = glue_;
    f.jointState = m.jointState;
    f.jointTime = m.jointTime;
    f.rebar = m.rebar;
    f.rebarState = m.barState;
    return f;
}

std::vector<Collider> RigidSolver::colliders() const {
    std::vector<Collider> out;
    if (!impl_) return out;
    Impl& m = *impl_;
    const JPH::BodyInterface& bi = m.physics.GetBodyInterfaceNoLock();
    for (size_t k = 0; k < m.pieces.size(); ++k) {
        Impl::Piece& p = m.pieces[k];
        const JPH::BodyID id = m.bodyOf(static_cast<int>(k));
        if (id.IsInvalid() || !p.mesh) continue;
        const Rotation r = rotationOf(bi.GetRotation(id));
        const Vec3 position = ours(bi.GetPosition(id));
        Collider c;
        c.shape = Shape::Mesh;
        c.mesh = p.mesh;
        c.size = p.mesh->half() * 2.0f;
        // Where the middle of the mesh's box has gone, and the turn about it.
        c.center = position + r.apply(p.mesh->center());
        p.euler = r.toEuler(p.euler);
        c.rotation = p.euler;
        const Vec3 v = ours(bi.GetLinearVelocity(id)), w = ours(bi.GetAngularVelocity(id));
        c.velocity = clampLength(v + cross(w, c.center - ours(bi.GetCenterOfMassPosition(id))), kMaxSpeed);
        c.spin = clampLength(w, kMaxSpin);
        c.node = scene_.node;
        out.push_back(std::move(c));
    }
    return out;
}

std::vector<RigidDust> RigidSolver::dust() const {
    std::vector<RigidDust> out;
    if (!impl_) return out;
    for (const Impl::Puff& p : impl_->puffs) {
        const float left = 1.0f - static_cast<float>(p.age) / static_cast<float>(kPuffLife + 1);
        // The air it pushes out as the swelling of a ball its size: m^3/s
        // over m^3.
        const float ball = 0.5236f * p.size * p.size * p.size;
        const float swell = std::min(p.air * left / std::max(ball, 1e-6f), kMaxSwell);
        out.push_back({p.at + p.velocity * (scene_.solver.timeStep * static_cast<float>(p.age)), p.velocity, p.size,
                       p.amount * left, swell});
    }
    return out;
}

#else  // no Jolt

bool rigidAvailable() { return false; }

struct RigidSolver::Impl {};

RigidSolver::RigidSolver(const RigidScene& scene) : scene_(scene) {
    layout_ = scene_.pieces ? rigidLayout(*scene_.pieces, scene_.attribute) : std::make_shared<RigidLayout>();
    glue_ = scene_.pieces ? rigidGlue(*scene_.pieces, *layout_, scene_.attribute, scene_.constraints.get())
                          : std::make_shared<RigidGlue>();
    error_ = "this build has no rigid bodies: it was built without Jolt (PG_WITH_JOLT=OFF)";
}
RigidSolver::~RigidSolver() = default;
size_t RigidSolver::pieceCount() const { return 0; }
void RigidSolver::setColliders(const std::vector<Collider>&) {}
void RigidSolver::setGuide(std::shared_ptr<const RigidGuide>, float) {}
void RigidSolver::step() {}
RigidFrame RigidSolver::capture() const {
    RigidFrame f;
    f.pieces = scene_.pieces;
    f.layout = layout_;
    f.attribute = scene_.attribute;
    f.glue = glue_;
    return f;
}
std::vector<Collider> RigidSolver::colliders() const { return {}; }
std::vector<RigidDust> RigidSolver::dust() const { return {}; }

#endif

}  // namespace pg::sim
