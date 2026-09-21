//
// The per-element language. At M5 the tree-walking evaluator behind these
// tests is replaced by a JIT; the tests themselves should survive unchanged,
// which is the point of writing them against behaviour rather than internals.
//
#include "pg/core/CookEngine.h"
#include "pg/core/Graph.h"
#include "pg/nodes/Expression.h"
#include "pg/nodes/Nodes.h"

#include "test_framework.h"

#include <cmath>

using namespace pg;

namespace {

Geometry pointsAlongX(size_t n) {
    Geometry g;
    g.addPoints(n);
    auto P = g.positionsForWrite();
    for (size_t i = 0; i < n; ++i) P[i] = Vec3(static_cast<float>(i), 0, 0);
    return g;
}

/// Parses and runs, failing the test with the compiler's own message.
void run(Geometry& g, const std::string& src, const CookContext& ctx = {}) {
    std::string error;
    auto prog = expr::Program::parse(src, error);
    if (!prog) ::testing::fail(__FILE__, __LINE__, "parse failed: " + error);
    if (!prog->run(g, ctx, error)) {
        ::testing::fail(__FILE__, __LINE__, "run failed: " + error);
    }
}

}  // namespace

TEST(arithmetic_and_precedence) {
    Geometry g = pointsAlongX(4);
    run(g, "@out = 2.0 + 3.0 * 4.0;");
    CHECK_EQ(g.points().find("out")->read<float>()[0], 14.0f);

    run(g, "@out2 = (2.0 + 3.0) * 4.0;");
    CHECK_EQ(g.points().find("out2")->read<float>()[0], 20.0f);

    run(g, "@out3 = -3.0 + 1.0;");
    CHECK_EQ(g.points().find("out3")->read<float>()[0], -2.0f);
}

TEST(a_new_attribute_gets_its_type_from_the_right_hand_side) {
    Geometry g = pointsAlongX(4);
    run(g, "@scalar = 1.0; @vector = @P * 2.0; @fromvec = vec3(1.0, 2.0, 3.0);");

    CHECK_EQ(g.points().find("scalar")->type(), AttrType::Float);
    CHECK_EQ(g.points().find("vector")->type(), AttrType::Vec3);
    CHECK_EQ(g.points().find("fromvec")->type(), AttrType::Vec3);
    CHECK_EQ(g.points().find("vector")->read<Vec3>()[3].x, 6.0f);
}

TEST(component_access_and_component_assignment) {
    Geometry g = pointsAlongX(4);
    run(g, "@P.y = @P.x * 10.0;");

    auto P = g.positions();
    CHECK_EQ(P[2].x, 2.0f);
    CHECK_EQ(P[2].y, 20.0f);
    CHECK_EQ(P[2].z, 0.0f);
}

TEST(assigning_a_component_of_a_new_attribute_makes_it_a_vector) {
    Geometry g = pointsAlongX(3);
    run(g, "@v.z = 5.0;");
    CHECK_EQ(g.points().find("v")->type(), AttrType::Vec3);
    CHECK_EQ(g.points().find("v")->read<Vec3>()[1].z, 5.0f);
}

TEST(point_number_and_point_count_are_bound) {
    Geometry g = pointsAlongX(5);
    run(g, "@t = @ptnum / @numpt;");
    auto t = g.points().find("t")->read<float>();
    CHECK_EQ(t[0], 0.0f);
    CHECK_NEAR(t[4], 0.8, 1e-6);
}

TEST(statements_run_in_order_within_one_snippet) {
    Geometry g = pointsAlongX(3);
    run(g, "@a = 2.0; @b = @a * 3.0;");
    CHECK_EQ(g.points().find("b")->read<float>()[0], 6.0f);
}

TEST(builtins_behave) {
    Geometry g = pointsAlongX(3);
    run(g, "@len = length(vec3(3.0, 4.0, 0.0));"
           "@cl = clamp(5.0, 0.0, 1.0);"
           "@ft = fit(0.5, 0.0, 1.0, 10.0, 20.0);"
           "@mx = max(2.0, 7.0);");
    CHECK_NEAR(g.points().find("len")->read<float>()[0], 5.0, 1e-5);
    CHECK_EQ(g.points().find("cl")->read<float>()[0], 1.0f);
    CHECK_NEAR(g.points().find("ft")->read<float>()[0], 15.0, 1e-5);
    CHECK_EQ(g.points().find("mx")->read<float>()[0], 7.0f);
}

TEST(noise_is_deterministic_and_bounded) {
    Geometry a = pointsAlongX(1000);
    Geometry b = pointsAlongX(1000);
    run(a, "@n = noise(@P * 0.7);");
    run(b, "@n = noise(@P * 0.7);");
    CHECK_EQ(a.hash(), b.hash());

    auto n = a.points().find("n")->read<float>();
    for (float v : n) CHECK(v >= 0.0f && v <= 1.0f);
}

TEST(reading_a_missing_attribute_yields_zero_rather_than_failing) {
    Geometry g = pointsAlongX(3);
    run(g, "@out = @nosuchattr + 1.0;");
    CHECK_EQ(g.points().find("out")->read<float>()[0], 1.0f);
}

TEST(parse_errors_are_reported_with_a_position) {
    std::string error;
    CHECK_EQ(expr::Program::parse("@P.y = ;", error), nullptr);
    CHECK(!error.empty());

    error.clear();
    CHECK_EQ(expr::Program::parse("@P.y = nosuchfn(1.0);", error), nullptr);
    CHECK(error.find("nosuchfn") != std::string::npos);

    error.clear();
    CHECK_EQ(expr::Program::parse("@P.w = 1.0;", error), nullptr);
    CHECK(error.find("component") != std::string::npos);

    error.clear();
    CHECK_EQ(expr::Program::parse("5.0 = @P.x;", error), nullptr);
    CHECK(!error.empty());

    error.clear();
    CHECK_EQ(expr::Program::parse("@P.y = pow(2.0);", error), nullptr);
    CHECK(error.find("argument") != std::string::npos);
}

TEST(writing_to_a_read_only_builtin_is_an_error) {
    Geometry g = pointsAlongX(3);
    std::string error;
    auto prog = expr::Program::parse("@ptnum = 5.0;", error);
    CHECK_NE(prog, nullptr);
    CHECK(!prog->run(g, CookContext{}, error));
    CHECK(error.find("ptnum") != std::string::npos);
}

TEST(the_wrangle_node_reports_time_dependency_from_its_snippet) {
    Graph g;
    Node* grid = g.create("grid", "grid");
    Node* w = g.create("pointwrangle", "w");
    w->setInput(0, grid);
    CookEngine engine(1ull << 30);

    w->setString("snippet", "@P.y = 1.0;");
    CHECK(!engine.isTimeDependent(*w));

    w->setString("snippet", "@P.y = @Time * 2.0;");
    CHECK(engine.isTimeDependent(*w));

    GeometryPtr at1 = engine.cook(*w, CookContext{1.0, 1, 24.0});
    GeometryPtr at2 = engine.cook(*w, CookContext{3.0, 3, 24.0});
    CHECK_NEAR(at1->positions()[0].y, 2.0, 1e-5);
    CHECK_NEAR(at2->positions()[0].y, 6.0, 1e-5);
}

TEST(the_wrangle_node_surfaces_its_parse_error_and_passes_geometry_through) {
    Graph g;
    Node* grid = g.create("grid", "grid");
    grid->setInt("rows", 3);
    grid->setInt("cols", 3);
    Node* w = g.create("pointwrangle", "w");
    w->setInput(0, grid);
    w->setString("snippet", "@P.y = ((;");

    CookEngine engine(1ull << 30);
    GeometryPtr out = engine.cook(*w, CookContext{});
    CHECK_EQ(out->pointCount(), 9u);
    CHECK(!wrangleError(*w).empty());
}

TEST(a_wrangle_that_writes_only_P_leaves_other_buffers_shared) {
    Graph g;
    Node* grid = g.create("grid", "grid");
    grid->setInt("rows", 32);
    grid->setInt("cols", 32);
    Node* colour = g.create("attribcreate", "colour");
    colour->setInput(0, grid);
    colour->setString("name", "Cd");
    colour->setBool("vector", true);
    Node* w = g.create("pointwrangle", "w");
    w->setInput(0, colour);
    w->setString("snippet", "@P.y = 1.0;");

    CookEngine engine(1ull << 30);
    GeometryPtr before = engine.cook(*colour, CookContext{});
    GeometryPtr after = engine.cook(*w, CookContext{});

    CHECK_NE(before->points().find("P")->bufferId(), after->points().find("P")->bufferId());
    CHECK_EQ(before->points().find("Cd")->bufferId(), after->points().find("Cd")->bufferId());
}
