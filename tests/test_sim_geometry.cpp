//
// Geometry in a simulation network (src/pg/sim/GeometryGraph.h): the
// geometry nodes cooked by the core, the shapes they give the simulations,
// and the simulations brought back as points and volumes.
//
#include "pg/core/Graph.h"
#include "pg/sim/Display.h"
#include "pg/sim/GeometryGraph.h"
#include "pg/sim/Mesh.h"
#include "pg/sim/Network.h"
#include "pg/sim/WaterMesh.h"
#include "pg/sim/World.h"

#include "test_framework.h"

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>

using namespace pg;
using namespace pg::sim;

namespace {

bool mentions(const Compiled& c, int node, const std::string& words) {
    for (const Problem& p : c.problems) {
        if (p.node == node && p.message.find(words) != std::string::npos) return true;
    }
    return false;
}

void boundsOf(const Geometry& geo, Vec3& lo, Vec3& hi) {
    lo = Vec3(1e30f);
    hi = Vec3(-1e30f);
    for (const Vec3& p : geo.positions()) {
        for (int a = 0; a < 3; ++a) {
            lo[a] = std::min(lo[a], p[a]);
            hi[a] = std::max(hi[a], p[a]);
        }
    }
}

bool near(const Vec3& a, const Vec3& b, float eps = 1e-4f) { return length(a - b) < eps; }

uint64_t cooks(const GeometryGraph& g, int id) { return g.coreNode(id) ? g.coreNode(id)->cookCount() : 0; }

}  // namespace

TEST(sim_geometry_nodes_are_the_cores) {
    registerSimGeometryNodes();
    Graph graph;
    int geometryTypes = 0;
    for (const NodeType& t : nodeTypes()) {
        if (std::string(t.category) != "Geometry") {
            CHECK(t.core == nullptr);
            continue;
        }
        ++geometryTypes;
        CHECK(t.core != nullptr);
        CHECK(t.bypassable);
        pg::Node* made = graph.create(t.core, std::string("probe_") + t.name);
        if (!made) {
            ::testing::fail(__FILE__, __LINE__, std::string("no core node ") + t.core);
            continue;
        }
        // Every parameter is one the core node knows by that name.
        for (const ParamDef& d : t.params) {
            if (!made->params().contains(d.name)) {
                ::testing::fail(__FILE__, __LINE__, std::string(t.name) + ": the core has no parameter " + d.name);
            }
        }
        // A geometry node puts out geometry.
        CHECK_EQ(t.outputs.size(), size_t(1));
        CHECK(t.outputs[0].type == PinType::Geometry);
    }
    CHECK(geometryTypes >= 20);
}

TEST(sim_geometry_text_and_code_read_back_the_same) {
    Network net;
    const int wrangle = net.add("point_wrangle");
    const std::string snippet = "// lift \"them\" # up\n@P.y += 0.5 * @ptnum;\n\tif (@P.x > 0) @Cd = {1, 0, 0};\\n\r\n";
    CHECK(net.setText(wrangle, "snippet", snippet));
    CHECK_EQ(net.text(wrangle, "snippet"), snippet);
    CHECK(!net.isDefault(wrangle, "snippet"));
    const int create = net.add("attribute_create");
    CHECK(net.setParam(create, "name", "heat"));
    CHECK(net.setParam(create, "class", "primitive"));
    const std::string text = net.save();
    Network back;
    std::string error;
    std::vector<std::string> warnings;
    CHECK(Network::load(text, back, error, &warnings));
    CHECK(warnings.empty());
    CHECK_EQ(back.text(wrangle, "snippet"), snippet);
    CHECK_EQ(back.text(create, "name"), std::string("heat"));
    const float cls = back.param(create, "class")[0];
    CHECK_EQ(cls, 3.0f);
    CHECK_EQ(back.save(), text);
    // The default text is not saved, and reads back.
    CHECK(net.resetParam(wrangle, "snippet"));
    CHECK(net.isDefault(wrangle, "snippet"));
    CHECK(net.save().find("snippet") == std::string::npos);
    CHECK_EQ(net.text(create, "missing"), std::string());
}

TEST(sim_geometry_one_node_is_displayed) {
    Network net;
    const int box = net.add("box");
    const int move = net.add("transform");
    const int solver = net.add("pyro_solver");
    CHECK(net.connect(box, "geometry", move, "geometry"));
    CHECK_EQ(net.displayed(), 0);
    CHECK(net.setDisplay(box));
    CHECK_EQ(net.displayed(), box);
    CHECK(net.setDisplay(move));
    CHECK_EQ(net.displayed(), move);
    CHECK(!net.setDisplay(solver));  // no geometry to show
    CHECK_EQ(net.displayed(), move);
    const std::string text = net.save();
    Network back;
    std::string error;
    CHECK(Network::load(text, back, error));
    CHECK_EQ(back.displayed(), move);
    CHECK_EQ(back.save(), text);
    // The last flag read wins; one on a node with no geometry is left off.
    std::string both = text;
    const size_t boxLine = both.find("node " + std::to_string(box) + " box ");
    CHECK(boxLine != std::string::npos);
    both.insert(both.find('\n', boxLine) + 1, "  display\n");
    Network twice;
    std::vector<std::string> warnings;
    CHECK(Network::load(both, twice, error, &warnings));
    CHECK(warnings.empty());
    CHECK_EQ(twice.displayed(), move);
    const size_t solverLine = both.find("node " + std::to_string(solver) + " pyro_solver ");
    CHECK(solverLine != std::string::npos);
    both.insert(both.find('\n', solverLine) + 1, "  display\n");
    Network odd;
    CHECK(Network::load(both, odd, error, &warnings));
    CHECK_EQ(warnings.size(), size_t(1));
    CHECK_EQ(odd.displayed(), move);
    CHECK(net.setDisplay(0));
    CHECK_EQ(net.displayed(), 0);
    CHECK(net.setDisplay(box));
    CHECK(net.remove(box));
    CHECK_EQ(net.displayed(), 0);
}

TEST(sim_geometry_links_make_no_loop) {
    Network net;
    const int a = net.add("transform");
    const int b = net.add("normal");
    const int m = net.add("merge");
    std::string error;
    CHECK(net.connect(a, "geometry", b, "geometry"));
    CHECK(net.connect(b, "geometry", m, "geometry"));
    CHECK(!net.connect(m, "geometry", a, "geometry", &error));
    CHECK(error.find("loop") != std::string::npos);
    CHECK(!net.connect(b, "geometry", b, "geometry", &error));
    // Geometry goes into geometry, and into a shape -- not into a force.
    const int fire = net.add("pyro_source");
    const int solver = net.add("pyro_solver");
    CHECK(net.connect(m, "geometry", fire, "shape"));
    CHECK(!net.connect(m, "geometry", solver, "forces", &error));
}

TEST(sim_geometry_graph_cooks_only_what_changed) {
    Network net;
    const int box = net.add("box");
    CHECK(net.setParam(box, "size", "2 2 2"));
    CHECK(net.setParam(box, "center", "0 0 0"));
    const int move = net.add("transform");
    CHECK(net.setParam(move, "t", "1 0 0"));
    const int normals = net.add("normal");
    CHECK(net.connect(box, "geometry", move, "geometry"));
    CHECK(net.connect(move, "geometry", normals, "geometry"));

    GeometryGraph g;
    g.engine().setParallelBranches(false);
    g.sync(net);
    CHECK(g.contains(box) && g.contains(normals));
    GeometryPtr geo = g.cook(normals, 1);
    CHECK(geo && geo->pointCount() == 8 && geo->primitiveCount() == 6);
    CHECK(geo->points().find("N"));
    Vec3 lo, hi;
    boundsOf(*geo, lo, hi);
    CHECK(near(lo, Vec3(0.0f, -1.0f, -1.0f)) && near(hi, Vec3(2.0f, 1.0f, 1.0f)));
    CHECK_EQ(cooks(g, box), uint64_t(1));

    // Again, unchanged: nothing cooks.
    g.sync(net);
    CHECK(g.cook(normals, 1) == geo);
    CHECK_EQ(cooks(g, box) + cooks(g, move) + cooks(g, normals), uint64_t(3));
    // The move changed: the box is not cooked again.
    CHECK(net.setParam(move, "t", "0 3 0"));
    g.sync(net);
    geo = g.cook(normals, 1);
    boundsOf(*geo, lo, hi);
    CHECK(near(lo, Vec3(-1.0f, 2.0f, -1.0f)));
    CHECK_EQ(cooks(g, box), uint64_t(1));
    CHECK_EQ(cooks(g, move), uint64_t(2));
    // Set to what it is: nothing is dirty.
    CHECK(net.setParam(move, "t", "0 3 0"));
    g.sync(net);
    g.cook(normals, 1);
    CHECK_EQ(cooks(g, move), uint64_t(2));

    // Bypassed, the move passes the box on.
    CHECK(net.setBypass(move, true));
    g.sync(net);
    geo = g.cook(normals, 1);
    boundsOf(*geo, lo, hi);
    CHECK(near(lo, Vec3(-1.0f)) && near(hi, Vec3(1.0f)));
    CHECK(g.cook(move, 1)->pointCount() == 8);  // shown bypassed: what comes in
    CHECK(net.setBypass(move, false));

    // Merged with a sphere, in the order of the links; switched between.
    const int ball = net.add("sphere");
    CHECK(net.setParam(ball, "rows", "4"));
    CHECK(net.setParam(ball, "columns", "4"));
    const int merge = net.add("merge");
    CHECK(net.connect(ball, "geometry", merge, "geometry"));
    CHECK(net.connect(move, "geometry", merge, "geometry"));
    g.sync(net);
    geo = g.cook(merge, 1);
    CHECK_EQ(geo->pointCount(), size_t(4 * 3 + 2 + 8));
    const int pick = net.add("switch");
    CHECK(net.connect(ball, "geometry", pick, "geometry"));
    CHECK(net.connect(normals, "geometry", pick, "geometry"));
    CHECK(net.setParam(pick, "index", "1"));
    g.sync(net);
    CHECK_EQ(g.cook(pick, 1)->pointCount(), size_t(8));
    // Turned round -- A -> B becomes B -> A -- and still wired.
    Network turned;
    const int p = turned.add("transform");
    const int q = turned.add("normal");
    const int start = turned.add("box");
    CHECK(turned.connect(start, "geometry", p, "geometry"));
    CHECK(turned.connect(p, "geometry", q, "geometry"));
    GeometryGraph h;
    h.sync(turned);
    CHECK_EQ(h.cook(q, 1)->pointCount(), size_t(8));
    turned.disconnect(turned.linksInto(q, "geometry")[0]);
    turned.disconnect(turned.linksInto(p, "geometry")[0]);
    CHECK(turned.connect(start, "geometry", q, "geometry"));
    CHECK(turned.connect(q, "geometry", p, "geometry"));
    h.sync(turned);
    CHECK_EQ(h.cook(p, 1)->pointCount(), size_t(8));
    CHECK(h.coreNode(p)->input(0) == h.coreNode(q));

    // A node taken out of the network is out of the graph.
    CHECK(net.remove(ball));
    g.sync(net);
    CHECK(!g.contains(ball));
    CHECK_EQ(g.cook(merge, 1)->pointCount(), size_t(8));
    CHECK(g.cook(12345, 1) == nullptr);
}

TEST(sim_geometry_graph_follows_another_network_loaded_where_one_was) {
    // The editor opens an example into the network it has open. Loaded from
    // text, both are a fresh network each: the graph must see the new one,
    // whose node 1 is another box -- not keep what it cooked of the old.
    Network net;
    std::string error;
    CHECK(Network::load("pgsim 1\nnode 1 box 1 small 0 0\n  param size 1 1 1\n", net, error));
    GeometryGraph g;
    g.sync(net);
    Vec3 lo, hi;
    boundsOf(*g.cook(1, 1), lo, hi);
    CHECK(near(hi - lo, Vec3(1.0f, 1.0f, 1.0f)));
    Network other;
    CHECK(Network::load("pgsim 1\nnode 1 box 1 big 0 0\n  param size 3 3 3\n", other, error));
    net = other;
    CHECK(net.revision() != 0u);
    g.sync(net);
    boundsOf(*g.cook(1, 1), lo, hi);
    CHECK(near(hi - lo, Vec3(3.0f, 3.0f, 3.0f)));
    // No two networks share a revision: each edit gets a new one.
    Network a, b;
    const int ia = a.add("box"), ib = b.add("box");
    CHECK(ia == ib);
    CHECK(a.revision() != b.revision());
}

TEST(sim_geometry_files_are_read_again_when_they_change) {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "pg_test_geometry_file";
    fs::create_directories(dir);
    auto write = [&](const std::string& text) {
        std::ofstream out(dir / "shape.obj", std::ios::binary);
        out << text;
    };
    write("v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
    Network net;
    const int file = net.add("file");
    CHECK(net.setText(file, "file", "shape.obj"));
    GeometryGraph g;
    g.sync(net, dir.string());
    GeometryPtr geo = g.cook(file, 1);
    CHECK(geo && geo->pointCount() == 3 && geo->primitiveCount() == 1);
    CHECK(g.error(file).empty());
    // Changed on disk: read again on the next sync.
    write("v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nf 1 2 3 4\nl 1 3\n");
    net.setParam(file, "file", "\"shape.obj\"");  // the network itself is the same
    g.sync(net, dir.string());
    geo = g.cook(file, 1);
    CHECK(geo && geo->pointCount() == 4 && geo->primitiveCount() == 2);
    // Not there: empty, and said.
    CHECK(net.setText(file, "file", "gone.obj"));
    g.sync(net, dir.string());
    geo = g.cook(file, 1);
    CHECK(geo && geo->pointCount() == 0);
    CHECK(g.error(file).find("gone.obj") != std::string::npos);
    fs::remove_all(dir);
}

TEST(sim_geometry_is_a_shape_for_the_simulations) {
    Network net;
    const int box = net.add("box");
    CHECK(net.setParam(box, "size", "0.4 0.6 0.4"));
    CHECK(net.setParam(box, "center", "0.3 0.5 -0.2"));
    const int rock = net.add("object");
    CHECK(net.connect(box, "geometry", rock, "shape"));
    const int ball = net.add("sphere");
    CHECK(net.setParam(ball, "radius", "0.25"));
    CHECK(net.setParam(ball, "center", "0 0.4 0"));
    const int fire = net.add("pyro_source");
    CHECK(net.setParam(fire, "smoke", "4"));
    CHECK(net.connect(ball, "geometry", fire, "shape"));
    const int solver = net.add("pyro_solver");
    const int look = net.add("volume_look");
    const int out = net.add("output");
    CHECK(net.connect(fire, "source", solver, "sources"));
    CHECK(net.connect(rock, "collider", solver, "colliders"));
    CHECK(net.connect(solver, "gas", look, "gas"));
    CHECK(net.connect(look, "look", out, "look"));

    GeometryGraph g;
    Compiled c = net.compile({}, &g);
    CHECK(c.ok);
    for (const Problem& p : c.problems) ::testing::fail(__FILE__, __LINE__, p.message);
    CHECK_EQ(c.solids.size(), size_t(1));
    const Collider& body = c.solids[0].body;
    CHECK(body.shape == Shape::Mesh && body.mesh);
    CHECK(near(body.center, Vec3(0.3f, 0.5f, -0.2f)));
    CHECK(near(body.size, Vec3(0.4f, 0.6f, 0.4f)));
    CHECK(near(body.rotation, Vec3()));
    CHECK(body.mesh->distance(Vec3(0.3f, 0.5f, -0.2f)) < -0.15f);
    CHECK(body.mesh->distance(Vec3(0.3f, 1.0f, -0.2f)) > 0.15f);
    CHECK(c.world.gas.colliders.size() == 1 && c.world.gas.colliders[0].mesh == body.mesh);
    CHECK_EQ(c.world.gas.emitters.size(), size_t(1));
    const Emitter& e = c.world.gas.emitters[0];
    CHECK(e.shape == Shape::Mesh && e.mesh);
    CHECK(near(e.center, Vec3(0.0f, 0.4f, 0.0f), 1e-3f));
    CHECK(e.mesh->distance(Vec3(0.0f, 0.4f, 0.0f)) < -0.2f);
    // The geometry takes part.
    CHECK(c.isActive(box) && c.isActive(ball));
    // Compiled again, unchanged: nothing cooks again, and the meshes are the same.
    const uint64_t before = cooks(g, box) + cooks(g, ball);
    Compiled again = net.compile({}, &g);
    CHECK_EQ(cooks(g, box) + cooks(g, ball), before);
    CHECK(again.solids[0].body.mesh == body.mesh);
    // Without a graph of its own the network makes one: the same shapes.
    Compiled alone = net.compile();
    CHECK(alone.solids[0].body.mesh == body.mesh);

    // Empty geometry: its own shape stands in, and that is said.
    const int far = net.add("group_box");
    CHECK(net.setParam(far, "min", "5 5 5"));
    CHECK(net.setParam(far, "max", "6 6 6"));
    const int blast = net.add("blast");
    CHECK(net.setParam(blast, "invert", "on"));  // keeps the group: nothing
    net.disconnect(net.linksInto(rock, "shape")[0]);
    CHECK(net.connect(box, "geometry", far, "geometry"));
    CHECK(net.connect(far, "geometry", blast, "geometry"));
    CHECK(net.connect(blast, "geometry", rock, "shape"));
    c = net.compile({}, &g);
    CHECK(mentions(c, rock, "empty"));
    CHECK(c.solids[0].body.shape == static_cast<Shape>(net.param(rock, "shape")[0]) && !c.solids[0].body.mesh);
    CHECK(near(c.solids[0].body.center, Vec3(net.param(rock, "center")[0], net.param(rock, "center")[1],
                                              net.param(rock, "center")[2])));

    // Points: a ball round each.
    const int cloud = net.add("point_cloud");
    CHECK(net.setParam(cloud, "count", "40"));
    CHECK(net.setParam(cloud, "size", "0.5"));
    net.disconnect(net.linksInto(rock, "shape")[0]);
    CHECK(net.connect(cloud, "geometry", rock, "shape"));
    c = net.compile({}, &g);
    CHECK(!mentions(c, rock, "empty"));
    CHECK(c.solids[0].body.mesh && c.solids[0].body.mesh->mesh().triangles.size() == 40 * 20);

    // Geometry from a simulation has not been simulated when shapes are made.
    const int water = net.add("liquid_points");
    net.disconnect(net.linksInto(rock, "shape")[0]);
    CHECK(net.connect(water, "geometry", rock, "shape"));
    c = net.compile({}, &g);
    CHECK(mentions(c, rock, "comes from a simulation"));
    CHECK(mentions(c, water, "Nothing comes in"));
}

TEST(sim_geometry_overlapping_shapes_fill_their_overlap) {
    // Two boxes merged, overlapping: inside both is inside.
    Network net;
    const int a = net.add("box");
    CHECK(net.setParam(a, "center", "0 0 0"));
    const int b = net.add("box");
    CHECK(net.setParam(b, "center", "0.5 0 0"));
    const int merge = net.add("merge");
    CHECK(net.connect(a, "geometry", merge, "geometry"));
    CHECK(net.connect(b, "geometry", merge, "geometry"));
    GeometryGraph g;
    g.sync(net);
    const auto boxes = meshFromGeometry(*g.cook(merge, 1));
    CHECK(boxes);
    CHECK(boxes->distance(Vec3(0.25f, 0.0f, 0.0f)) < -0.2f);  // in both: 0.25 from a face within
    CHECK(boxes->distance(Vec3(-0.3f, 0.0f, 0.0f)) < -0.1f);  // in one
    CHECK(boxes->distance(Vec3(0.9f, 0.0f, 0.0f)) < -0.05f);  // in the other
    CHECK(boxes->distance(Vec3(0.25f, 0.8f, 0.0f)) > 0.2f);   // above
    // A soup -- the corners not shared -- is one shell still.
    Geometry soup;
    const Vec3 c[8] = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}};
    const int faces[6][4] = {{0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4}, {3, 7, 6, 2}, {0, 4, 7, 3}, {1, 2, 6, 5}};
    for (const auto& f : faces) {
        const size_t first = soup.pointCount();
        soup.addPoints(4);
        std::vector<uint32_t> corners;
        for (int k = 0; k < 4; ++k) {
            soup.positionsForWrite()[first + static_cast<size_t>(k)] = c[f[k]];
            corners.push_back(static_cast<uint32_t>(first) + static_cast<uint32_t>(k));
        }
        soup.addPrimitive(corners, true);
    }
    const auto cube = meshFromGeometry(soup);
    CHECK(cube && cube->distance(Vec3(0.5f)) < -0.4f && cube->distance(Vec3(0.5f, 0.5f, 1.5f)) > 0.4f);
    // Two points close together: their balls overlap, and fill it.
    Geometry pair;
    pair.addPoints(2);
    pair.positionsForWrite()[0] = Vec3(0.0f);
    pair.positionsForWrite()[1] = Vec3(0.1f, 0.0f, 0.0f);
    auto scale = pair.points().create("pscale", AttrType::Float).write<float>();
    scale[0] = scale[1] = 0.1f;
    const auto balls = meshFromGeometry(pair);
    CHECK(balls && balls->distance(Vec3(0.05f, 0.0f, 0.0f)) < -0.03f);
    CHECK(balls->distance(Vec3(0.05f, 0.3f, 0.0f)) > 0.1f);
    // The same geometry, the same mesh; none from nothing.
    CHECK(meshFromGeometry(pair) == balls);
    CHECK(meshFromGeometry(Geometry()) == nullptr);
}

TEST(sim_geometry_simulations_come_back_as_points_and_volumes) {
    Network net;
    const int hose = net.add("water_source");
    CHECK(net.setParam(hose, "mode", "flow"));
    const int liquid = net.add("liquid_solver");
    CHECK(net.setParam(liquid, "resolution", "16"));
    const int waterLook = net.add("water_look");
    const int fire = net.add("pyro_source");
    CHECK(net.setParam(fire, "fuel", "10"));
    const int pyro = net.add("pyro_solver");
    CHECK(net.setParam(pyro, "resolution", "16"));
    const int smoke = net.add("volume_look");
    const int rain = net.add("rain");
    const int out = net.add("output");
    CHECK(net.connect(hose, "water", liquid, "sources"));
    CHECK(net.connect(liquid, "liquid", waterLook, "liquid"));
    CHECK(net.connect(waterLook, "look", out, "look"));
    CHECK(net.connect(fire, "source", pyro, "sources"));
    CHECK(net.connect(pyro, "gas", smoke, "gas"));
    CHECK(net.connect(smoke, "look", out, "look"));
    CHECK(net.connect(rain, "look", out, "look"));
    const int points = net.add("liquid_points");
    const int drops = net.add("rain_points");
    const int gas = net.add("gas_volume");
    CHECK(net.connect(liquid, "liquid", points, "liquid"));
    CHECK(net.connect(rain, "rain", drops, "rain"));
    CHECK(net.connect(pyro, "gas", gas, "gas"));
    const int colored = net.add("color");
    CHECK(net.connect(points, "geometry", colored, "geometry"));
    CHECK(net.setDisplay(colored));

    Compiled c = net.compile();
    CHECK(c.ok);
    CHECK(c.problems.empty());
    CHECK(c.world.keepParticles);
    CHECK_EQ(c.display, colored);
    CHECK(c.isActive(colored) && c.isActive(points));

    WorldSolver sim(c.world);
    std::map<int, std::shared_ptr<const Frame>> frames;
    for (int f = 1; f <= 4; ++f) {
        sim.step();
        frames[f] = std::make_shared<Frame>(sim.capture());
    }
    GeometryGraph g;
    g.setFrames([&](int f) -> std::shared_ptr<const Frame> {
        const auto it = frames.find(f);
        return it == frames.end() ? nullptr : it->second;
    });
    g.sync(net);

    // The water's particles, with their velocity and colour.
    GeometryPtr geo = g.cook(colored, 4);
    CHECK(geo && geo->pointCount() == sim.water()->particleCount() && geo->pointCount() > 0);
    CHECK(geo->points().find("v") && geo->points().find("foam") && geo->points().find("Cd"));
    // ... and each one's number, the same from frame to frame.
    const AttributeArray* id = geo->points().find("id");
    CHECK(id && id->type() == AttrType::Int);
    if (id) {
        const auto ids = id->read<int32_t>();
        for (size_t i = 0; i < ids.size(); ++i) CHECK_EQ(static_cast<uint32_t>(ids[i]), frames[4]->water.ids[i]);
    }
    // Another frame, other points -- the hose has poured less by then; one
    // not simulated yet, none.
    CHECK(g.cook(points, 2)->pointCount() > 0);
    CHECK(g.cook(points, 2)->pointCount() < geo->pointCount());
    CHECK_EQ(g.cook(points, 9)->pointCount(), size_t(0));
    // Back to a frame cooked before: from the cache.
    uint64_t before = cooks(g, points);
    CHECK(g.cook(colored, 4) == geo);
    CHECK_EQ(cooks(g, points), before);
    // Simulated again: cooked again.
    frames[4] = std::make_shared<Frame>(*frames[4]);
    CHECK(g.cook(colored, 4) != geo);
    CHECK_EQ(cooks(g, points), before + 1);
    // Simulated at last: no longer empty.
    frames[9] = frames[4];
    CHECK(g.cook(points, 9)->pointCount() > 0);

    // The rain: drops, and splashes marked.
    geo = g.cook(drops, 4);
    CHECK(geo && geo->pointCount() == frames[4]->rain.dropCount() + frames[4]->rain.dropletCount());
    CHECK(geo->pointCount() > 0 && geo->points().find("droplet") && geo->points().find("v"));
    // Numbered: the drops as the rain numbers them, the droplets from 2^30.
    const AttributeArray* rainId = geo->points().find("id");
    CHECK(rainId != nullptr);
    if (rainId) {
        const auto ids = rainId->read<int32_t>();
        const auto droplet = geo->points().find("droplet")->read<int32_t>();
        for (size_t i = 0; i < ids.size(); ++i) CHECK_EQ(ids[i] >= (1 << 30), droplet[i] == 1);
        CHECK_EQ(static_cast<uint32_t>(ids[0]), frames[4]->rain.dropIds[0]);
    }
    // The gas: three volumes on the solver's grid -- and the steam, as the
    // rain falls through the fire -- and how fast it goes, on blocks of
    // 2 x 2 x 2 cells.
    geo = g.cook(gas, 4);
    CHECK(!frames[4]->steam.empty());
    CHECK(!frames[4]->velocity.empty());
    CHECK(geo && geo->volumeCount() == 7);
    if (const Volume* vel = geo ? geo->findVolume("vel.y") : nullptr) {
        CHECK_EQ(vel->res[0], frames[4]->domain.cells[0] / 2);
        CHECK(std::fabs(vel->voxel - 2.0f * frames[4]->domain.voxel) < 1e-6f);
        float rising = 0.0f;
        for (const float v : *vel->values) rising = std::max(rising, v);
        CHECK(rising > 0.0f);  // the hot gas goes up
    } else {
        CHECK(false);
    }
    const Volume* density = geo->findVolume("density");
    const Volume* flame = geo->findVolume("flame");
    const Volume* steam = geo->findVolume("steam");
    CHECK(density && flame && steam && geo->findVolume("temperature"));
    CHECK_EQ(density->res[0], frames[4]->domain.cells[0]);
    CHECK_EQ(density->count(), frames[4]->domain.cellCount());
    CHECK_EQ(steam->count(), frames[4]->domain.cellCount());
    float sum = 0.0f, steamSum = 0.0f;
    for (const float v : *density->values) sum += v;
    for (const float v : *steam->values) steamSum += v;
    CHECK(sum > 0.0f);
    CHECK(steamSum > 0.0f);

    // From a solver that does not reach the Output: said, and not kept.
    const int other = net.add("liquid_solver");
    net.disconnect(net.linksInto(points, "liquid")[0]);
    CHECK(net.connect(other, "liquid", points, "liquid"));
    c = net.compile();
    CHECK(mentions(c, points, "not simulated"));
    CHECK(!c.world.keepParticles);
}

TEST(sim_geometry_water_comes_back_as_a_closed_surface) {
    // A block of water let go in a tank: the Liquid Surface node gives a
    // closed mesh round it, turned outward, holding as much as the water
    // does, moving as the water does, with its foam.
    Network net;
    const int block = net.add("water_source");
    CHECK(net.setParam(block, "center", "-0.2 0.25 0"));
    CHECK(net.setParam(block, "size", "0.5 0.5 0.8"));
    const int liquid = net.add("liquid_solver");
    CHECK(net.setParam(liquid, "resolution", "24"));
    CHECK(net.setParam(liquid, "size", "1.2 0.8 1"));
    const int look = net.add("water_look");
    const int out = net.add("output");
    CHECK(net.connect(block, "water", liquid, "sources"));
    CHECK(net.connect(liquid, "liquid", look, "liquid"));
    CHECK(net.connect(look, "look", out, "look"));
    const int surface = net.add("liquid_surface");
    CHECK(net.connect(liquid, "liquid", surface, "liquid"));
    CHECK(net.setDisplay(surface));
    Compiled c = net.compile();
    CHECK(c.ok && c.problems.empty());
    CHECK(!c.world.keepParticles);  // the surface needs no particles
    WorldSolver sim(c.world);
    for (int f = 0; f < 6; ++f) sim.step();
    auto frame = std::make_shared<const Frame>(sim.capture());
    GeometryGraph g;
    g.setFrames([&](int f) { return f == 6 ? frame : nullptr; });
    g.sync(net);
    const GeometryPtr geo = g.cook(surface, 6);
    CHECK(geo && geo->primitiveCount() > 100);
    if (!geo || geo->primitiveCount() == 0) return;
    // Closed: each edge between two faces, once each way.
    std::map<std::pair<uint32_t, uint32_t>, int> edges;
    for (size_t f = 0; f < geo->primitiveCount(); ++f) {
        const auto p = geo->primitivePoints(f);
        for (size_t i = 0; i < p.size(); ++i) ++edges[{p[i], p[(i + 1) % p.size()]}];
    }
    size_t open = 0;
    for (const auto& [e, n] : edges) {
        if (n != 1 || edges.count({e.second, e.first}) == 0) ++open;
    }
    CHECK_EQ(open, size_t(0));
    // As much as the frame's distance holds below 0 -- and so a little more
    // than the water itself: the particles' spheres reach past it.
    const auto P = geo->positions();
    double held = 0.0;
    for (size_t f = 0; f < geo->primitiveCount(); ++f) {
        const auto p = geo->primitivePoints(f);
        for (size_t i = 1; i + 1 < p.size(); ++i) held += dot(P[p[0]], cross(P[p[i]], P[p[i + 1]])) / 6.0;
    }
    const WaterFrame& w = frame->water;
    double inside = 0.0;
    for (int k = 0; k < w.domain.cells[2]; ++k) {
        for (int j = 0; j < w.domain.cells[1]; ++j) {
            for (int i = 0; i < w.domain.cells[0]; ++i) inside += w.distance(i, j, k) < 0.0f ? 1.0 : 0.0;
        }
    }
    inside *= static_cast<double>(w.domain.voxel) * w.domain.voxel * w.domain.voxel;
    CHECK(std::fabs(held / inside - 1.0) < 0.05);
    CHECK(held * 1000.0 > w.litres && held * 1000.0 < 1.15 * w.litres);
    // Its normals, its velocity -- the frame's flow where it is --, its foam.
    const AttributeArray* n = geo->points().find("N");
    const AttributeArray* v = geo->points().find("v");
    const AttributeArray* foam = geo->points().find("foam");
    CHECK(n && v && foam);
    if (!n || !v || !foam) return;
    float fastest = 0.0f;
    for (size_t i = 0; i < P.size(); ++i) {
        CHECK(std::fabs(length(n->read<Vec3>()[i]) - 1.0f) < 1e-4f);
        CHECK(near(v->read<Vec3>()[i], frame->water.flowAt(P[i])));
        CHECK(foam->read<float>()[i] >= 0.0f && foam->read<float>()[i] <= 1.0f);
        fastest = std::max(fastest, length(v->read<Vec3>()[i]));
    }
    CHECK(fastest > 0.3f);  // the block falls apart

    // Rain rings the water: the ripples raise and tilt its top, not its sides
    // or its bottom.
    Frame rained = *frame;
    RainFrame& r = rained.rain;
    const Domain d = rained.water.flowDomain();
    r.rippleOrigin = d.origin();
    r.rippleCell = 0.02f;
    r.rippleCells[0] = static_cast<int>(d.size().x / r.rippleCell);
    r.rippleCells[1] = static_cast<int>(d.size().z / r.rippleCell);
    r.ripples.assign(static_cast<size_t>(r.rippleCells[0]) * static_cast<size_t>(r.rippleCells[1]), toHalf(0.004f));
    const auto calm = waterMesh(rained.water), rippled = waterMesh(rained.water, &rained.rain);
    CHECK_EQ(calm->pointCount(), rippled->pointCount());
    const auto Nc = calm->points().find("N")->read<Vec3>();
    const auto a = calm->positions(), b = rippled->positions();
    int raised = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        const float up = std::max(Nc[i].y, 0.0f);
        CHECK(std::fabs(b[i].y - a[i].y - 0.004f * up * up) < 1e-4f);
        if (Nc[i].y > 0.99f) ++raised;
        CHECK(b[i].x == a[i].x && b[i].z == a[i].z);
    }
    CHECK(raised > 50);
}

TEST(sim_geometry_is_drawn_in_its_colours) {
    // A box: twelve triangles, their corners bent by their own faces.
    Network net;
    const int box = net.add("box");
    CHECK(net.setParam(box, "center", "0 0 0"));
    GeometryGraph g;
    g.sync(net);
    DisplayGeometry d = displayOf(*g.cook(box, 1));
    CHECK_EQ(d.triangleCount(), size_t(12));
    CHECK_EQ(d.dotCount(), size_t(0));
    CHECK_EQ(d.segmentCount(), size_t(0));
    CHECK(near(d.lo, Vec3(-0.5f)) && near(d.hi, Vec3(0.5f)));
    for (size_t c = 0; c < 36; ++c) {
        const float* v = d.triangles.data() + 9 * c;
        const Vec3 p(v[0], v[1], v[2]), n(v[3], v[4], v[5]);
        // A face's normal: along one axis, out of the box.
        CHECK(std::fabs(length(n) - 1.0f) < 1e-4f);
        CHECK(dot(n, p) > 0.49f);
        CHECK(near(Vec3(v[6], v[7], v[8]), Vec3(0.72f, 0.72f, 0.74f)));  // no Cd: grey
    }

    // Cd: the vertex's before the point's, the primitive's, the detail's.
    Geometry geo;
    geo.addPoints(4);
    auto P = geo.positionsForWrite();
    P[0] = Vec3(0, 0, 0);
    P[1] = Vec3(1, 0, 0);
    P[2] = Vec3(1, 1, 0);
    P[3] = Vec3(0, 1, 0);
    const uint32_t quad[4] = {0, 1, 2, 3};
    geo.addPrimitive(quad, true);
    geo.detail().create("Cd", AttrType::Vec3).write<Vec3>()[0] = Vec3(0, 0, 1);
    d = displayOf(geo);
    CHECK_EQ(d.triangleCount(), size_t(2));
    CHECK(near(Vec3(d.triangles[6], d.triangles[7], d.triangles[8]), Vec3(0, 0, 1)));
    geo.primitives().create("Cd", AttrType::Vec3).write<Vec3>()[0] = Vec3(0, 1, 0);
    d = displayOf(geo);
    CHECK(near(Vec3(d.triangles[6], d.triangles[7], d.triangles[8]), Vec3(0, 1, 0)));
    geo.points().create("Cd", AttrType::Vec3).write<Vec3>()[0] = Vec3(1, 0, 0);
    d = displayOf(geo);
    CHECK(near(Vec3(d.triangles[6], d.triangles[7], d.triangles[8]), Vec3(1, 0, 0)));  // corner 0 is point 0
    geo.vertices().create("Cd", AttrType::Float).write<float>()[0] = 0.5f;
    d = displayOf(geo);
    CHECK(near(Vec3(d.triangles[6], d.triangles[7], d.triangles[8]), Vec3(0.5f)));
    // Point normals, where there are some, bend the corners.
    auto N = geo.points().create("N", AttrType::Vec3).write<Vec3>();
    for (size_t i = 0; i < 4; ++i) N[i] = Vec3(0, 0, -1);
    d = displayOf(geo);
    CHECK(near(Vec3(d.triangles[3], d.triangles[4], d.triangles[5]), Vec3(0, 0, -1)));
    // No velocity v: none for the corners; with one, each corner its point's
    // -- what the passes' motion vectors come from.
    CHECK(d.velocities.empty());
    auto V = geo.points().create("v", AttrType::Vec3).write<Vec3>();
    for (size_t i = 0; i < 4; ++i) V[i] = Vec3(static_cast<float>(i), 0, 0);
    d = displayOf(geo);
    CHECK_EQ(d.velocities.size(), d.triangles.size() / 3);
    for (size_t c = 0; c < d.triangleCount() * 3; ++c) {
        const Vec3 at(d.triangles[9 * c], d.triangles[9 * c + 1], d.triangles[9 * c + 2]);
        const Vec3 v(d.velocities[3 * c], d.velocities[3 * c + 1], d.velocities[3 * c + 2]);
        for (size_t i = 0; i < 4; ++i) {
            if (near(at, P[i])) CHECK(near(v, V[i]));  // the corner's point's
        }
    }

    // An open polyline: its segments; a point no primitive uses: a dot, as
    // wide as its pscale.
    Geometry wire;
    wire.addPoints(4);
    auto W = wire.positionsForWrite();
    for (size_t i = 0; i < 4; ++i) W[i] = Vec3(static_cast<float>(i), 0.0f, 0.0f);
    const uint32_t line[3] = {0, 1, 2};
    wire.addPrimitive(line, false);
    auto scale = wire.points().create("pscale", AttrType::Float).write<float>();
    scale[3] = 0.25f;
    d = displayOf(wire);
    CHECK_EQ(d.triangleCount(), size_t(0));
    CHECK_EQ(d.segmentCount(), size_t(2));
    CHECK_EQ(d.dotCount(), size_t(1));
    CHECK(near(Vec3(d.dots[0], d.dots[1], d.dots[2]), Vec3(3, 0, 0)));
    CHECK_EQ(d.dots[6], 0.25f);
    CHECK(near(d.hi, Vec3(3, 0, 0)));

    // A volume: its box, and a dot in each voxel that is not empty -- no
    // more than there is room for.
    Geometry fog;
    std::vector<float> values(4 * 4 * 4, 0.0f);
    values[5] = 1.0f;
    values[6] = 0.5f;
    fog.addVolume(Volume::make("density", Vec3(0.0f), 0.25f, 4, 4, 4, values));
    d = displayOf(fog);
    CHECK_EQ(d.segmentCount(), size_t(12));
    CHECK_EQ(d.dotCount(), size_t(2));
    CHECK(near(d.lo, Vec3(0.0f)) && near(d.hi, Vec3(1.0f)));
    std::vector<float> full(32 * 32 * 32, 1.0f);
    Geometry thick;
    thick.addVolume(Volume::make("density", Vec3(0.0f), 0.1f, 32, 32, 32, full));
    d = displayOf(thick, 5000);
    CHECK(d.dotCount() <= 5000 && d.dotCount() > 1000);
    CHECK(d.dots[6] > 0.03f);  // thinned dots are bigger
}

namespace {

/// A DisplayMesh's corners laid out as displayOf lays out its triangles:
/// nine floats a corner -- and their velocities, three.
void expand(const DisplayMesh& m, std::vector<float>& corners, std::vector<float>& velocities) {
    corners.clear();
    velocities.clear();
    for (const uint32_t v : m.indices) {
        corners.insert(corners.end(), m.places.begin() + 6 * v, m.places.begin() + 6 * v + 6);
        corners.insert(corners.end(), m.colors.begin() + 3 * v, m.colors.begin() + 3 * v + 3);
        if (!m.velocities.empty()) velocities.insert(velocities.end(), m.velocities.begin() + 3 * v, m.velocities.begin() + 3 * v + 3);
    }
}

/// The same floats, bit for bit.
bool sameFloats(const std::vector<float>& a, const std::vector<float>& b) {
    return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0);
}

/// Whether the mesh draws what displayOf draws of `geo`, corner for corner.
bool drawsAsDisplayOf(const DisplayMesh& mesh, const Geometry& geo) {
    const DisplayGeometry d = displayOf(geo);
    std::vector<float> corners, velocities;
    expand(mesh, corners, velocities);
    return sameFloats(corners, d.triangles) && sameFloats(velocities, d.velocities);
}

}  // namespace

TEST(display_mesh_is_the_triangles_of_displayOf_with_corners_shared) {
    // A box keeps its edges -- three vertices at each of its points -- and a
    // ball is smooth -- one a point: corner for corner, bit for bit, what
    // displayOf draws, a sixth of the corners or fewer.
    Network net;
    const int box = net.add("box");
    const int ball = net.add("sphere");
    GeometryGraph g;
    g.sync(net);
    for (const int id : {box, ball}) {
        const GeometryPtr geo = g.cook(id, 1);
        DisplayMesher mesher;
        DisplayMesh mesh;
        CHECK(mesher.make(geo, mesh) == DisplayMesher::Made::Anew);
        CHECK(drawsAsDisplayOf(mesh, *geo));
        CHECK(!mesher.hasRest());
        const DisplayGeometry d = displayOf(*geo);
        CHECK(near(mesh.lo, d.lo) && near(mesh.hi, d.hi));
        CHECK_EQ(mesh.triangleCount(), d.triangleCount());
        CHECK_EQ(mesh.vertexCount(), id == box ? size_t(24) : geo->pointCount());
    }

    // Colours of vertices, points and primitives, velocities, a pane of
    // glass, a line, a loose point: the faces not glass are the mesh's, the
    // rest displayOf's without them.
    Geometry geo;
    geo.addPoints(13);
    auto P = geo.positionsForWrite();
    for (size_t i = 0; i < 6; ++i) P[i] = Vec3(static_cast<float>(i % 3), 0.0f, static_cast<float>(i / 3));
    for (size_t i = 6; i < 10; ++i) P[i] = Vec3(static_cast<float>((i - 6) % 2), 1.0f, static_cast<float>((i - 6) / 2) + 3.0f);
    P[10] = Vec3(-1.0f, 0.0f, 0.0f);
    P[11] = Vec3(-1.0f, 1.0f, 0.0f);
    P[12] = Vec3(4.0f, 2.0f, 1.0f);
    const uint32_t a[4] = {0, 1, 4, 3}, b[4] = {1, 2, 5, 4}, pane[4] = {6, 7, 9, 8}, line[2] = {10, 11};
    geo.addPrimitive(a, true);
    geo.addPrimitive(b, true);
    geo.addPrimitive(pane, true);
    geo.addPrimitive(line, false);
    geo.primitives().create("glass", AttrType::Int).write<int32_t>()[2] = 1;
    geo.primitives().create("Cd", AttrType::Vec3).write<Vec3>()[1] = Vec3(0.1f, 0.8f, 0.2f);
    auto pointCd = geo.points().create("Cd", AttrType::Vec3).write<Vec3>();
    for (size_t i = 0; i < 13; ++i) pointCd[i] = Vec3(0.05f * static_cast<float>(i), 0.3f, 0.6f);
    geo.vertices().create("Cd", AttrType::Float).write<float>()[1] = 0.9f;  // one corner of point 1 its own
    auto V = geo.points().create("v", AttrType::Vec3).write<Vec3>();
    for (size_t i = 0; i < 13; ++i) V[i] = Vec3(0.0f, static_cast<float>(i), 1.0f);
    const auto shared = std::make_shared<const Geometry>(geo);
    DisplayMesher mesher;
    DisplayMesh mesh;
    mesher.make(shared, mesh);
    CHECK(drawsAsDisplayOf(mesh, geo));
    CHECK(mesher.hasRest());
    CHECK_EQ(mesh.triangleCount(), size_t(4));
    CHECK_EQ(mesh.velocities.size(), mesh.colors.size());
    CHECK_EQ(mesh.vertexCount(), size_t(7));  // six points, one with a corner of its own colour
    const DisplayGeometry all = displayOf(geo), rest = displayOf(geo, 400000, false);
    CHECK(rest.triangles.empty() && rest.velocities.empty());
    CHECK(sameFloats(rest.glass, all.glass) && sameFloats(rest.dots, all.dots) && sameFloats(rest.lines, all.lines));
    CHECK_EQ(rest.glassCount(), size_t(2));
    CHECK(rest.lo == all.lo && rest.hi == all.hi);
    // Nothing: nothing.
    CHECK(mesher.make(nullptr, mesh) == DisplayMesher::Made::Anew);
    CHECK(mesh.indices.empty() && mesh.places.empty() && !mesher.hasRest());
}

TEST(display_takes_the_corners_own_normals) {
    // Two quads folded 10 degrees along their edge -- less than the crease,
    // smooth across it without normals -- each corner with a normal of its
    // own, its face's: the edge sharp in the viewport and the renderers, a
    // vertex for each side at its points.
    const float s = std::sin(0.17453292f), c = std::cos(0.17453292f);
    Geometry geo;
    geo.addPoints(6);
    auto P = geo.positionsForWrite();
    P[0] = Vec3(0, 0, 0), P[1] = Vec3(1, 0, 0), P[2] = Vec3(1, 0, 1), P[3] = Vec3(0, 0, 1);
    P[4] = Vec3(1 + c, s, 0), P[5] = Vec3(1 + c, s, 1);
    const uint32_t a[4] = {0, 3, 2, 1}, b[4] = {1, 2, 5, 4};
    geo.addPrimitive(a, true);
    geo.addPrimitive(b, true);
    const auto smooth = std::make_shared<const Geometry>(geo);
    auto N = geo.vertices().create("N", AttrType::Vec3).write<Vec3>();
    for (size_t k = 0; k < 4; ++k) N[k] = Vec3(0, 1, 0);
    for (size_t k = 4; k < 8; ++k) N[k] = Vec3(-s, c, 0);
    const auto sharp = std::make_shared<const Geometry>(geo);
    DisplayMesher mesher;
    DisplayMesh mesh;
    CHECK(mesher.make(smooth, mesh) == DisplayMesher::Made::Anew);
    CHECK_EQ(mesh.vertexCount(), size_t(6));
    CHECK(mesher.make(sharp, mesh) == DisplayMesher::Made::Anew);
    CHECK(drawsAsDisplayOf(mesh, *sharp));
    CHECK_EQ(mesh.vertexCount(), size_t(8));
    for (size_t w = 0; w < mesh.vertexCount(); ++w) {
        const Vec3 n(mesh.places[w * 6 + 3], mesh.places[w * 6 + 4], mesh.places[w * 6 + 5]);
        CHECK(near(n, Vec3(0, 1, 0)) || near(n, Vec3(-s, c, 0)));
    }
    // Moved, the corners' normals the same: made again quickly.
    auto moved = std::make_shared<Geometry>(*sharp);
    for (Vec3& p : moved->positionsForWrite()) p.y += 0.5f;
    CHECK(mesher.make(moved, mesh) == DisplayMesher::Made::Moved);
    CHECK(drawsAsDisplayOf(mesh, *moved));
    // The renderers' triangles: each corner its face's.
    const ShadedTriangles tris = shadedTriangles(*sharp);
    CHECK_EQ(tris.count(), size_t(4));
    for (size_t t = 0; t < tris.count(); ++t) {
        const Vec3 want = tris.prims[t] == 0 ? Vec3(0, 1, 0) : Vec3(-s, c, 0);
        for (size_t k = 0; k < 3; ++k) CHECK(near(tris.normals[3 * t + k], want));
    }
}

TEST(display_mesh_is_made_again_quickly_when_only_the_points_move) {
    Network net;
    const int grid = net.add("grid");
    CHECK(net.setParam(grid, "rows", "31"));
    CHECK(net.setParam(grid, "cols", "31"));
    GeometryGraph g;
    g.sync(net);
    const GeometryPtr flat = g.cook(grid, 1);
    DisplayMesher mesher;
    DisplayMesh mesh;
    CHECK(mesher.make(flat, mesh) == DisplayMesher::Made::Anew);
    // Hills of the same grid: the places and normals made again, the rest
    // kept -- as displayOf draws them.
    const auto moved = [&](auto&& height) {
        auto geo = std::make_shared<Geometry>(*flat);
        for (Vec3& p : geo->positionsForWrite()) p.y = height(p.x, p.z);
        return geo;
    };
    const auto hills = moved([](float x, float z) { return 0.1f * std::sin(6.0f * x) * std::cos(4.0f * z); });
    const std::vector<uint32_t> indices = mesh.indices;
    CHECK(mesher.make(hills, mesh) == DisplayMesher::Made::Moved);
    CHECK(mesh.indices == indices);
    CHECK(drawsAsDisplayOf(mesh, *hills));
    // A fold sharper than the crease parts the corners along it: made anew.
    const auto fold = moved([](float x, float) { return x > 0.0f ? 2.0f * x : 0.0f; });
    CHECK(mesher.make(fold, mesh) == DisplayMesher::Made::Anew);
    CHECK(drawsAsDisplayOf(mesh, *fold));
    CHECK(mesh.vertexCount() > flat->pointCount());
    // Smooth again: the corners parted may stay so -- they draw the same.
    CHECK(mesher.make(hills, mesh) == DisplayMesher::Made::Moved);
    CHECK(drawsAsDisplayOf(mesh, *hills));
    // New colours, or point normals where there were none: anew.
    auto painted = std::make_shared<Geometry>(*hills);
    auto Cd = painted->points().create("Cd", AttrType::Vec3).write<Vec3>();
    for (size_t i = 0; i < Cd.size(); ++i) Cd[i] = Vec3(static_cast<float>(i % 7) / 7.0f, 0.5f, 0.2f);
    CHECK(mesher.make(painted, mesh) == DisplayMesher::Made::Anew);
    CHECK(drawsAsDisplayOf(mesh, *painted));
    auto normals = std::make_shared<Geometry>(*painted);
    auto N = normals->points().create("N", AttrType::Vec3).write<Vec3>();
    for (size_t i = 0; i < N.size(); ++i) N[i] = normalize(Vec3(0.1f * static_cast<float>(i % 5), 1.0f, 0.0f));
    CHECK(mesher.make(normals, mesh) == DisplayMesher::Made::Anew);
    CHECK_EQ(mesh.vertexCount(), flat->pointCount());
    // ... and those moved with the points, as a node finds them again.
    auto again = std::make_shared<Geometry>(*normals);
    auto Q = again->positionsForWrite();
    auto M = again->points().find("N")->write<Vec3>();
    for (size_t i = 0; i < Q.size(); ++i) {
        Q[i].y += 0.05f;
        M[i] = normalize(Vec3(0.0f, 1.0f, 0.1f * static_cast<float>(i % 3)));
    }
    CHECK(mesher.make(again, mesh) == DisplayMesher::Made::Moved);
    CHECK(drawsAsDisplayOf(mesh, *again));
    CHECK(near(mesh.lo, displayOf(*again).lo) && near(mesh.hi, displayOf(*again).hi));
}

TEST(sim_geometry_examples_of_geometry_alone_cook) {
    // The examples of geometry alone -- models, without an Output, and
    // stills, whose Output has no solver to simulate: what they display
    // cooks, without an error, into something.
    int models = 0;
    for (const std::string& name : Network::exampleNames()) {
        Network net;
        std::string error;
        CHECK(Network::load(Network::exampleText(name), net, error));
        auto any = [&](auto&& is) { return std::any_of(net.nodes().begin(), net.nodes().end(), is); };
        const bool output = any([](const sim::Node& n) { return n.type == "output"; });
        const bool solver = any([](const sim::Node& n) {
            const sim::NodeType* t = sim::findNodeType(n.type);
            return t && std::string_view(t->category) == "Simulation";
        });
        if (output && solver) continue;
        ++models;
        GeometryGraph geo;
        geo.sync(net, PG_SIM_EXAMPLES_DIR);
        const GeometryPtr g = geo.cook(net.displayed(), 1);
        CHECK(net.displayed() != 0 && g && g->primitiveCount() > 0);
        for (const sim::Node& n : net.nodes()) {
            if (!geo.error(n.id).empty()) ::testing::fail(__FILE__, __LINE__, name + ": " + n.name + ": " + geo.error(n.id));
        }
    }
    CHECK(models >= 1);
}
