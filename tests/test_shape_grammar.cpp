//
// The shape grammar: its operations, its two front-ends, and how it sits on
// the core.
//
// The headline assertion is that one grammar gives bit-identical geometry
// whether it is written as text or wired up as a chain of rule nodes. That is
// the claim behind building a node editor on top of a text grammar at all:
// the graph is just another notation for the same derivation.
//
#include "pg/core/CookEngine.h"
#include "pg/core/Graph.h"
#include "pg/core/Parallel.h"
#include "pg/grammar/Grammar.h"
#include "pg/nodes/Nodes.h"

#include "test_framework.h"

#include <cmath>
#include <map>
#include <utility>

using namespace pg;
using namespace pg::grammar;

namespace {

constexpr size_t kBigBudget = 1ull << 30;

SplitPattern pattern(const std::string& text) {
    SplitPattern p;
    std::string error;
    if (!parseSplitPattern(text, p, error)) {
        ::testing::fail(__FILE__, __LINE__, "pattern did not parse: " + error);
    }
    return p;
}

bool near(const Vec3& a, const Vec3& b, float eps = 1e-5f) {
    return length(a - b) <= eps;
}

std::map<std::string, size_t> symbolCounts(const Geometry& shapes) {
    const ShapeView view(shapes);
    std::map<std::string, size_t> counts;
    for (size_t i = 0; i < view.size(); ++i) ++counts[view.symbol(i)];
    return counts;
}

Vec3 polygonNormal(const Geometry& g, size_t prim) {
    const auto P = g.positions();
    const auto pts = g.primitivePoints(prim);
    Vec3 n(0.0f);
    for (size_t i = 0; i < pts.size(); ++i) {
        n += cross(P[pts[i]], P[pts[(i + 1) % pts.size()]]);
    }
    return n;
}

Vec3 polygonCentre(const Geometry& g, size_t prim) {
    const auto P = g.positions();
    const auto pts = g.primitivePoints(prim);
    Vec3 c(0.0f);
    for (uint32_t p : pts) c += P[p];
    return c * (1.0f / static_cast<float>(pts.size()));
}

/// Divergence theorem: positive exactly when a closed mesh faces outwards.
double signedVolume(const Geometry& g) {
    const auto P = g.positions();
    double v = 0.0;
    for (size_t prim = 0; prim < g.primitiveCount(); ++prim) {
        const auto pts = g.primitivePoints(prim);
        for (size_t k = 1; k + 1 < pts.size(); ++k) {
            v += dot(P[pts[0]], cross(P[pts[k]], P[pts[k + 1]])) / 6.0;
        }
    }
    return v;
}

/// Appends a node to a chain and sets its string parameters.
Node* append(Graph& g, Node*& tail, const std::string& type, const std::string& name,
             std::initializer_list<std::pair<std::string, std::string>> params) {
    Node* n = g.create(type, name);
    n->setInput(0, tail);
    for (const auto& [key, value] : params) n->setString(key, value);
    tail = n;
    return n;
}

constexpr const char* kCityRules = R"(
    // four lots, streets around each, a building on every parcel
    Lot      --> split(x) { 2: NIL | ~1: Strip | 2: NIL }
    Strip    --> split(z) { 2: NIL | ~1: Parcel | 2: NIL }
    Parcel   --> extrude(height) Mass
    Mass     --> comp(f) { front: Entrance | side: Facade | top: RoofBase }
    RoofBase --> roofHip(30) Roof
    Entrance --> split(y) { 3.5: Ground | { ~3: Floor }* }
    Facade   --> split(y) { { ~3: Floor }* }
    Ground   --> split(x) { ~1: Wall | 2: Door | ~1: Wall }
    Door     --> extrude(-0.3) DoorRecess
    Floor    --> split(x) { 0.5: Wall | { ~2.5: Tile }* | 0.5: Wall }
    Tile     --> split(x) { ~1: Wall | 1.2: Bay | ~1: Wall }
    Bay      --> split(y) { ~1: Wall | 1.4: Window | 0.5: Wall }
    Window   --> extrude(-0.2) Glass
)";

/// Four lots with their own heights, derived twice from the same shapes: once
/// by the text grammar above, once by a hand-wired chain of rule nodes.
struct City {
    Graph graph;
    Node* heights = nullptr;
    Node* text = nullptr;   ///< shapegrammar
    Node* nodes = nullptr;  ///< last node of the rule chain
    Node* tiles = nullptr;
};

std::unique_ptr<City> buildCity() {
    auto c = std::make_unique<City>();
    Graph& g = c->graph;

    Node* grid = g.create("grid", "grid");
    grid->setInt("rows", 3);
    grid->setInt("cols", 3);
    grid->setFloat("sizex", 40.0f);
    grid->setFloat("sizez", 40.0f);
    Node* lots = g.create("lot", "lots");
    lots->setInput(0, grid);
    c->heights = g.create("pointwrangle", "heights");
    c->heights->setInput(0, lots);
    c->heights->setString("snippet", "@height = 6.0 + @ptnum * 3.0;");

    c->text = g.create("shapegrammar", "text");
    c->text->setInput(0, c->heights);
    c->text->setString("rules", kCityRules);

    Node* tail = c->heights;
    append(g, tail, "split", "strips", {{"shape", "Lot"}, {"axis", "x"},
                                        {"pattern", "{ 2: NIL | ~1: Strip | 2: NIL }"}});
    append(g, tail, "split", "parcels", {{"shape", "Strip"}, {"axis", "z"},
                                         {"pattern", "{ 2: NIL | ~1: Parcel | 2: NIL }"}});
    append(g, tail, "extrude", "mass", {{"shape", "Parcel"}, {"attrib", "height"}, {"name", "Mass"}});
    append(g, tail, "comp", "faces", {{"shape", "Mass"}, {"front", "Entrance"},
                                      {"side", "Facade"}, {"top", "RoofBase"}});
    append(g, tail, "roof", "roof", {{"shape", "RoofBase"}, {"type", "hip"}, {"name", "Roof"}})
        ->setFloat("angle", 30.0f);
    append(g, tail, "split", "entrance", {{"shape", "Entrance"}, {"axis", "y"},
                                          {"pattern", "{ 3.5: Ground | { ~3: Floor }* }"}});
    append(g, tail, "split", "facade", {{"shape", "Facade"}, {"axis", "y"},
                                        {"pattern", "{ { ~3: Floor }* }"}});
    append(g, tail, "split", "ground", {{"shape", "Ground"}, {"axis", "x"},
                                        {"pattern", "{ ~1: Wall | 2: Door | ~1: Wall }"}});
    append(g, tail, "extrude", "door", {{"shape", "Door"}, {"name", "DoorRecess"}})
        ->setFloat("amount", -0.3f);
    append(g, tail, "split", "floors", {{"shape", "Floor"}, {"axis", "x"},
                                        {"pattern", "{ 0.5: Wall | { ~2.5: Tile }* | 0.5: Wall }"}});
    c->tiles = append(g, tail, "split", "tiles", {{"shape", "Tile"}, {"axis", "x"},
                                                  {"pattern", "{ ~1: Wall | 1.2: Bay | ~1: Wall }"}});
    append(g, tail, "split", "bays", {{"shape", "Bay"}, {"axis", "y"},
                                      {"pattern", "{ ~1: Wall | 1.4: Window | 0.5: Wall }"}});
    append(g, tail, "extrude", "windows", {{"shape", "Window"}, {"name", "Glass"}})
        ->setFloat("amount", -0.2f);
    c->nodes = tail;
    return c;
}

/// Restores the pool size however the test exits.
struct ThreadCountGuard {
    unsigned saved = TaskPool::instance().threadCount();
    ~ThreadCountGuard() { TaskPool::instance().setThreadCount(saved); }
};

}  // namespace

// --- split -------------------------------------------------------------------

TEST(split_gives_absolute_sizes_first_and_shares_the_rest_between_floating_ones) {
    const SplitPattern p = pattern("{ 4: A | ~1: B | ~2: C }");
    const auto pieces = layoutSplit(p, 10.0f);
    CHECK_EQ(pieces.size(), 3u);
    CHECK_EQ(pieces[0].part->symbol, "A");
    CHECK_NEAR(pieces[0].length, 4.0, 1e-5);
    CHECK_NEAR(pieces[1].start, 4.0, 1e-5);
    CHECK_NEAR(pieces[1].length, 2.0, 1e-5);  // the 6 left over, shared 1 : 2
    CHECK_NEAR(pieces[2].length, 4.0, 1e-5);
    CHECK_EQ(pieces[2].part->symbol, "C");
}

TEST(split_relative_sizes_are_fractions_of_the_scope) {
    const SplitPattern p = pattern("{ '0.25: A | ~1: B }");
    const auto pieces = layoutSplit(p, 8.0f);
    CHECK_EQ(pieces.size(), 2u);
    CHECK_NEAR(pieces[0].length, 2.0, 1e-5);
    CHECK_NEAR(pieces[1].length, 6.0, 1e-5);
}

TEST(repeat_split_tiles_as_many_as_fit_and_stretches_them_to_fill) {
    const SplitPattern p = pattern("{ ~3: T }*");
    const auto pieces = layoutSplit(p, 10.0f);
    CHECK_EQ(pieces.size(), 3u);  // 10 / 3 rounds to 3
    for (const auto& piece : pieces) CHECK_NEAR(piece.length, 10.0 / 3.0, 1e-5);
    CHECK_NEAR(pieces.back().start + pieces.back().length, 10.0, 1e-5);
}

TEST(repeat_split_keeps_fixed_parts_around_the_repeated_group) {
    const SplitPattern p = pattern("{ 1: W | { ~3: T }* | 1: W }");
    const auto pieces = layoutSplit(p, 11.0f);
    CHECK_EQ(pieces.size(), 5u);
    const char* expected[] = {"W", "T", "T", "T", "W"};
    for (size_t i = 0; i < 5; ++i) CHECK_EQ(pieces[i].part->symbol, expected[i]);
    CHECK_NEAR(pieces[1].length, 3.0, 1e-5);
    CHECK_NEAR(pieces[4].start, 10.0, 1e-5);
}

TEST(an_absolute_repeat_keeps_its_size_and_leaves_the_remainder_empty) {
    const SplitPattern p = pattern("{ 3: A }*");
    const auto pieces = layoutSplit(p, 10.0f);
    CHECK_EQ(pieces.size(), 3u);
    CHECK_NEAR(pieces[2].start + pieces[2].length, 9.0, 1e-5);
}

TEST(nil_takes_up_space_but_makes_no_shape) {
    const SplitPattern p = pattern("{ 2: NIL | ~1: A | 2: NIL }");
    CHECK_EQ(layoutSplit(p, 10.0f).size(), 3u);

    Shape s;
    s.scope.size = Vec3(10, 0, 5);
    std::vector<Successor> out;
    emitSplit(0, s, 0, p, out);
    CHECK_EQ(out.size(), 1u);
    CHECK(near(out[0].shape.scope.origin, Vec3(2, 0, 0)));
    CHECK_NEAR(out[0].shape.scope.size.x, 6.0, 1e-5);
    CHECK_NEAR(out[0].shape.scope.size.z, 5.0, 1e-5);  // other extents untouched
}

TEST(parts_that_do_not_fit_are_clipped_at_the_end_of_the_scope) {
    const SplitPattern p = pattern("{ 6: A | 6: B | 6: C }");
    const auto pieces = layoutSplit(p, 10.0f);
    CHECK_EQ(pieces.size(), 2u);
    CHECK_NEAR(pieces[1].length, 4.0, 1e-5);
}

TEST(pattern_errors_say_where_and_what) {
    SplitPattern p;
    std::string error;
    CHECK(!parseSplitPattern("{ 3 A }", p, error));
    CHECK(error.find("line 1, col 5") != std::string::npos);
    CHECK(error.find("':'") != std::string::npos);

    CHECK(!parseSplitPattern("{ 1: A | { ~1: B }* | { ~1: C }* }", p, error));
    CHECK(error.find("one repeat group") != std::string::npos);

    CHECK(!parseSplitPattern("{ -1: A }", p, error));
    CHECK(error.find("negative") != std::string::npos);

    CHECK(!parseSplitPattern("{ ~1: A", p, error));
    CHECK(error.find("'}'") != std::string::npos);
}

// --- comp, extrude, lots ------------------------------------------------------

TEST(comp_faces_are_right_handed_and_face_outwards) {
    Scope box;
    box.origin = Vec3(1, 2, 3);
    box.size = Vec3(4, 5, 6);
    const Vec3 centre = box.at(2.0f, 2.5f, 3.0f);

    for (int f = 0; f < kFaceCount; ++f) {
        const Scope s = faceScope(box, static_cast<Face>(f));
        CHECK(near(cross(s.x, s.y), s.z));
        CHECK_EQ(s.size.z, 0.0f);
        const Vec3 faceCentre = s.at(s.size.x * 0.5f, s.size.y * 0.5f, 0.0f);
        CHECK(dot(s.z, faceCentre - centre) > 0.0f);
        // Every corner of the face is a corner of the box.
        for (float u : {0.0f, s.size.x}) {
            for (float v : {0.0f, s.size.y}) {
                const Vec3 p = s.at(u, v, 0.0f) - box.origin;
                CHECK(p.x >= -1e-5f && p.x <= 4.00001f);
                CHECK(p.y >= -1e-5f && p.y <= 5.00001f);
                CHECK(p.z >= -1e-5f && p.z <= 6.00001f);
            }
        }
        if (f < 4) CHECK(near(s.y, Vec3(0, 1, 0)));  // side faces keep "up"
    }

    // The front faces +Z, and its x runs along the box's own x.
    const Scope front = faceScope(box, Face::Front);
    CHECK(near(front.z, Vec3(0, 0, 1)));
    CHECK(near(front.x, Vec3(1, 0, 0)));
    CHECK(near(front.origin, Vec3(1, 2, 9)));
    CHECK(near(faceScope(box, Face::Right).size, Vec3(6, 5, 0)));
    CHECK(near(faceScope(box, Face::Top).size, Vec3(4, 6, 0)));
}

TEST(extrude_grows_a_flat_shape_along_its_normal) {
    Shape lot;
    lot.scope.size = Vec3(10, 0, 8);
    const Shape mass = extrude(lot, 12.0f);
    CHECK(near(mass.scope.size, Vec3(10, 12, 8)));  // a footprint grows up, along y

    Shape face{faceScope(mass.scope, Face::Front), Asset::Box};
    const Shape slab = extrude(face, 0.5f);
    CHECK(near(slab.scope.size, Vec3(10, 12, 0.5f)));  // a facade grows out, along z
    CHECK(near(slab.scope.origin, face.scope.origin));

    const Shape recess = extrude(face, -0.3f);
    CHECK(recess.asset == Asset::Recess);
    CHECK_NEAR(recess.scope.size.z, 0.3, 1e-6);
    CHECK(near(recess.scope.origin, face.scope.origin - face.scope.z * 0.3f));

    CHECK(near(extrude(mass, 20.0f).scope.size, Vec3(10, 20, 8)));  // a volume: new height
}

TEST(a_lot_is_fitted_to_its_polygon_facing_up_whatever_the_winding) {
    Graph g;
    Node* grid = g.create("grid", "grid");
    grid->setInt("rows", 2);
    grid->setInt("cols", 2);
    grid->setFloat("sizex", 20.0f);
    grid->setFloat("sizez", 12.0f);
    Node* lots = g.create("lot", "lots");
    lots->setInput(0, grid);

    CookEngine engine(kBigBudget);
    GeometryPtr out = engine.cook(*lots, CookContext{});
    const ShapeView view(*out);
    CHECK_EQ(view.size(), 1u);
    const Scope s = view.shape(0).scope;
    CHECK(near(s.origin, Vec3(-10, 0, -6)));
    CHECK(near(s.x, Vec3(1, 0, 0)));
    CHECK(near(s.y, Vec3(0, 1, 0)));
    CHECK(near(s.z, Vec3(0, 0, 1)));
    CHECK(near(s.size, Vec3(20, 0, 12)));
    CHECK_EQ(view.symbol(0), "Lot");

    // The same rectangle wound the other way gives the same scope.
    const Vec3 reversed[] = {Vec3(-10, 0, 6), Vec3(10, 0, 6), Vec3(10, 0, -6), Vec3(-10, 0, -6)};
    Scope r;
    CHECK(fitPolygon(reversed, r));
    CHECK(near(r.origin, s.origin));
    CHECK(near(r.y, Vec3(0, 1, 0)));
    CHECK(near(r.size, s.size));
}

// --- the grammar as nodes ---------------------------------------------------------

TEST(a_chain_of_rule_nodes_derives_a_building) {
    Graph g;
    Node* tail = g.create("axiom", "axiom");
    tail->setVec3("size", Vec3(10, 12, 8));
    append(g, tail, "comp", "faces", {{"shape", "Axiom"}, {"side", "Facade"}, {"top", "Roof"}});
    append(g, tail, "split", "floors", {{"shape", "Facade"}, {"axis", "y"}, {"pattern", "{ ~3: Floor }*"}});

    CookEngine engine(kBigBudget);
    GeometryPtr out = engine.cook(*tail, CookContext{});
    auto counts = symbolCounts(*out);
    CHECK_EQ(counts["Floor"], 16u);  // 4 facades x 12 / 3 floors
    CHECK_EQ(counts["Roof"], 1u);
    CHECK_EQ(out->pointCount(), 17u);

    const AttributeArray* path = out->points().find(kAttrPath);
    CHECK_EQ(path->stringValue(path->read<int32_t>()[0]), "Axiom/Facade/Floor");
}

TEST(a_rule_that_matches_nothing_hands_its_input_on_untouched) {
    Graph g;
    Node* axiom = g.create("axiom", "axiom");
    Node* split = g.create("split", "split");
    split->setInput(0, axiom);
    split->setString("shape", "NoSuchSymbol");

    CookEngine engine(kBigBudget);
    GeometryPtr in = engine.cook(*axiom, CookContext{});
    GeometryPtr out = engine.cook(*split, CookContext{});
    CHECK_EQ(in.get(), out.get());  // not a copy: the very same geometry
}

TEST(extrude_can_take_its_amount_from_an_attribute) {
    auto c = buildCity();
    Node* mass = c->graph.find("mass");
    CookEngine engine(kBigBudget);
    GeometryPtr out = engine.cook(*mass, CookContext{});

    const ShapeView view(*out);
    const auto height = out->points().find("height")->read<float>();
    CHECK_EQ(view.size(), 4u);
    for (size_t i = 0; i < view.size(); ++i) {
        CHECK_EQ(view.symbol(i), "Mass");
        CHECK_NEAR(view.shape(i).scope.size.y, 6.0 + 3.0 * static_cast<double>(i), 1e-5);
        CHECK_EQ(view.shape(i).scope.size.y, height[i]);
    }
}

TEST(successors_inherit_attributes_and_remember_their_path) {
    auto c = buildCity();
    CookEngine engine(kBigBudget);
    GeometryPtr out = engine.cook(*c->nodes, CookContext{});

    const ShapeView view(*out);
    const auto height = out->points().find("height")->read<float>();
    const AttributeArray* path = out->points().find(kAttrPath);
    size_t glass = 0;
    for (size_t i = 0; i < view.size(); ++i) {
        if (view.symbol(i) != "Glass") continue;
        ++glass;
        // Nobody copied `height` down to the windows; gather did.
        const float h = height[i];
        CHECK(h == 6.0f || h == 9.0f || h == 12.0f || h == 15.0f);
        CHECK(view.shape(i).scope.origin.y < h);
        const std::string& p = path->stringValue(path->read<int32_t>()[i]);
        CHECK(p == "Lot/Strip/Parcel/Mass/Facade/Floor/Tile/Bay/Window/Glass" ||
              p == "Lot/Strip/Parcel/Mass/Entrance/Floor/Tile/Bay/Window/Glass");
    }
    CHECK(glass > 100u);
}

// --- text and nodes ---------------------------------------------------------

TEST(the_text_grammar_and_the_node_chain_build_identical_geometry) {
    auto c = buildCity();
    CookEngine engine(kBigBudget);
    GeometryPtr fromText = engine.cook(*c->text, CookContext{});
    GeometryPtr fromNodes = engine.cook(*c->nodes, CookContext{});

    CHECK(grammarError(*c->text).empty());
    CHECK(fromText->pointCount() > 1000u);
    CHECK_EQ(fromText->pointCount(), fromNodes->pointCount());
    CHECK_EQ(fromText->hash(), fromNodes->hash());
    CHECK_EQ(mesh(*fromText)->hash(), mesh(*fromNodes)->hash());

    auto counts = symbolCounts(*fromText);
    CHECK_EQ(counts["Roof"], 4u);
    CHECK_EQ(counts["DoorRecess"], 4u);
    // Floors per building: entrance 1..4 above the ground floor, and 2..5 on each
    // of the three other faces -- 52 in all, 16 m wide, so 6 tiles and 6 windows each.
    CHECK_EQ(counts["Glass"], 52u * 6u);
    CHECK_EQ(counts["Lot"] + counts["Mass"] + counts["Window"], 0u);  // all rewritten
}

TEST(the_articles_example_as_text_and_as_nodes) {
    // nelari.us: a unit cube; its front face tiled 2 x 2, each tile framed and
    // its window pushed out by 0.1; a hip roof at 29.5 degrees on top. The
    // article's pairs of binary splits become one three-part split here.
    Graph g;
    Node* cube = g.create("axiom", "cube");
    Node* text = g.create("shapegrammar", "text");
    text->setInput(0, cube);
    text->setString("rules", R"(
        Axiom    --> comp(f) { front: Facade | side: Wall | top: RoofBase | bottom: Wall }
        Facade   --> split(y) { ~1: Row | ~1: Row }
        Row      --> split(x) { ~1: Tile | ~1: Tile }
        Tile     --> split(x) { '0.2: Wall | '0.592: Column | ~1: Wall }
        Column   --> split(y) { '0.2: Wall | '0.592: Window | ~1: Wall }
        Window   --> extrude(0.1) WindowBox
        RoofBase --> roofHip(29.5) Roof
    )");

    Node* tail = cube;
    append(g, tail, "comp", "select", {{"shape", "Axiom"}, {"front", "Facade"}, {"side", "Wall"},
                                       {"top", "RoofBase"}, {"bottom", "Wall"}});
    append(g, tail, "split", "rows", {{"shape", "Facade"}, {"axis", "y"},
                                      {"pattern", "{ ~1: Row | ~1: Row }"}});
    append(g, tail, "split", "tiles", {{"shape", "Row"}, {"axis", "x"},
                                       {"pattern", "{ ~1: Tile | ~1: Tile }"}});
    append(g, tail, "split", "frame_x", {{"shape", "Tile"}, {"axis", "x"},
                                         {"pattern", "{ '0.2: Wall | '0.592: Column | ~1: Wall }"}});
    append(g, tail, "split", "frame_y", {{"shape", "Column"}, {"axis", "y"},
                                         {"pattern", "{ '0.2: Wall | '0.592: Window | ~1: Wall }"}});
    append(g, tail, "extrude", "window", {{"shape", "Window"}, {"name", "WindowBox"}})
        ->setFloat("amount", 0.1f);
    append(g, tail, "roof", "roof", {{"shape", "RoofBase"}, {"type", "hip"}, {"name", "Roof"}})
        ->setFloat("angle", 29.5f);

    CookEngine engine(kBigBudget);
    GeometryPtr a = engine.cook(*text, CookContext{});
    GeometryPtr b = engine.cook(*tail, CookContext{});
    CHECK_EQ(a->hash(), b->hash());

    auto counts = symbolCounts(*a);
    CHECK_EQ(counts["WindowBox"], 4u);
    CHECK_EQ(counts["Wall"], 4u + 4u * 4u);  // 4 other faces + 4 frame pieces per tile
    CHECK_EQ(counts["Roof"], 1u);
}

TEST(a_recursive_grammar_stops_at_maxdepth_and_says_so) {
    Graph g;
    Node* axiom = g.create("axiom", "axiom");
    Node* text = g.create("shapegrammar", "text");
    text->setInput(0, axiom);
    text->setString("rules", "Axiom --> split(x) { ~1: Axiom | ~1: Done }");
    text->setInt("maxdepth", 5);

    CookEngine engine(kBigBudget);
    GeometryPtr out = engine.cook(*text, CookContext{});
    auto counts = symbolCounts(*out);
    CHECK_EQ(counts["Done"], 5u);
    CHECK_EQ(counts["Axiom"], 1u);  // still waiting for a sixth pass
    CHECK(grammarError(*text).find("recursive") != std::string::npos);
}

TEST(a_grammar_syntax_error_is_reported_and_the_shapes_pass_through) {
    Graph g;
    Node* axiom = g.create("axiom", "axiom");
    Node* text = g.create("shapegrammar", "text");
    text->setInput(0, axiom);
    text->setString("rules", "Axiom --> comp(f) { top: Roof }\nRoof --> split(y) { 3 Floor }");

    CookEngine engine(kBigBudget);
    GeometryPtr in = engine.cook(*axiom, CookContext{});
    GeometryPtr out = engine.cook(*text, CookContext{});
    CHECK_EQ(in.get(), out.get());
    CHECK(grammarError(*text).find("line 2, col 23") != std::string::npos);

    std::string error;
    CHECK(!Grammar::parse("A --> B\nA --> C", error));
    CHECK(error.find("already has a rule") != std::string::npos);
    CHECK(!Grammar::parse("A --> twist(3) B", error));
    CHECK(error.find("unknown operation 'twist'") != std::string::npos);
    CHECK(!Grammar::parse("A --> B C", error));
    CHECK(error.find("after the successor") != std::string::npos);
}

TEST(a_split_node_with_a_bad_pattern_reports_it_and_passes_through) {
    Graph g;
    Node* axiom = g.create("axiom", "axiom");
    Node* split = g.create("split", "split");
    split->setInput(0, axiom);
    split->setString("shape", "Axiom");
    split->setString("pattern", "{ ~1: A | }");

    CookEngine engine(kBigBudget);
    GeometryPtr out = engine.cook(*split, CookContext{});
    CHECK_EQ(out->pointCount(), 1u);
    CHECK(!grammarError(*split).empty());

    split->setString("pattern", "{ ~1: A | ~1: B }");
    engine.cook(*split, CookContext{});
    CHECK(grammarError(*split).empty());
}

// --- the core underneath ------------------------------------------------------

TEST(a_derivation_is_bitwise_identical_on_one_and_many_threads) {
    ThreadCountGuard guard;
    auto c = buildCity();

    TaskPool::instance().setThreadCount(1);
    CookEngine serial(kBigBudget);
    const uint64_t a = serial.cook(*c->text, CookContext{})->hash();
    const uint64_t b = serial.cook(*c->nodes, CookContext{})->hash();

    TaskPool::instance().setThreadCount(4);
    CookEngine parallel(kBigBudget);
    CHECK_EQ(parallel.cook(*c->text, CookContext{})->hash(), a);
    CHECK_EQ(parallel.cook(*c->nodes, CookContext{})->hash(), b);
}

TEST(editing_one_rule_recooks_only_the_rules_after_it) {
    auto c = buildCity();
    CookEngine engine(kBigBudget);
    engine.cook(*c->nodes, CookContext{});
    for (Node* n : c->graph.nodes()) n->resetCookCount();

    c->tiles->setString("pattern", "{ ~1: Wall | 1.6: Bay | ~1: Wall }");
    engine.cook(*c->nodes, CookContext{});

    CHECK_EQ(c->graph.find("heights")->cookCount(), 0u);
    CHECK_EQ(c->graph.find("floors")->cookCount(), 0u);
    CHECK_EQ(c->tiles->cookCount(), 1u);
    CHECK_EQ(c->graph.find("bays")->cookCount(), 1u);
    CHECK_EQ(c->nodes->cookCount(), 1u);
}

// --- meshing ------------------------------------------------------------------

TEST(a_box_meshes_closed_and_facing_outwards) {
    Graph g;
    Node* axiom = g.create("axiom", "axiom");
    axiom->setVec3("origin", Vec3(5, 0, -3));
    axiom->setVec3("size", Vec3(2, 3, 4));
    Node* m = g.create("shapemesh", "mesh");
    m->setInput(0, axiom);

    CookEngine engine(kBigBudget);
    GeometryPtr out = engine.cook(*m, CookContext{});
    CHECK_EQ(out->pointCount(), 8u);
    CHECK_EQ(out->primitiveCount(), 6u);
    CHECK_NEAR(signedVolume(*out), 24.0, 1e-3);

    // Polygons carry the attributes of their shape, but not its scope.
    CHECK(out->primitives().contains(kAttrShape));
    CHECK(out->primitives().contains(kAttrPath));
    CHECK(!out->primitives().contains(kAttrSize));
}

TEST(a_recess_is_an_open_box_facing_inwards) {
    Shape face;
    face.scope.size = Vec3(2, 3, 0);  // a facade piece facing +z
    Geometry seed;
    seed.addPoints(1);
    GeometryPtr shapes = materialize(seed, {Successor{0, extrude(face, -0.5f), nullptr, true}});
    GeometryPtr out = mesh(*shapes);

    CHECK_EQ(out->primitiveCount(), 5u);  // no lid over the opening
    const Vec3 centre(1.0f, 1.5f, -0.25f);
    for (size_t prim = 0; prim < out->primitiveCount(); ++prim) {
        CHECK(dot(polygonNormal(*out, prim), polygonCentre(*out, prim) - centre) < 0.0f);
    }
}

TEST(roofs_rise_at_their_angle_and_face_outwards) {
    Shape top;  // a 10 x 6 top face, facing up
    top.scope.x = Vec3(1, 0, 0);
    top.scope.y = Vec3(0, 0, -1);
    top.scope.z = Vec3(0, 1, 0);
    top.scope.origin = Vec3(0, 5, 6);
    top.scope.size = Vec3(10, 6, 0);

    for (RoofType type : {RoofType::Hip, RoofType::Gable}) {
        const Shape r = roof(top, type, 45.0f);
        CHECK_NEAR(r.scope.size.z, 3.0, 1e-4);  // tan(45) x half the span

        Geometry seed;
        seed.addPoints(1);
        GeometryPtr out = mesh(*materialize(seed, {Successor{0, r, nullptr, true}}));
        CHECK_EQ(out->primitiveCount(), 4u);
        float highest = 0.0f;
        for (const Vec3& p : out->positions()) highest = std::max(highest, p.y);
        CHECK_NEAR(highest, 8.0, 1e-4);

        const Vec3 inside(5.0f, 5.5f, 3.0f);
        for (size_t prim = 0; prim < out->primitiveCount(); ++prim) {
            CHECK(dot(polygonNormal(*out, prim), polygonCentre(*out, prim) - inside) > 0.0f);
        }
    }

    // On a square, a hip roof closes into a pyramid.
    top.scope.size = Vec3(6, 6, 0);
    Geometry seed;
    seed.addPoints(1);
    GeometryPtr pyramid =
        mesh(*materialize(seed, {Successor{0, roof(top, RoofType::Hip, 30.0f), nullptr, true}}));
    CHECK_EQ(pyramid->primitiveCount(), 4u);
    for (size_t prim = 0; prim < pyramid->primitiveCount(); ++prim) {
        CHECK_EQ(pyramid->primitivePoints(prim).size(), 3u);
    }
}
