//
// The wrangle language (pg/lang): what VEX users reach for -- variables,
// control flow, functions, arrays, strings, running over primitives and the
// detail, reading other elements and inputs, making and deleting geometry --
// and that the result never depends on the number of threads.
//
#include "pg/core/CookEngine.h"
#include "pg/core/Graph.h"
#include "pg/core/Parallel.h"
#include "pg/core/Spatial.h"
#include "pg/lang/Lang.h"
#include "pg/nodes/Nodes.h"

#include "test_framework.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

using namespace pg;

namespace {

Geometry pointsAlongX(size_t n) {
    Geometry g;
    g.addPoints(n);
    auto P = g.positionsForWrite();
    for (size_t i = 0; i < n; ++i) P[i] = Vec3(static_cast<float>(i), 0, 0);
    return g;
}

/// A grid of rows x cols points, quads between them.
Geometry grid(int rows, int cols) {
    Geometry g;
    g.addPoints(static_cast<size_t>(rows * cols));
    auto P = g.positionsForWrite();
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) P[static_cast<size_t>(r * cols + c)] = Vec3(static_cast<float>(c), 0, static_cast<float>(r));
    }
    for (int r = 0; r + 1 < rows; ++r) {
        for (int c = 0; c + 1 < cols; ++c) {
            const auto a = static_cast<uint32_t>(r * cols + c);
            const uint32_t quad[4] = {a, a + 1, a + 1 + static_cast<uint32_t>(cols), a + static_cast<uint32_t>(cols)};
            g.addPrimitive(quad, true);
        }
    }
    return g;
}

bool run(Geometry& g, const std::string& src, const lang::RunOptions& o, std::string* error = nullptr,
         lang::RunReport* report = nullptr) {
    std::string e;
    auto prog = lang::Program::parse(src, e);
    if (!prog) {
        if (error) *error = e;
        else ::testing::fail(__FILE__, __LINE__, "parse failed: " + e);
        return false;
    }
    const bool ok = prog->run(g, o, e, report);
    if (!ok) {
        if (error) *error = e;
        else ::testing::fail(__FILE__, __LINE__, "run failed: " + e);
    }
    return ok;
}

bool run(Geometry& g, const std::string& src, AttrClass over = AttrClass::Point) {
    lang::RunOptions o;
    o.runOver = over;
    return run(g, src, o);
}

template <class T> T at(const Geometry& g, const char* name, size_t i, AttrClass c = AttrClass::Point) {
    const AttributeArray* a = g.attributes(c).find(name);
    if (!a) ::testing::fail(__FILE__, __LINE__, std::string("no attribute ") + name);
    return a->read<T>()[i];
}

std::string text(const Geometry& g, const char* name, size_t i) {
    const AttributeArray* a = g.points().find(name);
    if (!a) ::testing::fail(__FILE__, __LINE__, std::string("no attribute ") + name);
    return a->stringValue(a->read<int32_t>()[i]);
}

/// ch() of fixed numbers, and $F of a fixed frame.
class FixedHost : public lang::Host {
public:
    std::map<std::string, double> values;
    double frame = 1.0;
    bool variable(std::string_view name, double& out) const override {
        if (name == "F") {
            out = frame;
            return true;
        }
        return false;
    }
    bool channel(std::string_view path, int, double& out, std::string& error) const override {
        const auto it = values.find(std::string(path));
        if (it == values.end()) {
            error = "no parameter";
            return false;
        }
        out = it->second;
        return true;
    }
};

}  // namespace

TEST(lang_variables_loops_break_and_continue) {
    Geometry g = pointsAlongX(3);
    run(g, "int n = 0;\n"
           "for (int i = 0; i < 10; i++) {\n"
           "    if (i == 3) continue;\n"
           "    if (i == 7) break;\n"
           "    n += i;\n"
           "}\n"
           "int w = 0; while (w < 5) w++;\n"
           "int d = 10; do { d -= 3; } while (d > 0);\n"
           "i@n = n; i@w = w; i@d = d;");
    CHECK_EQ(at<int32_t>(g, "n", 0), 18);  // 0 + 1 + 2 + 4 + 5 + 6
    CHECK_EQ(at<int32_t>(g, "w", 1), 5);
    CHECK_EQ(at<int32_t>(g, "d", 2), -2);
    CHECK_EQ(g.points().find("n")->type(), AttrType::Int);
}

TEST(lang_division_is_real_and_ints_wrap) {
    Geometry g = pointsAlongX(2);
    run(g, "@a = 7 / 2; int k = 7 / 2; @b = k; i@m = 7 % 3; i@big = 2147483647 + 1; i@bits = (5 & 3) | (1 << 4);"
           "@z = 1 / 0; i@zm = 5 % 0;");
    CHECK_EQ(at<float>(g, "a", 0), 3.5f);
    CHECK_EQ(at<float>(g, "b", 0), 3.0f);
    CHECK_EQ(at<int32_t>(g, "m", 0), 1);
    CHECK_EQ(at<int32_t>(g, "big", 0), INT32_MIN);
    CHECK_EQ(at<int32_t>(g, "bits", 0), 17);
    CHECK_EQ(at<float>(g, "z", 0), 0.0f);
    CHECK_EQ(at<int32_t>(g, "zm", 0), 0);
}

TEST(lang_functions_return_and_take_variables_by_reference) {
    Geometry g = pointsAlongX(4);
    run(g, "float twice(float x) { return x * 2; }\n"
           "void bump(int n) { n += 5; }\n"
           "vector up(float h; int k) { return set(0, h * k, 0); }\n"
           "@y = twice(@P.x);\n"
           "int k = 1; bump(k); i@k = k;\n"
           "v@u = up(0.5, 4);\n"
           "@nested = twice(twice(@P.x));");
    CHECK_EQ(at<float>(g, "y", 3), 6.0f);
    CHECK_EQ(at<int32_t>(g, "k", 0), 6);
    CHECK_EQ(at<Vec3>(g, "u", 0).y, 2.0f);
    CHECK_EQ(at<float>(g, "nested", 2), 8.0f);

    std::string error;
    Geometry h = pointsAlongX(1);
    CHECK(!run(h, "int f(int n) { return f(n); } i@x = f(1);", lang::RunOptions{}, &error));
    CHECK(error.find("recurse") != std::string::npos);
}

TEST(lang_arrays_sort_append_negative_index_and_foreach) {
    Geometry g = pointsAlongX(1);
    run(g, "int a[] = {3, 1, 2};\n"
           "a = sort(a);\n"
           "append(a, 7);\n"
           "i@first = a[0]; i@last = a[-1]; i@count = len(a);\n"
           "int total = 0; foreach (int v; a) total += v; i@total = total;\n"
           "float f[]; f[3] = 1.5; i@grown = len(f);\n"
           "string names[] = split(\"a b  c\"); i@words = len(names);\n"
           "vector pts[] = {{0, 1, 0}, {2, 0, 0}}; v@second = pts[1];\n"
           "i@where = find(a, 7); i@nowhere = find(a, 99);");
    CHECK_EQ(at<int32_t>(g, "first", 0), 1);
    CHECK_EQ(at<int32_t>(g, "last", 0), 7);
    CHECK_EQ(at<int32_t>(g, "count", 0), 4);
    CHECK_EQ(at<int32_t>(g, "total", 0), 13);
    CHECK_EQ(at<int32_t>(g, "grown", 0), 4);
    CHECK_EQ(at<int32_t>(g, "words", 0), 3);
    CHECK_EQ(at<Vec3>(g, "second", 0).x, 2.0f);
    CHECK_EQ(at<int32_t>(g, "where", 0), 3);
    CHECK_EQ(at<int32_t>(g, "nowhere", 0), -1);
}

TEST(lang_strings_and_typed_attributes) {
    Geometry g = pointsAlongX(3);
    run(g, "s@name = sprintf(\"piece%d\", @ptnum);\n"
           "s@up = toupper(\"ab\") + \"c\";\n"
           "i@id = @ptnum * 10; f@m = 1; v@dir = {0, 1, 0}; p@q = quaternion(M_PI / 2, {0, 1, 0});\n"
           "u@st = set(0.25, 0.75);");
    CHECK_EQ(text(g, "name", 2), std::string("piece2"));
    CHECK_EQ(text(g, "up", 0), std::string("ABc"));
    CHECK_EQ(g.points().find("id")->type(), AttrType::Int);
    CHECK_EQ(g.points().find("m")->type(), AttrType::Float);
    CHECK_EQ(g.points().find("dir")->type(), AttrType::Vec3);
    CHECK_EQ(g.points().find("q")->type(), AttrType::Vec4);
    CHECK_EQ(g.points().find("st")->type(), AttrType::Vec2);
    CHECK_EQ(at<int32_t>(g, "id", 2), 20);
    CHECK_NEAR(at<Vec4>(g, "q", 0).w, std::cos(M_PI / 4), 1e-6);
}

TEST(lang_known_names_and_unprefixed_ints_become_floats) {
    Geometry g = pointsAlongX(2);
    run(g, "@count = 1; @Cd = 0.5; @dir = {1, 2, 3}; @id = 4;");
    CHECK_EQ(g.points().find("count")->type(), AttrType::Float);
    CHECK_EQ(g.points().find("Cd")->type(), AttrType::Vec3);  // Cd is a colour, 0.5 everywhere
    CHECK_EQ(at<Vec3>(g, "Cd", 0).z, 0.5f);
    CHECK_EQ(g.points().find("dir")->type(), AttrType::Vec3);
    CHECK_EQ(g.points().find("id")->type(), AttrType::Int);
}

TEST(lang_runs_over_primitives_with_their_middle_as_P) {
    Geometry g = grid(3, 3);  // 4 quads
    run(g, "@Cd = {1, 0, 0}; @h = @P.x + @P.z; i@corners = len(primpoints(0, @primnum));", AttrClass::Primitive);
    CHECK_EQ(g.primitives().find("Cd")->type(), AttrType::Vec3);
    CHECK(g.points().find("Cd") == nullptr);
    CHECK_NEAR(at<float>(g, "h", 0, AttrClass::Primitive), 1.0, 1e-6);  // middle of the first quad: (0.5, 0, 0.5)
    CHECK_EQ(at<int32_t>(g, "corners", 3, AttrClass::Primitive), 4);
}

TEST(lang_a_vertex_wrangle_reads_its_points) {
    Geometry g = grid(2, 2);  // one quad
    run(g, "i@pt = @ptnum; i@prim = @primnum; v@pos = @P;", AttrClass::Vertex);
    CHECK_EQ(at<int32_t>(g, "pt", 2, AttrClass::Vertex), 3);  // corners 0 1 3 2
    CHECK_EQ(at<int32_t>(g, "prim", 3, AttrClass::Vertex), 0);
    CHECK_EQ(at<Vec3>(g, "pos", 1, AttrClass::Vertex).x, 1.0f);
}

TEST(lang_a_detail_wrangle_makes_points_and_a_line) {
    Geometry g;
    run(g, "int pts[];\n"
           "for (int i = 0; i < 5; i++) append(pts, addpoint(0, set(i, 0, 0)));\n"
           "addprim(0, \"polyline\", pts);\n"
           "int tri = addprim(0, \"poly\");\n"
           "addvertex(0, tri, pts[0]); addvertex(0, tri, pts[1]); addvertex(0, tri, pts[2]);\n"
           "setdetailattrib(0, \"made\", len(pts));",
        AttrClass::Detail);
    CHECK_EQ(g.pointCount(), 5u);
    CHECK_EQ(g.primitiveCount(), 2u);
    CHECK_EQ(g.primitiveVertexCount(0), 5u);
    CHECK(!g.primitiveClosed(0));
    CHECK(g.primitiveClosed(1));
    CHECK_EQ(g.primitivePoints(1)[2], 2u);
    CHECK_EQ(at<int32_t>(g, "made", 0, AttrClass::Detail), 5);
    CHECK_EQ(g.positions()[4].x, 4.0f);
}

TEST(lang_points_are_removed_after_the_run) {
    Geometry g = pointsAlongX(10);
    run(g, "if (@ptnum % 2 == 0) removepoint(0, @ptnum); @seen = @numpt;");
    CHECK_EQ(g.pointCount(), 5u);
    CHECK_EQ(g.positions()[0].x, 1.0f);
    CHECK_EQ(at<float>(g, "seen", 0), 10.0f);  // every element saw the geometry as it came in

    Geometry q = grid(3, 3);
    run(q, "if (@primnum == 0) removeprim(0, @primnum, 1);", AttrClass::Primitive);
    CHECK_EQ(q.primitiveCount(), 3u);
    CHECK_EQ(q.pointCount(), 8u);  // only the corner no other quad used went with it
}

TEST(lang_writes_to_other_elements_wait_for_the_end_of_the_run) {
    Geometry g = pointsAlongX(4);
    run(g, "setpointattrib(0, \"mark\", 3, 1.0); setpointattrib(0, \"sum\", 0, 1.0, \"add\"); f@seen = point(0, \"mark\", 3);");
    CHECK_EQ(at<float>(g, "mark", 3), 1.0f);
    CHECK_EQ(at<float>(g, "sum", 0), 4.0f);  // one add from each of the four points
    CHECK_EQ(at<float>(g, "seen", 3), 0.0f);  // point() reads the geometry as it came in
}

TEST(lang_nearpoints_neighbours_and_bounding_box) {
    Geometry line = pointsAlongX(5);
    run(line, "int near[] = nearpoints(0, @P, 1.5); i@n = len(near); i@closest = nearpoint(0, @P + {0.2, 0, 0});");
    CHECK_EQ(at<int32_t>(line, "n", 0), 2);
    CHECK_EQ(at<int32_t>(line, "n", 2), 3);
    CHECK_EQ(at<int32_t>(line, "closest", 1), 1);

    Geometry g = grid(3, 3);
    run(g, "i@nb = neighbourcount(0, @ptnum); v@rel = relbbox(0, @P); i@prims = len(pointprims(0, @ptnum));");
    CHECK_EQ(at<int32_t>(g, "nb", 4), 4);  // the middle point
    CHECK_EQ(at<int32_t>(g, "nb", 0), 2);  // a corner
    CHECK_EQ(at<int32_t>(g, "prims", 4), 4);
    CHECK_NEAR(at<Vec3>(g, "rel", 8).x, 1.0, 1e-6);
}

TEST(lang_reads_other_inputs_and_the_same_element_of_them) {
    Geometry g = pointsAlongX(3);
    auto other = std::make_shared<Geometry>(pointsAlongX(3));
    for (Vec3& p : other->positionsForWrite()) p.y = 5.0f;
    lang::RunOptions o;
    o.inputs[1] = other;
    run(g, "@P = lerp(@P, point(1, \"P\", @ptnum), 0.5); @y1 = @opinput1_P.y; i@n1 = npoints(1);", o);
    CHECK_EQ(g.positions()[2].y, 2.5f);
    CHECK_EQ(at<float>(g, "y1", 0), 5.0f);
    CHECK_EQ(at<int32_t>(g, "n1", 0), 3);
}

TEST(lang_groups_are_read_written_and_limit_the_run) {
    Geometry g = pointsAlongX(6);
    run(g, "@group_far = @P.x > 2.5;");
    const Group* far = g.findGroup("far");
    CHECK(far != nullptr);
    CHECK_EQ(far->memberCount(), 3u);
    lang::RunOptions o;
    o.group = "far";
    run(g, "@hit = 1;", o);
    CHECK_EQ(at<float>(g, "hit", 2), 0.0f);
    CHECK_EQ(at<float>(g, "hit", 3), 1.0f);

    std::string error;
    o.group = "nosuch";
    CHECK(!run(g, "@hit = 2;", o, &error));
    CHECK(error.find("nosuch") != std::string::npos);
}

TEST(lang_ch_and_frame_come_from_the_host) {
    Geometry g = pointsAlongX(2);
    FixedHost host;
    host.values["scale"] = 3.0;
    host.frame = 12.0;
    lang::RunOptions o;
    o.host = &host;
    lang::RunReport report;
    run(g, "@s = ch(\"scale\") * 2; i@f = $F; @missing = ch(\"nosuch\");", o, nullptr, &report);
    CHECK_EQ(at<float>(g, "s", 0), 6.0f);
    CHECK_EQ(at<int32_t>(g, "f", 1), 12);
    CHECK_EQ(at<float>(g, "missing", 0), 0.0f);
    CHECK(!report.warnings.empty());
    CHECK(report.warnings.front().find("nosuch") != std::string::npos);

    std::string error;
    auto prog = lang::Program::parse("@a = ch(\"x\") + chf(\"y\"); v@b = chv(\"dir\"); s@c = chs(\"label\");", error);
    CHECK(prog != nullptr);
    CHECK_EQ(prog->channels().size(), 4u);
    CHECK_EQ(prog->channels()[2].type, lang::Type::Vec3);
    CHECK(!prog->readsTime());
    CHECK(lang::Program::parse("@t = $F;", error)->readsTime());
}

TEST(lang_matrices_and_quaternions_turn_the_same_way) {
    Geometry g = pointsAlongX(1);
    run(g, "matrix3 m = ident(); rotate(m, M_PI / 2, {0, 1, 0}); v@r = {1, 0, 0} * m;"
           "v@q = qrotate(quaternion(M_PI / 2, {0, 1, 0}), {1, 0, 0});"
           "matrix t = ident(); translate(t, {1, 2, 3}); v@moved = {0, 0, 0} * t;"
           "v@back = {1, 0, 0} * m * invert(m);"
           "v@d = {0, 1, 0} * dihedral({0, 1, 0}, {1, 0, 0});");
    const Vec3 r = at<Vec3>(g, "r", 0), q = at<Vec3>(g, "q", 0);
    CHECK_NEAR(r.x, 0.0, 1e-6);
    CHECK_NEAR(r.z, -1.0, 1e-6);
    CHECK_NEAR(q.x, r.x, 1e-6);
    CHECK_NEAR(q.z, r.z, 1e-6);
    CHECK_EQ(at<Vec3>(g, "moved", 0), Vec3(1, 2, 3));
    CHECK_NEAR(at<Vec3>(g, "back", 0).x, 1.0, 1e-5);
    CHECK_NEAR(at<Vec3>(g, "d", 0).x, 1.0, 1e-6);
}

TEST(lang_errors_say_where_and_what) {
    std::string error;
    CHECK(lang::Program::parse("@a = 1;\n@b = (2 + ;", error) == nullptr);
    CHECK(error.find("line 2") != std::string::npos);

    Geometry g = pointsAlongX(2);
    CHECK(!run(g, "@a = nosuchvar + 1;", lang::RunOptions{}, &error));
    CHECK(error.find("nosuchvar") != std::string::npos);
    CHECK(!run(g, "float x = \"text\";", lang::RunOptions{}, &error));
    CHECK(error.find("string") != std::string::npos);
    CHECK(!run(g, "error(\"bad point %d\", @ptnum);", lang::RunOptions{}, &error));
    CHECK_EQ(error, std::string("bad point 0"));
    CHECK(!run(g, "break;", lang::RunOptions{}, &error));
    CHECK(error.find("loop") != std::string::npos);

    lang::RunReport report;
    lang::RunOptions o;
    CHECK(run(g, "printf(\"pt %d at %g\\n\", @ptnum, @P.x);", o, nullptr, &report));
    CHECK_EQ(report.log, std::string("pt 0 at 0\npt 1 at 1\n"));
}

TEST(lang_results_do_not_depend_on_the_thread_count) {
    const unsigned saved = TaskPool::instance().threadCount();
    auto cook = [](const std::string& src, AttrClass over) {
        Geometry g = grid(60, 60);
        lang::RunOptions o;
        o.runOver = over;
        std::string error;
        auto prog = lang::Program::parse(src, error);
        if (!prog || !prog->run(g, o, error)) ::testing::fail(__FILE__, __LINE__, error);
        return g.hash();
    };
    const std::string parallel = "vector n = noise(@P * 0.3); float s = 0; for (int i = 0; i < 4; i++) s += rand(@ptnum * 4 + i);"
                                 "@P.y = n.y + s * 0.1; @Cd = n;";
    const std::string ordered = "if (rand(@ptnum) < 0.3) removepoint(0, @ptnum); else addpoint(0, @P + {0, 1, 0});"
                                "s@tag = sprintf(\"p%d\", @ptnum % 7);";
    TaskPool::instance().setThreadCount(1);
    const uint64_t a1 = cook(parallel, AttrClass::Point), b1 = cook(ordered, AttrClass::Point);
    TaskPool::instance().setThreadCount(4);
    const uint64_t a4 = cook(parallel, AttrClass::Point), b4 = cook(ordered, AttrClass::Point);
    TaskPool::instance().setThreadCount(saved);
    CHECK_EQ(a1, a4);
    CHECK_EQ(b1, b4);
}

TEST(lang_expressions_of_parameters) {
    std::string error;
    auto e = lang::Expression::parse("$F * 2 + 1", error);
    CHECK(e != nullptr);
    CHECK(e->readsTime());
    FixedHost host;
    host.frame = 3.0;
    double v = 0.0;
    CHECK(e->evalFloat(host, v, error));
    CHECK_EQ(v, 7.0);

    auto vec = lang::Expression::parse("{1, $F, ch(\"k\")}", error);
    CHECK(vec != nullptr);
    host.values["k"] = 0.25;
    Vec3 out;
    CHECK(vec->evalVector(host, out, error));
    CHECK_EQ(out, Vec3(1.0f, 3.0f, 0.25f));
    CHECK_EQ(vec->channels().size(), 1u);

    auto bad = lang::Expression::parse("ch(\"missing\") + 1", error);
    CHECK(bad != nullptr);
    CHECK(!bad->evalFloat(host, v, error));
    CHECK(lang::Expression::parse("@P.x", error) == nullptr);
    CHECK(lang::isNumber("-1.5e2"));
    CHECK(!lang::isNumber("$F"));
}

TEST(lang_the_wrangle_node_runs_over_primitives_and_reads_its_second_input) {
    registerBuiltinNodes();
    Graph gr;
    Node* grid1 = gr.create("grid", "grid");
    grid1->setInt("rows", 3);
    grid1->setInt("cols", 3);
    Node* pts = gr.create("pointcloud", "pts");
    pts->setInt("count", 7);
    Node* w = gr.create("attribwrangle", "w");
    w->setInput(0, grid1);
    w->setInput(1, pts);
    w->setInt("runover", 1);
    w->setFloat("amount", 2.0f);
    w->setString("snippet", "@area = primarea(0, @primnum) * ch(\"amount\"); i@others = npoints(1);");
    CookEngine engine(1ull << 28);
    GeometryPtr out = engine.cook(*w, CookContext{});
    CHECK(w->cookError().empty());
    CHECK(out->primitives().find("area") != nullptr);
    CHECK_EQ(out->primitives().find("others")->read<int32_t>()[0], 7);
    const float area = out->primitives().find("area")->read<float>()[0];
    CHECK(area > 0.0f);
    w->setFloat("amount", 4.0f);
    out = engine.cook(*w, CookContext{});
    CHECK_NEAR(out->primitives().find("area")->read<float>()[0], area * 2.0f, 1e-5);
}

TEST(point_tree_answers_like_a_search_of_every_point) {
    std::vector<Vec3> pts;
    for (int i = 0; i < 500; ++i) {
        const float a = static_cast<float>(i) * 0.37f;
        pts.emplace_back(std::sin(a) * 3.0f, std::cos(a * 1.3f) * 2.0f, static_cast<float>(i % 17) * 0.1f);
    }
    PointTree tree(pts);
    for (int q = 0; q < 40; ++q) {
        const Vec3 p(static_cast<float>(q) * 0.13f - 2.0f, 0.5f, 0.3f);
        std::vector<std::pair<float, int32_t>> all;
        for (size_t i = 0; i < pts.size(); ++i) {
            const Vec3 d = pts[i] - p;
            const float d2 = dot(d, d);
            if (d2 <= 1.0f) all.emplace_back(d2, static_cast<int32_t>(i));
        }
        std::sort(all.begin(), all.end());
        std::vector<int32_t> got;
        tree.near(p, 1.0f, 0, got);
        CHECK_EQ(got.size(), all.size());
        for (size_t i = 0; i < got.size() && i < all.size(); ++i) CHECK_EQ(got[i], all[i].second);
        std::vector<int32_t> three;
        tree.near(p, -1.0f, 3, three);
        CHECK_EQ(three.size(), 3u);
    }
}

TEST(adjacency_is_what_asking_every_primitive_gives) {
    // Faces of all sizes over a jumble of points, a line, a point twice in
    // a face, a side of two points the same, a closed "face" of two points
    // (a line), points of nothing: each point's lists as a search of every
    // primitive finds them, sorted, each entry once.
    Geometry geo;
    geo.addPoints(60);
    uint32_t seed = 5;
    auto next = [&seed](uint32_t n) {
        seed = seed * 1664525u + 1013904223u;
        return (seed >> 8) % n;
    };
    for (int k = 0; k < 90; ++k) {
        std::vector<uint32_t> pts(2 + next(5));
        for (uint32_t& p : pts) p = next(50);  // 50 to 59 in nothing
        geo.addPrimitive(pts, k % 7 != 0);
    }
    geo.addPrimitive(std::vector<uint32_t>{3, 3, 4, 3}, true);
    geo.addPrimitive(std::vector<uint32_t>{8, 9}, true);
    Adjacency adjacency;
    adjacency.build(geo);
    std::vector<std::set<int32_t>> nb(60), prims(60), verts(60);
    for (size_t prim = 0; prim < geo.primitiveCount(); ++prim) {
        const auto pts = geo.primitivePoints(prim);
        const bool closed = geo.primitiveClosed(prim) && pts.size() > 2;
        for (size_t i = 0; i < pts.size(); ++i) {
            prims[pts[i]].insert(static_cast<int32_t>(prim));
            verts[pts[i]].insert(static_cast<int32_t>(geo.primitiveVertexStart(prim) + i));
            if (i + 1 == pts.size() && !closed) continue;
            const uint32_t a = pts[i], b = pts[(i + 1) % pts.size()];
            if (a == b) continue;
            nb[a].insert(static_cast<int32_t>(b));
            nb[b].insert(static_cast<int32_t>(a));
        }
    }
    const auto same = [](std::span<const int32_t> got, const std::set<int32_t>& want) {
        return got.size() == want.size() && std::equal(got.begin(), got.end(), want.begin());
    };
    for (size_t p = 0; p < 60; ++p) {
        CHECK(same(adjacency.neighbours(p), nb[p]));
        CHECK(same(adjacency.primitives(p), prims[p]));
        CHECK(same(adjacency.vertices(p), verts[p]));
    }
    CHECK(adjacency.neighbours(55).empty());
    CHECK(adjacency.neighbours(60).empty());
}

// --- in a network -------------------------------------------------------------------------------

#include "pg/sim/GeometryGraph.h"
#include "pg/sim/Network.h"

TEST(lang_a_snippet_makes_the_parameters_it_asks_for) {
    sim::Network net;
    const int grid = net.add("grid");
    const int w = net.add("point_wrangle");
    std::string error;
    CHECK(net.connect(grid, "geometry", w, "geometry", &error));
    CHECK(net.setText(w, "snippet", "@P.y += ch(\"lift\"); v@tint = chv(\"tint\"); i@k = chi(\"count\");"));
    CHECK(net.paramDef(w, "lift") != nullptr);
    CHECK(net.paramDef(w, "tint")->kind == sim::ParamKind::Vector);
    CHECK(net.paramDef(w, "count")->kind == sim::ParamKind::Int);
    CHECK_EQ(net.params(w).size(), 3u + 3u);  // snippet, runover, group -- and the three it asks for
    CHECK(net.setParam(w, "lift", sim::ParamValue{0.5f, 0.0f, 0.0f}));
    CHECK(net.setKey(w, "count", 1.0f, sim::ParamValue{2.0f, 0.0f, 0.0f}, sim::Interp::Linear));
    CHECK(net.setKey(w, "count", 11.0f, sim::ParamValue{4.0f, 0.0f, 0.0f}, sim::Interp::Linear));

    sim::GeometryGraph geo;
    geo.sync(net);
    GeometryPtr at1 = geo.cook(w, 1);
    CHECK(geo.error(w).empty());
    CHECK_EQ(at1->positions()[0].y, 0.5f);
    CHECK_EQ(at1->points().find("k")->read<int32_t>()[0], 2);
    GeometryPtr at6 = geo.cook(w, 6);
    CHECK_EQ(at6->points().find("k")->read<int32_t>()[0], 3);

    // Its values go to the file and come back, keys too.
    sim::Network back;
    std::vector<std::string> warnings;
    CHECK(sim::Network::load(net.save(), back, error, &warnings));
    CHECK(warnings.empty());
    CHECK_EQ(back.param(w, "lift")[0], 0.5f);
    CHECK(back.keys(w, "count") != nullptr);
    CHECK_EQ(back.keys(w, "count")->size(), 2u);

    // Half typed, a snippet keeps what it asked for; without the call, it goes.
    CHECK(net.setText(w, "snippet", "@P.y += ch(\"lift\") +"));
    CHECK_EQ(net.param(w, "lift")[0], 0.5f);
    CHECK(net.setText(w, "snippet", "@P.y += 1;"));
    CHECK(net.paramDef(w, "lift") == nullptr);
    CHECK(net.save().find("lift") == std::string::npos);
}

TEST(lang_primitive_and_detail_wrangles_in_a_network) {
    sim::Network net;
    const int box = net.add("box");
    const int prims = net.add("primitive_wrangle");
    const int made = net.add("detail_wrangle");
    const int merged = net.add("merge");
    std::string error;
    CHECK(net.connect(box, "geometry", prims, "geometry", &error));
    CHECK(net.setText(prims, "snippet", "@Cd = set(@P.y, 0, 0); if (@P.y < 0.01) removeprim(0, @primnum, 1);"));
    CHECK(net.setText(made, "snippet",
                      "for (int i = 0; i < 4; i++) addpoint(0, set(i, 2, 0));\n"
                      "addprim(0, \"polyline\", 0, 1, 2, 3);"));
    CHECK(net.connect(prims, "geometry", merged, "geometry", &error));
    CHECK(net.connect(made, "geometry", merged, "geometry", &error));
    sim::GeometryGraph geo;
    geo.sync(net);
    GeometryPtr out = geo.cook(merged, 1);
    CHECK(geo.error(prims).empty());
    CHECK(geo.error(made).empty());
    CHECK_EQ(out->primitiveCount(), 5u + 1u);  // the box without its bottom, and the line
    CHECK(out->primitives().find("Cd") != nullptr);
}

// --- expressions on parameters ------------------------------------------------------------------

TEST(expressions_drive_parameters_frame_by_frame_and_read_each_other) {
    sim::Network net;
    const int a = net.add("box");
    const int b = net.add("box");
    CHECK(net.rename(a, "box1"));
    CHECK(net.setParam(a, "size", sim::ParamValue{2.0f, 3.0f, 4.0f}));
    CHECK(net.setExpression(b, "center.y", "$F * 0.1"));
    CHECK_NEAR(net.valueAt(b, "center", 5.0f)[1], 0.5, 1e-6);
    CHECK_NEAR(net.valueAt(b, "center", 5.0f)[0], 0.0, 1e-6);  // the other components keep their values
    CHECK(net.varies(b, "center"));
    CHECK(net.anyAnimated());

    // Another node's parameter, a whole vector or one component of it.
    CHECK(net.setExpression(b, "size.x", "ch(\"../box1/sizey\") * 2"));
    CHECK(net.setExpression(b, "size.y", "ch(\"box1/size.z\")"));
    CHECK(net.setExpression(b, "size.z", "chv(\"../box1/size\").x"));
    CHECK_EQ(net.valueAt(b, "size", 1.0f)[0], 6.0f);
    CHECK_EQ(net.valueAt(b, "size", 1.0f)[1], 4.0f);
    CHECK_EQ(net.valueAt(b, "size", 1.0f)[2], 2.0f);
    CHECK(!net.varies(b, "size"));
    CHECK(net.setKey(a, "size", 1.0f, sim::ParamValue{1.0f, 1.0f, 1.0f}));
    CHECK(net.setKey(a, "size", 11.0f, sim::ParamValue{1.0f, 11.0f, 1.0f}));
    CHECK(net.varies(b, "size"));  // it reads a parameter that is animated
    CHECK_NEAR(net.valueAt(b, "size", 6.0f)[0], 12.0, 1e-4);

    // Kept within the parameter's limits; an int is whole.
    CHECK(net.setExpression(b, "divisions", "2.6 + $F"));
    CHECK_EQ(net.valueAt(b, "divisions", 1.0f)[0], 4.0f);
    CHECK(net.setExpression(b, "divisions", "-50"));
    CHECK_EQ(net.valueAt(b, "divisions", 1.0f)[0], 1.0f);

    // Not a parameter to drive; a text parameter; a number by a component.
    CHECK(!net.setExpression(b, "nosuch", "1"));
    CHECK(!net.setExpression(b, "divisions.x", "1"));
    CHECK(!net.setExpression(b, "size", "1"));
}

TEST(expressions_that_are_wrong_say_so_and_keep_the_value) {
    sim::Network net;
    const int a = net.add("sphere");
    const int b = net.add("sphere");
    CHECK(net.rename(a, "one"));
    CHECK(net.rename(b, "two"));
    CHECK(net.setParam(a, "radius", sim::ParamValue{0.7f, 0.0f, 0.0f}));
    CHECK(net.setExpression(a, "radius", "1 +"));
    CHECK(!net.expressionError(a, "radius").empty());
    CHECK_EQ(net.valueAt(a, "radius", 1.0f)[0], 0.7f);
    CHECK(net.setExpression(a, "radius", "ch(\"../nobody/radius\")"));
    CHECK(net.expressionError(a, "radius").find("nobody") != std::string::npos);

    // Two that read each other: a loop, said, not a hang.
    CHECK(net.setExpression(a, "radius", "ch(\"../two/radius\") + 1"));
    CHECK(net.setExpression(b, "radius", "ch(\"../one/radius\") + 1"));
    CHECK(net.expressionError(a, "radius").find("loop") != std::string::npos);
    CHECK(std::isfinite(net.valueAt(a, "radius", 1.0f)[0]));
}

TEST(expressions_go_to_the_file_and_back_and_reset_takes_them_off) {
    sim::Network net;
    const int w = net.add("point_wrangle");
    CHECK(net.setText(w, "snippet", "@P.y += ch(\"lift\");"));
    CHECK(net.setExpression(w, "lift", "sin($T * 2) * 0.5"));
    const int s = net.add("sphere");
    CHECK(net.setExpression(s, "center.x", "ch(\"../point_wrangle1/lift\") + \"x\" == \"y\""));
    const std::string text = net.save();
    CHECK(text.find("  expr lift \"sin($T * 2) * 0.5\"") != std::string::npos);
    sim::Network back;
    std::string error;
    std::vector<std::string> warnings;
    CHECK(sim::Network::load(text, back, error, &warnings));
    CHECK(warnings.empty());
    CHECK_EQ(back.expression(w, "lift"), std::string("sin($T * 2) * 0.5"));
    CHECK_EQ(back.expression(s, "center.x"), net.expression(s, "center.x"));
    CHECK_EQ(back.save(), text);
    CHECK(!back.isDefault(w, "lift"));
    CHECK(back.resetParam(w, "lift"));
    CHECK(back.expression(w, "lift").empty());
    CHECK(back.isDefault(w, "lift"));
}

TEST(expressions_drive_the_geometry_and_what_is_simulated) {
    sim::Network net;
    const int a = net.add("box");
    const int b = net.add("box");
    CHECK(net.rename(a, "base"));
    CHECK(net.setParam(a, "size", sim::ParamValue{2.0f, 1.0f, 1.0f}));
    CHECK(net.setExpression(b, "size.x", "ch(\"../base/sizex\") * 2"));
    CHECK(net.setExpression(b, "center.y", "$F * 0.25"));
    sim::GeometryGraph geo;
    geo.sync(net);
    auto width = [](const GeometryPtr& g) {
        float lo = 1e9f, hi = -1e9f;
        for (const Vec3& p : g->positions()) {
            lo = std::min(lo, p.x);
            hi = std::max(hi, p.x);
        }
        return hi - lo;
    };
    auto lowest = [](const GeometryPtr& g) {
        float lo = 1e9f;
        for (const Vec3& p : g->positions()) lo = std::min(lo, p.y);
        return lo;
    };
    CHECK_NEAR(width(geo.cook(b, 1)), 4.0, 1e-5);
    CHECK_NEAR(lowest(geo.cook(b, 4)), 1.0 - 0.5, 1e-5);  // centre 1, a box 1 high
    CHECK(net.setParam(a, "size", sim::ParamValue{3.0f, 1.0f, 1.0f}));
    geo.sync(net);
    CHECK_NEAR(width(geo.cook(b, 1)), 6.0, 1e-5);

    // A simulation takes it frame by frame, as it takes keys.
    sim::Network smoke;
    std::string error;
    CHECK(sim::Network::example("smoke", smoke));
    int source = 0;
    for (const sim::Node& n : smoke.nodes()) {
        if (n.type == "pyro_source") source = n.id;
    }
    CHECK(source != 0);
    CHECK(smoke.setExpression(source, "center.x", "$F * 0.01"));
    const sim::Compiled c = smoke.compile();
    CHECK(c.ok);
    CHECK(c.world.animation.frames != nullptr);
    CHECK_NEAR(c.worldAt(10).gas.emitters.front().center.x, 0.1, 1e-5);
}
