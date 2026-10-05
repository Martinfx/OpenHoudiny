#include "pg/sim/Cloth.h"

#include "pg/core/Half.h"
#include "pg/core/Parallel.h"
#include "pg/sim/Shared.h"
#include "pg/sim/State.h"

#include <algorithm>
#include <atomic>
#include <array>
#include <bit>
#include <cmath>
#include <map>
#include <mutex>
#include <numeric>
#include <utility>

namespace pg::sim {

namespace {

/// The air: how heavy a cubic metre is, kg, and how hard it pushes a plate
/// that faces it (its drag coefficient).
constexpr float kAirDensity = 1.2f;
constexpr float kPlateDrag = 1.2f;
/// The most colours the links are sorted into; those that fit none are
/// solved one after another.
constexpr int kColours = 64;

/// A point attribute as a number: Float or Int; `fallback` without it.
float pointNumber(const AttributeArray* a, size_t i, float fallback) {
    if (!a || i >= a->size()) return fallback;
    if (a->type() == AttrType::Float) return a->read<float>()[i];
    if (a->type() == AttrType::Int) return static_cast<float>(a->read<int32_t>()[i]);
    return fallback;
}

uint64_t pairKey(uint32_t a, uint32_t b) {
    if (a > b) std::swap(a, b);
    return (static_cast<uint64_t>(a) << 32) | b;
}

/// Six times the signed volume of the tetrahedron of the origin and a triangle.
double tripleProduct(const Vec3& a, const Vec3& b, const Vec3& c) {
    return static_cast<double>(dot(a, cross(b, c)));
}

/// The point of triangle abc nearest p, and how much of each corner it is
/// (Ericson, Real-Time Collision Detection, 5.1.5).
Vec3 nearestOnTriangle(const Vec3& p, const Vec3& a, const Vec3& b, const Vec3& c, Vec3& weights) {
    const Vec3 ab = b - a, ac = c - a, ap = p - a;
    const float d1 = dot(ab, ap), d2 = dot(ac, ap);
    if (d1 <= 0.0f && d2 <= 0.0f) {
        weights = Vec3(1.0f, 0.0f, 0.0f);
        return a;
    }
    const Vec3 bp = p - b;
    const float d3 = dot(ab, bp), d4 = dot(ac, bp);
    if (d3 >= 0.0f && d4 <= d3) {
        weights = Vec3(0.0f, 1.0f, 0.0f);
        return b;
    }
    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
        const float v = d1 / (d1 - d3);
        weights = Vec3(1.0f - v, v, 0.0f);
        return a + ab * v;
    }
    const Vec3 cp = p - c;
    const float d5 = dot(ab, cp), d6 = dot(ac, cp);
    if (d6 >= 0.0f && d5 <= d6) {
        weights = Vec3(0.0f, 0.0f, 1.0f);
        return c;
    }
    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
        const float w = d2 / (d2 - d6);
        weights = Vec3(1.0f - w, 0.0f, w);
        return a + ac * w;
    }
    const float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && d4 - d3 >= 0.0f && d5 - d6 >= 0.0f) {
        const float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        weights = Vec3(0.0f, 1.0f - w, w);
        return b + (c - b) * w;
    }
    const float denom = 1.0f / (va + vb + vc);
    const float v = vb * denom, w = vc * denom;
    weights = Vec3(1.0f - v - w, v, w);
    return a + ab * v + ac * w;
}

/// How far along segments p1q1 and p2q2 their nearest points are, 0 to 1
/// (Ericson, 5.1.9); parallel ones at the start of the first.
void nearestOnSegments(const Vec3& p1, const Vec3& q1, const Vec3& p2, const Vec3& q2, float& s, float& t) {
    const Vec3 d1 = q1 - p1, d2 = q2 - p2, r = p1 - p2;
    const float a = dot(d1, d1), e = dot(d2, d2), f = dot(d2, r);
    constexpr float kTiny = 1e-14f;
    if (a <= kTiny && e <= kTiny) {
        s = t = 0.0f;
        return;
    }
    if (a <= kTiny) {
        s = 0.0f;
        t = std::clamp(f / e, 0.0f, 1.0f);
        return;
    }
    const float c = dot(d1, r);
    if (e <= kTiny) {
        t = 0.0f;
        s = std::clamp(-c / a, 0.0f, 1.0f);
        return;
    }
    const float b = dot(d1, d2), denom = a * e - b * b;
    s = denom > 1e-7f * a * e ? std::clamp((b * f - c * e) / denom, 0.0f, 1.0f) : 0.0f;
    t = (b * s + f) / e;
    if (t < 0.0f) {
        t = 0.0f;
        s = std::clamp(-c / a, 0.0f, 1.0f);
    } else if (t > 1.0f) {
        t = 1.0f;
        s = std::clamp((b - c) / a, 0.0f, 1.0f);
    }
}

/// The rotation that best turns points about their middle at rest onto
/// where they are about theirs, `s` the sum of each one's mass times its
/// place at rest times its place now (s[a][b]: at rest along a, now along
/// b): Horn's quaternion (1987) -- the eigenvector of the largest eigenvalue
/// of his 4 x 4 matrix, found by Jacobi's rotations. Whole, flat or along a
/// line, as unique as the points make it; none for points all in one place.
Mat3 bestTurn(const double s[3][3]) {
    const double xx = s[0][0], xy = s[0][1], xz = s[0][2];
    const double yx = s[1][0], yy = s[1][1], yz = s[1][2];
    const double zx = s[2][0], zy = s[2][1], zz = s[2][2];
    double a[4][4] = {{xx + yy + zz, yz - zy, zx - xz, xy - yx},
                      {yz - zy, xx - yy - zz, xy + yx, zx + xz},
                      {zx - xz, xy + yx, -xx + yy - zz, yz + zy},
                      {xy - yx, zx + xz, yz + zy, -xx - yy + zz}};
    double v[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
    for (int sweep = 0; sweep < 32; ++sweep) {
        double off = 0.0, diagonal = 0.0;
        for (int i = 0; i < 4; ++i) {
            diagonal += a[i][i] * a[i][i];
            for (int j = i + 1; j < 4; ++j) off += a[i][j] * a[i][j];
        }
        if (off <= 1e-26 * diagonal || off == 0.0) break;
        for (int p = 0; p < 3; ++p) {
            for (int q = p + 1; q < 4; ++q) {
                if (a[p][q] == 0.0) continue;
                const double theta = (a[q][q] - a[p][p]) / (2.0 * a[p][q]);
                const double t = (theta >= 0.0 ? 1.0 : -1.0) / (std::fabs(theta) + std::sqrt(theta * theta + 1.0));
                const double c = 1.0 / std::sqrt(t * t + 1.0), sn = t * c;
                for (int k = 0; k < 4; ++k) {
                    const double kp = a[k][p], kq = a[k][q];
                    a[k][p] = c * kp - sn * kq;
                    a[k][q] = sn * kp + c * kq;
                }
                for (int k = 0; k < 4; ++k) {
                    const double pk = a[p][k], qk = a[q][k];
                    a[p][k] = c * pk - sn * qk;
                    a[q][k] = sn * pk + c * qk;
                }
                for (int k = 0; k < 4; ++k) {
                    const double kp = v[k][p], kq = v[k][q];
                    v[k][p] = c * kp - sn * kq;
                    v[k][q] = sn * kp + c * kq;
                }
            }
        }
    }
    int best = 0;
    for (int k = 1; k < 4; ++k) {
        if (a[k][k] > a[best][best]) best = k;
    }
    double w = v[0][best], x = v[1][best], y = v[2][best], z = v[3][best];
    const double norm = std::sqrt(w * w + x * x + y * y + z * z);
    if (!(norm > 0.0)) return Mat3(1.0f);
    w /= norm, x /= norm, y /= norm, z /= norm;
    // The quaternion's matrix, column by column.
    return Mat3(Vec3(static_cast<float>(1.0 - 2.0 * (y * y + z * z)), static_cast<float>(2.0 * (x * y + w * z)),
                     static_cast<float>(2.0 * (x * z - w * y))),
                Vec3(static_cast<float>(2.0 * (x * y - w * z)), static_cast<float>(1.0 - 2.0 * (x * x + z * z)),
                     static_cast<float>(2.0 * (y * z + w * x))),
                Vec3(static_cast<float>(2.0 * (x * z + w * y)), static_cast<float>(2.0 * (y * z - w * x)),
                     static_cast<float>(1.0 - 2.0 * (x * x + y * y))));
}

bool overlaps(const Vec3& lo0, const Vec3& hi0, const Vec3& lo1, const Vec3& hi1) {
    return lo0.x <= hi1.x && lo1.x <= hi0.x && lo0.y <= hi1.y && lo1.y <= hi0.y && lo0.z <= hi1.z && lo1.z <= hi0.z;
}

/// Points kept apart: up to four, each with its share of the way between
/// the two sides -- a point and the weights of a triangle's corners with
/// the sign turned, or an edge's ends and the other edge's -- pushed apart
/// along `normal` until they are `gap` apart.
struct Contact {
    uint64_t order = 0;  // which pair: the same order on any number of threads
    std::array<uint32_t, 4> points{};
    std::array<float, 4> shares{};
    Vec3 normal;
    float gap = 0.0f;
};

}  // namespace

ClothScene ClothScene::sanitized() const {
    ClothScene c = *this;
    ClothSettings& s = c.solver;
    const ClothSettings d;
    auto finite = [](float v, float fallback) { return std::isfinite(v) ? v : fallback; };
    s.density = std::clamp(finite(s.density, d.density), 1e-4f, 1e4f);
    s.stretch = std::clamp(finite(s.stretch, d.stretch), 1e-2f, 1e9f);
    s.shear = std::clamp(finite(s.shear, d.shear), 1e-2f, 1e9f);
    s.bend = std::clamp(finite(s.bend, d.bend), 0.0f, 1e9f);
    s.pressure = std::clamp(finite(s.pressure, d.pressure), 0.0f, 100.0f);
    s.thickness = std::clamp(finite(s.thickness, d.thickness), 1e-4f, 1.0f);
    s.friction = std::clamp(finite(s.friction, d.friction), 0.0f, 10.0f);
    s.damping = std::clamp(finite(s.damping, d.damping), 0.0f, 1000.0f);
    s.airDrag = std::clamp(finite(s.airDrag, d.airDrag), 0.0f, 100.0f);
    s.tear = std::clamp(finite(s.tear, d.tear), 0.0f, 100.0f);
    s.shape = std::clamp(finite(s.shape, d.shape), 0.0f, 1e9f);
    s.plasticity = std::clamp(finite(s.plasticity, d.plasticity), 0.0f, 1.0f);
    s.yield = std::clamp(finite(s.yield, d.yield), 0.0f, 100.0f);
    s.substeps = std::clamp(s.substeps, 1, 200);
    for (int a = 0; a < 3; ++a) s.gravity[a] = std::clamp(finite(s.gravity[a], 0.0f), -1000.0f, 1000.0f);
    s.timeStep = std::clamp(finite(s.timeStep, d.timeStep), 1e-4f, 1.0f);
    detail::sanitize(c.colliders);
    return c;
}

// --- the frame, drawn ------------------------------------------------------------------

bool clothFits(const ClothFrame& f, const Geometry& geo) {
    const size_t n0 = geo.pointCount(), n = f.positions.size();
    if (n != n0 + f.copies.size()) return false;
    if (!f.corners.empty() && f.corners.size() != geo.vertexCount()) return false;
    if (f.corners.empty() && !f.copies.empty()) return false;
    for (const uint32_t o : f.copies) {
        if (o >= n0) return false;
    }
    for (const uint32_t c : f.corners) {
        if (c >= n) return false;
    }
    for (const uint32_t v : f.cuts) {
        if (v >= geo.vertexCount()) return false;
    }
    return true;
}

namespace {

/// The geometry `geo` torn as `f` has it: the points split off, with their
/// own's attributes; each corner on its point; each line parted into as
/// many as its cuts make, each with its primitive's attributes.
std::shared_ptr<Geometry> tornApart(const Geometry& geo, const ClothFrame& f) {
    const size_t n0 = geo.pointCount(), n = n0 + f.copies.size();
    std::vector<uint32_t> from(n);
    std::iota(from.begin(), from.begin() + static_cast<std::ptrdiff_t>(n0), 0u);
    std::copy(f.copies.begin(), f.copies.end(), from.begin() + static_cast<std::ptrdiff_t>(n0));
    std::vector<uint8_t> cut(geo.vertexCount(), 0);
    for (const uint32_t v : f.cuts) cut[v] = 1;
    auto g = std::make_shared<Geometry>();
    g->addPoints(n);
    // The primitives, lines parted where they are cut; the corners in the
    // order they were, so each keeps its attributes.
    std::vector<uint32_t> primOf;  // each primitive: the geometry's it is part of
    std::vector<uint32_t> corners;
    for (size_t p = 0; p < geo.primitiveCount(); ++p) {
        const size_t first = geo.primitiveVertexStart(p), count = geo.primitiveVertexCount(p);
        const bool closed = geo.primitiveClosed(p);
        corners.clear();
        for (size_t v = first; v < first + count; ++v) {
            corners.push_back(f.corners.empty() ? geo.vertexPoint(v) : f.corners[v]);
            if (!closed && (cut[v] || v + 1 == first + count)) {
                g->addPrimitive(corners, false);
                primOf.push_back(static_cast<uint32_t>(p));
                corners.clear();
            }
        }
        if (closed) {
            g->addPrimitive(corners, true);
            primOf.push_back(static_cast<uint32_t>(p));
        }
    }
    AttributeSet points = geo.points();
    points.gather(from);
    g->points() = std::move(points);
    g->vertices() = geo.vertices();
    AttributeSet prims = geo.primitives();
    prims.gather(primOf);
    g->primitives() = std::move(prims);
    g->detail() = geo.detail();
    for (const std::string& name : geo.groupNames()) {
        const Group* group = geo.findGroup(name);
        if (group->classOf() == AttrClass::Point) {
            Group& to = g->createGroup(name, AttrClass::Point);
            for (size_t i = 0; i < n; ++i) to.set(i, group->contains(from[i]));
        } else if (group->classOf() == AttrClass::Primitive) {
            Group& to = g->createGroup(name, AttrClass::Primitive);
            for (size_t p = 0; p < primOf.size(); ++p) to.set(p, group->contains(primOf[p]));
        } else if (group->classOf() == AttrClass::Vertex) {
            g->createGroup(name, AttrClass::Vertex) = *group;
        }
    }
    return g;
}

}  // namespace

std::shared_ptr<Geometry> posedCloth(const ClothFrame& f) {
    if (!f.geometry || !clothFits(f, *f.geometry)) return nullptr;
    auto g = f.torn() ? tornApart(*f.geometry, f) : std::make_shared<Geometry>(*f.geometry);
    const size_t n = f.positions.size();
    {
        auto P = g->positionsForWrite();
        std::copy(f.positions.begin(), f.positions.end(), P.begin());
    }
    if (f.velocities.size() == 3 * n) {
        auto v = g->points().create("v", AttrType::Vec3).write<Vec3>();
        for (size_t i = 0; i < n; ++i) {
            v[i] = Vec3(floatFromHalf(f.velocities[3 * i]), floatFromHalf(f.velocities[3 * i + 1]),
                        floatFromHalf(f.velocities[3 * i + 2]));
        }
    }
    // Smooth: each point's normal the sum of the faces round it, each as big
    // as it is.
    std::vector<Vec3> sum(n);
    for (size_t p = 0; p < g->primitiveCount(); ++p) {
        if (!g->primitiveClosed(p)) continue;
        const auto c = g->primitivePoints(p);
        for (size_t k = 1; k + 1 < c.size(); ++k) {
            const Vec3 a = f.positions[c[0]], b = f.positions[c[k]], d = f.positions[c[k + 1]];
            const Vec3 face = cross(b - a, d - a);
            sum[c[0]] += face;
            sum[c[k]] += face;
            sum[c[k + 1]] += face;
        }
    }
    auto N = g->points().create("N", AttrType::Vec3).write<Vec3>();
    for (size_t i = 0; i < n; ++i) {
        const float l = length(sum[i]);
        N[i] = l > 1e-12f ? sum[i] * (1.0f / l) : Vec3(0.0f, 1.0f, 0.0f);
    }
    return g;
}

std::shared_ptr<Geometry> drawnCloth(const ClothFrame& f, const Vec3& color) {
    auto g = posedCloth(f);
    if (!g) return nullptr;
    // The colour of every corner -- its own, its point's, its face's, the
    // whole's, else `color` -- as the pieces have theirs: drawn together, a
    // colour of one class is not taken for none of the other.
    auto colorIn = [](const AttributeSet& set, size_t i, Vec3& out) {
        const AttributeArray* a = set.find("Cd");
        if (!a || a->type() != AttrType::Vec3 || i >= a->size()) return false;
        out = a->read<Vec3>()[i];
        return true;
    };
    std::vector<Vec3> corners(g->vertexCount(), color);
    Vec3 whole = color;
    colorIn(g->detail(), 0, whole);
    for (size_t p = 0; p < g->primitiveCount(); ++p) {
        const size_t first = g->primitiveVertexStart(p), count = g->primitiveVertexCount(p);
        for (size_t v = first; v < first + count; ++v) {
            if (!colorIn(g->vertices(), v, corners[v]) && !colorIn(g->points(), g->vertexPoint(v), corners[v]) &&
                !colorIn(g->primitives(), p, corners[v])) {
                corners[v] = whole;
            }
        }
    }
    g->points().erase("Cd");
    g->primitives().erase("Cd");
    g->detail().erase("Cd");
    g->vertices().erase("Cd");
    auto cd = g->vertices().create("Cd", AttrType::Vec3).write<Vec3>();
    std::copy(corners.begin(), corners.end(), cd.begin());
    return g;
}

// --- set-up ----------------------------------------------------------------------------

ClothSolver::ClothSolver(const ClothScene& scene) : scene_(scene.sanitized()) {
    build();
    setScene(scene);
}

void ClothSolver::build() {
    const Geometry* geo = scene_.geometry.get();
    if (!geo) return;
    const size_t n = geo->pointCount();
    const auto P = geo->positions();
    x_.assign(P.begin(), P.end());
    rest_ = x_;
    v_.assign(n, Vec3());
    if (const AttributeArray* v = geo->points().find("v"); v && v->type() == AttrType::Vec3 && v->size() == n) {
        const auto vel = v->read<Vec3>();
        v_.assign(vel.begin(), vel.end());
    }
    prev_ = x_;
    start_ = x_;
    target_ = x_;
    pinned_.assign(n, 0);
    tearOf_.assign(n, 1.0f);
    shapeOf_.assign(n, 1.0f);
    plasticOf_.assign(n, 1.0f);
    shapeRest_ = rest_;
    const AttributeArray* pin = geo->points().find("pin");
    const AttributeArray* tear = geo->points().find("tear");
    const AttributeArray* shape = geo->points().find("shape");
    const AttributeArray* plastic = geo->points().find("plasticity");
    for (size_t i = 0; i < n; ++i) {
        pinned_[i] = pointNumber(pin, i, 0.0f) > 0.5f ? 1 : 0;
        tearOf_[i] = std::max(pointNumber(tear, i, 1.0f), 0.0f);
        const float held = pointNumber(shape, i, 1.0f);
        shapeOf_[i] = std::isfinite(held) ? std::clamp(held, 0.0f, 1e6f) : 1.0f;
        const float gives = pointNumber(plastic, i, 1.0f);
        plasticOf_[i] = std::isfinite(gives) ? std::clamp(gives, 0.0f, 1.0f) : 1.0f;
    }
    origin_.resize(n);
    std::iota(origin_.begin(), origin_.end(), 0u);
    corner_.resize(geo->vertexCount());
    for (size_t v = 0; v < corner_.size(); ++v) corner_[v] = geo->vertexPoint(v);
    ropeCut_.assign(corner_.size(), 0);
    cut_.clear();
    connect();
    // How close it lets the rest come: the edges at rest.
    double edges = 0.0;
    for (size_t l = 0; l < stretchCount_; ++l) edges += links_[l].rest;
    const float meanEdge =
        stretchCount_ ? static_cast<float>(edges / static_cast<double>(stretchCount_)) : scene_.solver.thickness;
    meanEdge_ = std::max(meanEdge, 1e-4f);
    selfRadius_ = std::max(scene_.solver.thickness, 0.3f * meanEdge);
}

void ClothSolver::connect() {
    const Geometry* geo = scene_.geometry.get();
    const size_t n = x_.size();
    tris_.clear();
    links_.clear();
    colours_.clear();
    leftOver_.clear();
    near_.clear();
    balloons_.clear();
    auto isCut = [&](uint32_t a, uint32_t b) { return std::binary_search(cut_.begin(), cut_.end(), pairKey(a, b)); };

    // The triangles -- polygons as fans -- and the links: the edges of the
    // polygons and the segments of the lines, but those torn; the diagonals
    // of quads. How long each is: at rest.
    const ClothSettings& s = scene_.solver;
    std::vector<double> mass(n, 0.0);
    std::map<uint64_t, uint32_t> linked;  // pair -> link
    auto link = [&](uint32_t a, uint32_t b, float compliance, bool tears, uint32_t corner) {
        if (a == b || a >= n || b >= n) return;
        const uint64_t key = pairKey(a, b);
        if (linked.count(key)) return;
        linked[key] = static_cast<uint32_t>(links_.size());
        // As long as in the shape it holds: at rest, as it has given way.
        const float rest = length(shapeRest_[a] - shapeRest_[b]);
        const float give = s.tear * std::min(tearOf_[origin_[a]], tearOf_[origin_[b]]);
        links_.push_back({a, b, rest, compliance, tears && s.tear > 0.0f ? rest * (1.0f + give) : 0.0f, corner, give});
    };
    const float stretchCompliance = 1.0f / s.stretch;
    std::vector<std::array<uint32_t, 3>> ropeBends;
    // A torn edge the faces on either side no longer share has opened: it
    // is two edges now, each of one face, each holding its length.
    std::map<uint64_t, int> faceEdges;  // edge -> how many faces have it
    for (size_t p = 0; p < geo->primitiveCount(); ++p) {
        const size_t first = geo->primitiveVertexStart(p), count = geo->primitiveVertexCount(p);
        if (!geo->primitiveClosed(p) || count < 3) continue;
        for (size_t k = 0; k < count; ++k) ++faceEdges[pairKey(corner_[first + k], corner_[first + (k + 1) % count])];
    }
    std::erase_if(cut_, [&](uint64_t key) { return faceEdges[key] < 2; });
    for (size_t p = 0; p < geo->primitiveCount(); ++p) {
        const size_t first = geo->primitiveVertexStart(p), count = geo->primitiveVertexCount(p);
        if (count < 2) continue;
        auto at = [&](size_t k) { return corner_[first + k]; };
        if (!geo->primitiveClosed(p) || count == 2) {
            // A rope: its segments, and each point to the one after the next.
            for (size_t k = 0; k + 1 < count; ++k) {
                if (ropeCut_[first + k]) continue;
                link(at(k), at(k + 1), stretchCompliance, true, static_cast<uint32_t>(first + k));
                const double half = 0.5 * s.density * length(rest_[at(k)] - rest_[at(k + 1)]);
                mass[at(k)] += half;
                mass[at(k + 1)] += half;
                if (k + 2 < count && !ropeCut_[first + k + 1]) ropeBends.push_back({at(k), at(k + 1), at(k + 2)});
            }
            continue;
        }
        for (size_t k = 0; k < count; ++k) {
            // An edge of one face -- the border, or the lip of a tear --
            // holds: torn, it would open nothing, and be linked again here.
            const uint32_t a = at(k), b = at((k + 1) % count);
            if (!isCut(a, b)) link(a, b, stretchCompliance, faceEdges[pairKey(a, b)] >= 2, kNone);
        }
        if (count == 4) {
            link(at(0), at(2), 1.0f / s.shear, false, kNone);
            link(at(1), at(3), 1.0f / s.shear, false, kNone);
        }
        for (size_t k = 1; k + 1 < count; ++k) {
            const uint32_t t[3] = {at(0), at(k), at(k + 1)};
            tris_.insert(tris_.end(), t, t + 3);
            const double third =
                s.density * 0.5 * length(cross(rest_[t[1]] - rest_[t[0]], rest_[t[2]] - rest_[t[0]])) / 3.0;
            for (const uint32_t q : t) mass[q] += third;
        }
    }
    stretchCount_ = links_.size();

    // Bending: across each edge two triangles share, their far points --
    // not across a tear; along a rope, each point and the one after the next.
    if (s.bend > 0.0f) {
        const float bendCompliance = 1.0f / s.bend;
        std::map<uint64_t, std::vector<uint32_t>> across;  // edge -> the far points of its triangles
        for (size_t t = 0; t + 2 < tris_.size(); t += 3) {
            for (int e = 0; e < 3; ++e) {
                const uint32_t a = tris_[t + static_cast<size_t>(e)], b = tris_[t + static_cast<size_t>((e + 1) % 3)];
                across[pairKey(a, b)].push_back(tris_[t + static_cast<size_t>((e + 2) % 3)]);
            }
        }
        for (const auto& [edge, far] : across) {
            if (far.size() == 2 && !std::binary_search(cut_.begin(), cut_.end(), edge)) {
                link(far[0], far[1], bendCompliance, false, kNone);
            }
        }
        for (const auto& r : ropeBends) link(r[0], r[2], bendCompliance, false, kNone);
    }
    bendCount_ = links_.size() - stretchCount_;

    // Mass: the cloth round each point, or its attribute -- shared among the
    // points torn off one as the cloth round each is.
    const AttributeArray* given = geo->points().find("mass");
    std::vector<double> ofOrigin(geo->pointCount(), 0.0);
    for (size_t i = 0; i < n; ++i) ofOrigin[origin_[i]] += mass[i];
    w_.assign(n, 0.0f);
    m_.assign(n, 0.0f);
    for (size_t i = 0; i < n; ++i) {
        double m = mass[i];
        if (given) {
            const double whole = pointNumber(given, origin_[i], 0.0f);
            m = ofOrigin[origin_[i]] > 0.0 ? whole * mass[i] / ofOrigin[origin_[i]] : whole;
        }
        m_[i] = static_cast<float>(std::max(m, 1e-6));
        w_[i] = pinned_[i] ? 0.0f : 1.0f / m_[i];
    }

    // The edges that collide: of the triangles, and the ropes' segments --
    // each once, in order.
    {
        std::vector<uint64_t> keys;
        keys.reserve(tris_.size() + stretchCount_);
        for (size_t t = 0; t + 2 < tris_.size(); t += 3) {
            for (int e = 0; e < 3; ++e) keys.push_back(pairKey(tris_[t + static_cast<size_t>(e)], tris_[t + static_cast<size_t>((e + 1) % 3)]));
        }
        for (size_t l = 0; l < stretchCount_; ++l) {
            if (links_[l].corner != kNone) keys.push_back(pairKey(links_[l].a, links_[l].b));
        }
        std::sort(keys.begin(), keys.end());
        keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
        edges_.clear();
        edges_.reserve(2 * keys.size());
        for (const uint64_t k : keys) {
            edges_.push_back(static_cast<uint32_t>(k >> 32));
            edges_.push_back(static_cast<uint32_t>(k & 0xffffffffu));
        }
    }

    // The pieces: the points the edges and the segments hold together, each
    // piece's in order, the pieces by their first.
    {
        std::vector<uint32_t> parent(n);
        std::iota(parent.begin(), parent.end(), 0u);
        auto find = [&](uint32_t a) {
            while (parent[a] != a) a = parent[a] = parent[parent[a]];
            return a;
        };
        for (size_t l = 0; l < stretchCount_; ++l) {
            const uint32_t a = find(links_[l].a), b = find(links_[l].b);
            if (a != b) parent[std::max(a, b)] = std::min(a, b);
        }
        std::vector<uint32_t> pieceOf(n, kNone), count;
        for (uint32_t i = 0; i < n; ++i) {
            const uint32_t root = find(i);
            if (pieceOf[root] == kNone) {
                pieceOf[root] = static_cast<uint32_t>(count.size());
                count.push_back(0);
            }
            ++count[pieceOf[root]];
        }
        pieceStart_.assign(count.size() + 1, 0);
        for (size_t p = 0; p < count.size(); ++p) pieceStart_[p + 1] = pieceStart_[p] + count[p];
        piecePoints_.assign(n, 0);
        pieceOf_.assign(n, 0);
        std::vector<uint32_t> at(pieceStart_.begin(), pieceStart_.end() - 1);
        for (uint32_t i = 0; i < n; ++i) {
            pieceOf_[i] = pieceOf[find(i)];
            piecePoints_[at[pieceOf_[i]]++] = i;
        }
    }

    // Each point's triangles, for the air's push.
    triStart_.assign(n + 1, 0);
    for (const uint32_t q : tris_) ++triStart_[q + 1];
    for (size_t i = 0; i < n; ++i) triStart_[i + 1] += triStart_[i];
    triOf_.assign(tris_.size(), 0);
    {
        std::vector<uint32_t> at(triStart_.begin(), triStart_.end() - 1);
        for (size_t k = 0; k < tris_.size(); ++k) triOf_[at[tris_[k]]++] = static_cast<uint32_t>(k / 3);
    }

    // The closed meshes -- every edge of their triangles shared by two, none
    // torn: the air reaches only their outside, and with pressure they are
    // balloons. How much each holds: at rest.
    outside_.assign(tris_.size() / 3, 0);
    if (!tris_.empty()) {
        const size_t count = tris_.size() / 3;
        std::vector<uint32_t> parent(n);
        std::iota(parent.begin(), parent.end(), 0u);
        auto find = [&](uint32_t a) {
            while (parent[a] != a) a = parent[a] = parent[parent[a]];
            return a;
        };
        for (size_t t = 0; t < count; ++t) {
            const uint32_t a = find(tris_[3 * t]);
            for (int k = 1; k < 3; ++k) {
                const uint32_t b = find(tris_[3 * t + static_cast<size_t>(k)]);
                if (a != b) parent[std::max(a, b)] = std::min(a, b);
            }
        }
        std::map<uint32_t, std::vector<uint32_t>> parts;  // root -> triangles
        for (size_t t = 0; t < count; ++t) parts[find(tris_[3 * t])].push_back(static_cast<uint32_t>(t));
        for (const auto& [root, triangles] : parts) {
            std::map<uint64_t, int> uses;
            for (const uint32_t t : triangles) {
                for (int e = 0; e < 3; ++e) {
                    ++uses[pairKey(tris_[3 * t + static_cast<size_t>(e)], tris_[3 * t + static_cast<size_t>((e + 1) % 3)])];
                }
            }
            const bool closed = std::all_of(uses.begin(), uses.end(), [&](const auto& u) {
                return u.second == 2 && !std::binary_search(cut_.begin(), cut_.end(), u.first);
            });
            if (!closed) continue;
            Balloon b;
            b.triangles = triangles;
            double volume = 0.0;
            for (const uint32_t t : triangles) {
                volume += tripleProduct(rest_[tris_[3 * t]], rest_[tris_[3 * t + 1]], rest_[tris_[3 * t + 2]]);
            }
            b.rest = static_cast<float>(volume / 6.0);
            if (std::fabs(b.rest) <= 1e-9f) continue;
            for (const uint32_t t : triangles) outside_[t] = b.rest > 0.0f ? 1 : -1;
            if (s.pressure > 0.0f) balloons_.push_back(std::move(b));
        }
    }

    // Colours: each link the first none of whose points has it yet.
    std::vector<uint64_t> used(n, 0);
    colours_.assign(kColours, {});
    for (uint32_t l = 0; l < links_.size(); ++l) {
        const uint64_t taken = used[links_[l].a] | used[links_[l].b];
        if (~taken == 0) {
            leftOver_.push_back(l);
            continue;
        }
        const int c = std::countr_one(taken);
        colours_[static_cast<size_t>(c)].push_back(l);
        used[links_[l].a] |= 1ull << c;
        used[links_[l].b] |= 1ull << c;
    }
    while (!colours_.empty() && colours_.back().empty()) colours_.pop_back();

    // Each point's neighbours -- what it is linked to -- which it does not
    // push off itself.
    std::vector<std::vector<uint32_t>> nb(n);
    for (const Link& l : links_) {
        nb[l.a].push_back(l.b);
        nb[l.b].push_back(l.a);
    }
    nearStart_.assign(n + 1, 0);
    for (size_t i = 0; i < n; ++i) {
        std::sort(nb[i].begin(), nb[i].end());
        nearStart_[i + 1] = nearStart_[i] + static_cast<uint32_t>(nb[i].size());
        near_.insert(near_.end(), nb[i].begin(), nb[i].end());
    }
    airOfTri_.resize(tris_.size() / 3);
    buildFaceTree();
}

bool ClothSolver::tear() {
    std::vector<uint32_t> torn;
    for (uint32_t l = 0; l < stretchCount_; ++l) {
        const Link& k = links_[l];
        if (k.limit > 0.0f && length(x_[k.b] - x_[k.a]) > k.limit) torn.push_back(l);
    }
    if (torn.empty()) return false;
    std::vector<uint32_t> ends;
    for (const uint32_t l : torn) {
        const Link& k = links_[l];
        if (k.corner != kNone) {
            ropeCut_[k.corner] = 1;  // a rope parts: its points are its own already
            continue;
        }
        cut_.push_back(pairKey(k.a, k.b));
        ends.push_back(k.a);
        ends.push_back(k.b);
    }
    std::sort(cut_.begin(), cut_.end());
    cut_.erase(std::unique(cut_.begin(), cut_.end()), cut_.end());
    std::sort(ends.begin(), ends.end());
    ends.erase(std::unique(ends.begin(), ends.end()), ends.end());
    for (const uint32_t p : ends) split(p);
    connect();
    return true;
}

void ClothSolver::split(uint32_t p) {
    // The faces round p, and which of them still hang together there: those
    // that share an edge from p not torn.
    const Geometry* geo = scene_.geometry.get();
    struct Corner {
        uint32_t vertex, face;
    };
    std::vector<Corner> round;
    std::vector<std::pair<uint32_t, uint32_t>> sides;  // (the far point of an edge from p, face)
    for (size_t f = 0; f < geo->primitiveCount(); ++f) {
        const size_t first = geo->primitiveVertexStart(f), count = geo->primitiveVertexCount(f);
        if (!geo->primitiveClosed(f) || count < 3) continue;
        for (size_t k = 0; k < count; ++k) {
            if (corner_[first + k] != p) continue;
            const uint32_t face = static_cast<uint32_t>(f);
            round.push_back({static_cast<uint32_t>(first + k), face});
            for (const size_t o : {(k + 1) % count, (k + count - 1) % count}) {
                const uint32_t q = corner_[first + o];
                if (!std::binary_search(cut_.begin(), cut_.end(), pairKey(p, q))) sides.push_back({q, face});
            }
        }
    }
    if (round.size() < 2) return;
    std::map<uint32_t, uint32_t> parent;
    for (const Corner& c : round) parent[c.face] = c.face;
    auto find = [&](uint32_t a) {
        while (parent[a] != a) a = parent[a] = parent[parent[a]];
        return a;
    };
    std::sort(sides.begin(), sides.end());
    for (size_t i = 1; i < sides.size(); ++i) {
        if (sides[i].first != sides[i - 1].first) continue;
        const uint32_t a = find(sides[i].second), b = find(sides[i - 1].second);
        if (a != b) parent[std::max(a, b)] = std::min(a, b);
    }
    // The first face's part keeps p; each other part gets a point of its own,
    // where p is and going as it goes.
    std::map<uint32_t, uint32_t> pointOf;  // part -> its point
    pointOf[find(round.front().face)] = p;
    for (const Corner& c : round) {
        const uint32_t part = find(c.face);
        auto it = pointOf.find(part);
        if (it == pointOf.end()) {
            const uint32_t q = static_cast<uint32_t>(x_.size());
            x_.push_back(x_[p]);
            v_.push_back(v_[p]);
            prev_.push_back(prev_[p]);
            start_.push_back(start_[p]);
            target_.push_back(target_[p]);
            rest_.push_back(rest_[p]);
            shapeRest_.push_back(shapeRest_[p]);
            pinned_.push_back(pinned_[p]);
            origin_.push_back(origin_[p]);
            it = pointOf.emplace(part, q).first;
        }
        corner_[c.vertex] = it->second;
    }
}

void ClothSolver::setScene(const ClothScene& scene) {
    ClothScene next = scene.sanitized();
    // What it is made of and how it is held together stay.
    next.geometry = scene_.geometry;
    next.solver.density = scene_.solver.density;
    next.solver.stretch = scene_.solver.stretch;
    next.solver.shear = scene_.solver.shear;
    next.solver.bend = scene_.solver.bend;
    next.solver.pressure = scene_.solver.pressure;
    next.solver.tear = scene_.solver.tear;
    next.solver.shape = scene_.solver.shape;
    next.solver.plasticity = scene_.solver.plasticity;
    next.solver.yield = scene_.solver.yield;
    scene_ = std::move(next);
    shapes_.clear();
    for (const Collider& c : scene_.colliders) shapes_.push_back(c.instance());
}

void ClothSolver::setAir(std::function<Vec3(const Vec3&)> air) { air_ = std::move(air); }

float ClothSolver::balloonVolume(size_t b) const {
    double volume = 0.0;
    for (const uint32_t t : balloons_[b].triangles) volume += tripleProduct(x_[tris_[3 * t]], x_[tris_[3 * t + 1]], x_[tris_[3 * t + 2]]);
    return static_cast<float>(volume / 6.0);
}

// --- a step ----------------------------------------------------------------------------

void ClothSolver::aero(std::vector<Vec3>& accel) const {
    const ClothSettings& s = scene_.solver;
    const size_t count = tris_.size() / 3;
    std::vector<Vec3> force(count);
    const float h = s.timeStep / static_cast<float>(s.substeps);
    pg::parallelFor(count, 256, [&](size_t begin, size_t end) {
        for (size_t t = begin; t < end; ++t) {
            const uint32_t a = tris_[3 * t], b = tris_[3 * t + 1], c = tris_[3 * t + 2];
            const Vec3 middle = (x_[a] + x_[b] + x_[c]) * (1.0f / 3.0f);
            Vec3 flow = airOfTri_[t];
            for (const Force& f : scene_.forces) {
                if (f.kind == ForceKind::Wind) flow += detail::windAt(f, time_, f.seed * 7919u + 17u, middle);
            }
            const Vec3 face = cross(x_[b] - x_[a], x_[c] - x_[a]);
            const float twice = length(face);
            if (twice < 1e-12f) continue;
            const Vec3 normal = face * (1.0f / twice);
            const Vec3 rel = flow - (v_[a] + v_[b] + v_[c]) * (1.0f / 3.0f);
            const float along = dot(rel, normal);
            // A closed mesh: only where the air comes at its outside.
            if (outside_[t] != 0 && along * static_cast<float>(outside_[t]) >= 0.0f) continue;
            // Pushed along its normal as a plate is, as hard as the air comes
            // at it -- no more in a substep than takes it the air's way.
            float push = 0.5f * kAirDensity * kPlateDrag * s.airDrag * 0.5f * twice * std::fabs(along) * along;
            float lightest = 1e30f;
            for (const uint32_t q : {a, b, c}) {
                if (w_[q] > 0.0f) lightest = std::min(lightest, 1.0f / w_[q]);
            }
            if (lightest < 1e30f) {
                const float most = 3.0f * lightest * std::fabs(along) / h;
                push = std::clamp(push, -most, most);
            }
            force[t] = normal * push;
        }
    });
    const size_t n = x_.size();
    pg::parallelFor(n, 1024, [&](size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i) {
            Vec3 sum;
            for (uint32_t k = triStart_[i]; k < triStart_[i + 1]; ++k) sum += force[triOf_[k]];
            accel[i] = sum * (w_[i] / 3.0f);
        }
    });
}

void ClothSolver::solveLinks(float h) {
    const float h2 = h * h;
    auto solve = [&](const Link& l) {
        const float wa = w_[l.a], wb = w_[l.b];
        const float wsum = wa + wb;
        if (wsum <= 0.0f) return;
        const Vec3 d = x_[l.b] - x_[l.a];
        const float len = length(d);
        if (len < 1e-9f) return;
        const Vec3 n = d * (1.0f / len);
        const float lambda = -(len - l.rest) / (wsum + l.compliance / h2);
        x_[l.a] = x_[l.a] - n * (wa * lambda);
        x_[l.b] += n * (wb * lambda);
    };
    for (const std::vector<uint32_t>& colour : colours_) {
        pg::parallelFor(colour.size(), 512, [&](size_t begin, size_t end) {
            for (size_t k = begin; k < end; ++k) solve(links_[colour[k]]);
        });
    }
    for (const uint32_t l : leftOver_) solve(links_[l]);
}

void ClothSolver::solveBalloons(float h) {
    (void)h;
    const float pressure = scene_.solver.pressure;
    for (const Balloon& b : balloons_) {
        // C = V - pressure * V0; its gradient at a point, the sum over its
        // triangles of the cross product of the other two corners, over 6.
        std::map<uint32_t, Vec3> grad;
        double volume = 0.0;
        for (const uint32_t t : b.triangles) {
            const uint32_t p0 = tris_[3 * t], p1 = tris_[3 * t + 1], p2 = tris_[3 * t + 2];
            volume += tripleProduct(x_[p0], x_[p1], x_[p2]);
            grad[p0] += cross(x_[p1], x_[p2]) * (1.0f / 6.0f);
            grad[p1] += cross(x_[p2], x_[p0]) * (1.0f / 6.0f);
            grad[p2] += cross(x_[p0], x_[p1]) * (1.0f / 6.0f);
        }
        const double c = volume / 6.0 - static_cast<double>(pressure) * b.rest;
        double sum = 0.0;
        for (const auto& [p, g] : grad) sum += w_[p] * static_cast<double>(dot(g, g));
        if (sum < 1e-18) continue;
        const float lambda = static_cast<float>(-c / sum);
        for (const auto& [p, g] : grad) x_[p] += g * (w_[p] * lambda);
    }
}

void ClothSolver::matchShapes(float h) {
    const ClothSettings& s = scene_.solver;
    if (s.shape <= 0.0f || pieceStart_.size() < 2) return;
    const float h2 = h * h;
    pg::parallelFor(pieceStart_.size() - 1, 1, [&](size_t begin, size_t end) {
        for (size_t piece = begin; piece < end; ++piece) {
            const uint32_t first = pieceStart_[piece], last = pieceStart_[piece + 1];
            if (last - first < 2) continue;
            // Its middle where it is and in the shape it holds, each point as
            // heavy as it is.
            double mass = 0.0, now[3] = {0.0, 0.0, 0.0}, held[3] = {0.0, 0.0, 0.0};
            for (uint32_t k = first; k < last; ++k) {
                const uint32_t i = piecePoints_[k];
                const double m = m_[i];
                mass += m;
                for (int a = 0; a < 3; ++a) {
                    now[a] += m * x_[i][a];
                    held[a] += m * shapeRest_[i][a];
                }
            }
            if (!(mass > 0.0)) continue;
            const Vec3 middle(static_cast<float>(now[0] / mass), static_cast<float>(now[1] / mass),
                              static_cast<float>(now[2] / mass));
            const Vec3 shapeMiddle(static_cast<float>(held[0] / mass), static_cast<float>(held[1] / mass),
                                   static_cast<float>(held[2] / mass));
            // How it is turned: the turn that best takes the shape onto it.
            double sum[3][3] = {{0.0, 0.0, 0.0}, {0.0, 0.0, 0.0}, {0.0, 0.0, 0.0}};
            for (uint32_t k = first; k < last; ++k) {
                const uint32_t i = piecePoints_[k];
                const Vec3 q = shapeRest_[i] - shapeMiddle, p = x_[i] - middle;
                const double m = m_[i];
                for (int a = 0; a < 3; ++a) {
                    for (int b = 0; b < 3; ++b) sum[a][b] += m * static_cast<double>(q[a]) * static_cast<double>(p[b]);
                }
            }
            const Mat3 turn = bestTurn(sum);
            const Mat3 back = glm::transpose(turn);
            for (uint32_t k = first; k < last; ++k) {
                const uint32_t i = piecePoints_[k];
                const Vec3 goal = middle + turn * (shapeRest_[i] - shapeMiddle);
                // Bent further than it yields: a share of how much further
                // stays -- the shape it holds gives way.
                const float plastic = s.plasticity * plasticOf_[origin_[i]];
                if (plastic > 0.0f) {
                    const Vec3 off = x_[i] - goal;
                    const float far = length(off);
                    if (far > s.yield) shapeRest_[i] += back * (off * ((far - s.yield) * plastic / far));
                }
                const float stiffness = s.shape * shapeOf_[origin_[i]];
                if (w_[i] <= 0.0f || stiffness <= 0.0f) continue;
                x_[i] += (goal - x_[i]) * (w_[i] / (w_[i] + 1.0f / (stiffness * h2)));
            }
        }
    });
}

void ClothSolver::buildFaceTree() {
    faceNodes_.clear();
    faceOrder_.clear();
    const size_t triangles = tris_.size() / 3;
    // The elements: the triangles, then the ropes' segments; each one's
    // edges -- a segment's one -- by their numbers in edges_.
    auto edgeIndex = [&](uint32_t a, uint32_t b) {
        const uint64_t key = pairKey(a, b);
        size_t lo = 0, hi = edges_.size() / 2;
        while (lo < hi) {
            const size_t mid = (lo + hi) / 2;
            if (pairKey(edges_[2 * mid], edges_[2 * mid + 1]) < key) lo = mid + 1;
            else hi = mid;
        }
        return static_cast<uint32_t>(lo);
    };
    ropeEdges_.clear();
    for (size_t l = 0; l < stretchCount_; ++l) {
        if (links_[l].corner != kNone) ropeEdges_.push_back(edgeIndex(links_[l].a, links_[l].b));
    }
    std::sort(ropeEdges_.begin(), ropeEdges_.end());
    ropeEdges_.erase(std::unique(ropeEdges_.begin(), ropeEdges_.end()), ropeEdges_.end());
    const size_t count = triangles + ropeEdges_.size();
    elementEdges_.assign(3 * count, kNone);
    for (size_t t = 0; t < triangles; ++t) {
        for (int e = 0; e < 3; ++e) {
            elementEdges_[3 * t + static_cast<size_t>(e)] =
                edgeIndex(tris_[3 * t + static_cast<size_t>(e)], tris_[3 * t + static_cast<size_t>((e + 1) % 3)]);
        }
    }
    for (size_t k = 0; k < ropeEdges_.size(); ++k) elementEdges_[3 * (triangles + k)] = ropeEdges_[k];
    if (count == 0) return;
    // Split where they are at rest, the longer way, in halves -- sorted by
    // place and then by number: the same tree everywhere.
    std::vector<Vec3> middle(count);
    for (size_t k = 0; k < count; ++k) {
        if (k < triangles) {
            middle[k] = (rest_[tris_[3 * k]] + rest_[tris_[3 * k + 1]] + rest_[tris_[3 * k + 2]]) * (1.0f / 3.0f);
        } else {
            const uint32_t e = ropeEdges_[k - triangles];
            middle[k] = (rest_[edges_[2 * e]] + rest_[edges_[2 * e + 1]]) * 0.5f;
        }
    }
    restMiddle_ = middle;
    faceOrder_.resize(count);
    std::iota(faceOrder_.begin(), faceOrder_.end(), 0u);
    faceNodes_.push_back({0, static_cast<uint32_t>(count), -1, -1});
    for (size_t at = 0; at < faceNodes_.size(); ++at) {
        const uint32_t first = faceNodes_[at].first, size = faceNodes_[at].count;
        if (size <= 4) continue;
        Vec3 lo(1e30f), hi(-1e30f);
        for (uint32_t k = first; k < first + size; ++k) {
            lo = glm::min(lo, middle[faceOrder_[k]]);
            hi = glm::max(hi, middle[faceOrder_[k]]);
        }
        const Vec3 extent = hi - lo;
        const int axis = extent.x >= extent.y && extent.x >= extent.z ? 0 : (extent.y >= extent.z ? 1 : 2);
        std::sort(faceOrder_.begin() + first, faceOrder_.begin() + first + size, [&](uint32_t a, uint32_t b) {
            return middle[a][axis] < middle[b][axis] || (middle[a][axis] == middle[b][axis] && a < b);
        });
        const uint32_t half = size / 2;
        faceNodes_[at].left = static_cast<int32_t>(faceNodes_.size());
        faceNodes_.push_back({first, half, -1, -1});
        faceNodes_[at].right = static_cast<int32_t>(faceNodes_.size());
        faceNodes_.push_back({first + half, size - half, -1, -1});
    }
    // Where each point's elements are in the tree, in order -- and the points
    // of each node that elements outside it have too: what it shares with
    // the rest of the cloth.
    const size_t n = x_.size();
    std::vector<uint32_t> placeOf(count);
    for (size_t k = 0; k < count; ++k) placeOf[faceOrder_[k]] = static_cast<uint32_t>(k);
    auto pointsOf = [&](uint32_t element, uint32_t out[3]) {
        if (element < triangles) {
            for (int k = 0; k < 3; ++k) out[k] = tris_[3 * element + static_cast<size_t>(k)];
            return 3;
        }
        const uint32_t e = ropeEdges_[element - triangles];
        out[0] = edges_[2 * e];
        out[1] = edges_[2 * e + 1];
        return 2;
    };
    placeStart_.assign(n + 1, 0);
    for (size_t k = 0; k < count; ++k) {
        uint32_t q[3];
        const int m = pointsOf(static_cast<uint32_t>(k), q);
        for (int j = 0; j < m; ++j) ++placeStart_[q[j] + 1];
    }
    for (size_t i = 0; i < n; ++i) placeStart_[i + 1] += placeStart_[i];
    places_.assign(placeStart_[n], 0);
    {
        std::vector<uint32_t> fill(placeStart_.begin(), placeStart_.end() - 1);
        for (size_t k = 0; k < count; ++k) {
            uint32_t q[3];
            const int m = pointsOf(static_cast<uint32_t>(k), q);
            for (int j = 0; j < m; ++j) places_[fill[q[j]]++] = placeOf[k];
        }
        for (size_t i = 0; i < n; ++i) std::sort(places_.begin() + placeStart_[i], places_.begin() + placeStart_[i + 1]);
    }
    shareStart_.assign(faceNodes_.size() + 1, 0);
    shared_.clear();
    std::vector<uint32_t> mine;
    for (size_t at = 0; at < faceNodes_.size(); ++at) {
        const uint32_t first = faceNodes_[at].first, last = first + faceNodes_[at].count;
        mine.clear();
        for (uint32_t k = first; k < last; ++k) {
            uint32_t q[3];
            const int m = pointsOf(faceOrder_[k], q);
            for (int j = 0; j < m; ++j) mine.push_back(q[j]);
        }
        std::sort(mine.begin(), mine.end());
        mine.erase(std::unique(mine.begin(), mine.end()), mine.end());
        for (const uint32_t q : mine) {
            if (places_[placeStart_[q]] < first || places_[placeStart_[q + 1] - 1] >= last) shared_.push_back(q);
        }
        shareStart_[at + 1] = static_cast<uint32_t>(shared_.size());
    }
}

void ClothSolver::selfCollide(bool points, bool faces) {
    const size_t n = x_.size();
    if (points) {
        const float r = selfRadius_, reach = 2.0f * r;
        const float cell = reach;
        auto keyOf = [&](const Vec3& p, int di, int dj, int dk) {
            const int64_t i = static_cast<int64_t>(std::floor(p.x / cell)) + di;
            const int64_t j = static_cast<int64_t>(std::floor(p.y / cell)) + dj;
            const int64_t k = static_cast<int64_t>(std::floor(p.z / cell)) + dk;
            return static_cast<uint64_t>((i * 73856093) ^ (j * 19349663) ^ (k * 83492791));
        };
        std::vector<std::pair<uint64_t, uint32_t>> sorted(n);
        for (size_t i = 0; i < n; ++i) sorted[i] = {keyOf(x_[i], 0, 0, 0), static_cast<uint32_t>(i)};
        std::sort(sorted.begin(), sorted.end());
        std::vector<Vec3> push(n);
        pg::parallelFor(n, 256, [&](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i) {
                if (w_[i] <= 0.0f) continue;
                Vec3 sum;
                const uint32_t* nb = near_.data() + nearStart_[i];
                const uint32_t* nbEnd = near_.data() + nearStart_[i + 1];
                for (int dk = -1; dk <= 1; ++dk) {
                    for (int dj = -1; dj <= 1; ++dj) {
                        for (int di = -1; di <= 1; ++di) {
                            const uint64_t key = keyOf(x_[i], di, dj, dk);
                            auto it = std::lower_bound(sorted.begin(), sorted.end(), std::make_pair(key, 0u));
                            for (; it != sorted.end() && it->first == key; ++it) {
                                const uint32_t j = it->second;
                                if (j == i || std::binary_search(nb, nbEnd, j)) continue;
                                // Points nearer than that at rest -- round the
                                // pole of a sphere -- kept no nearer than they were.
                                const float apart = std::min(reach, length(rest_[i] - rest_[j]));
                                const Vec3 d = x_[i] - x_[j];
                                const float dist = length(d);
                                if (dist >= apart || dist < 1e-9f) continue;
                                const float share = w_[i] / (w_[i] + w_[j]);
                                sum += d * ((apart - dist) * share / dist);
                            }
                        }
                    }
                }
                push[i] = sum;
            }
        });
        for (size_t i = 0; i < n; ++i) x_[i] += push[i];
    }
    if (faces && !faceNodes_.empty()) collideFaceTree();
}

void ClothSolver::collideFaceTree() {
    const ClothSettings& s = scene_.solver;
    // As far from itself as Thickness says.
    const float gap = s.thickness;
    const size_t triangles = tris_.size() / 3, count = faceOrder_.size(), nodes = faceNodes_.size();
    // Each element: its box, from where it was at the start of the substep
    // to where it is, and which way it faces.
    std::vector<Vec3> lo(count), hi(count), facing(count);
    pg::parallelFor(count, 1024, [&](size_t begin, size_t end) {
        for (size_t k = begin; k < end; ++k) {
            uint32_t q[3];
            int m = 3;
            if (k < triangles) {
                q[0] = tris_[3 * k], q[1] = tris_[3 * k + 1], q[2] = tris_[3 * k + 2];
                const Vec3 face = cross(x_[q[1]] - x_[q[0]], x_[q[2]] - x_[q[0]]);
                const float area = length(face);
                facing[k] = area > 1e-12f ? face * (1.0f / area) : Vec3();
            } else {
                const uint32_t e = ropeEdges_[k - triangles];
                q[0] = edges_[2 * e], q[1] = edges_[2 * e + 1];
                m = 2;
                facing[k] = Vec3();
            }
            Vec3 l = glm::min(x_[q[0]], prev_[q[0]]), h = glm::max(x_[q[0]], prev_[q[0]]);
            for (int j = 1; j < m; ++j) {
                l = glm::min(l, glm::min(x_[q[j]], prev_[q[j]]));
                h = glm::max(h, glm::max(x_[q[j]], prev_[q[j]]));
            }
            lo[k] = l;
            hi[k] = h;
        }
    });
    // Each node: its box, and the cone its faces face within -- how far, in
    // radians, from its axis; more than half a turn for a segment, or for
    // faces that face every way.
    nodeLo_.resize(nodes);
    nodeHi_.resize(nodes);
    coneAxis_.resize(nodes);
    coneAngle_.resize(nodes);
    constexpr float kAnyWay = 4.0f;
    for (size_t a = nodes; a-- > 0;) {
        const FaceNode& node = faceNodes_[a];
        if (node.left < 0) {
            Vec3 l(1e30f), h(-1e30f), sum;
            bool cone = true;
            for (uint32_t k = node.first; k < node.first + node.count; ++k) {
                const uint32_t e = faceOrder_[k];
                l = glm::min(l, lo[e]);
                h = glm::max(h, hi[e]);
                if (facing[e] == Vec3()) cone = false;
                sum += facing[e];
            }
            nodeLo_[a] = l;
            nodeHi_[a] = h;
            const float len = length(sum);
            if (!cone || len < 1e-6f) {
                coneAxis_[a] = Vec3();
                coneAngle_[a] = kAnyWay;
                continue;
            }
            const Vec3 axis = sum * (1.0f / len);
            float widest = 0.0f;
            for (uint32_t k = node.first; k < node.first + node.count; ++k) {
                widest = std::max(widest, std::acos(std::clamp(dot(axis, facing[faceOrder_[k]]), -1.0f, 1.0f)));
            }
            coneAxis_[a] = axis;
            coneAngle_[a] = widest;
            continue;
        }
        const size_t l = static_cast<size_t>(node.left), r = static_cast<size_t>(node.right);
        nodeLo_[a] = glm::min(nodeLo_[l], nodeLo_[r]);
        nodeHi_[a] = glm::max(nodeHi_[l], nodeHi_[r]);
        coneAngle_[a] = mergedCone(l, r, coneAxis_[a]);
    }

    // The pairs of nodes that may touch: not a node whose faces all face
    // much the same way -- it cannot fold onto itself -- nor two such that
    // share points and face the same way together (Volino and
    // Magnenat-Thalmann 1994); not two whose boxes are further apart than
    // the gap. Of the leaves left, each point with the triangles, each edge
    // with the edges.
    constexpr float kFlat = 1.0f;  // radians: some 57 degrees
    std::vector<std::pair<uint32_t, uint32_t>> pointPairs, edgePairs;
    auto linked = [&](uint32_t p, uint32_t q) {
        return p == q || std::binary_search(near_.data() + nearStart_[p], near_.data() + nearStart_[p + 1], q);
    };
    auto pointsOf = [&](uint32_t element, uint32_t out[3]) {
        if (element < triangles) {
            for (int k = 0; k < 3; ++k) out[k] = tris_[3 * element + static_cast<size_t>(k)];
            return 3;
        }
        const uint32_t e = ropeEdges_[element - triangles];
        out[0] = edges_[2 * e];
        out[1] = edges_[2 * e + 1];
        return 2;
    };
    const Vec3 pad(gap);
    // Two elements of one piece nearer than twice an edge at rest: the cloth
    // round each other, which their links keep as it is.
    const float apartAtRest = 2.0f * meanEdge_;
    auto elementsInto = [&](uint32_t a, uint32_t b, std::vector<std::pair<uint32_t, uint32_t>>& outPoints,
                            std::vector<std::pair<uint32_t, uint32_t>>& outEdges) {
        // The points of a against the triangle b, and the edges of both.
        if (!overlaps(lo[a] - pad, hi[a] + pad, lo[b], hi[b])) return;
        uint32_t pa[3], pb[3];
        const int ma = pointsOf(a, pa), mb = pointsOf(b, pb);
        const Vec3 d = restMiddle_[a] - restMiddle_[b];
        if (pieceOf_[pa[0]] == pieceOf_[pb[0]] && dot(d, d) < apartAtRest * apartAtRest) return;
        if (b < triangles) {
            for (int j = 0; j < ma; ++j) {
                const uint32_t i = pa[j];
                if (linked(i, pb[0]) || linked(i, pb[1]) || linked(i, pb[2])) continue;
                if (w_[i] + w_[pb[0]] + w_[pb[1]] + w_[pb[2]] <= 0.0f) continue;
                outPoints.push_back({i, b});
            }
        }
        if (a < triangles) {
            for (int j = 0; j < mb; ++j) {
                const uint32_t i = pb[j];
                if (linked(i, pa[0]) || linked(i, pa[1]) || linked(i, pa[2])) continue;
                if (w_[i] + w_[pa[0]] + w_[pa[1]] + w_[pa[2]] <= 0.0f) continue;
                outPoints.push_back({i, a});
            }
        }
        for (int x = 0; x < 3; ++x) {
            const uint32_t e = elementEdges_[3 * a + static_cast<size_t>(x)];
            if (e == kNone) continue;
            for (int y = 0; y < 3; ++y) {
                const uint32_t f = elementEdges_[3 * b + static_cast<size_t>(y)];
                if (f == kNone || e == f) continue;
                const uint32_t ea = edges_[2 * e], eb = edges_[2 * e + 1], fa = edges_[2 * f], fb = edges_[2 * f + 1];
                if (linked(ea, fa) || linked(ea, fb) || linked(eb, fa) || linked(eb, fb)) continue;
                if (w_[ea] + w_[eb] + w_[fa] + w_[fb] <= 0.0f) continue;
                outEdges.push_back({std::min(e, f), std::max(e, f)});
            }
        }
    };
    auto elements = [&](uint32_t a, uint32_t b) { elementsInto(a, b, pointPairs, edgePairs); };
    auto leaves = [&](const FaceNode& A, const FaceNode& B, bool same, auto& emit) {
        for (uint32_t i = A.first; i < A.first + A.count; ++i) {
            for (uint32_t j = same ? i + 1 : B.first; j < B.first + B.count; ++j) emit(faceOrder_[i], faceOrder_[j]);
        }
    };
    // One pair of nodes: done with, or the pairs of their halves next.
    auto visit = [&](uint32_t a, uint32_t b, std::vector<std::pair<uint32_t, uint32_t>>& next, auto& emit) {
        const FaceNode& A = faceNodes_[a];
        if (a == b) {
            if (A.left < 0) {
                leaves(A, A, true, emit);
                return;
            }
            if (coneAngle_[a] < kFlat) return;
            const uint32_t l = static_cast<uint32_t>(A.left), r = static_cast<uint32_t>(A.right);
            next.push_back({l, l});
            next.push_back({r, r});
            next.push_back({l, r});
            return;
        }
        const FaceNode& B = faceNodes_[b];
        if (!overlaps(nodeLo_[a] - pad, nodeHi_[a] + pad, nodeLo_[b], nodeHi_[b])) return;
        if (coneAngle_[a] < kFlat && coneAngle_[b] < kFlat && sharePoints(a, b)) {
            Vec3 axis;
            if (mergedCone(a, b, axis) < kFlat) return;
        }
        if (A.left < 0 && B.left < 0) {
            leaves(A, B, false, emit);
            return;
        }
        // The bigger one in halves.
        if (B.left < 0 || (A.left >= 0 && A.count >= B.count)) {
            next.push_back({static_cast<uint32_t>(A.left), b});
            next.push_back({static_cast<uint32_t>(A.right), b});
        } else {
            next.push_back({a, static_cast<uint32_t>(B.left)});
            next.push_back({a, static_cast<uint32_t>(B.right)});
        }
    };
    // The first levels one by one, till there are pairs enough for every
    // thread; then each pair's on a thread, what it finds put together.
    std::vector<std::pair<uint32_t, uint32_t>> pairs = {{0u, 0u}}, next;
    auto collect = [&](uint32_t a, uint32_t b) { elements(a, b); };
    for (int level = 0; level < 12 && !pairs.empty() && pairs.size() < 256; ++level) {
        next.clear();
        for (const auto& [a, b] : pairs) visit(a, b, next, collect);
        pairs.swap(next);
    }
    std::mutex gathered;
    pg::parallelFor(pairs.size(), 1, [&](size_t begin, size_t end) {
        std::vector<std::pair<uint32_t, uint32_t>> myPoints, myEdges, stack;
        auto emit = [&](uint32_t a, uint32_t b) { elementsInto(a, b, myPoints, myEdges); };
        for (size_t k = begin; k < end; ++k) {
            stack.assign(1, pairs[k]);
            while (!stack.empty()) {
                const auto [a, b] = stack.back();
                stack.pop_back();
                visit(a, b, stack, emit);
            }
        }
        std::lock_guard<std::mutex> lock(gathered);
        pointPairs.insert(pointPairs.end(), myPoints.begin(), myPoints.end());
        edgePairs.insert(edgePairs.end(), myEdges.begin(), myEdges.end());
    });
    if (pointPairs.empty() && edgePairs.empty()) return;
    std::sort(pointPairs.begin(), pointPairs.end());
    pointPairs.erase(std::unique(pointPairs.begin(), pointPairs.end()), pointPairs.end());
    std::sort(edgePairs.begin(), edgePairs.end());
    edgePairs.erase(std::unique(edgePairs.begin(), edgePairs.end()), edgePairs.end());

    // A point near a triangle not its own -- nearer than the gap, or gone
    // through it in this substep: pushed off it on the side it came from.
    auto pointTriangle = [&](uint32_t i, uint32_t t, Contact& k) {
        const uint32_t a = tris_[3 * t], b = tris_[3 * t + 1], c = tris_[3 * t + 2];
        const Vec3 face = cross(x_[b] - x_[a], x_[c] - x_[a]);
        const float area = length(face);
        if (area < 1e-12f) return false;
        const Vec3 up = face * (1.0f / area);
        Vec3 weights;
        const Vec3 near = nearestOnTriangle(x_[i], x_[a], x_[b], x_[c], weights);
        const Vec3 off = x_[i] - near;
        const float d = length(off);
        const Vec3 face0 = cross(prev_[b] - prev_[a], prev_[c] - prev_[a]);
        const float area0 = length(face0);
        const float side0 = area0 > 1e-12f ? dot(prev_[i] - prev_[a], face0) / area0 : 0.0f;
        const float side1 = dot(x_[i] - x_[a], up);
        bool through = false;
        Vec3 normal;
        if (side0 * side1 < 0.0f && std::fabs(side0) > 1e-7f) {
            // Across its plane: through the triangle, where its way crosses it?
            const Vec3 way = x_[i] - prev_[i];
            const float along = dot(way, up);
            if (std::fabs(along) > 1e-12f) {
                const Vec3 hit = prev_[i] + way * std::clamp(dot(x_[a] - prev_[i], up) / along, 0.0f, 1.0f);
                Vec3 at;
                const Vec3 onFace = nearestOnTriangle(hit, x_[a], x_[b], x_[c], at);
                if (length(hit - onFace) <= 0.05f * gap) {
                    through = true;
                    normal = up * (side0 > 0.0f ? 1.0f : -1.0f);
                    weights = at;
                }
            }
        }
        if (!through) {
            if (d >= gap) return false;
            normal = d > 1e-9f ? off * (1.0f / d) : up * (side1 >= 0.0f ? 1.0f : -1.0f);
        }
        // Nearer than the gap at rest -- a fine mesh, a fold made so -- kept
        // no nearer than they were; in one place at rest, the lips of a tear.
        Vec3 restWeights;
        const Vec3 restNear = nearestOnTriangle(rest_[i], rest_[a], rest_[b], rest_[c], restWeights);
        const float apart = std::min(gap, length(rest_[i] - restNear));
        if (apart <= 1e-6f || (!through && d >= apart)) return false;
        k.order = (static_cast<uint64_t>(i) << 31) | t;
        k.points = {i, a, b, c};
        k.shares = {1.0f, -weights.x, -weights.y, -weights.z};
        k.normal = normal;
        k.gap = apart;
        return true;
    };
    // Two edges with no point in common, likewise: nearer than the gap, or
    // gone through each other -- each kept on the side it came from.
    auto edgeEdge = [&](uint32_t e, uint32_t f, Contact& k) {
        const uint32_t a = edges_[2 * e], b = edges_[2 * e + 1], c = edges_[2 * f], d = edges_[2 * f + 1];
        float s1 = 0.0f, t1 = 0.0f;
        nearestOnSegments(x_[a], x_[b], x_[c], x_[d], s1, t1);
        const Vec3 off = (x_[a] + (x_[b] - x_[a]) * s1) - (x_[c] + (x_[d] - x_[c]) * t1);
        const float dist = length(off);
        float s0 = 0.0f, t0 = 0.0f;
        nearestOnSegments(prev_[a], prev_[b], prev_[c], prev_[d], s0, t0);
        const Vec3 off0 = (prev_[a] + (prev_[b] - prev_[a]) * s0) - (prev_[c] + (prev_[d] - prev_[c]) * t0);
        const float dist0 = length(off0);
        bool through = false;
        Vec3 normal;
        if (dist0 > 1e-7f && s0 > 0.0f && s0 < 1.0f && t0 > 0.0f && t0 < 1.0f && s1 > 0.0f && s1 < 1.0f && t1 > 0.0f &&
            t1 < 1.0f) {
            const Vec3 was = off0 * (1.0f / dist0);
            const Vec3 now = (x_[a] + (x_[b] - x_[a]) * s0) - (x_[c] + (x_[d] - x_[c]) * t0);
            if (dot(now, was) < 0.0f) {
                through = true;
                normal = was;
                s1 = s0;
                t1 = t0;
            }
        }
        if (!through) {
            if (dist >= gap) return false;
            if (dist > 1e-9f) {
                normal = off * (1.0f / dist);
            } else if (dist0 > 1e-9f) {
                normal = off0 * (1.0f / dist0);
            } else {
                return false;
            }
        }
        float rs = 0.0f, rt = 0.0f;
        nearestOnSegments(rest_[a], rest_[b], rest_[c], rest_[d], rs, rt);
        const float restDist = length((rest_[a] + (rest_[b] - rest_[a]) * rs) - (rest_[c] + (rest_[d] - rest_[c]) * rt));
        const float apart = std::min(gap, restDist);
        if (apart <= 1e-6f || (!through && dist >= apart)) return false;
        k.order = (1ull << 62) | (static_cast<uint64_t>(e) << 31) | f;
        k.points = {a, b, c, d};
        k.shares = {1.0f - s1, s1, -(1.0f - t1), -t1};
        k.normal = normal;
        k.gap = apart;
        return true;
    };

    std::vector<Contact> contacts;
    std::mutex found;
    pg::parallelFor(pointPairs.size(), 512, [&](size_t begin, size_t end) {
        std::vector<Contact> mine;
        for (size_t k = begin; k < end; ++k) {
            Contact c;
            if (pointTriangle(pointPairs[k].first, pointPairs[k].second, c)) mine.push_back(c);
        }
        std::lock_guard<std::mutex> lock(found);
        contacts.insert(contacts.end(), mine.begin(), mine.end());
    });
    pg::parallelFor(edgePairs.size(), 512, [&](size_t begin, size_t end) {
        std::vector<Contact> mine;
        for (size_t k = begin; k < end; ++k) {
            Contact c;
            if (edgeEdge(edgePairs[k].first, edgePairs[k].second, c)) mine.push_back(c);
        }
        std::lock_guard<std::mutex> lock(found);
        contacts.insert(contacts.end(), mine.begin(), mine.end());
    });

    // Pushed apart one after another, in the same order on any number of
    // threads; held back as they slide past each other, by friction.
    std::sort(contacts.begin(), contacts.end(), [](const Contact& p, const Contact& q) { return p.order < q.order; });
    const float mu = s.friction;
    for (const Contact& k : contacts) {
        Vec3 between;
        float weight = 0.0f;
        for (int j = 0; j < 4; ++j) {
            const uint32_t q = k.points[static_cast<size_t>(j)];
            const float share = k.shares[static_cast<size_t>(j)];
            between += x_[q] * share;
            weight += w_[q] * share * share;
        }
        if (weight <= 0.0f) continue;
        const float c = dot(between, k.normal) - k.gap;
        if (c >= 0.0f) continue;
        const float lambda = -c / weight;
        for (int j = 0; j < 4; ++j) {
            const uint32_t q = k.points[static_cast<size_t>(j)];
            x_[q] += k.normal * (w_[q] * k.shares[static_cast<size_t>(j)] * lambda);
        }
        if (mu <= 0.0f) continue;
        Vec3 moved;
        for (int j = 0; j < 4; ++j) {
            const uint32_t q = k.points[static_cast<size_t>(j)];
            moved += (x_[q] - prev_[q]) * k.shares[static_cast<size_t>(j)];
        }
        const Vec3 along = moved - k.normal * dot(moved, k.normal);
        const float slide = length(along);
        if (slide < 1e-12f) continue;
        const float hold = std::min(slide, mu * -c) / weight;
        const Vec3 way = along * (1.0f / slide);
        for (int j = 0; j < 4; ++j) {
            const uint32_t q = k.points[static_cast<size_t>(j)];
            x_[q] = x_[q] - way * (w_[q] * k.shares[static_cast<size_t>(j)] * hold);
        }
    }
}

float ClothSolver::mergedCone(size_t a, size_t b, Vec3& axis) const {
    constexpr float kAnyWay = 4.0f;
    if (coneAngle_[a] >= kAnyWay || coneAngle_[b] >= kAnyWay) {
        axis = Vec3();
        return kAnyWay;
    }
    const Vec3 sum = coneAxis_[a] + coneAxis_[b];
    const float len = length(sum);
    if (len < 1e-6f) {
        axis = Vec3();
        return kAnyWay;
    }
    axis = sum * (1.0f / len);
    const float wa = std::acos(std::clamp(dot(axis, coneAxis_[a]), -1.0f, 1.0f)) + coneAngle_[a];
    const float wb = std::acos(std::clamp(dot(axis, coneAxis_[b]), -1.0f, 1.0f)) + coneAngle_[b];
    return std::max(wa, wb);
}

bool ClothSolver::sharePoints(size_t a, size_t b) const {
    // A point of a's that b has too: one of its places in b's stretch of the
    // tree.
    const uint32_t first = faceNodes_[b].first, last = first + faceNodes_[b].count;
    for (uint32_t k = shareStart_[a]; k < shareStart_[a + 1]; ++k) {
        const uint32_t q = shared_[k];
        const uint32_t* at = std::lower_bound(places_.data() + placeStart_[q], places_.data() + placeStart_[q + 1], first);
        if (at != places_.data() + placeStart_[q + 1] && *at < last) return true;
    }
    return false;
}

void ClothSolver::collideFaces(float h, std::vector<Vec3>& took, std::vector<Vec3>& turned) {
    const ClothSettings& s = scene_.solver;
    const float r = s.thickness, mu = s.friction;
    const size_t edges = edges_.size() / 2, count = tris_.size() / 3;
    // A place on an edge or a face that went into an object: its corners,
    // how much of each it is, where it was and how far in, which way out.
    struct Touch {
        uint64_t order = 0;
        std::array<uint32_t, 3> points{};
        std::array<float, 3> weights{};
        uint32_t collider = 0;
        Vec3 at, normal;
        float distance = 0.0f;
    };
    std::vector<Touch> touches;
    std::mutex found;
    for (size_t c = 0; c < shapes_.size(); ++c) {
        const ShapeInstance& shape = shapes_[c];
        const Vec3 middle = shape.center();
        const Vec3 size = scene_.colliders[c].size;
        const float reach = 0.5f * length(size) + r;
        // The box round it, as it is turned, Thickness wider.
        const Rotation& turn = shape.turn();
        const Vec3 half = glm::abs(turn.x) * shape.half().x + glm::abs(turn.y) * shape.half().y +
                          glm::abs(turn.z) * shape.half().z + Vec3(r);
        const Vec3 boxLo = middle - half, boxHi = middle + half;
        // As finely as its thinnest side: a rod between two points is found.
        const float thin = std::max(std::min({size.x, size.y, size.z}), 2.0f * r);
        // The edges: their places nearest the object, found along them and
        // then between the nearest two -- not their ends, which are points.
        pg::parallelFor(edges, 512, [&](size_t begin, size_t end) {
            std::vector<Touch> mine;
            for (size_t e = begin; e < end; ++e) {
                const uint32_t a = edges_[2 * e], b = edges_[2 * e + 1];
                if (w_[a] + w_[b] <= 0.0f) continue;
                if (!overlaps(glm::min(x_[a], x_[b]), glm::max(x_[a], x_[b]), boxLo, boxHi)) continue;
                const Vec3 xa = x_[a], along = x_[b] - x_[a];
                const float len2 = dot(along, along);
                if (len2 < 1e-12f) continue;
                const float toMiddle = std::clamp(dot(middle - xa, along) / len2, 0.0f, 1.0f);
                if (length(xa + along * toMiddle - middle) > reach) continue;
                const float len = std::sqrt(len2);
                const int samples = std::clamp(static_cast<int>(std::ceil(len / (0.5f * thin))), 1, 16);
                const float spacing = 1.0f / static_cast<float>(samples + 1);
                float best = 1e30f, bestAt = spacing;
                for (int j = 1; j <= samples; ++j) {
                    const float u = static_cast<float>(j) * spacing;
                    const float d = shape.distance(xa + along * u);
                    if (d < best) {
                        best = d;
                        bestAt = u;
                    }
                }
                // No nearer anywhere between them than half the way between
                // two -- the distance changes no faster than the way along.
                if (best - 0.5f * spacing * len >= r) continue;
                float lo = std::max(bestAt - spacing, 0.0f), hi = std::min(bestAt + spacing, 1.0f);
                for (int it = 0; it < 6; ++it) {
                    const float u1 = lo + (hi - lo) * 0.382f, u2 = lo + (hi - lo) * 0.618f;
                    if (shape.distance(xa + along * u1) < shape.distance(xa + along * u2)) {
                        hi = u2;
                    } else {
                        lo = u1;
                    }
                }
                const float u = 0.5f * (lo + hi);
                if (u < 0.02f || u > 0.98f) continue;
                const Vec3 at = xa + along * u;
                const float d = shape.distance(at);
                if (d >= r) continue;
                Touch t;
                t.order = (static_cast<uint64_t>(c) << 40) | e;
                t.points = {a, b, b};
                t.weights = {1.0f - u, u, 0.0f};
                t.collider = static_cast<uint32_t>(c);
                t.at = at;
                t.normal = shape.normal(at);
                t.distance = d;
                mine.push_back(t);
            }
            std::lock_guard<std::mutex> lock(found);
            touches.insert(touches.end(), mine.begin(), mine.end());
        });
        // The faces: where each is nearest the object's middle, inside it --
        // a ball smaller than a face does not go through it.
        pg::parallelFor(count, 512, [&](size_t begin, size_t end) {
            std::vector<Touch> mine;
            for (size_t f = begin; f < end; ++f) {
                const uint32_t a = tris_[3 * f], b = tris_[3 * f + 1], q = tris_[3 * f + 2];
                if (w_[a] + w_[b] + w_[q] <= 0.0f) continue;
                if (!overlaps(glm::min(glm::min(x_[a], x_[b]), x_[q]), glm::max(glm::max(x_[a], x_[b]), x_[q]), boxLo, boxHi)) {
                    continue;
                }
                Vec3 weights;
                const Vec3 at = nearestOnTriangle(middle, x_[a], x_[b], x_[q], weights);
                if (length(at - middle) > reach) continue;
                if (weights.x < 0.05f || weights.y < 0.05f || weights.z < 0.05f) continue;
                const float d = shape.distance(at);
                if (d >= r) continue;
                Touch t;
                t.order = (static_cast<uint64_t>(c) << 40) | (1ull << 39) | f;
                t.points = {a, b, q};
                t.weights = {weights.x, weights.y, weights.z};
                t.collider = static_cast<uint32_t>(c);
                t.at = at;
                t.normal = shape.normal(at);
                t.distance = d;
                mine.push_back(t);
            }
            std::lock_guard<std::mutex> lock(found);
            touches.insert(touches.end(), mine.begin(), mine.end());
        });
    }
    if (touches.empty()) return;
    // Out, one after another, in the same order on any number of threads;
    // held back along the object by friction; a piece given what it gave.
    std::sort(touches.begin(), touches.end(), [](const Touch& p, const Touch& q) { return p.order < q.order; });
    for (const Touch& t : touches) {
        const size_t c = t.collider;
        Vec3 now;
        float weight = 0.0f;
        for (int j = 0; j < 3; ++j) {
            const size_t k = static_cast<size_t>(j);
            now += x_[t.points[k]] * t.weights[k];
            weight += w_[t.points[k]] * t.weights[k] * t.weights[k];
        }
        if (weight <= 0.0f) continue;
        const float depth = r - (t.distance + dot(now - t.at, t.normal));
        if (depth <= 0.0f) continue;
        const Vec3 moving = drift_.empty() ? Vec3() : kick_[c] + cross(twist_[c], now - shapes_[c].center());
        const Vec3 surface = scene_.colliders[c].velocityAt(now) + moving;
        std::array<Vec3, 3> was;
        for (int j = 0; j < 3; ++j) was[static_cast<size_t>(j)] = x_[t.points[static_cast<size_t>(j)]];
        const float lambda = depth / weight;
        for (int j = 0; j < 3; ++j) {
            const size_t k = static_cast<size_t>(j);
            x_[t.points[k]] += t.normal * (w_[t.points[k]] * t.weights[k] * lambda);
        }
        if (mu > 0.0f) {
            Vec3 moved = surface * -h;
            for (int j = 0; j < 3; ++j) {
                const size_t k = static_cast<size_t>(j);
                moved += (x_[t.points[k]] - prev_[t.points[k]]) * t.weights[k];
            }
            const Vec3 along = moved - t.normal * dot(moved, t.normal);
            const float slide = length(along);
            if (slide > 1e-12f) {
                const float hold = std::min(slide, mu * depth) / weight;
                const Vec3 way = along * (1.0f / slide);
                for (int j = 0; j < 3; ++j) {
                    const size_t k = static_cast<size_t>(j);
                    x_[t.points[k]] = x_[t.points[k]] - way * (w_[t.points[k]] * t.weights[k] * hold);
                }
            }
        }
        // Put out of it, its corners do not spring off it.
        for (int j = 0; j < 3; ++j) {
            const size_t k = static_cast<size_t>(j);
            const uint32_t q = t.points[k];
            if (t.weights[k] < 0.25f || leans_[q]) continue;
            leans_[q] = 1;
            leanNormal_[q] = t.normal;
            leanVelocity_[q] = surface;
        }
        // A piece: what the points took of their momentum, it gave.
        if (scene_.colliders[c].mass > 0.0f && !took.empty()) {
            Vec3 given;
            for (int j = 0; j < 3; ++j) {
                const size_t k = static_cast<size_t>(j);
                const uint32_t q = t.points[k];
                if (w_[q] > 0.0f && (j == 0 || t.points[k] != t.points[k - 1])) given += (x_[q] - was[k]) * (1.0f / (w_[q] * h));
            }
            took[c] += given;
            turned[c] += cross(now - shapes_[c].center(), given);
        }
    }
}

void ClothSolver::collide(float h) {
    const ClothSettings& s = scene_.solver;
    const float r = s.thickness;
    const float mu = s.friction;
    const size_t n = x_.size();
    // Out of what it went into, and held back along it by friction: no more
    // than it went in, against how that moves.
    auto rub = [&](size_t i, const Vec3& normal, float depth, const Vec3& surfaceVelocity) {
        const Vec3 moved = x_[i] - prev_[i] - surfaceVelocity * h;
        const Vec3 along = moved - normal * dot(moved, normal);
        const float slide = length(along);
        if (slide < 1e-12f) return;
        x_[i] = x_[i] - along * std::min(1.0f, mu * depth / slide);
    };
    pg::parallelFor(n, 512, [&](size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i) {
            touched_[i] = -1;
            leans_[i] = 0;
            if (w_[i] <= 0.0f) continue;
            bool held = false;  // by the floor, or by what does not give
            if (s.floor && x_[i].y < r) {
                const float depth = r - x_[i].y;
                x_[i].y = r;
                rub(i, Vec3(0.0f, 1.0f, 0.0f), depth, Vec3());
                held = true;
                leans_[i] = 1;
                leanNormal_[i] = Vec3(0.0f, 1.0f, 0.0f);
                leanVelocity_[i] = Vec3();
            }
            for (size_t c = 0; c < shapes_.size(); ++c) {
                // Far outside the ball round it: not near it.
                const Vec3 off = x_[i] - shapes_[c].center();
                const float reach = 0.5f * length(scene_.colliders[c].size) + r;
                if (dot(off, off) > reach * reach) continue;
                const float d = shapes_[c].distance(x_[i]);
                if (d >= r) continue;
                const Vec3 normal = shapes_[c].normal(x_[i]);
                const Vec3 was = x_[i];
                x_[i] += normal * (r - d);
                const Vec3 moving = drift_.empty() ? Vec3() : kick_[c] + cross(twist_[c], x_[i] - shapes_[c].center());
                const Vec3 surface = scene_.colliders[c].velocityAt(x_[i]) + moving;
                rub(i, normal, r - d, surface);
                leans_[i] = 1;
                leanNormal_[i] = normal;
                leanVelocity_[i] = surface;
                // A piece: how far it moved the point, what the point's
                // momentum it took.
                if (scene_.colliders[c].mass > 0.0f) {
                    // Between two pieces: they meet each other in the RBD
                    // Solver, the cloth pushes neither.
                    if (touched_[i] >= 0 && touched_[i] != static_cast<int32_t>(c)) held = true;
                    pushed_[i] = (touched_[i] == static_cast<int32_t>(c) ? pushed_[i] : Vec3()) + (x_[i] - was);
                    touched_[i] = static_cast<int32_t>(c);
                } else {
                    held = true;
                }
            }
            // Caught between a piece and the ground, the ground holds the
            // piece up -- the RBD Solver's floor -- not the cloth.
            if (held) touched_[i] = -1;
        }
    });
    // The edges and the faces too, a thin rod caught between two points.
    std::vector<Vec3> took(drift_.empty() ? 0 : shapes_.size()), turned(took.size());
    if (s.faces && !shapes_.empty()) collideFaces(h, took, turned);
    // The pieces that give: what the points they pushed took of their
    // momentum -- in order, point by point, the same on any number of
    // threads -- slows them and turns them; they go on so.
    if (drift_.empty()) return;
    for (size_t i = 0; i < n; ++i) {
        if (touched_[i] < 0) continue;
        const size_t c = static_cast<size_t>(touched_[i]);
        const Vec3 given = pushed_[i] * (1.0f / (w_[i] * h));
        took[c] += given;
        turned[c] += cross(x_[i] - shapes_[c].center(), given);
        touched_[i] = -1;
    }
    for (size_t c = 0; c < shapes_.size(); ++c) {
        const Collider& o = scene_.colliders[c];
        if (o.mass <= 0.0f || (took[c] == Vec3() && turned[c] == Vec3())) continue;
        // A body its size, as a box is: the mean of its moments of inertia.
        const float inertia = std::max(o.mass * dot(o.size, o.size) / 18.0f, 1e-6f);
        kick_[c] = kick_[c] - took[c] * (1.0f / o.mass);
        twist_[c] = twist_[c] - turned[c] * (1.0f / inertia);
    }
}

void ClothSolver::step() {
    const ClothSettings& s = scene_.solver;
    size_t n = x_.size();
    const size_t colliders = scene_.colliders.size();
    const bool gives = std::any_of(scene_.colliders.begin(), scene_.colliders.end(), [](const Collider& c) { return c.mass > 0.0f; });
    drift_.assign(gives ? colliders : 0, Vec3());
    kick_.assign(drift_.size(), Vec3());
    twist_.assign(drift_.size(), Vec3());
    if (n == 0) {
        ++frame_;
        time_ += s.timeStep;
        return;
    }
    const int steps = s.substeps;
    const float h = s.timeStep / static_cast<float>(steps);
    // Where the pins go: from where they are to where the geometry has them
    // at the end of the step.
    start_ = x_;
    target_ = start_;
    const Geometry* goal = scene_.target && scene_.geometry && scene_.target->pointCount() == scene_.geometry->pointCount()
                               ? scene_.target.get()
                               : scene_.geometry.get();
    if (goal) {
        const auto P = goal->positions();
        for (size_t i = 0; i < n; ++i) {
            if (pinned_[i]) target_[i] = P[origin_[i]];
        }
    }
    // The gas's flow at each triangle, once a step.
    const size_t count = tris_.size() / 3;
    if (air_ && s.airDrag > 0.0f) {
        for (size_t t = 0; t < count; ++t) {
            const Vec3 middle = (x_[tris_[3 * t]] + x_[tris_[3 * t + 1]] + x_[tris_[3 * t + 2]]) * (1.0f / 3.0f);
            airOfTri_[t] = air_(middle);
        }
    } else {
        std::fill(airOfTri_.begin(), airOfTri_.end(), Vec3());
    }
    // Still air too holds it back: a sheet falls no faster than a couple of
    // metres a second, flat.
    const bool blown = s.airDrag > 0.0f && count > 0;
    std::vector<Vec3> accel(n);
    touched_.assign(n, -1);
    pushed_.assign(n, Vec3());
    leans_.assign(n, 0);
    leanNormal_.assign(n, Vec3());
    leanVelocity_.assign(n, Vec3());
    const float fade = 1.0f / (1.0f + s.damping * h);
    for (int k = 0; k < steps; ++k) {
        const float t = static_cast<float>(k + 1) / static_cast<float>(steps);
        if (blown) aero(accel);
        pg::parallelFor(n, 2048, [&](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i) {
                prev_[i] = x_[i];
                if (pinned_[i]) {
                    x_[i] = start_[i] + (target_[i] - start_[i]) * t;
                    continue;
                }
                v_[i] += (s.gravity + accel[i]) * h;
                x_[i] += v_[i] * h;
            }
        });
        solveLinks(h);
        if (s.tear > 0.0f && tear()) {
            // Torn: points split off, the links made again.
            ++relinks_;
            n = x_.size();
            accel.resize(n);
            touched_.resize(n, -1);
            pushed_.resize(n);
            leans_.resize(n, 0);
            leanNormal_.resize(n);
            leanVelocity_.resize(n);
        }
        if (!balloons_.empty()) solveBalloons(h);
        if (s.shape > 0.0f) matchShapes(h);
        if (s.selfCollision) {
            const bool points = count > 0, faces = s.faces && !edges_.empty();
            if (points || faces) selfCollide(points, faces);
        }
        // What moves -- the pieces, animated objects -- where it is at this
        // substep: coming to where the scene has it at the end of the step,
        // as fast as it goes, not jumping there at the first.
        for (size_t c = 0; c < shapes_.size(); ++c) {
            const Collider& o = scene_.colliders[c];
            if (!drift_.empty()) drift_[c] += kick_[c] * h;
            if (!o.moves() && (drift_.empty() || drift_[c] == Vec3())) continue;
            const Vec3 back = o.velocity * (s.timeStep * (1.0f - t));
            shapes_[c] = ShapeInstance(o.shape, o.center - back + (drift_.empty() ? Vec3() : drift_[c]), o.rotation,
                                       o.size, o.mesh);
        }
        collide(h);
        pg::parallelFor(n, 2048, [&](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i) {
                v_[i] = (x_[i] - prev_[i]) * (fade / h);
                // Put out of what it went into, it does not spring off it:
                // no faster away from it than it goes -- how far it was put
                // out is no speed (cloth hardly bounces).
                if (!leans_[i]) continue;
                const Vec3 off = v_[i] - leanVelocity_[i];
                const float away = dot(off, leanNormal_[i]);
                if (away > 0.0f) v_[i] = v_[i] - leanNormal_[i] * away;
            }
        });
        time_ += h;
    }
    time_ = static_cast<float>(frame_ + 1) * s.timeStep;
    ++frame_;
    // The shape it holds has given way: its links as long as they are in it
    // -- the dent stays.
    if (s.shape > 0.0f && s.plasticity > 0.0f) {
        for (Link& l : links_) {
            l.rest = length(shapeRest_[l.a] - shapeRest_[l.b]);
            if (l.limit > 0.0f) l.limit = l.rest * (1.0f + l.give);
        }
    }
    // The pieces that gave: where the cloth left them, as against where
    // they would have gone.
    reactions_.clear();
    for (size_t c = 0; c < drift_.size(); ++c) {
        const int32_t piece = scene_.colliders[c].piece;
        if (piece < 0 || (drift_[c] == Vec3() && kick_[c] == Vec3() && twist_[c] == Vec3())) continue;
        reactions_.push_back({static_cast<uint32_t>(piece), drift_[c], kick_[c], twist_[c]});
    }
}

ClothFrame ClothSolver::capture() const {
    ClothFrame f;
    f.geometry = scene_.geometry;
    f.positions = x_;
    if (tornPoints() > 0 || std::any_of(ropeCut_.begin(), ropeCut_.end(), [](uint8_t c) { return c != 0; })) {
        const size_t n0 = x_.size() - tornPoints();
        f.copies.assign(origin_.begin() + static_cast<std::ptrdiff_t>(n0), origin_.end());
        if (!f.copies.empty()) f.corners = corner_;
        for (size_t v = 0; v < ropeCut_.size(); ++v) {
            if (ropeCut_[v]) f.cuts.push_back(static_cast<uint32_t>(v));
        }
    }
    f.velocities.reserve(3 * v_.size());
    for (const Vec3& v : v_) {
        for (int a = 0; a < 3; ++a) f.velocities.push_back(halfFromFloat(v[a]));
    }
    return f;
}

// --- the state -------------------------------------------------------------------------

void ClothSolver::saveState(StateWriter& out) const {
    out.pod(static_cast<int32_t>(frame_));
    out.pod(time_);
    out.list(x_);
    out.list(v_);
    // Torn: which point each corner is on, where the points came from,
    // what has parted.
    out.list(corner_);
    out.list(origin_);
    out.list(ropeCut_);
    out.list(cut_);
    out.list(reactions_);
    // The shape it holds, where it has given way.
    const bool gave = scene_.solver.shape > 0.0f && scene_.solver.plasticity > 0.0f;
    out.list(gave ? shapeRest_ : std::vector<Vec3>());
}

bool ClothSolver::loadState(StateReader& in) {
    int32_t frame = 0;
    float time = 0.0f;
    std::vector<Vec3> x, v;
    std::vector<uint32_t> corners, origins;
    std::vector<uint8_t> ropeCuts;
    std::vector<uint64_t> cuts;
    std::vector<Reaction> reactions;
    std::vector<Vec3> shape;
    if (!in.pod(frame) || !in.pod(time) || !in.list(x) || !in.list(v) || !in.list(corners) || !in.list(origins) ||
        !in.list(ropeCuts) || !in.list(cuts) || !in.list(reactions) || !in.list(shape)) {
        return false;
    }
    const size_t n0 = x_.size() - tornPoints();
    const size_t n = x.size();
    bool fits = n >= n0 && v.size() == n && origins.size() == n && corners.size() == corner_.size() &&
                ropeCuts.size() == ropeCut_.size() && std::is_sorted(cuts.begin(), cuts.end());
    for (size_t i = 0; i < n && fits; ++i) fits = origins[i] < n0 && (i >= n0 || origins[i] == i);
    for (size_t k = 0; k < corners.size() && fits; ++k) fits = corners[k] < n;
    fits = fits && (shape.empty() || shape.size() == n);
    if (!fits) return in.fail();
    // The points torn off, as their own are at rest.
    for (size_t i = x_.size(); i < n; ++i) {
        const uint32_t o = origins[i];
        rest_.push_back(rest_[o]);
        pinned_.push_back(pinned_[o]);
    }
    rest_.resize(n);
    pinned_.resize(n);
    shapeRest_ = shape.empty() ? rest_ : std::move(shape);
    x_ = std::move(x);
    v_ = std::move(v);
    prev_ = x_;
    start_ = x_;
    target_ = x_;
    corner_ = std::move(corners);
    origin_ = std::move(origins);
    ropeCut_ = std::move(ropeCuts);
    cut_ = std::move(cuts);
    reactions_ = std::move(reactions);
    connect();
    frame_ = frame;
    time_ = time;
    return true;
}

}  // namespace pg::sim
