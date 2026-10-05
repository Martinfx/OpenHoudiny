//
// Vertices -- the corners of the primitives -- as the viewport's vertex
// mode picks them (5): named by number, by primitive and corner ("5v2"),
// by groups of any class; a Group, an Edit and a Blast of them; each one's
// mark a little inside its polygon, picked where it is seen.
//
#include "pg/core/CookEngine.h"
#include "pg/core/Graph.h"
#include "pg/core/Pick.h"
#include "pg/core/Selection.h"
#include "pg/nodes/Nodes.h"

#include "test_framework.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <string>

using namespace pg;

namespace {

/// A grid of `n` x `n` quads, `size` m square, flat on the floor.
Node* gridNode(Graph& g, int n, float size) {
    registerBuiltinNodes();
    Node* grid = g.create("grid", "grid");
    grid->setFloat("sizex", size);
    grid->setFloat("sizez", size);
    grid->setInt("rows", n + 1);
    grid->setInt("cols", n + 1);
    return grid;
}

GeometryPtr gridOf(int n, float size) {
    Graph g;
    CookEngine engine;
    return std::make_shared<Geometry>(*engine.cook(*gridNode(g, n, size), CookContext{}));
}

/// Looking straight down at the origin from `height`, 400 x 400 pixels.
PickView fromAbove(float height) {
    PickView v;
    v.eye = Vec3(0.0f, height, 0.0f);
    v.forward = Vec3(0.0f, -1.0f, 0.0f);
    v.right = Vec3(1.0f, 0.0f, 0.0f);
    v.up = Vec3(0.0f, 0.0f, -1.0f);
    v.tanHalfFov = std::tan(20.0f * 3.14159265f / 180.0f);
    v.width = v.height = 400.0f;
    return v;
}

size_t countOf(const std::vector<uint8_t>& mask) {
    return static_cast<size_t>(std::count_if(mask.begin(), mask.end(), [](uint8_t m) { return m != 0; }));
}

/// The vertices of `geo` on point `p`.
std::vector<uint8_t> cornersAt(const Geometry& geo, uint32_t p) {
    std::vector<uint8_t> out(geo.vertexCount(), 0);
    for (size_t v = 0; v < out.size(); ++v) out[v] = geo.vertexPoint(v) == p ? 1 : 0;
    return out;
}

}  // namespace

TEST(vertices_are_named_by_number_by_corner_and_by_groups) {
    // Two by two quads: 9 points, 4 primitives of 4 corners each.
    Geometry geo(*gridOf(2, 2.0f));
    CHECK_EQ(geo.vertexCount(), 16u);
    const auto pick = [&](std::string_view pattern) { return selectElements(geo, AttrClass::Vertex, pattern); };
    CHECK_EQ(patternOf(pick("0-3 9")), "0-3 9");
    CHECK_EQ(countOf(pick("*")), 16u);
    // Corner 2 of primitive 1, as Houdini names it; a run of corners; one
    // taken away again; a corner there is not.
    const size_t start1 = geo.primitiveVertexStart(1);
    CHECK_EQ(patternOf(pick("1v2")), std::to_string(start1 + 2));
    CHECK_EQ(patternOf(pick("1v0-2 ^1v1")), std::to_string(start1) + " " + std::to_string(start1 + 2));
    CHECK_EQ(countOf(pick("3v4 7v0")), 0u);
    bool named = false;
    selectElements(geo, AttrClass::Vertex, "1v2", &named);
    CHECK(named);
    // As points, its point; as primitives, its primitive.
    const std::vector<uint8_t> asPoints = selectElements(geo, AttrClass::Point, "1v2");
    CHECK_EQ(countOf(asPoints), 1u);
    CHECK(asPoints[geo.vertexPoint(start1 + 2)] != 0);
    CHECK_EQ(patternOf(selectElements(geo, AttrClass::Primitive, "1v2")), "1");

    // The middle point is a corner of all four quads.
    uint32_t middle = 0;
    for (uint32_t p = 0; p < geo.pointCount(); ++p) {
        if (length(geo.positions()[p]) < 1e-6f) middle = p;
    }
    Group& pts = geo.createGroup("middle", AttrClass::Point);
    pts.set(middle, true);
    CHECK(pick("middle") == cornersAt(geo, middle));
    CHECK_EQ(countOf(pick("middle")), 4u);
    Group& prim = geo.createGroup("first", AttrClass::Primitive);
    prim.set(0, true);
    CHECK_EQ(patternOf(pick("first")), "0-3");
    // A group of vertices: its points; the primitives all of whose corners it holds.
    Group& corners = geo.createGroup("corners", AttrClass::Vertex);
    for (size_t v = 0; v < 4; ++v) corners.set(v, true);
    corners.set(start1, true);
    CHECK_EQ(patternOf(selectElements(geo, AttrClass::Primitive, "corners")), "0");
    std::set<uint32_t> want;
    for (const size_t v : {size_t(0), size_t(1), size_t(2), size_t(3), start1}) want.insert(geo.vertexPoint(v));
    const std::vector<uint8_t> cornerPoints = selectElements(geo, AttrClass::Point, "corners");
    CHECK_EQ(countOf(cornerPoints), want.size());
    for (const uint32_t p : want) CHECK(cornerPoints[p] != 0);
    CHECK_EQ(patternOf(pick("corners ^first")), std::to_string(start1));

    // An edge from the middle out: the corners at its two ends of the two
    // quads it is a side of.
    uint32_t out = 0;
    for (const Edge& e : edgesOf(geo)) {
        if (e.first == middle || e.second == middle) out = e.first == middle ? e.second : e.first;
    }
    const std::string edge = "p" + std::to_string(middle) + "-" + std::to_string(out);
    const std::vector<uint8_t> along = pick(edge);
    CHECK_EQ(countOf(along), 4u);
    std::set<uint32_t> prims;
    for (size_t v = 0; v < along.size(); ++v) {
        if (!along[v]) continue;
        CHECK(geo.vertexPoint(v) == middle || geo.vertexPoint(v) == out);
        for (size_t p = 0; p < geo.primitiveCount(); ++p) {
            if (v >= geo.primitiveVertexStart(p) && v < geo.primitiveVertexStart(p) + geo.primitiveVertexCount(p)) prims.insert(static_cast<uint32_t>(p));
        }
    }
    CHECK_EQ(prims.size(), 2u);
}

TEST(vertices_taken_out_leave_the_polygons_their_other_corners) {
    // A corner out of a quad: a triangle, its corners' attributes as they
    // were, the point only that corner used gone. Two out of a quad: too
    // few for a polygon -- it goes whole. The groups follow.
    Geometry geo(*gridOf(2, 2.0f));
    const auto P0 = geo.positions();
    auto uv = geo.vertices().create("uv", AttrType::Vec2).write<glm::vec2>();
    for (size_t v = 0; v < geo.vertexCount(); ++v) uv[v] = glm::vec2(P0[geo.vertexPoint(v)].x, P0[geo.vertexPoint(v)].z);
    auto part = geo.primitives().create("part", AttrType::Int).write<int32_t>();
    for (size_t p = 0; p < geo.primitiveCount(); ++p) part[p] = static_cast<int32_t>(10 + p);
    Group& kept = geo.createGroup("kept", AttrClass::Vertex);
    kept.set(5, true);
    // Corner 0 of quad 0 is on a corner of the grid: no other quad has it.
    const uint32_t lone = geo.vertexPoint(0);
    CHECK_EQ(countOf(cornersAt(geo, lone)), 1u);
    std::vector<uint8_t> keep(geo.vertexCount(), 1);
    keep[0] = 0;
    Geometry one = geo;
    one.deleteVertices(keep, true);
    CHECK_EQ(one.primitiveCount(), 4u);
    CHECK_EQ(one.vertexCount(), 15u);
    CHECK_EQ(one.primitiveVertexCount(0), 3u);
    CHECK_EQ(one.pointCount(), geo.pointCount() - 1);
    const auto P = one.positions();
    const auto U = one.vertices().find("uv")->read<glm::vec2>();
    for (size_t v = 0; v < one.vertexCount(); ++v) {
        CHECK(U[v] == glm::vec2(P[one.vertexPoint(v)].x, P[one.vertexPoint(v)].z));
    }
    const auto Part = one.primitives().find("part")->read<int32_t>();
    for (size_t p = 0; p < 4; ++p) CHECK_EQ(Part[p], static_cast<int32_t>(10 + p));
    CHECK(one.findGroup("kept")->contains(4));  // vertex 5 is the fifth now
    CHECK_EQ(one.findGroup("kept")->memberCount(), 1u);

    // Two corners of quad 1: it goes, and with it the points only it used.
    const size_t s1 = geo.primitiveVertexStart(1);
    std::fill(keep.begin(), keep.end(), 1);
    keep[s1] = keep[s1 + 1] = 0;
    Geometry two = geo;
    two.deleteVertices(keep, true);
    CHECK_EQ(two.primitiveCount(), 3u);
    CHECK_EQ(two.vertexCount(), 12u);
    const auto Two = two.primitives().find("part")->read<int32_t>();
    CHECK(Two[0] == 10 && Two[1] == 12 && Two[2] == 13);

    // The Blast node: of class Vertices, the corners it names; kept, the rest.
    Graph g;
    CookEngine engine;
    Node* grid = gridNode(g, 2, 2.0f);
    Node* blast = g.create("blast", "blast");
    blast->setInput(0, grid);
    blast->setInt("class", 2);
    blast->setString("group", "0");
    GeometryPtr cut = engine.cook(*blast, CookContext{});
    CHECK_EQ(cut->vertexCount(), 15u);
    CHECK_EQ(cut->pointCount(), 8u);
    blast->setString("group", "0v0-1");
    CHECK_EQ(engine.cook(*blast, CookContext{})->primitiveCount(), 3u);
    blast->setBool("invert", true);  // keep only those two: no polygon of two
    CHECK_EQ(engine.cook(*blast, CookContext{})->primitiveCount(), 0u);
}

TEST(group_and_edit_of_vertices) {
    // A Group of class Vertices holds them; an Edit of them moves their
    // points -- and only those.
    Graph g;
    CookEngine engine;
    Node* grid = gridNode(g, 2, 2.0f);
    Node* group = g.create("groupcreate", "group");
    group->setInput(0, grid);
    group->setString("name", "picked");
    group->setInt("class", 2);
    group->setString("pattern", "1v0-1 3");
    GeometryPtr grouped = engine.cook(*group, CookContext{});
    const Group* picked = grouped->findGroup("picked");
    CHECK(picked != nullptr);
    CHECK(picked->classOf() == AttrClass::Vertex);
    CHECK_EQ(picked->memberCount(), 3u);
    const size_t s1 = grouped->primitiveVertexStart(1);
    CHECK(picked->contains(s1) && picked->contains(s1 + 1) && picked->contains(3));

    Node* edit = g.create("edit", "edit");
    edit->setInput(0, grid);
    edit->setInt("class", 2);
    edit->setString("group", "1v2");
    edit->setVec3("t", Vec3(0.0f, 1.0f, 0.0f));
    const GeometryPtr before = engine.cook(*grid, CookContext{});
    const GeometryPtr after = engine.cook(*edit, CookContext{});
    const uint32_t moved = before->vertexPoint(before->primitiveVertexStart(1) + 2);
    for (size_t p = 0; p < after->pointCount(); ++p) {
        const float rise = after->positions()[p].y - before->positions()[p].y;
        CHECK_NEAR(rise, p == moved ? 1.0f : 0.0f, 1e-6f);
    }
}

TEST(picker_finds_the_vertex_whose_mark_is_nearest_and_seen) {
    // Each corner's mark a fifth of the way to its quad's middle. The mouse
    // near one picks it; a box round a point takes in the corners of the
    // quads round it.
    const GeometryPtr geo = gridOf(2, 2.0f);
    const ElementPicker picker(geo);
    const PickView view = fromAbove(3.0f);
    const auto P = geo->positions();
    for (size_t v = 0; v < geo->vertexCount(); ++v) {
        const uint32_t prim = picker.vertexPrimitive(v);
        CHECK(v >= geo->primitiveVertexStart(prim) && v < geo->primitiveVertexStart(prim) + geo->primitiveVertexCount(prim));
        const Vec3 want = P[geo->vertexPoint(v)] + (picker.middle(prim) - P[geo->vertexPoint(v)]) * kVertexInset;
        CHECK(length(picker.vertexMark(v) - want) < 1e-6f);
    }
    for (const size_t v : {size_t(0), size_t(6), size_t(13)}) {
        float sx = 0.0f, sy = 0.0f;
        CHECK(view.project(picker.vertexMark(v), sx, sy));
        CHECK_EQ(picker.vertex(view, sx + 1.0f, sy - 1.0f, 6.0f), static_cast<int32_t>(v));
    }
    CHECK_EQ(picker.vertex(view, 5.0f, 5.0f, 6.0f), -1);
    // The middle of the screen is the middle point: a box round it takes
    // in its four corners, and nothing else.
    uint32_t middle = 0;
    for (uint32_t p = 0; p < geo->pointCount(); ++p) {
        if (length(P[p]) < 1e-6f) middle = p;
    }
    CHECK(picker.verticesIn(view, 170.0f, 170.0f, 230.0f, 230.0f) == cornersAt(*geo, middle));
    CHECK_EQ(countOf(picker.verticesIn(view, ScreenRegion::brush(200.0f, 200.0f, 200.0f, 200.0f, 40.0f))), 4u);

    // A box seen from above: the corners of its top face, not those the
    // top face hides -- unless asked for.
    Graph g;
    CookEngine engine;
    Node* box = g.create("box", "box");
    const GeometryPtr cube = engine.cook(*box, CookContext{});
    const ElementPicker boxPicker(cube);
    const std::vector<uint8_t> seen = boxPicker.verticesIn(view, 0.0f, 0.0f, 400.0f, 400.0f);
    CHECK_EQ(countOf(seen), 4u);
    for (size_t v = 0; v < seen.size(); ++v) {
        if (seen[v]) CHECK(cube->positions()[cube->vertexPoint(v)].y > 0.0f);
    }
    CHECK_EQ(countOf(boxPicker.verticesIn(view, 0.0f, 0.0f, 400.0f, 400.0f, true)), cube->vertexCount());
}
