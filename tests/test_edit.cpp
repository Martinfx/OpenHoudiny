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
#include "pg/core/Selection.h"
#include "pg/nodes/Nodes.h"
#include "pg/sim/GeometryGraph.h"
#include "pg/sim/Network.h"
#include "pg/sim/Shape.h"

#include "test_framework.h"

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
        CHECK_NEAR(length(Mat4::rotate(e.r).transformDirection(v) - axes.apply(v)), 0.0f, 1e-5f);
    }
}
