#include "pg/sim/Rigid.h"

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
#include <Jolt/Physics/Collision/ObjectLayerPairFilterTable.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/CylinderShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/Body/BodyLockMulti.h>
#include <Jolt/Physics/Constraints/FixedConstraint.h>
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
    for (int a = 0; a < 3; ++a) s.gravity[a] = std::clamp(finite(s.gravity[a], 0.0f), -1000.0f, 1000.0f);
    s.substeps = std::clamp(s.substeps, 1, 16);
    s.dust = std::clamp(finite(s.dust, d.dust), 0.0f, 1000.0f);
    s.impactDust = std::clamp(finite(s.impactDust, d.impactDust), 0.0f, 1000.0f);
    s.dustSize = std::clamp(finite(s.dustSize, d.dustSize), 0.01f, 100.0f);
    s.debris = std::clamp(finite(s.debris, d.debris), 0.0f, 100.0f);
    s.air = std::clamp(finite(s.air, d.air), 0.0f, 100.0f);
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

}  // namespace

std::shared_ptr<const RigidLayout> rigidLayout(const Geometry& geo, const std::string& attribute) {
    auto out = std::make_shared<RigidLayout>();
    RigidLayout& L = *out;
    const size_t nprims = geo.primitiveCount();
    L.bodyOf.assign(nprims, -1);
    if (nprims == 0) return out;
    int pieceCount = 0;
    const std::vector<int32_t> piece = pieceOfPrimitives(geo, attribute, pieceCount);
    const auto P = geo.positions();

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

    // The faces, part by part.
    std::vector<Face> faces;
    std::vector<uint32_t> firstFace(parts.size() + 1, 0);
    for (size_t k = 0; k < parts.size(); ++k) {
        firstFace[k] = static_cast<uint32_t>(faces.size());
        for (const uint32_t p : parts[k].prims) {
            const auto c = geo.primitivePoints(p);
            if (c.size() < 3 || !geo.primitiveClosed(p)) continue;
            Face f;
            f.part = static_cast<uint32_t>(k);
            Vec3 n, mid;
            f.lo = Vec3(1e30f, 1e30f, 1e30f);
            f.hi = Vec3(-1e30f, -1e30f, -1e30f);
            for (size_t i = 0; i < c.size(); ++i) {
                const Vec3& a = P[c[i]];
                const Vec3& b = P[c[(i + 1) % c.size()]];
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
            f.d = static_cast<double>(dot(f.normal, mid * (1.0f / static_cast<float>(c.size()))));
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
    for (size_t oi = 0; oi < order.size(); ++oi) {
        const uint32_t i = order[oi];
        for (size_t oj = oi + 1; oj < order.size(); ++oj) {
            const uint32_t j = order[oj];
            if (parts[j].lo.x > parts[i].hi.x + eps) break;
            if (!boxesMeet(parts[i].lo, parts[i].hi, parts[j].lo, parts[j].hi)) continue;
            const uint32_t a = std::min(i, j), b = std::max(i, j);
            Touch t;
            for (uint32_t fa = firstFace[a]; fa < firstFace[a + 1]; ++fa) {
                const Face& f = faces[fa];
                for (uint32_t fb = firstFace[b]; fb < firstFace[b + 1]; ++fb) {
                    const Face& g = faces[fb];
                    if (dot(f.normal, g.normal) > -0.9995f) continue;
                    if (std::fabs(f.d + g.d) > static_cast<double>(eps)) continue;
                    if (!boxesMeet(f.lo, f.hi, g.lo, g.hi)) continue;
                    Vec3 sum;
                    const double area = overlapArea(f, g, sum);
                    if (area <= 0.0) continue;
                    t.area += area;
                    t.sum += sum;
                    t.normal += f.normal * static_cast<float>(area);
                }
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

// --- Posed, and drawn ------------------------------------------------------------------

namespace {

std::shared_ptr<const RigidLayout> layoutOf(const RigidFrame& f, const Geometry& geo) {
    if (f.layout && f.layout->bodyOf.size() == geo.primitiveCount()) return f.layout;
    return rigidLayout(geo, f.attribute);
}

}  // namespace

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
    if (!f.vanished.empty()) {
        std::vector<uint8_t> keep(geo->primitiveCount(), 1);
        for (size_t p = 0; p < keep.size(); ++p) {
            keep[p] = !std::binary_search(f.vanished.begin(), f.vanished.end(), static_cast<uint32_t>(layout->bodyOf[p]));
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

}  // namespace

std::shared_ptr<Geometry> drawnPieces(const RigidFrame& f, const Vec3& color, const Vec3& inside,
                                      const std::string& insideGroup) {
    std::shared_ptr<Geometry> geo = posedPieces(f);
    const size_t nprims = geo->primitiveCount();
    const Group* cut = insideGroup.empty() ? nullptr : geo->findGroup(insideGroup);
    if (cut && cut->classOf() != AttrClass::Primitive) cut = nullptr;
    // The colour of every corner -- the corner's own, the point's, the
    // face's, the whole's -- so that a cut face can differ from the face
    // beside it, and the grit's points can have a colour of their own.
    const AttributeArray* vertexCd = geo->vertices().find("Cd");
    const AttributeArray* pointCd = geo->points().find("Cd");
    const AttributeArray* primCd = geo->primitives().find("Cd");
    const AttributeArray* detailCd = geo->detail().find("Cd");
    std::vector<Vec3> corners(geo->vertexCount(), color);
    for (size_t p = 0; p < nprims; ++p) {
        const auto c = geo->primitivePoints(p);
        const size_t start = geo->primitiveVertexStart(p);
        const bool isCut = cut && cut->contains(p);
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
    // The grit: loose points, as big as it is, in the colour of a cut.
    const size_t first = appendGrit(*geo, f);
    if (geo->pointCount() > first) {
        auto pc = geo->points().create("Cd", AttrType::Vec3).write<Vec3>();
        for (size_t i = first; i < pc.size(); ++i) pc[i] = inside * 0.9f;
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
/// How much of a knock goes on from a piece to the pieces glued to it.
constexpr float kSpread = 0.5f;
/// How much of the speed it had before a knock a body keeps when the glue
/// under it broke: it was crushing what broke off, not standing on it.
constexpr float kCarry = 0.985f;
/// Marks the user data of a still piece's body, above its number.
constexpr uint64_t kStillTag = 1ull << 62;

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
        bool moves = true;                      ///< active: else a still body of its own
        int cluster = -1;                       ///< the body it is in; -1: gone, or none
        std::shared_ptr<const MeshShape> mesh;  ///< at rest, for the water and the gas; null if not wanted
        Vec3 euler;                             ///< the last angles given out: the next near them
        float release = 0.0f;                   ///< seconds; 0 or less: never
        Vec3 kick;
        bool vanish = false;                    ///< blown to dust when released
        float crush = 0.0f;                     ///< crushed to dust by a knock this many times its glue; 0: never
        bool released = false, gone = false;
        float size = 0.1f;                      ///< how big it is across, metres
        Vec3 centre;                            ///< the middle of its box, at rest
    };
    std::vector<Piece> pieces;
    std::vector<JPH::BodyID> still;             ///< each still piece's body
    std::vector<int> dirty;                     ///< pieces whose glue broke: their clusters come apart
    struct Edge {
        int a = 0, b = 0;      ///< the pieces it glues
        Vec3 at;               ///< where their faces touch, at rest
        float area = 0.0f;
        float strength = 0.0f; ///< newtons it holds
        bool broken = false;
    };
    std::vector<Edge> edges;
    std::vector<std::vector<int>> edgesOf;  ///< each piece's
    size_t broken = 0;
    struct Cluster {
        JPH::BodyID id;
        std::vector<int> pieces;       ///< in order
        std::vector<int> subPiece;     ///< for each sub-shape of its compound, the piece
        std::vector<JPH::Ref<JPH::Constraint>> anchors;  ///< to the still pieces it is glued to
        bool alive = false;
        float mass = 0.0f;             ///< kilograms
        Vec3 before, beforeSpin;       ///< its velocity before the step, at its centre of mass
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

    /// `count` bits of grit from `at`, flying off at about `speed` round `base`.
    void throwGrit(const Vec3& at, const Vec3& base, int count, float speed, float spread) {
        for (int i = 0; i < count; ++i) {
            Grit g;
            g.p = at + inBall() * spread;
            g.v = base + inBall() * speed;
            g.size = (0.02f + 0.08f * unit() * unit()) * gritScale;
            g.id = gritThrown++;
            grit.push_back(g);
        }
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
        dirty.push_back(e.a);
        dirty.push_back(e.b);
        const int k = pieces[static_cast<size_t>(e.a)].cluster >= 0 ? e.a : e.b;
        const Vec3 at = whereNow(k, e.at);
        const Vec3 v = velocityAt(k, at);
        const RigidSettings& s = settings;
        const float size = s.dustSize * std::clamp(std::sqrt(e.area) * 1.5f, 0.6f, 2.5f);
        puff(at, v * 0.5f, size, s.dust * dustScale * std::clamp(e.area * 4.0f, 0.3f, 2.0f));
        const int count = static_cast<int>(std::lround(s.debris * std::clamp(e.area * 24.0f, 2.0f, 10.0f)));
        throwGrit(at, v, count, 2.5f, std::sqrt(e.area) * 0.3f);
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
        puff(middle, v * 0.5f, s.dustSize * std::clamp(p.size, 1.0f, 3.0f), 2.0f * s.impactDust,
             squeezed(p.size, length(v)));
        throwGrit(middle, v, static_cast<int>(std::lround(std::clamp(16.0f * p.size, 4.0f, 30.0f) * s.debris)), 3.0f,
                  0.3f * p.size);
    }

    /// A knock of `force` newtons on piece `k`: the joints it cannot hold
    /// break; half of it goes on through those that hold, and so on.
    void spread(int k, float force, std::vector<float>& felt) {
        std::vector<std::pair<int, float>> queue{{k, force}};
        for (size_t at = 0; at < queue.size(); ++at) {
            const auto [q, f] = queue[at];
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
                // What broke it went on through it -- half of it.
                const float on = f * kSpread;
                if (on > felt[static_cast<size_t>(other)] && on > 1e-3f * e.strength) queue.push_back({other, on});
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
        bi.RemoveBody(old.id);
        bi.DestroyBody(old.id);
        old.alive = false;
        const Vec3 before = old.before, beforeSpin = old.beforeSpin;
        for (const int k : old.pieces) pieces[static_cast<size_t>(k)].cluster = -1;
        for (const auto& [root, group] : groups) {
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
};

RigidSolver::RigidSolver(const RigidScene& scene) : scene_(scene) {
    startJolt();
    impl_ = std::make_unique<Impl>();
    Impl& m = *impl_;
    const RigidSettings& s = scene_.solver;
    m.settings = s;
    const Geometry* geo = scene_.pieces.get();
    layout_ = geo ? rigidLayout(*geo, scene_.attribute) : std::make_shared<RigidLayout>();
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

    // The pieces: the hull of each part, where it rests; what their
    // attributes say.
    const auto P = geo->positions();
    const bool meshes = scene_.intoGas || scene_.intoWater || scene_.intoRain;
    m.pieces.resize(count);
    m.still.assign(count, JPH::BodyID());
    std::vector<float> glueOf(count, 1.0f);
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
        piece.size = piece.hulls.empty() ? 0.0f : std::max(length(hi - lo), 0.01f);
        piece.centre = piece.hulls.empty() ? Vec3() : (lo + hi) * 0.5f;
        glueOf[k] = std::max(numberOf(*geo, L, body, "glue", 1.0f), 0.0f);
        if (piece.hulls.empty()) piece.gone = true;  // nothing to it
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

    // The glue: where pieces touch face to face, as strong as the faces are big.
    m.edgesOf.resize(count);
    if (s.glue > 0.0f) {
        for (const RigidLayout::Contact& c : L.contacts) {
            const Impl::Piece& a = m.pieces[static_cast<size_t>(c.a)];
            const Impl::Piece& b = m.pieces[static_cast<size_t>(c.b)];
            if (a.gone || b.gone || (!a.moves && !b.moves)) continue;
            const float strength = s.glue * c.area * std::min(glueOf[static_cast<size_t>(c.a)], glueOf[static_cast<size_t>(c.b)]);
            if (strength <= 0.0f) continue;
            Impl::Edge e;
            e.a = c.a;
            e.b = c.b;
            e.at = c.at;
            e.area = c.area;
            e.strength = strength;
            m.edgesOf[static_cast<size_t>(c.a)].push_back(static_cast<int>(m.edges.size()));
            m.edgesOf[static_cast<size_t>(c.b)].push_back(static_cast<int>(m.edges.size()));
            m.edges.push_back(e);
        }
    }

    // The floor.
    if (s.floor) {
        JPH::BodyCreationSettings floor(new JPH::BoxShape(JPH::Vec3(500.0f, 1.0f, 500.0f)), JPH::RVec3(0.0f, -1.0f, 0.0f),
                                        JPH::Quat::sIdentity(), JPH::EMotionType::Static, Layers::still);
        floor.mFriction = s.friction;
        floor.mRestitution = s.bounce;
        bi.CreateAndAddBody(floor, JPH::EActivation::DontActivate);
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
                        0.3f * p.size);
        } else {
            kicked.push_back(static_cast<int>(k));
            m.puff(middle, p.kick * 0.5f, s.dustSize * std::clamp(p.size, 1.0f, 3.0f), 2.0f * s.dust);
            m.throwGrit(middle, p.kick, static_cast<int>(std::lround(12.0f * s.debris)), 4.0f, 0.25f * p.size);
        }
    }
    mend();
    for (const int k : kicked) {
        const JPH::BodyID id = m.bodyOf(k);
        if (id.IsInvalid()) continue;
        bi.SetLinearVelocity(id, bi.GetLinearVelocity(id) + jolt(m.pieces[static_cast<size_t>(k)].kick));
        bi.ActivateBody(id);
    }

    // The step, and how each body moved before it.
    for (Impl::Cluster& c : m.clusters) {
        if (!c.alive) continue;
        c.before = ours(bi.GetLinearVelocity(c.id));
        c.beforeSpin = ours(bi.GetAngularVelocity(c.id));
    }
    m.knocks.clear();
    m.physics.Update(dt, substeps, &m.temp, &m.jobs);
    m.time += dt;

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
        if (!cl.alive || knocksOn[c] == 0) continue;
        const Vec3 change = ours(bi.GetLinearVelocity(cl.id)) - cl.before - s.gravity * dt;
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
        m.puff(k.at, k.velocity * 0.5f, s.dustSize * (0.8f + 0.6f * hard), s.impactDust * (0.3f + hard) * big,
               m.squeezed(k.size, k.speed - kKnockSpeed));
        m.throwGrit(k.at, k.velocity * 0.5f + k.normal * (0.5f * k.speed * 0.3f),
                    static_cast<int>(std::lround(s.debris * (2.0f + 6.0f * hard) * big)), 0.4f * k.speed, 0.05f);
    }
    // No more than so many puffs: the oldest go first.
    if (m.puffs.size() > kMaxPuffs) m.puffs.erase(m.puffs.begin(), m.puffs.end() - static_cast<long>(kMaxPuffs));

    // The grit flies, lands, bounces and lies still.
    const Vec3 g = s.gravity;
    for (Impl::Grit& q : m.grit) {
        if (q.resting) continue;
        q.v += g * dt;
        q.p += q.v * dt;
        const float r = 0.5f * q.size;
        if (s.floor && q.p.y < r) {
            q.p.y = r;
            if (q.v.y < 0.0f) q.v.y = -0.25f * q.v.y;
            q.v.x *= 0.55f;
            q.v.z *= 0.55f;
            if (length(q.v) < 0.35f) {
                q.v = Vec3();
                q.resting = true;
            }
        }
    }
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
    for (const Impl::Grit& q : m.grit) {
        f.debris.insert(f.debris.end(), {q.p.x, q.p.y, q.p.z, q.size});
        f.debrisVelocity.insert(f.debrisVelocity.end(), {q.v.x, q.v.y, q.v.z});
        f.debrisIds.push_back(q.id);
    }
    f.joints = m.edges.size();
    f.broken = m.broken;
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
    error_ = "this build has no rigid bodies: it was built without Jolt (PG_WITH_JOLT=OFF)";
}
RigidSolver::~RigidSolver() = default;
size_t RigidSolver::pieceCount() const { return 0; }
void RigidSolver::setColliders(const std::vector<Collider>&) {}
void RigidSolver::step() {}
RigidFrame RigidSolver::capture() const {
    RigidFrame f;
    f.pieces = scene_.pieces;
    f.layout = layout_;
    f.attribute = scene_.attribute;
    return f;
}
std::vector<Collider> RigidSolver::colliders() const { return {}; }
std::vector<RigidDust> RigidSolver::dust() const { return {}; }

#endif

}  // namespace pg::sim
