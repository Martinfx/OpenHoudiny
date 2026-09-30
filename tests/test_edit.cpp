//
// What the viewport's selection and brush make of geometry: elements named
// by a pattern (Selection.h), the Group, Edit and Attribute Paint nodes,
// Blast of a pattern or of primitives; and the same nodes in a network.
//
#include "pg/core/CookEngine.h"
#include "pg/core/Graph.h"
#include "pg/core/Selection.h"
#include "pg/nodes/Nodes.h"
#include "pg/sim/GeometryGraph.h"
#include "pg/sim/Network.h"

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
