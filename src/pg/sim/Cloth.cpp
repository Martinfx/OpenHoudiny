#include "pg/sim/Cloth.h"

#include "pg/core/Half.h"
#include "pg/core/Parallel.h"
#include "pg/sim/Shared.h"
#include "pg/sim/State.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <map>
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
    const AttributeArray* pin = geo->points().find("pin");
    const AttributeArray* tear = geo->points().find("tear");
    for (size_t i = 0; i < n; ++i) {
        pinned_[i] = pointNumber(pin, i, 0.0f) > 0.5f ? 1 : 0;
        tearOf_[i] = std::max(pointNumber(tear, i, 1.0f), 0.0f);
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
        const float rest = length(rest_[a] - rest_[b]);
        const float give = s.tear * std::min(tearOf_[origin_[a]], tearOf_[origin_[b]]);
        links_.push_back({a, b, rest, compliance, tears && s.tear > 0.0f ? rest * (1.0f + give) : 0.0f, corner});
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
    for (size_t i = 0; i < n; ++i) {
        double m = mass[i];
        if (given) {
            const double whole = pointNumber(given, origin_[i], 0.0f);
            m = ofOrigin[origin_[i]] > 0.0 ? whole * mass[i] / ofOrigin[origin_[i]] : whole;
        }
        w_[i] = pinned_[i] ? 0.0f : static_cast<float>(1.0 / std::max(m, 1e-6));
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

void ClothSolver::selfCollide() {
    const size_t n = x_.size();
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
    // The pieces that give: what the points they pushed took of their
    // momentum -- in order, point by point, the same on any number of
    // threads -- slows them and turns them; they go on so.
    if (drift_.empty()) return;
    std::vector<Vec3> took(shapes_.size()), turned(shapes_.size());
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
        if (s.selfCollision && count > 0) selfCollide();
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
}

bool ClothSolver::loadState(StateReader& in) {
    int32_t frame = 0;
    float time = 0.0f;
    std::vector<Vec3> x, v;
    std::vector<uint32_t> corners, origins;
    std::vector<uint8_t> ropeCuts;
    std::vector<uint64_t> cuts;
    std::vector<Reaction> reactions;
    if (!in.pod(frame) || !in.pod(time) || !in.list(x) || !in.list(v) || !in.list(corners) || !in.list(origins) ||
        !in.list(ropeCuts) || !in.list(cuts) || !in.list(reactions)) {
        return false;
    }
    const size_t n0 = x_.size() - tornPoints();
    const size_t n = x.size();
    bool fits = n >= n0 && v.size() == n && origins.size() == n && corners.size() == corner_.size() &&
                ropeCuts.size() == ropeCut_.size() && std::is_sorted(cuts.begin(), cuts.end());
    for (size_t i = 0; i < n && fits; ++i) fits = origins[i] < n0 && (i >= n0 || origins[i] == i);
    for (size_t k = 0; k < corners.size() && fits; ++k) fits = corners[k] < n;
    if (!fits) return in.fail();
    // The points torn off, as their own are at rest.
    for (size_t i = x_.size(); i < n; ++i) {
        const uint32_t o = origins[i];
        rest_.push_back(rest_[o]);
        pinned_.push_back(pinned_[o]);
    }
    rest_.resize(n);
    pinned_.resize(n);
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
