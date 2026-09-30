//
// What the viewport's selection and brush make of geometry: elements named
// by a pattern (Selection.h), edges by their points, the Group, Edit and
// Attribute Paint nodes, Blast of a pattern or of primitives; and the same
// nodes in a network. What the mouse picks through a camera (Pick.h): the
// point, the edge, the primitive under it, what a box holds -- not what the
// surface hides.
//
#include "pg/core/CookEngine.h"
#include "pg/core/Graph.h"
#include "pg/core/Pick.h"
#include "pg/core/Sculpt.h"
#include "pg/core/Selection.h"
#include "pg/core/Soft.h"
#include "pg/nodes/Nodes.h"
#include "pg/sim/GeometryGraph.h"
#include "pg/sim/Network.h"
#include "pg/sim/Shape.h"

#include "test_framework.h"

#include <algorithm>
#include <cmath>

using namespace pg;

namespace {

/// A grid of `n` x `n` quads, 1 by 1, flat on the floor.
Node* flatGrid(Graph& g, int n) {
    registerBuiltinNodes();
    Node* grid = g.create("grid", "grid");
    grid->setFloat("sizex", 1.0f);
    grid->setFloat("sizez", 1.0f);
    grid->setInt("rows", n + 1);
    grid->setInt("cols", n + 1);
    return grid;
}

size_t countOf(const std::vector<uint8_t>& mask) {
    size_t n = 0;
    for (const uint8_t m : mask) n += m;
    return n;
}

}  // namespace

TEST(selection_patterns_name_numbers_ranges_groups_and_all) {
    Graph g;
    CookEngine engine;
    Node* grid = flatGrid(g, 4);  // 25 points, 16 quads
    auto geo = std::make_shared<Geometry>(*engine.cook(*grid, CookContext{}));
    CHECK_EQ(geo->pointCount(), 25u);
    auto pick = [&](const char* pattern) { return selectElements(*geo, AttrClass::Point, pattern); };
    CHECK_EQ(countOf(pick("")), 0u);
    CHECK_EQ(countOf(pick("0-4 7")), 6u);
    CHECK(pick("0-4 7")[7] && !pick("0-4 7")[5]);
    CHECK_EQ(countOf(pick("4-0")), 5u);        // a range either way round
    CHECK_EQ(countOf(pick("*")), 25u);
    CHECK_EQ(countOf(pick("* ^0-9")), 15u);    // in order: all, then ten taken away
    CHECK_EQ(countOf(pick("20-99")), 5u);      // past the last: nothing more
    CHECK_EQ(countOf(pick("0,1,2")), 3u);
    CHECK_EQ(countOf(pick("nosuch")), 0u);
    // A group by its name -- and of the other class, by its points.
    Group& corner = geo->createGroup("corner", AttrClass::Primitive);
    corner.resize(geo->primitiveCount());
    corner.set(0, true);
    CHECK_EQ(countOf(pick("corner")), 4u);  // the four points of the first quad
    CHECK_EQ(countOf(selectElements(*geo, AttrClass::Primitive, "corner")), 1u);
    Group& row = geo->createGroup("row", AttrClass::Point);
    row.resize(geo->pointCount());
    for (int i = 0; i < 10; ++i) row.set(static_cast<size_t>(i), true);  // the first two rows of points
    CHECK_EQ(countOf(selectElements(*geo, AttrClass::Primitive, "row")), 4u);  // the quads all of whose points are in it
    // Written back: the shortest pattern, that names the same.
    const std::vector<uint8_t> mask = pick("0-4 7 9 10 11 24");
    CHECK_EQ(patternOf(mask), std::string("0-4 7 9-11 24"));
    CHECK(pick(patternOf(mask).c_str()) == mask);
}

TEST(group_node_makes_a_group_of_what_the_pattern_names) {
    Graph g;
    CookEngine engine;
    Node* grid = flatGrid(g, 4);
    Node* group = g.create("groupcreate", "group");
    group->setInput(0, grid);
    group->setString("name", "picked");
    group->setString("pattern", "0-2 10");
    GeometryPtr out = engine.cook(*group, CookContext{});
    const Group* picked = out->findGroup("picked");
    CHECK(picked != nullptr);
    CHECK(picked->classOf() == AttrClass::Point);
    CHECK_EQ(picked->memberCount(), 4u);
    // Of primitives; and a group of that name is made again, not added to.
    group->setInt("class", 1);
    group->setString("pattern", "5");
    out = engine.cook(*group, CookContext{});
    picked = out->findGroup("picked");
    CHECK(picked && picked->classOf() == AttrClass::Primitive && picked->memberCount() == 1u);
}

TEST(edit_moves_turns_and_sizes_the_picked_points_and_soft_takes_the_rest_along) {
    Graph g;
    CookEngine engine;
    Node* grid = flatGrid(g, 10);  // 121 points, 0.1 apart
    Node* edit = g.create("edit", "edit");
    edit->setInput(0, grid);
    GeometryPtr before = engine.cook(*grid, CookContext{});
    // The middle point, up 0.3.
    size_t middle = 0;
    for (size_t i = 0; i < before->pointCount(); ++i) {
        if (length(before->positions()[i]) < 1e-5f) middle = i;
    }
    edit->setString("group", std::to_string(middle));
    edit->setVec3("t", Vec3(0.0f, 0.3f, 0.0f));
    GeometryPtr out = engine.cook(*edit, CookContext{});
    const auto P0 = before->positions(), P = out->positions();
    size_t moved = 0;
    for (size_t i = 0; i < P.size(); ++i) moved += length(P[i] - P0[i]) > 1e-6f;
    CHECK_EQ(moved, 1u);
    CHECK_NEAR(P[middle].y, 0.3f, 1e-6f);
    // Soft: the points within 0.25 go along, less the further they are.
    edit->setFloat("soft", 0.25f);
    out = engine.cook(*edit, CookContext{});
    const auto S = out->positions();
    size_t along = 0;
    for (size_t i = 0; i < S.size(); ++i) {
        const float d = length(P0[i] - P0[middle]);
        if (i == middle) continue;
        if (d < 0.25f - 1e-4f) {
            CHECK(S[i].y > 0.0f && S[i].y < 0.3f);
            ++along;
        } else {
            CHECK_NEAR(S[i].y, 0.0f, 1e-6f);
        }
    }
    CHECK(along >= 8u);
    // A point at 0.1 goes further than one at 0.2.
    auto at = [&](float x, float z) {
        for (size_t i = 0; i < S.size(); ++i) {
            if (length(P0[i] - Vec3(x, 0.0f, z)) < 1e-4f) return S[i].y;
        }
        return -1.0f;
    };
    CHECK(at(0.1f, 0.0f) > at(0.2f, 0.0f));
    // A primitive's points, turned about the pivot: 90 degrees about y.
    edit->setFloat("soft", 0.0f);
    edit->setInt("class", 1);
    edit->setString("group", "0");
    edit->setVec3("t", Vec3());
    edit->setVec3("r", Vec3(0.0f, 90.0f, 0.0f));
    const auto quad = before->primitivePoints(0);
    Vec3 pivot;
    for (const uint32_t q : quad) pivot += P0[q];
    pivot = pivot * (1.0f / static_cast<float>(quad.size()));
    edit->setVec3("p", pivot);
    out = engine.cook(*edit, CookContext{});
    for (const uint32_t q : quad) {
        const Vec3 was = P0[q] - pivot, now = out->positions()[q] - pivot;
        CHECK_NEAR(length(now), length(was), 1e-5f);
        CHECK_NEAR(dot(now, was), 0.0f, 1e-5f);  // a quarter turn
    }
}

TEST(attribute_paint_lays_its_dabs_on_in_order_and_keeps_to_places) {
    Graph g;
    CookEngine engine;
    Node* grid = flatGrid(g, 10);
    Node* paint = g.create("attribpaint", "paint");
    paint->setInput(0, grid);
    paint->setString("name", "pin");
    // A dab at the middle, of radius 0.15, full strength: 1 there, less out
    // to its edge, 0 past it.
    paint->setString("strokes", "0 0 0 0.15 1 1");
    GeometryPtr out = engine.cook(*paint, CookContext{});
    const AttributeArray* pin = out->points().find("pin");
    CHECK(pin && pin->type() == AttrType::Float);
    const auto P = out->positions();
    const auto v = pin->read<float>();
    for (size_t i = 0; i < P.size(); ++i) {
        const float d = length(P[i]);
        if (d < 1e-5f) CHECK_NEAR(v[i], 1.0f, 1e-6f);
        else if (d < 0.15f) CHECK(v[i] > 0.0f && v[i] < 1.0f);
        else CHECK_EQ(v[i], 0.0f);
    }
    // In order: a second dab of 0 there, larger, takes it off again -- all of
    // it at its middle, most of it round there.
    paint->setString("strokes", "0 0 0 0.15 1 1; 0 0 0 0.3 0 1");
    out = engine.cook(*paint, CookContext{});
    const auto erased = out->points().find("pin")->read<float>();
    for (size_t i = 0; i < P.size(); ++i) {
        if (length(P[i]) < 1e-5f) CHECK_EQ(erased[i], 0.0f);
        CHECK(erased[i] < 0.1f);
    }
    // What cannot be read is left out; a dab of no size too.
    paint->setString("strokes", "garbage; 0 0 0 0 1 1; 0.5 0 0.5 0.12 1 1");
    out = engine.cook(*paint, CookContext{});
    float most = 0.0f;
    for (const float x : out->points().find("pin")->read<float>()) most = std::max(most, x);
    CHECK_NEAR(most, 1.0f, 1e-6f);
    // Places, not point numbers: a finer grid is painted where the coarse one was.
    paint->setString("strokes", "0.25 0 0.25 0.2 1 1");
    grid->setInt("rows", 41);
    grid->setInt("cols", 41);
    out = engine.cook(*paint, CookContext{});
    const auto Q = out->positions();
    const auto w = out->points().find("pin")->read<float>();
    for (size_t i = 0; i < Q.size(); ++i) {
        if (length(Q[i] - Vec3(0.25f, 0.0f, 0.25f)) >= 0.2f) CHECK_EQ(w[i], 0.0f);
    }
    // An attribute of whole numbers stays one.
    Node* ints = g.create("attribcreate", "ints");
    ints->setInput(0, grid);
    ints->setString("name", "pin");
    ints->setInt("class", 1);
    ints->setFloat("value", 0.0f);
    paint->setInput(0, ints);
    out = engine.cook(*paint, CookContext{});
    const AttributeArray* stays = out->points().find("pin");
    CHECK(stays != nullptr);
}

TEST(blast_deletes_what_a_pattern_names_points_or_primitives) {
    Graph g;
    CookEngine engine;
    Node* grid = flatGrid(g, 4);  // 25 points, 16 quads
    Node* blast = g.create("blast", "blast");
    blast->setInput(0, grid);
    blast->setString("group", "0");  // a corner point: its one quad goes with it
    GeometryPtr out = engine.cook(*blast, CookContext{});
    CHECK_EQ(out->pointCount(), 24u);
    CHECK_EQ(out->primitiveCount(), 15u);
    // Primitives: the quads go, and the points only they used.
    blast->setInt("class", 1);
    blast->setString("group", "0-3");  // the first row of quads
    out = engine.cook(*blast, CookContext{});
    CHECK_EQ(out->primitiveCount(), 12u);
    CHECK_EQ(out->pointCount(), 20u);
    // Kept: only them.
    blast->setBool("invert", true);
    out = engine.cook(*blast, CookContext{});
    CHECK_EQ(out->primitiveCount(), 4u);
    // Naming nothing there is, nothing goes -- kept or not; an empty group
    // kept leaves nothing.
    blast->setString("group", "nosuch");
    out = engine.cook(*blast, CookContext{});
    CHECK_EQ(out->primitiveCount(), 16u);
    Node* empty = g.create("groupcreate", "empty");
    empty->setInput(0, grid);
    empty->setString("name", "nothing");
    blast->setInput(0, empty);
    blast->setString("group", "nothing");
    out = engine.cook(*blast, CookContext{});
    CHECK_EQ(out->pointCount(), 0u);
}

TEST(group_edit_and_paint_in_a_network) {
    // grid -> edit -> paint -> group, as the viewport chains them.
    sim::Network net;
    const int grid = net.add("grid");
    const int edit = net.add("edit");
    const int paint = net.add("attribute_paint");
    const int group = net.add("group");
    CHECK(net.connect(grid, "geometry", edit, "geometry"));
    CHECK(net.connect(edit, "geometry", paint, "geometry"));
    CHECK(net.connect(paint, "geometry", group, "geometry"));
    net.setText(edit, "group", "0-3");
    CHECK(net.setParam(edit, "t", sim::ParamValue{0.0f, 1.0f, 0.0f}));
    net.setText(paint, "strokes", "0 0 0 0.3 1 1");
    net.setText(group, "pattern", "0-3");
    net.setDisplay(group);
    sim::GeometryGraph geo;
    geo.sync(net);
    GeometryPtr out = geo.cook(group, 1);
    CHECK(out != nullptr);
    if (!out) return;
    for (int i = 0; i < 4; ++i) CHECK_NEAR(out->positions()[static_cast<size_t>(i)].y, 1.0f, 1e-6f);
    CHECK(out->points().find("pin") != nullptr);
    CHECK(out->findGroup("group1") && out->findGroup("group1")->memberCount() == 4u);
    // Saved and read back, the dabs and the pattern with it.
    const std::string text = net.save();
    sim::Network back;
    std::string error;
    CHECK(sim::Network::load(text, back, error));
    CHECK_EQ(back.text(paint, "strokes"), std::string("0 0 0 0.3 1 1"));
    CHECK_EQ(back.save(), text);
}

TEST(selection_edges_by_the_points_they_join) {
    Graph g;
    CookEngine engine;
    Node* grid = flatGrid(g, 4);  // points r * 5 + c
    auto geo = std::make_shared<Geometry>(*engine.cook(*grid, CookContext{}));
    const std::vector<Edge> edges = edgesOf(*geo);
    CHECK_EQ(edges.size(), 40u);  // 5 rows of 4 and 5 columns of 4
    CHECK(std::is_sorted(edges.begin(), edges.end()));
    CHECK(std::binary_search(edges.begin(), edges.end(), Edge(1, 6)));
    CHECK(!std::binary_search(edges.begin(), edges.end(), Edge(0, 6)));  // across a quad: no edge
    auto points = [&](const char* pattern) { return countOf(selectElements(*geo, AttrClass::Point, pattern)); };
    auto prims = [&](const char* pattern) { return countOf(selectElements(*geo, AttrClass::Primitive, pattern)); };
    CHECK_EQ(points("p0-1"), 2u);
    CHECK_EQ(points("p1-0"), 2u);           // either way round
    CHECK_EQ(points("p0-1-2-3-4"), 5u);     // a path: the first row
    CHECK_EQ(points("p0-6"), 0u);           // not an edge
    CHECK_EQ(points("* ^p0-1"), 23u);
    CHECK_EQ(prims("p1-6"), 2u);            // the quads on both sides of it
    CHECK_EQ(prims("p0-1"), 1u);            // at the border, one
    bool named = false;
    selectElements(*geo, AttrClass::Point, "p0-6", &named);
    CHECK(named);
    // A group called so is still a group.
    Group& pin = geo->createGroup("pin", AttrClass::Point);
    pin.resize(geo->pointCount());
    pin.set(3, true);
    CHECK_EQ(points("pin"), 1u);
    // Edges back from a pattern, and a selection of them written as paths.
    CHECK_EQ(selectEdges(edges, "p0-1-2 p5-10").size(), 3u);
    CHECK_EQ(selectEdges(edges, "* ^p0-1").size(), 39u);
    const std::vector<Edge> path = {{0, 1}, {1, 2}, {2, 3}};
    CHECK_EQ(edgePatternOf(path), std::string("p0-1-2-3"));
    const std::vector<Edge> some = {{0, 1}, {1, 6}, {3, 4}, {5, 6}, {18, 19}, {19, 24}};
    const std::string text = edgePatternOf(some);
    CHECK(selectEdges(edges, text) == some);
    CHECK_EQ(edgePatternOf({}), std::string());
    // The Edit moves an edge's points; Blast deletes the quads on an edge.
    Node* edit = g.create("edit", "edit");
    edit->setInput(0, grid);
    edit->setString("group", "p0-1-2-3-4");
    edit->setVec3("t", Vec3(0.0f, 0.5f, 0.0f));
    GeometryPtr moved = engine.cook(*edit, CookContext{});
    size_t up = 0;
    for (const Vec3& p : moved->positions()) up += p.y > 0.25f;
    CHECK_EQ(up, 5u);
    Node* blast = g.create("blast", "blast");
    blast->setInput(0, grid);
    blast->setInt("class", 1);
    blast->setString("group", "p1-6");
    CHECK_EQ(engine.cook(*blast, CookContext{})->primitiveCount(), 14u);
}

namespace {

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

}  // namespace

TEST(picker_finds_the_point_edge_and_primitive_under_the_mouse) {
    Graph g;
    CookEngine engine;
    Node* grid = flatGrid(g, 4);
    GeometryPtr geo = engine.cook(*grid, CookContext{});
    const ElementPicker picker(geo);
    const PickView view = fromAbove(3.0f);
    // The middle of the screen is the middle of the grid: point 12.
    CHECK_EQ(picker.point(view, 200.0f, 200.0f, 6.0f), 12);
    float sx = 0.0f, sy = 0.0f;
    CHECK(view.project(geo->positions()[7], sx, sy));
    CHECK_EQ(picker.point(view, sx + 3.0f, sy - 2.0f, 6.0f), 7);
    CHECK_EQ(picker.point(view, sx + 30.0f, sy, 6.0f), -1);  // nothing that near
    // A ray down: the quad it meets, 3 away, its normal up to the eye.
    float t = 0.0f;
    Vec3 n;
    const int32_t quad = picker.raycast(Vec3(0.1f, 3.0f, 0.1f), Vec3(0.0f, -1.0f, 0.0f), t, &n);
    CHECK(quad >= 0);
    CHECK_NEAR(t, 3.0f, 1e-5f);
    CHECK_NEAR(n.y, 1.0f, 1e-5f);
    CHECK_EQ(picker.raycast(Vec3(2.0f, 3.0f, 0.0f), Vec3(0.0f, -1.0f, 0.0f), t), -1);
    // Under the mouse at a quad's middle: that quad; between two points: their edge.
    float mx = 0.0f, my = 0.0f;
    CHECK(view.project(picker.middle(5), mx, my));
    CHECK_EQ(picker.primitive(view, mx, my, 6.0f), 5);
    float ax = 0.0f, ay = 0.0f, bx = 0.0f, by = 0.0f;
    view.project(geo->positions()[6], ax, ay);
    view.project(geo->positions()[7], bx, by);
    const int32_t e = picker.edge(view, (ax + bx) * 0.5f, (ay + by) * 0.5f + 2.0f, 6.0f);
    CHECK(e >= 0 && picker.edges()[static_cast<size_t>(e)] == Edge(6, 7));
    // A box round the middle nine points.
    view.project(geo->positions()[6], ax, ay);
    view.project(geo->positions()[18], bx, by);
    CHECK_EQ(countOf(picker.pointsIn(view, ax - 5.0f, ay - 5.0f, bx + 5.0f, by + 5.0f)), 9u);
    CHECK_EQ(countOf(picker.edgesIn(view, ax - 5.0f, ay - 5.0f, bx + 5.0f, by + 5.0f)), 12u);
    CHECK_EQ(countOf(picker.primitivesIn(view, ax - 5.0f, ay - 5.0f, bx + 5.0f, by + 5.0f)), 4u);
}

TEST(picker_leaves_out_what_the_surface_hides) {
    Graph g;
    CookEngine engine;
    registerBuiltinNodes();
    Node* box = g.create("box", "box");  // 8 points, 6 faces, -0.5 to 0.5
    GeometryPtr geo = engine.cook(*box, CookContext{});
    const ElementPicker picker(geo);
    const PickView view = fromAbove(3.0f);
    // The whole screen: the four points on top are seen, the four below not.
    CHECK_EQ(countOf(picker.pointsIn(view, 0.0f, 0.0f, 400.0f, 400.0f)), 4u);
    CHECK_EQ(countOf(picker.pointsIn(view, 0.0f, 0.0f, 400.0f, 400.0f, true)), 8u);
    const std::vector<uint8_t> seen = picker.pointsIn(view, 0.0f, 0.0f, 400.0f, 400.0f);
    for (size_t i = 0; i < seen.size(); ++i) CHECK_EQ(seen[i] != 0, geo->positions()[i].y > 0.0f);
    // The top face alone, of the faces; its four edges, of the twelve.
    CHECK_EQ(countOf(picker.primitivesIn(view, 0.0f, 0.0f, 400.0f, 400.0f)), 1u);
    CHECK_EQ(countOf(picker.primitivesIn(view, 0.0f, 0.0f, 400.0f, 400.0f, true)), 6u);
    CHECK_EQ(picker.edges().size(), 12u);
    CHECK_EQ(countOf(picker.edgesIn(view, 0.0f, 0.0f, 400.0f, 400.0f)), 4u);
    // A point below, under the mouse: not picked; one on top is.
    for (size_t i = 0; i < geo->pointCount(); ++i) {
        const Vec3 p = geo->positions()[i];
        float sx = 0.0f, sy = 0.0f;
        view.project(p, sx, sy);
        CHECK_EQ(picker.point(view, sx, sy, 3.0f), p.y > 0.0f ? static_cast<int32_t>(i) : -1);
    }
    CHECK(!picker.visible(view.eye, Vec3(0.0f, -0.5f, 0.0f)));
    CHECK(picker.visible(view.eye, Vec3(0.0f, 0.5f, 0.0f)));
    // Painted, not moved: the same tree does.
    auto painted = std::make_shared<Geometry>(*geo);
    painted->points().create("pin", AttrType::Float);
    CHECK(picker.fits(*painted));
    auto moved = std::make_shared<Geometry>(*geo);
    moved->positionsForWrite()[0] = Vec3(0.0f, 2.0f, 0.0f);
    CHECK(!picker.fits(*moved));
}

TEST(screen_regions_a_box_a_lasso_and_the_way_of_a_brush) {
    const ScreenRegion box = ScreenRegion::box(30.0f, 40.0f, 10.0f, 20.0f);  // corners either way round
    CHECK(box.contains(10.0f, 20.0f) && box.contains(30.0f, 40.0f) && box.contains(20.0f, 30.0f));
    CHECK(!box.contains(31.0f, 30.0f) && !box.contains(20.0f, 19.0f));
    // A U: its hollow is out.
    const ScreenRegion u = ScreenRegion::lasso({{0.0f, 0.0f}, {10.0f, 0.0f}, {10.0f, 30.0f}, {20.0f, 30.0f}, {20.0f, 0.0f},
                                                {30.0f, 0.0f}, {30.0f, 40.0f}, {0.0f, 40.0f}});
    CHECK(u.contains(5.0f, 5.0f) && u.contains(25.0f, 5.0f) && u.contains(15.0f, 35.0f));
    CHECK(!u.contains(15.0f, 10.0f) && !u.contains(-1.0f, 5.0f) && !u.contains(15.0f, 41.0f));
    // A star drawn in one line round itself: its middle, gone round twice, is out.
    std::vector<Vec2> star;
    for (int k = 0; k < 5; ++k) {
        const float a = (90.0f + 144.0f * static_cast<float>(k)) * 3.14159265f / 180.0f;
        star.push_back({100.0f * std::cos(a), -100.0f * std::sin(a)});
    }
    const ScreenRegion s = ScreenRegion::lasso(star);
    CHECK(!s.contains(0.0f, 0.0f));
    CHECK(s.contains(0.0f, -70.0f));  // in the top point
    CHECK(!ScreenRegion::lasso({{0.0f, 0.0f}, {10.0f, 10.0f}}).contains(5.0f, 5.0f));  // no inside to a line
    // A lasso of many points: the same as asking every side.
    std::vector<Vec2> blob;
    uint32_t seed = 7;
    auto next = [&seed]() {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<float>(seed >> 8) / static_cast<float>(1u << 24);
    };
    for (int k = 0; k < 500; ++k) {
        const float a = 6.2831853f * static_cast<float>(k) / 500.0f, r = 60.0f + 40.0f * next();
        blob.push_back({200.0f + r * std::cos(a), 200.0f + r * std::sin(a)});
    }
    const ScreenRegion b = ScreenRegion::lasso(blob);
    int agree = 0, in = 0;
    for (int k = 0; k < 4000; ++k) {
        const float x = 80.0f + 240.0f * next(), y = 80.0f + 240.0f * next();
        bool odd = false;
        for (size_t i = 0; i < blob.size(); ++i) {
            const Vec2& p = blob[i];
            const Vec2& q = blob[(i + 1) % blob.size()];
            if ((p.y > y) != (q.y > y) && x < (q.x - p.x) * (y - p.y) / (q.y - p.y) + p.x) odd = !odd;
        }
        agree += b.contains(x, y) == odd ? 1 : 0;
        in += odd ? 1 : 0;
    }
    CHECK_EQ(agree, 4000);
    CHECK(in > 1000 && in < 3000);
    // A brush moved from (0, 0) to (100, 0), 10 across: what is near its way.
    const ScreenRegion brush = ScreenRegion::brush(0.0f, 0.0f, 100.0f, 0.0f, 10.0f);
    CHECK(brush.contains(50.0f, 9.0f) && brush.contains(-7.0f, 7.0f) && brush.contains(107.0f, -7.0f));
    CHECK(!brush.contains(50.0f, 11.0f) && !brush.contains(-8.0f, 8.0f));
    float along = 0.0f;
    CHECK(brush.touches(40.0f, -50.0f, 40.0f, 50.0f, along));  // across its way: where it crosses
    CHECK_NEAR(along, 0.5f, 1e-5f);
    CHECK(brush.touches(50.0f, 30.0f, 50.0f, 5.0f, along));  // an end in reach
    CHECK_NEAR(along, 1.0f, 1e-5f);
    CHECK(brush.touches(-30.0f, 5.0f, -6.0f, 5.0f, along));  // reached from the brush's start
    CHECK_NEAR(along, 1.0f, 1e-5f);
    CHECK(!brush.touches(0.0f, 20.0f, 100.0f, 20.0f, along));
    CHECK(!brush.touches(120.0f, -50.0f, 120.0f, 50.0f, along));
    float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
    brush.bounds(x0, y0, x1, y1);
    CHECK(x0 == -10.0f && y0 == -10.0f && x1 == 110.0f && y1 == 10.0f);
}

TEST(picker_takes_in_what_a_lasso_goes_round_and_what_a_brush_goes_over) {
    Graph g;
    CookEngine engine;
    Node* grid = flatGrid(g, 4);  // 5 x 5 points, a quarter apart
    GeometryPtr geo = engine.cook(*grid, CookContext{});
    const ElementPicker picker(geo);
    const PickView view = fromAbove(3.0f);
    auto at = [&](size_t i) {
        Vec2 s;
        view.project(geo->positions()[i], s.x, s.y);
        return s;
    };
    // An L round the row of 6 7 8 and down the column of 6 11 16: not 12,
    // in its corner.
    const float m = 10.0f;
    const Vec2 p6 = at(6), p8 = at(8), p16 = at(16);
    const ScreenRegion l = ScreenRegion::lasso({{p6.x - m, p6.y - m}, {p8.x + m, p6.y - m}, {p8.x + m, p6.y + m},
                                                {p6.x + m, p6.y + m}, {p6.x + m, p16.y + m}, {p6.x - m, p16.y + m}});
    const std::vector<uint8_t> points = picker.pointsIn(view, l);
    CHECK_EQ(countOf(points), 5u);
    for (const size_t i : {6u, 7u, 8u, 11u, 16u}) CHECK(points[i] == 1);
    CHECK(points[12] == 0);
    const std::vector<uint8_t> edges = picker.edgesIn(view, l);
    CHECK_EQ(countOf(edges), 4u);  // 6-7, 7-8, 6-11, 11-16
    CHECK_EQ(countOf(picker.primitivesIn(view, l)), 0u);  // no quad's middle is in it
    // A brush along the row: the three points, the ten edges they end, no face.
    const ScreenRegion row = ScreenRegion::brush(p6.x, p6.y, p8.x, p8.y, 8.0f);
    CHECK_EQ(countOf(picker.pointsIn(view, row)), 3u);
    CHECK_EQ(countOf(picker.edgesIn(view, row)), 10u);
    // Along the middles of two quads: the two, the edge between them, no point.
    float ax = 0.0f, ay = 0.0f, bx = 0.0f, by = 0.0f;
    const std::vector<uint8_t> none(picker.edges().size(), 0);
    size_t first = 0, second = 0;
    for (size_t p = 0; p < geo->primitiveCount(); ++p) {
        const auto pts = geo->primitivePoints(p);
        const bool has6 = std::find(pts.begin(), pts.end(), 6u) != pts.end();
        const bool has12 = std::find(pts.begin(), pts.end(), 12u) != pts.end();
        const bool has8 = std::find(pts.begin(), pts.end(), 8u) != pts.end();
        if (has6 && has12) first = p;
        if (has8 && has12) second = p;
    }
    view.project(picker.middle(first), ax, ay);
    view.project(picker.middle(second), bx, by);
    const ScreenRegion across = ScreenRegion::brush(ax, ay, bx, by, 5.0f);
    CHECK_EQ(countOf(picker.pointsIn(view, across)), 0u);
    const std::vector<uint8_t> faces = picker.primitivesIn(view, across);
    CHECK_EQ(countOf(faces), 2u);
    CHECK(faces[first] == 1 && faces[second] == 1);
    const std::vector<uint8_t> crossed = picker.edgesIn(view, across);
    CHECK_EQ(countOf(crossed), 1u);
    for (size_t i = 0; i < crossed.size(); ++i) {
        if (crossed[i]) CHECK(picker.edges()[i] == Edge(7, 12));
    }
    // A dab bigger than a quad on one of them: the quad and its four corners.
    const ScreenRegion dab = ScreenRegion::brush(ax, ay, ax, ay, 34.0f);
    CHECK_EQ(countOf(picker.pointsIn(view, dab)), 4u);
    CHECK_EQ(countOf(picker.primitivesIn(view, dab)), 1u);
}

TEST(picker_regions_on_a_fine_grid_are_what_asking_each_element_gives) {
    // Thousands of points, edges and quads: the threads split them, the
    // answer is the same as one element after another.
    Graph g;
    CookEngine engine;
    Node* grid = flatGrid(g, 80);  // 6561 points, 6400 quads
    GeometryPtr geo = engine.cook(*grid, CookContext{});
    const ElementPicker picker(geo);
    const PickView view = fromAbove(2.0f);
    const auto P = geo->positions();
    std::vector<Vec2> star;
    for (int k = 0; k < 40; ++k) {
        const float a = 6.2831853f * static_cast<float>(k) / 40.0f, r = k % 2 ? 60.0f : 150.0f;
        star.push_back({200.0f + r * std::cos(a), 200.0f + r * std::sin(a)});
    }
    const ScreenRegion lasso = ScreenRegion::lasso(star);
    const ScreenRegion brush = ScreenRegion::brush(90.0f, 120.0f, 310.0f, 250.0f, 14.0f);
    for (const ScreenRegion* region : {&lasso, &brush}) {
        const std::vector<uint8_t> points = picker.pointsIn(view, *region);
        const std::vector<uint8_t> edges = picker.edgesIn(view, *region);
        size_t in = 0, wrong = 0;
        for (size_t i = 0; i < P.size(); ++i) {
            float x = 0.0f, y = 0.0f;
            const bool expect = view.project(P[i], x, y) && region->contains(x, y);
            wrong += (points[i] != 0) != expect ? 1 : 0;
            in += expect ? 1 : 0;
        }
        CHECK_EQ(wrong, 0u);
        CHECK(in > 300);
        size_t touched = 0;
        wrong = 0;
        for (size_t i = 0; i < picker.edges().size(); ++i) {
            const Edge& e = picker.edges()[i];
            float ax = 0.0f, ay = 0.0f, bx = 0.0f, by = 0.0f, along = 0.0f;
            view.project(P[e.first], ax, ay);
            view.project(P[e.second], bx, by);
            const bool expect = region->touches(ax, ay, bx, by, along);
            wrong += (edges[i] != 0) != expect ? 1 : 0;
            touched += expect ? 1 : 0;
        }
        CHECK_EQ(wrong, 0u);
        CHECK(touched > 300);
    }
    // The quads whose middle the lasso goes round.
    const std::vector<uint8_t> quads = picker.primitivesIn(view, lasso);
    size_t wrong = 0;
    for (size_t p = 0; p < geo->primitiveCount(); ++p) {
        float x = 0.0f, y = 0.0f;
        const bool expect = view.project(picker.middle(p), x, y) && lasso.contains(x, y);
        wrong += (quads[p] != 0) != expect ? 1 : 0;
    }
    CHECK_EQ(wrong, 0u);
}

TEST(picker_refit_to_a_sculpted_surface_answers_as_a_new_one) {
    // A grid pushed up into hills, its topology kept: the flat grid's tree
    // with its boxes made again finds what a tree made for the hills finds
    // -- under the mouse, in a lasso, along a ray, what the hills hide.
    Graph g;
    CookEngine engine;
    Node* grid = flatGrid(g, 40);
    const GeometryPtr flat = engine.cook(*grid, CookContext{});
    auto hills = std::make_shared<Geometry>(*flat);
    std::vector<SculptDab> dabs;
    for (int k = 0; k < 12; ++k) {
        SculptDab d;
        d.at = Vec3(-0.4f + 0.07f * static_cast<float>(k), 0.0f, 0.3f * std::sin(static_cast<float>(k)));
        d.normal = Vec3(0.0f, 1.0f, 0.0f);
        d.radius = 0.2f;
        d.strength = 3.0f;
        dabs.push_back(d);
    }
    sculpt(*hills, dabs);
    ElementPicker moved(flat);
    CHECK(moved.refit(hills));
    CHECK(moved.geometry() == hills);
    CHECK(moved.swell() > 1.0f && moved.swell() < 4.0f);
    const ElementPicker fresh(hills);
    // Looking down at the hills at a slant: some of the grid behind them.
    PickView view;
    view.eye = Vec3(0.0f, 0.6f, 1.2f);
    view.forward = normalize(Vec3(0.0f, 0.1f, 0.0f) - view.eye);
    view.right = Vec3(1.0f, 0.0f, 0.0f);
    view.up = cross(view.right, view.forward);
    view.tanHalfFov = std::tan(25.0f * 3.14159265f / 180.0f);
    view.width = view.height = 400.0f;
    size_t hits = 0, points = 0;
    for (float y = 10.0f; y < 400.0f; y += 23.0f) {
        for (float x = 10.0f; x < 400.0f; x += 19.0f) {
            const int32_t prim = fresh.primitive(view, x, y, 4.0f);
            CHECK_EQ(moved.primitive(view, x, y, 4.0f), prim);
            const int32_t point = fresh.point(view, x, y, 6.0f);
            CHECK_EQ(moved.point(view, x, y, 6.0f), point);
            CHECK_EQ(moved.edge(view, x, y, 6.0f), fresh.edge(view, x, y, 6.0f));
            Vec3 o, dir;
            view.ray(x, y, o, dir);
            float t0 = 0.0f, t1 = 0.0f;
            CHECK_EQ(moved.raycast(o, dir, t0), fresh.raycast(o, dir, t1));
            CHECK(t0 == t1);
            hits += prim >= 0 ? 1 : 0;
            points += point >= 0 ? 1 : 0;
        }
    }
    CHECK(hits > 100);
    CHECK(points > 10);
    std::vector<Vec2> ring;
    for (int k = 0; k < 24; ++k) {
        const float a = 6.2831853f * static_cast<float>(k) / 24.0f;
        ring.push_back({200.0f + 150.0f * std::cos(a), 220.0f + 110.0f * std::sin(a)});
    }
    const ScreenRegion lasso = ScreenRegion::lasso(ring);
    CHECK(moved.pointsIn(view, lasso) == fresh.pointsIn(view, lasso));
    CHECK(moved.edgesIn(view, lasso) == fresh.edgesIn(view, lasso));
    CHECK(moved.primitivesIn(view, lasso) == fresh.primitivesIn(view, lasso));
    // Another topology: not for a refit.
    Graph g2;
    Node* other = flatGrid(g2, 20);
    CHECK(!moved.refit(engine.cook(*other, CookContext{})));
    CHECK(moved.geometry() == hills);
}

TEST(picker_takes_what_is_hidden_too_when_asked) {
    Graph g;
    CookEngine engine;
    registerBuiltinNodes();
    Node* box = g.create("box", "box");
    GeometryPtr geo = engine.cook(*box, CookContext{});
    const ElementPicker picker(geo);
    const PickView view = fromAbove(3.0f);
    // A click on a point below picks it, asked for what is hidden too.
    for (size_t i = 0; i < geo->pointCount(); ++i) {
        float sx = 0.0f, sy = 0.0f;
        view.project(geo->positions()[i], sx, sy);
        CHECK_EQ(picker.point(view, sx, sy, 3.0f, true), static_cast<int32_t>(i));
    }
    // A brush over all of it: the top's four points -- or all eight.
    const ScreenRegion all = ScreenRegion::brush(200.0f, 200.0f, 200.0f, 200.0f, 300.0f);
    CHECK_EQ(countOf(picker.pointsIn(view, all)), 4u);
    CHECK_EQ(countOf(picker.pointsIn(view, all, true)), 8u);
    CHECK_EQ(countOf(picker.edgesIn(view, all)), 4u);
    CHECK_EQ(countOf(picker.edgesIn(view, all, true)), 12u);
    CHECK_EQ(countOf(picker.primitivesIn(view, all)), 1u);
    CHECK_EQ(countOf(picker.primitivesIn(view, all, true)), 6u);
    // A lasso round it all, likewise.
    const ScreenRegion round = ScreenRegion::lasso({{0.0f, 0.0f}, {400.0f, 0.0f}, {400.0f, 400.0f}, {0.0f, 400.0f}});
    CHECK_EQ(countOf(picker.pointsIn(view, round)), 4u);
    CHECK_EQ(countOf(picker.pointsIn(view, round, true)), 8u);
    // An edge of the bottom under the mouse: hidden -- asked, it is picked.
    const auto P = geo->positions();
    size_t low = 0, other = 0;
    while (low < P.size() && P[low].y > 0.0f) ++low;
    for (size_t i = 0; i < P.size(); ++i) {
        if (i != low && P[i].y == P[low].y && (P[i].x == P[low].x) != (P[i].z == P[low].z)) other = i;
    }
    CHECK(low < P.size() && other != low);
    float sa = 0.0f, ta = 0.0f, sb = 0.0f, tb = 0.0f;
    view.project(P[low], sa, ta);
    view.project(P[other], sb, tb);
    const Edge bottom(static_cast<uint32_t>(std::min(low, other)), static_cast<uint32_t>(std::max(low, other)));
    const int32_t seen = picker.edge(view, (sa + sb) * 0.5f, (ta + tb) * 0.5f, 3.0f);
    CHECK(seen < 0 || picker.edges()[static_cast<size_t>(seen)] != bottom);
    const int32_t e = picker.edge(view, (sa + sb) * 0.5f, (ta + tb) * 0.5f, 3.0f, true);
    CHECK(e >= 0);
    if (e >= 0) CHECK(picker.edges()[static_cast<size_t>(e)] == bottom);
}

TEST(the_handles_of_every_node_type_are_parameters_it_has) {
    // What the viewport's gizmo sets: a vector for a place, a turn, an
    // axis, a size, a pivot; a number for a radius, a height.
    int checked = 0;
    for (const sim::NodeType& t : sim::nodeTypes()) {
        const sim::Handles& h = t.handles;
        auto is = [&](const char* name, sim::ParamKind kind) {
            if (!name) return;
            const sim::ParamDef* d = t.param(name);
            CHECK(d != nullptr);
            if (d) CHECK(d->kind == kind);
            ++checked;
        };
        for (const char* v : {h.center, h.rotation, h.axis, h.size, h.pivot}) is(v, sim::ParamKind::Vector);
        for (const char* f : {h.radius, h.height}) is(f, sim::ParamKind::Float);
        CHECK(!h.pivot || h.center);  // a pivot is from where it is
    }
    CHECK(checked > 40);
    // Transform and Edit turn about their pivot: the handle is there. Clip
    // moves its plane and turns its normal.
    for (const char* type : {"transform", "edit"}) {
        const sim::NodeType* t = sim::findNodeType(type);
        CHECK(t && t->handles.center && std::string(t->handles.center) == "t");
        CHECK(t && t->handles.pivot && std::string(t->handles.pivot) == "p");
        CHECK(t && t->handles.rotation && t->handles.size);
    }
    const sim::NodeType* clip = sim::findNodeType("clip");
    CHECK(clip && clip->handles.center && std::string(clip->handles.center) == "origin");
    CHECK(clip && clip->handles.axis && std::string(clip->handles.axis) == "dir");
}

TEST(soft_weights_straight_and_along_the_surface) {
    // The shapes of the falloff, at the selection, halfway, at the radius.
    CHECK_NEAR(falloff(Falloff::Smooth, 0.0f), 1.0f, 1e-6f);
    CHECK_NEAR(falloff(Falloff::Smooth, 0.5f), 0.5625f, 1e-6f);
    CHECK_NEAR(falloff(Falloff::Linear, 0.5f), 0.5f, 1e-6f);
    CHECK_NEAR(falloff(Falloff::Sharp, 0.5f), 0.25f, 1e-6f);
    CHECK_NEAR(falloff(Falloff::Sphere, 0.5f), std::sqrt(0.75f), 1e-6f);
    CHECK_NEAR(falloff(Falloff::Constant, 0.5f), 1.0f, 1e-6f);
    for (const Falloff f : {Falloff::Smooth, Falloff::Linear, Falloff::Sharp, Falloff::Sphere, Falloff::Constant}) {
        CHECK_NEAR(falloff(f, 1.0f), 0.0f, 1e-6f);
    }
    // A flat sheet, 21 x 21 points 5 cm apart, its middle point picked.
    Graph g;
    CookEngine engine;
    Node* grid = flatGrid(g, 20);
    GeometryPtr sheet = engine.cook(*grid, CookContext{});
    const auto P = sheet->positions();
    const size_t middle = 220;
    CHECK_NEAR(length(P[middle]), 0.0f, 1e-6f);
    std::vector<uint8_t> picked(sheet->pointCount(), 0);
    picked[middle] = 1;
    const std::vector<float> space = softWeights(*sheet, picked, 0.3f);
    size_t moved = 0;
    for (size_t i = 0; i < P.size(); ++i) {
        const float d = length(P[i] - P[middle]);
        CHECK_NEAR(space[i], d < 0.3f ? falloff(Falloff::Smooth, d / 0.3f) : 0.0f, 1e-5f);
        moved += space[i] > 0.0f ? 1 : 0;
    }
    CHECK(moved > 80 && moved < 120);  // about pi 6^2 points
    // Along a flat sheet the surface is space: round, not a diamond of edges.
    const std::vector<float> along = softWeights(*sheet, picked, 0.3f, SoftDistance::Surface);
    for (size_t i = 0; i < P.size(); ++i) CHECK_NEAR(along[i], space[i], 1e-5f);
    // A finer sheet, split among the threads: the same, point by point.
    Graph g2;
    Node* fine = flatGrid(g2, 80);
    GeometryPtr many = engine.cook(*fine, CookContext{});
    std::vector<uint8_t> one(many->pointCount(), 0);
    one[many->pointCount() / 2] = 1;
    const std::vector<float> a = softWeights(*many, one, 0.2f, SoftDistance::Space, Falloff::Sphere);
    const std::vector<float> b = softWeights(*many, one, 0.2f, SoftDistance::Surface, Falloff::Sphere);
    size_t differ = 0, some = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        differ += std::fabs(a[i] - b[i]) > 1e-5f ? 1 : 0;
        some += a[i] > 0.0f ? 1 : 0;
    }
    CHECK_EQ(differ, 0u);
    CHECK(some > 700);  // pi 16^2 points
    // Nothing picked, or no radius: only what is picked moves.
    CHECK_EQ(softWeights(*sheet, std::vector<uint8_t>(), 0.3f), std::vector<float>(P.size(), 0.0f));
    const std::vector<float> none = softWeights(*sheet, picked, 0.0f);
    for (size_t i = 0; i < P.size(); ++i) CHECK_EQ(none[i], i == middle ? 1.0f : 0.0f);
    // A second sheet 10 cm over the first, not joined to it: straight, the
    // points over the one picked go along; along the surface they stay.
    Geometry two(*sheet);
    Geometry upper(*sheet);
    for (Vec3& p : upper.positionsForWrite()) p.y += 0.1f;
    two.append(upper);
    std::vector<uint8_t> low(two.pointCount(), 0);
    low[middle] = 1;
    const size_t above = sheet->pointCount() + middle;
    CHECK(softWeights(two, low, 0.3f, SoftDistance::Space)[above] > 0.5f);
    CHECK_EQ(softWeights(two, low, 0.3f, SoftDistance::Surface)[above], 0.0f);
    // A strip bent back over itself: 10 cm apart in space, 2 m along it.
    Geometry u;
    u.addPoints(41);
    auto Q = u.positionsForWrite();
    std::vector<uint32_t> line;
    for (uint32_t k = 0; k < 41; ++k) {
        Q[k] = k <= 20 ? Vec3(0.05f * static_cast<float>(k), 0.0f, 0.0f)
                       : Vec3(0.05f * static_cast<float>(40 - k), 0.1f, 0.0f);
        line.push_back(k);
    }
    u.addPrimitive(line, false);
    std::vector<uint8_t> end(41, 0);
    end[0] = 1;
    const std::vector<float> bent = softWeights(u, end, 0.3f, SoftDistance::Surface, Falloff::Linear);
    CHECK_NEAR(bent[1], 1.0f - 0.05f / 0.3f, 1e-5f);
    CHECK_NEAR(bent[5], 1.0f - 0.25f / 0.3f, 1e-5f);
    CHECK_EQ(bent[6], 0.0f);
    CHECK_EQ(bent[40], 0.0f);  // over the one picked, the other way round
    CHECK(softWeights(u, end, 0.3f, SoftDistance::Space, Falloff::Linear)[40] > 0.6f);
}

TEST(edit_moves_the_points_round_by_their_soft_weights) {
    Graph g;
    CookEngine engine;
    Node* grid = flatGrid(g, 20);
    Node* edit = g.create("edit", "edit");
    edit->setInput(0, grid);
    edit->setString("group", "220");
    edit->setFloat("soft", 0.3f);
    edit->setVec3("t", Vec3(0.0f, 1.0f, 0.0f));
    GeometryPtr before = engine.cook(*grid, CookContext{});
    std::vector<uint8_t> picked(before->pointCount(), 0);
    picked[220] = 1;
    for (int metric = 0; metric < 2; ++metric) {
        for (int shape = 0; shape < 5; ++shape) {
            edit->setInt("metric", metric);
            edit->setInt("falloff", shape);
            GeometryPtr after = engine.cook(*edit, CookContext{});
            const std::vector<float> w = softWeights(*before, picked, 0.3f, metric ? SoftDistance::Surface : SoftDistance::Space,
                                                     static_cast<Falloff>(shape));
            size_t wrong = 0;
            for (size_t i = 0; i < w.size(); ++i) {
                wrong += std::fabs(after->positions()[i].y - before->positions()[i].y - w[i]) > 1e-5f ? 1 : 0;
            }
            CHECK_EQ(wrong, 0u);
        }
    }
}

TEST(sculpt_dabs_read_and_write_as_text) {
    SculptDab push;
    push.tool = SculptDab::Tool::Push;
    push.at = Vec3(0.25f, 1.5f, -2.0f);
    push.normal = Vec3(0.0f, 1.0f, 0.0f);
    push.radius = 0.3f;
    push.strength = -0.5f;
    SculptDab smooth;
    smooth.tool = SculptDab::Tool::Smooth;
    smooth.at = Vec3(1.0f, 2.0f, 3.0f);
    smooth.radius = 0.1f;
    smooth.strength = 0.75f;
    SculptDab grab;
    grab.tool = SculptDab::Tool::Grab;
    grab.at = Vec3(-1.0f, 0.0f, 0.5f);
    grab.move = Vec3(0.0f, 0.25f, 0.125f);
    grab.radius = 0.5f;
    SculptDab flat = push;
    flat.tool = SculptDab::Tool::Flatten;
    flat.strength = 1.0f;
    const std::string text = sculptText(push) + "; " + sculptText(smooth) + "; nonsense 1 2; " + sculptText(grab) + "; " +
                             sculptText(flat) + "; p 0 0 0 0 0 0 1 1";  // no normal: left out
    const std::vector<SculptDab> back = parseSculpt(text);
    CHECK_EQ(back.size(), 4u);
    if (back.size() == 4) {
        CHECK(back[0].tool == SculptDab::Tool::Push && back[0].at == push.at && back[0].strength == -0.5f);
        CHECK(back[1].tool == SculptDab::Tool::Smooth && back[1].radius == 0.1f && back[1].strength == 0.75f);
        CHECK(back[2].tool == SculptDab::Tool::Grab && back[2].move == grab.move && back[2].radius == 0.5f);
        CHECK(back[3].tool == SculptDab::Tool::Flatten && back[3].normal == flat.normal);
    }
}

TEST(sculpt_pushes_grabs_flattens_and_smooths_where_the_points_are) {
    Graph g;
    CookEngine engine;
    Node* grid = flatGrid(g, 20);  // 21 x 21 points 5 cm apart, 220 in the middle
    GeometryPtr flat = engine.cook(*grid, CookContext{});
    auto dab = [](SculptDab::Tool tool, Vec3 at, float radius, float strength) {
        SculptDab d;
        d.tool = tool;
        d.at = at;
        d.normal = Vec3(0.0f, 1.0f, 0.0f);
        d.radius = radius;
        d.strength = strength;
        return d;
    };
    // Out along the normal, a fifth of the radius at the middle; in, below 0.
    const float top = kSculptPush * 0.3f;
    Geometry pushed(*flat);
    sculpt(pushed, std::vector<SculptDab>{dab(SculptDab::Tool::Push, Vec3(), 0.3f, 1.0f)});
    CHECK_NEAR(pushed.positions()[220].y, top, 1e-6f);
    for (size_t i = 0; i < pushed.pointCount(); ++i) {
        const float d = length(flat->positions()[i]);
        CHECK_NEAR(pushed.positions()[i].y, d < 0.3f ? top * falloff(Falloff::Smooth, d / 0.3f) : 0.0f, 1e-6f);
    }
    Geometry pulled(*flat);
    sculpt(pulled, std::vector<SculptDab>{dab(SculptDab::Tool::Push, Vec3(), 0.3f, -1.0f)}, Falloff::Linear);
    CHECK_NEAR(pulled.positions()[220].y, -top, 1e-6f);
    // Grabbed: what it held, by the move -- all of it at its middle.
    SculptDab up = dab(SculptDab::Tool::Grab, Vec3(), 0.02f, 1.0f);
    up.move = Vec3(0.0f, 0.5f, 0.0f);
    SculptDab again = up;               // where the point was: nothing there now
    SculptDab there = up;
    there.at = Vec3(0.0f, 0.5f, 0.0f);  // where it is: it goes on
    Geometry grabbed(*flat);
    sculpt(grabbed, std::vector<SculptDab>{up, again, there});
    CHECK_NEAR(grabbed.positions()[220].y, 1.0f, 1e-6f);
    for (size_t i = 0; i < grabbed.pointCount(); ++i) {
        if (i != 220) CHECK_EQ(grabbed.positions()[i].y, 0.0f);
    }
    // Flattened onto the plane where it is: the bump near gone -- its top,
    // over the dab's middle, takes a share a hair below all.
    Geometry flattened(pushed);
    sculpt(flattened, std::vector<SculptDab>{dab(SculptDab::Tool::Flatten, Vec3(), 0.3f, 1.0f)});
    CHECK_NEAR(flattened.positions()[220].y, top * (1.0f - falloff(Falloff::Smooth, top / 0.3f)), 1e-7f);
    CHECK(flattened.positions()[221].y < pushed.positions()[221].y);
    // A spike smoothed away towards its neighbours.
    Geometry spike(*flat);
    SculptDab lift = up;
    lift.move = Vec3(0.0f, 0.3f, 0.0f);
    sculpt(spike, std::vector<SculptDab>{lift, dab(SculptDab::Tool::Smooth, Vec3(0.0f, 0.3f, 0.0f), 0.1f, 1.0f)});
    CHECK_NEAR(spike.positions()[220].y, 0.0f, 1e-6f);
    // A point of the border pulled out: smoothed back onto the border's
    // line; a corner stays where it is.
    const size_t side = 10 * 21;  // the middle of the border at x = -0.5
    CHECK_NEAR(flat->positions()[side].x, -0.5f, 1e-6f);
    Geometry border(*flat);
    SculptDab out = dab(SculptDab::Tool::Grab, flat->positions()[side], 0.02f, 1.0f);
    out.move = Vec3(-0.1f, 0.0f, 0.0f);
    SculptDab corner = dab(SculptDab::Tool::Smooth, flat->positions()[0], 0.02f, 1.0f);
    sculpt(border, std::vector<SculptDab>{out, dab(SculptDab::Tool::Smooth, Vec3(-0.6f, 0.0f, flat->positions()[side].z), 0.03f, 1.0f),
                                          corner});
    CHECK_NEAR(border.positions()[side].x, -0.5f, 1e-6f);
    CHECK(border.positions()[0] == flat->positions()[0]);
    // Point normals found again: tilted on the bump's side, of unit length.
    Geometry withN(*flat);
    AttributeArray& N = withN.points().create("N", AttrType::Vec3);
    for (Vec3& n : N.write<Vec3>()) n = Vec3(0.0f, 1.0f, 0.0f);
    sculpt(withN, std::vector<SculptDab>{dab(SculptDab::Tool::Push, Vec3(), 0.3f, 4.0f)});
    const Vec3 slope = withN.points().find("N")->read<Vec3>()[223];
    CHECK_NEAR(length(slope), 1.0f, 1e-5f);
    CHECK(slope.x > 0.3f && slope.y > 0.5f);  // at x = 0.15 the bump falls away towards +x: turned that way
}

TEST(sculpt_is_what_asking_every_point_gives) {
    // Dabs small and large on a bumpy sheet: the grid that finds the points
    // near a dab, as they move, finds what asking every point finds.
    Graph g;
    CookEngine engine;
    Node* grid = flatGrid(g, 30);
    Geometry sheet(*engine.cook(*grid, CookContext{}));
    uint32_t seed = 11;
    auto next = [&seed]() {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<float>(seed >> 8) / static_cast<float>(1u << 24);
    };
    for (Vec3& p : sheet.positionsForWrite()) p.y = 0.05f * next();
    std::vector<SculptDab> dabs;
    for (int k = 0; k < 300; ++k) {
        SculptDab d;
        d.tool = k % 3 == 0 ? SculptDab::Tool::Push : k % 3 == 1 ? SculptDab::Tool::Grab : SculptDab::Tool::Flatten;
        d.at = Vec3(next() - 0.5f, 0.05f * next(), next() - 0.5f);
        d.normal = normalize(Vec3(next() - 0.5f, 1.0f, next() - 0.5f));
        d.move = Vec3(0.1f * (next() - 0.5f), 0.1f * next(), 0.1f * (next() - 0.5f));
        d.radius = k % 17 == 0 ? 0.8f : 0.01f + 0.15f * next();
        d.strength = 2.0f * next() - 0.5f;
        if (d.tool == SculptDab::Tool::Flatten) d.strength = std::clamp(d.strength, 0.0f, 1.0f);
        dabs.push_back(d);
    }
    Geometry fast(sheet);
    sculpt(fast, dabs, Falloff::Sphere);
    std::vector<Vec3> slow(sheet.positions().begin(), sheet.positions().end());
    for (const SculptDab& d : dabs) {
        std::vector<Vec3> now = slow;
        for (size_t i = 0; i < slow.size(); ++i) {
            const float dist = length(slow[i] - d.at);
            if (!(dist < d.radius)) continue;
            const float w = falloff(Falloff::Sphere, dist / d.radius);
            if (d.tool == SculptDab::Tool::Push) now[i] = slow[i] + d.normal * (d.strength * kSculptPush * d.radius * w);
            if (d.tool == SculptDab::Tool::Grab) now[i] = slow[i] + d.move * w;
            if (d.tool == SculptDab::Tool::Flatten) now[i] = slow[i] + d.normal * (-dot(slow[i] - d.at, d.normal) * d.strength * w);
        }
        slow = now;
    }
    size_t wrong = 0;
    for (size_t i = 0; i < slow.size(); ++i) wrong += length(fast.positions()[i] - slow[i]) > 1e-6f ? 1 : 0;
    CHECK_EQ(wrong, 0u);
}

TEST(sculpt_node_makes_its_geometry_of_its_strokes) {
    Graph g;
    CookEngine engine;
    Node* grid = flatGrid(g, 20);
    Node* s = g.create("sculpt", "sculpt");
    s->setInput(0, grid);
    GeometryPtr before = engine.cook(*grid, CookContext{});
    CHECK(engine.cook(*s, CookContext{})->positions().data() == before->positions().data());  // no strokes: shared
    s->setString("strokes", "p 0 0 0 0 1 0 0.3 1; g 0 0.06 0 0 0.2 0 0.1");
    GeometryPtr after = engine.cook(*s, CookContext{});
    CHECK_NEAR(after->positions()[220].y, kSculptPush * 0.3f + 0.2f, 1e-5f);
    s->setInt("falloff", 1);  // linear: the ring round it lower
    GeometryPtr linear = engine.cook(*s, CookContext{});
    CHECK(linear->positions()[223].y < after->positions()[223].y);
}

namespace {

/// The same positions and normals, to the bit.
bool sameShape(const Geometry& a, const Geometry& b) {
    if (a.pointCount() != b.pointCount()) return false;
    for (size_t i = 0; i < a.pointCount(); ++i) {
        if (!(a.positions()[i] == b.positions()[i])) return false;
    }
    const AttributeArray* na = a.points().find("N");
    const AttributeArray* nb = b.points().find("N");
    if (!na || !nb) return !na && !nb;
    for (size_t i = 0; i < a.pointCount(); ++i) {
        if (!(na->read<Vec3>()[i] == nb->read<Vec3>()[i])) return false;
    }
    return true;
}

}  // namespace

TEST(sculptor_goes_on_from_where_it_got_to) {
    // A sheet with normals and open borders, under dabs of every tool:
    // going on from the last cook -- more dabs, the last one other, one
    // taken back -- makes what all of them from the start make.
    Graph g;
    CookEngine engine;
    Node* grid = flatGrid(g, 30);
    auto sheet = std::make_shared<Geometry>(*engine.cook(*grid, CookContext{}));
    uint32_t seed = 3;
    auto next = [&seed]() {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<float>(seed >> 8) / static_cast<float>(1u << 24);
    };
    for (Vec3& p : sheet->positionsForWrite()) p.y = 0.05f * next();
    auto N = sheet->points().create("N", AttrType::Vec3).write<Vec3>();
    std::fill(N.begin(), N.end(), Vec3(0.0f, 1.0f, 0.0f));
    const GeometryPtr source = sheet;
    std::vector<SculptDab> dabs;
    for (int k = 0; k < 120; ++k) {
        SculptDab d;
        d.tool = static_cast<SculptDab::Tool>(k % 4);
        d.at = Vec3(next() - 0.5f, 0.05f * next(), next() - 0.5f);
        d.normal = normalize(Vec3(next() - 0.5f, 1.0f, next() - 0.5f));
        d.move = Vec3(0.1f * (next() - 0.5f), 0.1f * next(), 0.1f * (next() - 0.5f));
        d.radius = k % 13 == 0 ? 0.6f : 0.02f + 0.15f * next();
        d.strength = d.tool == SculptDab::Tool::Push ? 2.0f * next() - 1.0f : next();
        dabs.push_back(d);
    }
    const auto whole = [&](std::span<const SculptDab> some, Falloff shape) {
        Geometry geo(*source);
        sculpt(geo, some, shape);
        return geo;
    };
    const auto first = [&](size_t m) { return std::span<const SculptDab>(dabs.data(), m); };
    Sculptor sculptor;
    size_t had = 0;
    for (const size_t m : {1u, 2u, 3u, 30u, 31u, 90u, 120u}) {
        const GeometryPtr got = sculptor.cook(source, first(m), Falloff::Smooth);
        CHECK_EQ(sculptor.reused(), had);  // only the new ones
        CHECK(sameShape(*got, whole(first(m), Falloff::Smooth)));
        had = m;
    }
    // Asked again: what it gave.
    const GeometryPtr again = sculptor.cook(source, dabs, Falloff::Smooth);
    CHECK_EQ(sculptor.reused(), dabs.size());
    CHECK(sameShape(*again, whole(dabs, Falloff::Smooth)));
    // A grab that moves on: the last dab other, again and again.
    std::vector<SculptDab> grab = dabs;
    grab.back().tool = SculptDab::Tool::Grab;
    for (int step = 1; step <= 3; ++step) {
        grab.back().move = Vec3(0.0f, 0.05f * static_cast<float>(step), 0.02f);
        const GeometryPtr got = sculptor.cook(source, grab, Falloff::Smooth);
        CHECK_EQ(sculptor.reused(), dabs.size() - 1);
        CHECK(sameShape(*got, whole(grab, Falloff::Smooth)));
    }
    // One taken back, then another: the first from before the last, the
    // second from the start.
    GeometryPtr got = sculptor.cook(source, first(119), Falloff::Smooth);
    CHECK_EQ(sculptor.reused(), 119u);
    CHECK(sameShape(*got, whole(first(119), Falloff::Smooth)));
    got = sculptor.cook(source, first(118), Falloff::Smooth);
    CHECK_EQ(sculptor.reused(), 0u);
    CHECK(sameShape(*got, whole(first(118), Falloff::Smooth)));
    // Another falloff, a dab other in the middle, another geometry: all from the start.
    got = sculptor.cook(source, first(118), Falloff::Sphere);
    CHECK_EQ(sculptor.reused(), 0u);
    CHECK(sameShape(*got, whole(first(118), Falloff::Sphere)));
    std::vector<SculptDab> other(dabs.begin(), dabs.begin() + 118);
    other[50].radius *= 1.5f;
    got = sculptor.cook(source, other, Falloff::Sphere);
    CHECK_EQ(sculptor.reused(), 0u);
    CHECK(sameShape(*got, whole(other, Falloff::Sphere)));
    const GeometryPtr copy = std::make_shared<Geometry>(*source);
    got = sculptor.cook(copy, other, Falloff::Sphere);
    CHECK_EQ(sculptor.reused(), 0u);
    CHECK(sameShape(*got, whole(other, Falloff::Sphere)));
    // What it gave is not changed by what it does after.
    const Geometry kept(*got);
    sculptor.cook(copy, dabs, Falloff::Sphere);
    CHECK(sameShape(*got, kept));
}

TEST(sculpt_node_cooks_a_stroke_going_on_as_it_would_all_of_it) {
    // The brush adds dabs to the node's text, a few at a time: each cook
    // makes what a new node with all the text makes.
    Graph g;
    CookEngine engine;
    Node* grid = flatGrid(g, 40);
    Node* s = g.create("sculpt", "sculpt");
    s->setInput(0, grid);
    std::string strokes;
    for (int k = 0; k < 24; ++k) {
        SculptDab d;
        d.tool = k % 5 == 4 ? SculptDab::Tool::Smooth : SculptDab::Tool::Push;
        d.at = Vec3(-0.4f + 0.035f * static_cast<float>(k), 0.0f, 0.1f);
        d.normal = Vec3(0.0f, 1.0f, 0.0f);
        d.radius = 0.15f;
        d.strength = 0.8f;
        strokes += (strokes.empty() ? "" : "; ") + sculptText(d);
        if (k % 3 != 2) continue;
        s->setString("strokes", strokes);
        const GeometryPtr got = engine.cook(*s, CookContext{});
        Graph fresh;
        CookEngine other;
        Node* grid2 = flatGrid(fresh, 40);
        Node* s2 = fresh.create("sculpt", "sculpt");
        s2->setInput(0, grid2);
        s2->setString("strokes", strokes);
        CHECK(sameShape(*got, *other.cook(*s2, CookContext{})));
    }
}

TEST(edit_transform_is_the_edit_node_and_a_drag_after_it_is_one_again) {
    // What the Edit node cooks, the transform says.
    Graph g;
    CookEngine engine;
    Node* grid = flatGrid(g, 4);
    Node* edit = g.create("edit", "edit");
    edit->setInput(0, grid);
    edit->setString("group", "*");
    sim::EditTransform e;
    e.t = Vec3(0.2f, 0.5f, -0.1f);
    e.r = Vec3(30.0f, -40.0f, 75.0f);
    e.s = Vec3(1.5f, 0.5f, 2.0f);
    e.p = Vec3(0.1f, 0.0f, 0.3f);
    edit->setVec3("t", e.t);
    edit->setVec3("r", e.r);
    edit->setVec3("s", e.s);
    edit->setVec3("p", e.p);
    GeometryPtr before = engine.cook(*grid, CookContext{});
    GeometryPtr after = engine.cook(*edit, CookContext{});
    for (size_t i = 0; i < before->pointCount(); ++i) {
        const Vec3 want = e.apply(before->positions()[i]), got = after->positions()[i];
        CHECK_NEAR(length(want - got), 0.0f, 1e-5f);
    }
    // A turn, a size along its axes, a move -- after it, about a point:
    // the transform they make does both.
    const Vec3 c(0.3f, 0.7f, -0.2f);
    const sim::Rotation turn = sim::Rotation::about(Vec3(1.0f, 2.0f, 0.5f), 33.0f);
    const sim::EditTransform turned = e.turned(turn, c);
    const sim::Rotation axes = sim::Rotation::fromEuler(e.r);
    const Vec3 k(1.3f, 0.7f, 2.1f);
    const sim::EditTransform sized = e.sized(k, c);
    const sim::EditTransform moved = e.moved(Vec3(0.0f, 1.0f, 0.0f));
    for (const Vec3& x : {Vec3(0.0f, 0.0f, 0.0f), Vec3(1.0f, -2.0f, 0.5f), Vec3(-0.7f, 0.3f, 2.0f)}) {
        const Vec3 y = e.apply(x);
        CHECK_NEAR(length(turned.apply(x) - (turn.apply(y - c) + c)), 0.0f, 1e-4f);
        const Vec3 l = axes.inverse(y - c);
        CHECK_NEAR(length(sized.apply(x) - (axes.apply(Vec3(l.x * k.x, l.y * k.y, l.z * k.z)) + c)), 0.0f, 1e-4f);
        CHECK_NEAR(length(moved.apply(x) - (y + Vec3(0.0f, 1.0f, 0.0f))), 0.0f, 1e-5f);
    }
    // Its turn is the node's: Mat4's Euler angles and the Rotation's agree.
    for (const Vec3& v : {Vec3(1.0f, 0.0f, 0.0f), Vec3(0.0f, 1.0f, 0.0f), Vec3(0.3f, -0.4f, 0.8f)}) {
        CHECK_NEAR(length(transformDirection(rotationXYZ(e.r), v) - axes.apply(v)), 0.0f, 1e-5f);
    }
}

TEST(extrude_and_wrangles_take_a_pattern_of_elements) {
    Graph g;
    CookEngine engine;
    Node* grid = flatGrid(g, 4);  // 16 quads
    Node* extrude = g.create("polyextrude", "extrude");
    extrude->setInput(0, grid);
    extrude->setString("group", "0-1 5");  // three quads, as Tab in the viewport writes them
    GeometryPtr out = engine.cook(*extrude, CookContext{});
    const Group* front = out->findGroup("extrudeFront");
    CHECK(front && front->memberCount() == 3u);
    extrude->setString("group", "nosuch");
    out = engine.cook(*extrude, CookContext{});
    CHECK_EQ(out->primitiveCount(), 16u);  // names nothing there is: said, and left as it was
    // A Point Wrangle over the points a pattern names, and over an edge's.
    Node* wrangle = g.create("pointwrangle", "wrangle");
    wrangle->setInput(0, grid);
    wrangle->setString("snippet", "@P.y = 1;");
    wrangle->setString("group", "0-2 24");
    out = engine.cook(*wrangle, CookContext{});
    size_t up = 0;
    for (const Vec3& p : out->positions()) up += p.y > 0.5f;
    CHECK_EQ(up, 4u);
    wrangle->setString("group", "p5-6-7");
    out = engine.cook(*wrangle, CookContext{});
    up = 0;
    for (const Vec3& p : out->positions()) up += p.y > 0.5f;
    CHECK_EQ(up, 3u);
}
