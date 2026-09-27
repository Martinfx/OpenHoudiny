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
#include "pg/sim/World.h"

#include "test_framework.h"

#include <cmath>
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
    // The gas: three volumes on the solver's grid.
    geo = g.cook(gas, 4);
    CHECK(geo && geo->volumeCount() == 3);
    const Volume* density = geo->findVolume("density");
    const Volume* flame = geo->findVolume("flame");
    CHECK(density && flame && geo->findVolume("temperature"));
    CHECK_EQ(density->res[0], frames[4]->domain.cells[0]);
    CHECK_EQ(density->count(), frames[4]->domain.cellCount());
    float sum = 0.0f;
    for (const float v : *density->values) sum += v;
    CHECK(sum > 0.0f);

    // From a solver that does not reach the Output: said, and not kept.
    const int other = net.add("liquid_solver");
    net.disconnect(net.linksInto(points, "liquid")[0]);
    CHECK(net.connect(other, "liquid", points, "liquid"));
    c = net.compile();
    CHECK(mentions(c, points, "not simulated"));
    CHECK(!c.world.keepParticles);
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

TEST(sim_geometry_examples_of_geometry_alone_cook) {
    // The examples without an Output -- models: what they display cooks,
    // without an error, into something.
    int models = 0;
    for (const std::string& name : Network::exampleNames()) {
        Network net;
        std::string error;
        CHECK(Network::load(Network::exampleText(name), net, error));
        if (std::any_of(net.nodes().begin(), net.nodes().end(), [](const sim::Node& n) { return n.type == "output"; })) continue;
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
