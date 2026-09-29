//
// Geometry nodes (src/pg/nodes): the primitive shapes are closed and face
// out; scatter spreads points evenly and carries attributes; normals point
// out of a surface; copies land on points; OBJ files go in and out; volumes
// travel with the geometry; a node deleted from a graph leaves nothing
// behind in the cache.
//
#include "pg/core/CookEngine.h"
#include "pg/core/Graph.h"
#include "pg/core/Parallel.h"
#include "pg/io/Obj.h"
#include "pg/nodes/Nodes.h"

#include "test_framework.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <utility>

using namespace pg;

namespace {

struct ThreadCountGuard {
    unsigned saved = TaskPool::instance().threadCount();
    ~ThreadCountGuard() { TaskPool::instance().setThreadCount(saved); }
};

/// Every edge of a closed surface is shared by two faces, turned opposite
/// ways: a -> b in one, b -> a in the other.
bool watertight(const Geometry& geo) {
    std::map<std::pair<uint32_t, uint32_t>, int> edges;
    for (size_t prim = 0; prim < geo.primitiveCount(); ++prim) {
        const auto f = geo.primitivePoints(prim);
        for (size_t i = 0; i < f.size(); ++i) ++edges[{f[i], f[(i + 1) % f.size()]}];
    }
    for (const auto& [e, count] : edges) {
        if (count != 1) return false;
        const auto back = edges.find({e.second, e.first});
        if (back == edges.end() || back->second != 1) return false;
    }
    return true;
}

/// Every face turned away from `center`.
bool facesOut(const Geometry& geo, const Vec3& center) {
    const auto P = geo.positions();
    for (size_t prim = 0; prim < geo.primitiveCount(); ++prim) {
        const auto f = geo.primitivePoints(prim);
        Vec3 middle;
        for (const uint32_t p : f) middle += P[p];
        middle = middle * (1.0f / static_cast<float>(f.size()));
        if (dot(polygonNormal(geo, f), middle - center) <= 0.0f) return false;
    }
    return true;
}

void bounds(const Geometry& geo, Vec3& lo, Vec3& hi) {
    lo = Vec3(1e30f);
    hi = Vec3(-1e30f);
    for (const Vec3& p : geo.positions()) {
        for (int a = 0; a < 3; ++a) {
            lo[a] = std::min(lo[a], p[a]);
            hi[a] = std::max(hi[a], p[a]);
        }
    }
}

bool near(const Vec3& a, const Vec3& b, float eps = 1e-5f) { return length(a - b) < eps; }

}  // namespace

TEST(sops_box_is_closed_and_faces_out) {
    Graph g;
    CookEngine engine;
    Node* box = g.create("box", "box");
    box->setVec3("size", Vec3(2.0f, 1.0f, 1.0f));
    box->setVec3("center", Vec3(0.0f, 0.5f, 0.0f));
    GeometryPtr geo = engine.cook(*box, CookContext{});
    CHECK_EQ(geo->pointCount(), size_t(8));
    CHECK_EQ(geo->primitiveCount(), size_t(6));
    CHECK(watertight(*geo));
    CHECK(facesOut(*geo, Vec3(0.0f, 0.5f, 0.0f)));
    Vec3 lo, hi;
    bounds(*geo, lo, hi);
    CHECK(near(lo, Vec3(-1.0f, 0.0f, -0.5f)) && near(hi, Vec3(1.0f, 1.0f, 0.5f)));
    // Divided: the points on the surface of a 4 x 4 x 4 lattice, welded.
    box->setInt("divisions", 3);
    geo = engine.cook(*box, CookContext{});
    CHECK_EQ(geo->pointCount(), size_t(4 * 4 * 4 - 2 * 2 * 2));
    CHECK_EQ(geo->primitiveCount(), size_t(6 * 9));
    CHECK(watertight(*geo));
    CHECK(facesOut(*geo, Vec3(0.0f, 0.5f, 0.0f)));
}

TEST(sops_sphere_and_tube_are_closed_and_face_out) {
    Graph g;
    CookEngine engine;
    Node* sphere = g.create("sphere", "sphere");
    sphere->setFloat("radius", 0.7f);
    sphere->setVec3("center", Vec3(1.0f, 2.0f, 3.0f));
    sphere->setInt("rows", 8);
    sphere->setInt("columns", 10);
    GeometryPtr geo = engine.cook(*sphere, CookContext{});
    CHECK_EQ(geo->pointCount(), size_t(7 * 10 + 2));
    CHECK_EQ(geo->primitiveCount(), size_t(8 * 10));
    for (const Vec3& p : geo->positions()) CHECK(std::fabs(length(p - Vec3(1.0f, 2.0f, 3.0f)) - 0.7f) < 1e-5f);
    CHECK(watertight(*geo));
    CHECK(facesOut(*geo, Vec3(1.0f, 2.0f, 3.0f)));

    Node* tube = g.create("tube", "tube");
    tube->setInt("rows", 3);
    tube->setInt("columns", 12);
    geo = engine.cook(*tube, CookContext{});
    CHECK_EQ(geo->pointCount(), size_t(4 * 12));
    CHECK_EQ(geo->primitiveCount(), size_t(3 * 12 + 2));
    CHECK(watertight(*geo));
    CHECK(facesOut(*geo, Vec3()));
    tube->setBool("caps", false);
    geo = engine.cook(*tube, CookContext{});
    CHECK_EQ(geo->primitiveCount(), size_t(3 * 12));
}

TEST(sops_scatter_spreads_evenly_and_blends_attributes) {
    ThreadCountGuard guard;
    Graph g;
    Node* grid = g.create("grid", "grid");
    grid->setFloat("sizex", 2.0f);
    grid->setFloat("sizez", 1.0f);
    grid->setInt("rows", 5);
    grid->setInt("cols", 9);
    // A colour that runs from 0 at x = -1 to 1 at x = 1.
    Node* paint = g.create("pointwrangle", "paint");
    paint->setInput(0, grid);
    paint->setString("snippet", "@Cd = vec3(@P.x * 0.5 + 0.5, 0.0, 0.0);");
    Node* scatter = g.create("scatter", "scatter");
    scatter->setInput(0, paint);
    scatter->setInt("count", 20000);
    auto run = [&](unsigned threads) {
        TaskPool::instance().setThreadCount(threads);
        CookEngine engine;
        return engine.cook(*scatter, CookContext{});
    };
    const GeometryPtr one = run(1), four = run(4);
    CHECK_EQ(one->hash(), four->hash());
    CHECK_EQ(one->pointCount(), size_t(20000));
    CHECK_EQ(one->primitiveCount(), size_t(0));
    const auto P = one->positions();
    const auto Cd = one->points().find("Cd")->read<Vec3>();
    const auto N = one->points().find("N")->read<Vec3>();
    size_t left = 0;
    for (size_t i = 0; i < P.size(); ++i) {
        CHECK(std::fabs(P[i].y) < 1e-6f && P[i].x >= -1.0f - 1e-5f && P[i].x <= 1.0f + 1e-5f);
        CHECK(std::fabs(Cd[i].x - (P[i].x * 0.5f + 0.5f)) < 1e-4f);  // blended, as the colour runs
        CHECK(std::fabs(N[i].y - 1.0f) < 1e-5f);  // a grid faces up
        left += P[i].x < 0.0f;
    }
    CHECK(std::fabs(static_cast<double>(left) / 20000.0 - 0.5) < 0.02);
    // Another seed, other points; nothing to scatter on, nothing.
    scatter->setInt("seed", 3);
    CookEngine engine;
    CHECK(engine.cook(*scatter, CookContext{})->hash() != one->hash());
    Node* dots = g.create("pointcloud", "dots");
    scatter->setInput(0, dots);
    CHECK_EQ(engine.cook(*scatter, CookContext{})->pointCount(), size_t(0));
}

TEST(sops_normals_point_out_of_the_surface) {
    Graph g;
    CookEngine engine;
    Node* sphere = g.create("sphere", "sphere");
    sphere->setInt("rows", 16);
    sphere->setInt("columns", 32);
    Node* normal = g.create("normal", "normal");
    normal->setInput(0, sphere);
    const GeometryPtr geo = engine.cook(*normal, CookContext{});
    const auto P = geo->positions();
    const auto N = geo->points().find("N")->read<Vec3>();
    for (size_t i = 0; i < P.size(); ++i) {
        CHECK(std::fabs(length(N[i]) - 1.0f) < 1e-5f);
        CHECK(dot(N[i], normalize(P[i])) > 0.99f);
    }
}

TEST(sops_copies_land_on_points) {
    Graph g;
    CookEngine engine;
    Node* box = g.create("box", "box");
    box->setVec3("size", Vec3(0.2f, 0.4f, 0.2f));
    Node* dots = g.create("line", "dots");
    dots->setInt("points", 3);
    dots->setFloat("length", 2.0f);
    // Each point: a size, a colour, a way up.
    Node* mark = g.create("pointwrangle", "mark");
    mark->setInput(0, dots);
    mark->setString("snippet", "@pscale = 1.0 + @ptnum; @Cd = vec3(@ptnum, 0.0, 1.0); @N = vec3(1.0, 0.0, 0.0);");
    Node* copy = g.create("copytopoints", "copy");
    copy->setInput(0, box);
    copy->setInput(1, mark);
    GeometryPtr geo = engine.cook(*copy, CookContext{});
    CHECK_EQ(geo->pointCount(), size_t(3 * 8));
    CHECK_EQ(geo->primitiveCount(), size_t(3 * 6));
    CHECK(!geo->points().contains("pscale"));
    const auto P = geo->positions();
    const auto Cd = geo->points().find("Cd")->read<Vec3>();
    for (size_t c = 0; c < 3; ++c) {
        Vec3 lo(1e30f), hi(-1e30f);
        for (size_t i = c * 8; i < (c + 1) * 8; ++i) {
            for (int a = 0; a < 3; ++a) {
                lo[a] = std::min(lo[a], P[i][a]);
                hi[a] = std::max(hi[a], P[i][a]);
            }
            CHECK(near(Cd[i], Vec3(static_cast<float>(c), 0.0f, 1.0f)));
        }
        // Sized by pscale, its +y turned onto +x: 0.4 m long along x now.
        const float s = 1.0f + static_cast<float>(c);
        CHECK(near(hi - lo, Vec3(0.4f * s, 0.2f * s, 0.2f * s), 1e-4f));
        CHECK(near((lo + hi) * 0.5f, Vec3(static_cast<float>(c), 0.0f, 0.0f), 1e-4f));
    }
    // Not turned: as the box stands.
    copy->setBool("align", false);
    geo = engine.cook(*copy, CookContext{});
    Vec3 lo, hi;
    bounds(*geo, lo, hi);
    CHECK(std::fabs((hi.y - lo.y) - 0.4f * 3.0f) < 1e-4f);
}

TEST(sops_copies_turn_by_orient) {
    // A point's orient -- a unit quaternion x, y, z, w -- turns its copy,
    // over its normal: a stone copied onto grit turns as the bit does.
    Graph g;
    CookEngine engine;
    Node* box = g.create("box", "box");
    box->setVec3("size", Vec3(0.2f, 0.4f, 0.2f));
    Node* dots = g.create("line", "dots");
    dots->setInt("points", 2);
    dots->setFloat("length", 2.0f);
    // A quarter turn about x -- +y onto +z -- and half a turn about z, not
    // of unit length; both with a normal that would turn them otherwise.
    Node* turn = g.create("pointwrangle", "turn");
    turn->setInput(0, dots);
    turn->setString("snippet",
                    "@N = set(1.0, 0.0, 0.0);\n"
                    "if (@ptnum == 0) p@orient = set(0.7071068, 0.0, 0.0, 0.7071068);\n"
                    "else p@orient = set(0.0, 0.0, 2.0, 0.0);");
    Node* copy = g.create("copytopoints", "copy");
    copy->setInput(0, box);
    copy->setInput(1, turn);
    const GeometryPtr geo = engine.cook(*copy, CookContext{});
    const GeometryPtr shape = engine.cook(*box, CookContext{});
    const size_t n = shape->pointCount();
    CHECK_EQ(geo->pointCount(), 2 * n);
    CHECK(!geo->points().contains("orient"));
    const auto P = geo->positions();
    const auto from = shape->positions();
    for (size_t i = 0; i < n; ++i) {
        const Vec3 p = from[i];
        CHECK(near(P[i], Vec3(p.x, -p.z, p.y), 1e-5f));                     // about x
        CHECK(near(P[n + i], Vec3(2.0f - p.x, -p.y, p.z), 1e-5f));          // about z, at the second point
    }
}

TEST(sops_obj_files_go_in_and_out) {
    // Polygons stay polygons, lines are open; negative corners count back.
    const char* text = "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nv 2 0 0\n"
                       "f 1/1 2/2/2 3//3 -2\nl 2 5\n";
    Geometry geo;
    std::string error;
    CHECK(io::parseObj(text, geo, error));
    CHECK_EQ(geo.pointCount(), size_t(5));
    CHECK_EQ(geo.primitiveCount(), size_t(2));
    CHECK_EQ(geo.primitiveVertexCount(0), size_t(4));
    CHECK(geo.primitiveClosed(0) && !geo.primitiveClosed(1));
    CHECK(!io::parseObj("v 0 0 0\nf 1 2 3\n", geo, error));
    CHECK(error.find("line 2") != std::string::npos);

    // Out and back in: the same points and faces.
    Graph g;
    CookEngine engine;
    Node* sphere = g.create("sphere", "sphere");
    const GeometryPtr made = engine.cook(*sphere, CookContext{});
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "pg_test_sops";
    fs::create_directories(dir);
    const std::string path = (dir / "ball.obj").string();
    CHECK(io::writeObj(*made, path, error));
    Node* file = g.create("file", "file");
    file->setString("file", path);
    const GeometryPtr read = engine.cook(*file, CookContext{});
    CHECK(file->cookError().empty());
    CHECK_EQ(read->pointCount(), made->pointCount());
    CHECK_EQ(read->primitiveCount(), made->primitiveCount());
    for (size_t i = 0; i < made->pointCount(); ++i) CHECK(near(read->positions()[i], made->positions()[i], 1e-6f));
    for (size_t p = 0; p < made->primitiveCount(); ++p) {
        const auto a = made->primitivePoints(p), b = read->primitivePoints(p);
        CHECK(std::equal(a.begin(), a.end(), b.begin(), b.end()));
    }
    // A file not there: said, and nothing comes out.
    file->setString("file", (dir / "none.obj").string());
    CHECK_EQ(engine.cook(*file, CookContext{})->pointCount(), size_t(0));
    CHECK(file->cookError().find("cannot read") != std::string::npos);
    fs::remove_all(dir);
}

TEST(sops_volumes_travel_with_the_geometry) {
    // 3 x 2 x 2 voxels of 0.5 from (1, 0, 0), holding x (world) at their middles.
    std::vector<float> data;
    for (int k = 0; k < 2; ++k) {
        for (int j = 0; j < 2; ++j) {
            for (int i = 0; i < 3; ++i) data.push_back(1.0f + 0.5f * (static_cast<float>(i) + 0.5f));
        }
    }
    const Volume v = Volume::make("density", Vec3(1.0f, 0.0f, 0.0f), 0.5f, 3, 2, 2, data);
    CHECK_EQ(v.count(), size_t(12));
    CHECK_EQ(v.at(2, 1, 1), 2.25f);
    CHECK_EQ(v.at(3, 0, 0), 0.0f);  // outside
    CHECK(std::fabs(v.sample(Vec3(1.6f, 0.4f, 0.3f)) - 1.6f) < 1e-5f);  // linear in, linear out
    CHECK_EQ(v.sample(Vec3(0.0f, 0.5f, 0.5f)), 0.0f);

    Geometry a;
    a.addVolume(v);
    const Geometry copy = a;  // shares, copies nothing
    CHECK(copy.volumes()[0].values.get() == a.volumes()[0].values.get());
    CHECK(copy.findVolume("density") && !copy.findVolume("heat"));
    CHECK_EQ(copy.hash(), a.hash());
    CHECK(a.memoryUsage() >= 12 * sizeof(float));
    Geometry b;
    b.addVolume(Volume::make("heat", Vec3(), 1.0f, 1, 1, 1, {3.0f}));
    CHECK(b.hash() != a.hash());
    b.append(a);
    CHECK_EQ(b.volumeCount(), size_t(2));
    CHECK_EQ(b.volumes()[1].name, std::string("density"));
    CHECK_EQ(a.volumeCount(), size_t(1));  // the other one untouched
}

TEST(sops_detail_attributes_hold_a_value) {
    Graph g;
    CookEngine engine;
    Node* box = g.create("box", "box");
    Node* note = g.create("attribcreate", "note");
    note->setInput(0, box);
    note->setString("name", "strength");
    note->setInt("class", static_cast<int>(AttrClass::Detail));
    note->setFloat("value", 2.5f);
    const GeometryPtr geo = engine.cook(*note, CookContext{});
    const AttributeArray* a = geo->detail().find("strength");
    CHECK(a && a->size() == 1);
    CHECK_EQ(a->read<float>()[0], 2.5f);
    // Merged into geometry without it, it keeps its value.
    Geometry other;
    other.append(*geo);
    CHECK_EQ(other.detail().find("strength")->read<float>()[0], 2.5f);
}

TEST(sops_a_deleted_node_leaves_nothing_cached_for_the_next) {
    Graph g;
    CookEngine engine;
    for (int round = 0; round < 20; ++round) {
        // Made where the last one may have been, with the same name.
        Node* box = g.create("box", "shape");
        box->setVec3("size", Vec3(static_cast<float>(round + 1)));
        const GeometryPtr geo = engine.cook(*box, CookContext{});
        Vec3 lo, hi;
        bounds(*geo, lo, hi);
        CHECK(std::fabs((hi.x - lo.x) - static_cast<float>(round + 1)) < 1e-5f);
        CHECK(g.remove("shape"));
    }
    CHECK(!g.remove("shape"));
    // What a removed node fed loses its input.
    Node* a = g.create("box", "a");
    Node* t = g.create("transform", "t");
    t->setInput(0, a);
    CHECK_EQ(engine.cook(*t, CookContext{})->pointCount(), size_t(8));
    CHECK(g.remove("a"));
    CHECK(t->input(0) == nullptr);
    CHECK_EQ(engine.cook(*t, CookContext{})->pointCount(), size_t(0));
}
