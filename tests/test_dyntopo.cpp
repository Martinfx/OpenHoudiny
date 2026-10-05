//
// Dynamic topology for the sculpting brush (Dyntopo.h): the mesh made finer
// under the dabs and coarser where it is finer than they need -- a surface
// still, its borders where they were, a closed piece closed -- with its
// attributes and groups carried; the Sculptor going on from where it got
// to, to the bit; the Sculpt node's parameters.
//
#include "pg/core/CookEngine.h"
#include "pg/core/Dyntopo.h"
#include "pg/core/Graph.h"
#include "pg/core/Sculpt.h"
#include "pg/nodes/Nodes.h"

#include "test_framework.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <utility>

using namespace pg;

namespace {

/// A sheet `size` m square of `n` x `n` quads, flat on the floor.
GeometryPtr sheetOf(float size, int n) {
    registerBuiltinNodes();
    Graph g;
    Node* grid = g.create("grid", "grid");
    grid->setFloat("sizex", size);
    grid->setFloat("sizez", size);
    grid->setInt("rows", n + 1);
    grid->setInt("cols", n + 1);
    CookEngine engine;
    return std::make_shared<Geometry>(*engine.cook(*grid, CookContext{}));
}

/// A ball of radius 1, `rows` rows of quads round it.
GeometryPtr ballOf(int rows) {
    registerBuiltinNodes();
    Graph g;
    Node* ball = g.create("sphere", "ball");
    ball->setFloat("radius", 1.0f);
    ball->setInt("rows", rows);
    ball->setInt("columns", 2 * rows);
    CookEngine engine;
    return std::make_shared<Geometry>(*engine.cook(*ball, CookContext{}));
}

SculptDab dab(SculptDab::Tool tool, const Vec3& at, const Vec3& normal, float radius, float strength) {
    SculptDab d;
    d.tool = tool;
    d.at = at;
    d.normal = normal;
    d.radius = radius;
    d.strength = strength;
    return d;
}

/// How many triangles each edge is a side of; false if a primitive is not one.
bool sidesOf(const Geometry& geo, std::map<std::pair<uint32_t, uint32_t>, int>& sides) {
    sides.clear();
    for (size_t p = 0; p < geo.primitiveCount(); ++p) {
        const auto pts = geo.primitivePoints(p);
        if (pts.size() != 3 || !geo.primitiveClosed(p)) return false;
        for (size_t k = 0; k < 3; ++k) {
            const uint32_t a = pts[k], b = pts[(k + 1) % 3];
            ++sides[{std::min(a, b), std::max(a, b)}];
        }
    }
    return true;
}

float segmentDistance(const Vec3& p, const Vec3& a, const Vec3& b) {
    const Vec3 ab = b - a;
    const float t = std::clamp(dot(p - a, ab) / std::max(dot(ab, ab), 1e-20f), 0.0f, 1.0f);
    return length(p - (a + ab * t));
}

}  // namespace

TEST(dyntopo_makes_the_mesh_finer_under_the_dabs_and_leaves_the_rest) {
    // A 2 m sheet of 4 x 4 quads, dabs along a line that do not move it:
    // under them no edge is longer than the detail; the quads they come
    // nowhere near are the two triangles they were cut into.
    Geometry geo(*sheetOf(2.0f, 4));
    auto part = geo.primitives().create("part", AttrType::Int).write<int32_t>();
    for (size_t p = 0; p < geo.primitiveCount(); ++p) part[p] = static_cast<int32_t>(p);
    const Geometry source = geo;
    Dyntopo d;
    d.subdivide = true;
    std::vector<SculptDab> dabs;
    for (int i = 0; i < 5; ++i) {
        dabs.push_back(dab(SculptDab::Tool::Push, Vec3(-0.5f + 0.25f * static_cast<float>(i), 0.0f, -0.3f),
                           Vec3(0.0f, 1.0f, 0.0f), 0.2f, 0.0f));
    }
    sculpt(geo, dabs, Falloff::Smooth, d);
    std::map<std::pair<uint32_t, uint32_t>, int> sides;
    CHECK(sidesOf(geo, sides));
    CHECK(geo.primitiveCount() > 200u);
    const auto P = geo.positions();
    float longest = 0.0f;
    for (const auto& [e, n] : sides) {
        CHECK(n <= 2);
        for (const SculptDab& s : dabs) {
            if (segmentDistance(s.at, P[e.first], P[e.second]) < s.radius) longest = std::max(longest, length(P[e.first] - P[e.second]));
        }
    }
    CHECK(longest > 0.0f);
    CHECK(longest <= d.detail * 0.2f + 1e-6f);
    // The row of quads furthest from the line: as they were cut.
    const auto Part = geo.primitives().find("part")->read<int32_t>();
    for (size_t q = 0; q < source.primitiveCount(); ++q) {
        Vec3 middle;
        for (const uint32_t i : source.primitivePoints(q)) middle += source.positions()[i] * 0.25f;
        if (middle.z < 0.5f) continue;
        int triangles = 0;
        for (size_t p = 0; p < geo.primitiveCount(); ++p) {
            if (Part[p] != static_cast<int32_t>(q)) continue;
            ++triangles;
            for (const uint32_t i : geo.primitivePoints(p)) CHECK(i < source.pointCount());
        }
        CHECK_EQ(triangles, 2);
    }
    // The points it had come first, where they were.
    for (size_t i = 0; i < source.pointCount(); ++i) CHECK(P[i] == source.positions()[i]);
}

TEST(dyntopo_carries_the_attributes_and_the_groups) {
    // A colour that is where each point is, texture coordinates that are
    // where each corner is, a number for each quad, a group of points and
    // one of quads: the new points and corners are blended to where they
    // are, to the bit; each triangle is in the quad it was cut from.
    Geometry geo(*sheetOf(2.0f, 4));
    const auto P0 = geo.positions();
    auto Cd = geo.points().create("Cd", AttrType::Vec3).write<Vec3>();
    for (size_t i = 0; i < P0.size(); ++i) Cd[i] = P0[i];
    auto uv = geo.vertices().create("uv", AttrType::Vec2).write<glm::vec2>();
    for (size_t v = 0; v < geo.vertexCount(); ++v) uv[v] = glm::vec2(P0[geo.vertexPoint(v)].x, P0[geo.vertexPoint(v)].z);
    auto part = geo.primitives().create("part", AttrType::Int).write<int32_t>();
    for (size_t p = 0; p < geo.primitiveCount(); ++p) part[p] = static_cast<int32_t>(p);
    Group& left = geo.createGroup("left", AttrClass::Point);
    for (size_t i = 0; i < P0.size(); ++i) left.set(i, P0[i].x < -1e-6f);
    Group& first = geo.createGroup("first", AttrClass::Primitive);
    for (size_t p = 0; p < 4; ++p) first.set(p, true);
    const Geometry source = geo;

    Dyntopo d;
    d.subdivide = true;
    std::vector<SculptDab> dabs;
    for (int i = 0; i < 5; ++i) {
        const float t = static_cast<float>(i);
        dabs.push_back(dab(SculptDab::Tool::Push, Vec3(-0.6f + 0.3f * t, 0.0f, 0.2f * t - 0.4f), Vec3(0.0f, 1.0f, 0.0f),
                           0.3f, 0.0f));
    }
    sculpt(geo, dabs, Falloff::Smooth, d);
    CHECK(geo.pointCount() > 4 * source.pointCount());
    const auto P = geo.positions();
    const auto C = geo.points().find("Cd")->read<Vec3>();
    for (size_t i = 0; i < P.size(); ++i) CHECK(C[i] == P[i]);
    const auto U = geo.vertices().find("uv")->read<glm::vec2>();
    for (size_t v = 0; v < geo.vertexCount(); ++v) {
        const Vec3& p = P[geo.vertexPoint(v)];
        CHECK(U[v] == glm::vec2(p.x, p.z));
    }
    const auto Part = geo.primitives().find("part")->read<int32_t>();
    const Group* F = geo.findGroup("first");
    for (size_t p = 0; p < geo.primitiveCount(); ++p) {
        const auto pts = geo.primitivePoints(p);
        const Vec3 middle = (P[pts[0]] + P[pts[1]] + P[pts[2]]) * (1.0f / 3.0f);
        Vec3 lo(1e9f), hi(-1e9f);
        for (const uint32_t q : source.primitivePoints(static_cast<size_t>(Part[p]))) {
            lo = glm::min(lo, source.positions()[q]);
            hi = glm::max(hi, source.positions()[q]);
        }
        CHECK(middle.x >= lo.x - 1e-6f && middle.x <= hi.x + 1e-6f && middle.z >= lo.z - 1e-6f && middle.z <= hi.z + 1e-6f);
        CHECK_EQ(F->contains(p), Part[p] < 4);
    }
    // In the group where both ends of the edge it was made on were: on the
    // left of the line between the two halves, and all there was before.
    const Group* L = geo.findGroup("left");
    size_t in = 0;
    for (size_t i = 0; i < P.size(); ++i) {
        if (!L->contains(i)) continue;
        ++in;
        CHECK(P[i].x < 0.0f);
    }
    CHECK(in > source.findGroup("left")->memberCount());
}

TEST(dyntopo_makes_short_edges_one_and_keeps_the_border) {
    // A fine sheet, 1 cm a side, under dabs with a detail of 20 cm that do
    // not move it: edges shorter than 8 cm are made one point -- the sheet
    // flat still, every triangle facing up, the border where it was with
    // its corners, and no edge under the middle dab that short.
    const GeometryPtr fine = sheetOf(2.0f, 100);
    Geometry geo(*fine);
    Dyntopo d;
    d.collapse = true;
    d.constant = true;
    d.detail = 0.2f;
    std::vector<SculptDab> dabs;
    for (int i = 0; i < 3; ++i) {
        dabs.push_back(dab(SculptDab::Tool::Smooth, Vec3(0.4f * static_cast<float>(i) - 0.4f, 0.0f, 0.0f), Vec3(), 0.5f, 0.0f));
    }
    dabs.push_back(dab(SculptDab::Tool::Smooth, Vec3(1.0f, 0.0f, 1.0f), Vec3(), 0.4f, 0.0f));  // on a corner
    sculpt(geo, dabs, Falloff::Smooth, d);
    CHECK(geo.pointCount() < fine->pointCount() * 2 / 3);
    std::map<std::pair<uint32_t, uint32_t>, int> sides;
    CHECK(sidesOf(geo, sides));
    const auto P = geo.positions();
    for (size_t p = 0; p < geo.primitiveCount(); ++p) {
        const auto pts = geo.primitivePoints(p);
        CHECK(cross(P[pts[1]] - P[pts[0]], P[pts[2]] - P[pts[0]]).y > 0.0f);
    }
    float border = 0.0f, shortest = 1e9f;
    for (const auto& [e, n] : sides) {
        CHECK(n <= 2);
        const Vec3 &a = P[e.first], &b = P[e.second];
        if (n == 1) {
            // Along one side of the square.
            const bool alongX = std::fabs(std::fabs(a.z) - 1.0f) < 1e-6f && a.z == b.z;
            const bool alongZ = std::fabs(std::fabs(a.x) - 1.0f) < 1e-6f && a.x == b.x;
            CHECK(alongX || alongZ);
            border += length(a - b);
        }
        if (segmentDistance(Vec3(0.0f), a, b) < 0.3f) shortest = std::min(shortest, length(a - b));
        CHECK(a.y == 0.0f && b.y == 0.0f);
    }
    CHECK_NEAR(border, 8.0f, 1e-4f);
    CHECK(shortest >= kDyntopoShortest * d.detail);
    for (const Vec3 corner : {Vec3(1.0f, 0.0f, 1.0f), Vec3(-1.0f, 0.0f, 1.0f), Vec3(1.0f, 0.0f, -1.0f), Vec3(-1.0f, 0.0f, -1.0f)}) {
        CHECK(std::any_of(P.begin(), P.end(), [&](const Vec3& p) { return length(p - corner) < 1e-6f; }));
    }
}

namespace {

/// Dabs of every tool over a ball: pushed, pulled, smoothed, flattened,
/// grabbed, some with their mirror images joined to them.
std::vector<SculptDab> ballDabs(size_t count, uint32_t seed) {
    auto next = [&seed]() {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<float>(seed >> 8) / static_cast<float>(1u << 24);
    };
    std::vector<SculptDab> dabs;
    while (dabs.size() < count) {
        const Vec3 dir = normalize(Vec3(next() - 0.5f, next() - 0.5f, next() - 0.5f) + Vec3(1e-3f));
        const int kind = static_cast<int>(next() * 10.0f);
        SculptDab d = dab(kind < 4   ? SculptDab::Tool::Push
                          : kind < 6 ? SculptDab::Tool::Smooth
                          : kind < 8 ? SculptDab::Tool::Flatten
                                     : SculptDab::Tool::Grab,
                          dir, dir, 0.12f + 0.25f * next(), kind < 4 ? 1.6f * next() - 0.6f : next());
        d.move = dir * (0.15f * next());
        if (next() < 0.3f) {
            for (const SculptDab& both : mirroredDabs(d, Mirror::X)) dabs.push_back(both);
        } else {
            dabs.push_back(d);
        }
    }
    dabs.resize(count);
    if (dabs.back().joined) dabs.back().joined = false, dabs.back().tool = SculptDab::Tool::Push;
    return dabs;
}

Dyntopo everything() {
    Dyntopo d;
    d.subdivide = d.collapse = true;
    d.detail = 0.3f;
    return d;
}

}  // namespace

TEST(dyntopo_keeps_a_closed_surface_closed) {
    // A ball under three hundred dabs of every tool, finer and coarser
    // where they go: still a closed surface -- each edge a side of two
    // triangles -- of one piece with no handle (V - E + F = 2).
    const GeometryPtr ball = ballOf(12);
    Geometry geo(*ball);
    sculpt(geo, ballDabs(300, 11), Falloff::Smooth, everything());
    std::map<std::pair<uint32_t, uint32_t>, int> sides;
    CHECK(sidesOf(geo, sides));
    CHECK(geo.pointCount() > 3 * ball->pointCount());
    for (const auto& [e, n] : sides) CHECK_EQ(n, 2);
    CHECK_EQ(static_cast<long>(geo.pointCount()) - static_cast<long>(sides.size()) + static_cast<long>(geo.primitiveCount()), 2L);
    const auto P = geo.positions();
    for (const Vec3& p : P) CHECK(std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z));
    // Facing out, as the ball did, but where a dab dented it.
    size_t out = 0;
    for (size_t p = 0; p < geo.primitiveCount(); ++p) {
        const auto pts = geo.primitivePoints(p);
        out += dot(cross(P[pts[1]] - P[pts[0]], P[pts[2]] - P[pts[0]]), P[pts[0]] + P[pts[1]] + P[pts[2]]) > 0.0f ? 1 : 0;
    }
    CHECK(out > geo.primitiveCount() * 9 / 10);
}

TEST(dyntopo_sculptor_goes_on_from_where_it_got_to) {
    // Going on from the last cook -- more dabs, a grab that moves on, a dab
    // taken back -- makes what all of them from the start make, to the bit.
    const GeometryPtr source = ballOf(10);
    const Dyntopo d = everything();
    std::vector<SculptDab> dabs = ballDabs(120, 5);
    const auto whole = [&](std::span<const SculptDab> some) {
        Geometry geo(*source);
        sculpt(geo, some, Falloff::Smooth, d);
        return geo.hash();
    };
    const auto first = [&](size_t m) { return std::span<const SculptDab>(dabs.data(), m); };
    Sculptor sculptor;
    // No dab yet: the polygons are triangles already.
    const GeometryPtr none = sculptor.cook(source, {}, Falloff::Smooth, d);
    CHECK(none->primitiveCount() > source->primitiveCount());
    CHECK_EQ(none->hash(), whole({}));
    size_t had = 0;
    for (const size_t m : {1u, 2u, 3u, 40u, 41u, 100u, 120u}) {
        size_t at = m;
        while (at < dabs.size() && dabs[at].joined) ++at;  // a whole group
        const GeometryPtr got = sculptor.cook(source, first(at), Falloff::Smooth, d);
        CHECK_EQ(sculptor.reused(), had);
        CHECK_EQ(got->hash(), whole(first(at)));
        had = at;
    }
    // A grab that moves on: a dab more, then other again and again -- from
    // the mesh before it, kept.
    std::vector<SculptDab> grab = dabs;
    grab.push_back(dab(SculptDab::Tool::Grab, normalize(Vec3(0.3f, 1.0f, 0.2f)), Vec3(), 0.4f, 1.0f));
    for (int step = 1; step <= 3; ++step) {
        grab.back().move = Vec3(0.0f, 0.1f * static_cast<float>(step), 0.05f);
        const GeometryPtr got = sculptor.cook(source, grab, Falloff::Smooth, d);
        CHECK_EQ(sculptor.reused(), dabs.size());
        Geometry geo(*source);
        sculpt(geo, grab, Falloff::Smooth, d);
        CHECK_EQ(got->hash(), geo.hash());
    }
    // The grab taken back: from before it. Then another dab taken back --
    // not a grab, no mesh kept before it -- from the start.
    GeometryPtr got = sculptor.cook(source, dabs, Falloff::Smooth, d);
    CHECK_EQ(sculptor.reused(), dabs.size());
    CHECK_EQ(got->hash(), whole(dabs));
    size_t back = dabs.size() - 1;
    while (back > 0 && dabs[back].joined) --back;
    got = sculptor.cook(source, first(back), Falloff::Smooth, d);
    CHECK_EQ(sculptor.reused(), 0u);
    CHECK_EQ(got->hash(), whole(first(back)));
    // Another detail: from the start.
    Dyntopo finer = d;
    finer.detail = 0.2f;
    got = sculptor.cook(source, first(back), Falloff::Smooth, finer);
    CHECK_EQ(sculptor.reused(), 0u);
    Geometry geo(*source);
    sculpt(geo, first(back), Falloff::Smooth, finer);
    CHECK_EQ(got->hash(), geo.hash());
}

TEST(dyntopo_mirrored_strokes_make_a_symmetric_shape) {
    // A stroke and its mirror image across x over a ball: the bumps on the
    // two sides as high, though the triangles under them are made each
    // under its own dab.
    const GeometryPtr ball = ballOf(12);
    Geometry geo(*ball);
    std::vector<SculptDab> dabs;
    for (int i = 0; i < 30; ++i) {
        const float a = 0.03f * static_cast<float>(i);
        const Vec3 dir = normalize(Vec3(0.8f, 0.3f + std::sin(a), std::cos(a)));
        for (const SculptDab& both : mirroredDabs(dab(SculptDab::Tool::Push, dir, dir, 0.25f, 0.8f), Mirror::X)) dabs.push_back(both);
    }
    sculpt(geo, dabs, Falloff::Smooth, everything());
    float right = 0.0f, left = 0.0f;
    for (const Vec3& p : geo.positions()) {
        if (p.x > 0.0f) right = std::max(right, length(p));
        else left = std::max(left, length(p));
    }
    CHECK(right > 1.05f);
    CHECK_NEAR(right, left, 0.01f * right);
}

TEST(sculpt_node_has_dyntopo) {
    // The Sculpt node: Dyntopo on makes its polygons triangles, finer under
    // the dabs -- as fine as Detail of the radius says, or as Detail Size
    // says; Collapse alone makes a coarse mesh no finer.
    registerBuiltinNodes();
    Graph g;
    CookEngine engine;
    Node* grid = g.create("grid", "grid");
    grid->setFloat("sizex", 2.0f);
    grid->setFloat("sizez", 2.0f);
    grid->setInt("rows", 5);
    grid->setInt("cols", 5);
    Node* s = g.create("sculpt", "sculpt");
    s->setInput(0, grid);
    s->setString("strokes", "p 0 0 0 0 1 0 0.4 1; p 0.3 0 0.2 0 1 0 0.4 1");
    const GeometryPtr plain = engine.cook(*s, CookContext{});
    CHECK_EQ(plain->pointCount(), 25u);
    s->setBool("dyntopo", true);
    const GeometryPtr brush = engine.cook(*s, CookContext{});
    CHECK(brush->pointCount() > 100u);
    std::map<std::pair<uint32_t, uint32_t>, int> sides;
    CHECK(sidesOf(*brush, sides));
    float top = 0.0f;
    for (const Vec3& p : brush->positions()) top = std::max(top, p.y);
    CHECK(top > 0.5f * kSculptPush * 0.4f);
    s->setFloat("detail", 0.1f);
    CHECK(engine.cook(*s, CookContext{})->pointCount() > brush->pointCount());
    s->setInt("detailmode", 1);
    s->setFloat("detailsize", 0.05f);
    const GeometryPtr constant = engine.cook(*s, CookContext{});
    CHECK(sidesOf(*constant, sides));
    const auto P = constant->positions();
    float longest = 0.0f;
    for (const auto& [e, n] : sides) {
        // Under the first dab, before the second moved it: well inside.
        if (segmentDistance(Vec3(-0.2f, 0.0f, -0.2f), Vec3(P[e.first].x, 0.0f, P[e.first].z), Vec3(P[e.second].x, 0.0f, P[e.second].z)) < 0.05f) {
            longest = std::max(longest, length(P[e.first] - P[e.second]));
        }
    }
    CHECK(longest > 0.0f && longest < 0.06f);
    s->setInt("refine", 1);  // collapse only: nothing short here
    CHECK_EQ(engine.cook(*s, CookContext{})->pointCount(), 25u);
}
