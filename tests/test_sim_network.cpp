//
// Simulations built from nodes (src/pg/sim/Network.h): the table of node
// types, editing, the .pgsim files, and what a network compiles to.
//
#include "pg/sim/Network.h"
#include "pg/sim/Pyro.h"

#include "test_framework.h"

#include <filesystem>
#include <fstream>
#include <set>
#include <string>

using namespace pg;
using namespace pg::sim;

namespace {

/// The scene with the node ids taken out: what code builds has none.
Scene withoutNodes(Scene s) {
    for (Emitter& e : s.emitters) e.node = 0;
    for (Force& f : s.forces) f.node = 0;
    for (Collider& c : s.colliders) c.node = 0;
    return s;
}

bool mentions(const Compiled& c, int node, const std::string& words) {
    for (const Problem& p : c.problems) {
        if (p.node == node && p.message.find(words) != std::string::npos) return true;
    }
    return false;
}

/// Source -> solver -> look -> output; the ids in `ids`.
Network chain(int ids[4]) {
    Network net;
    ids[0] = net.add("pyro_source");
    ids[1] = net.add("pyro_solver", 300, 0);
    ids[2] = net.add("volume_look", 560, 0);
    ids[3] = net.add("output", 800, 0);
    net.setParam(ids[0], "smoke", "4");
    CHECK(net.connect(ids[0], "source", ids[1], "sources"));
    CHECK(net.connect(ids[1], "gas", ids[2], "gas"));
    CHECK(net.connect(ids[2], "look", ids[3], "look"));
    return net;
}

}  // namespace

TEST(sim_network_node_types_are_consistent) {
    std::set<std::string> names;
    for (const NodeType& t : nodeTypes()) {
        CHECK(names.insert(t.name).second);
        bool known = false;
        for (const char* c : nodeCategories()) known = known || std::string(c) == t.category;
        CHECK(known);
        std::set<std::string> params, pins;
        for (const ParamDef& p : t.params) {
            CHECK(params.insert(p.name).second);
            CHECK(p.min <= p.max);
            CHECK(p.lo <= p.hi);
            const int n = p.kind == ParamKind::Vector || p.kind == ParamKind::Color ? 3 : 1;
            for (int i = 0; i < n; ++i) {
                CHECK(p.value[static_cast<size_t>(i)] >= p.lo && p.value[static_cast<size_t>(i)] <= p.hi);
            }
            if (p.kind == ParamKind::Choice) {
                CHECK(!p.choices.empty());
                CHECK_EQ(p.choices.size(), p.choiceLabels.size());
            }
            CHECK(std::string(p.help).size() > 5);
            // What a file writes, it reads back.
            ParamValue back;
            std::string error;
            CHECK(parseParam(p, formatParam(p, p.value), back, error));
            CHECK(back == p.value);
        }
        for (const PinDef& p : t.inputs) CHECK(pins.insert(std::string("in ") + p.name).second);
        for (const PinDef& p : t.outputs) CHECK(pins.insert(std::string("out ") + p.name).second);
    }
    CHECK(findNodeType("pyro_solver") != nullptr);
    CHECK(findNodeType("no_such_node") == nullptr);
}

TEST(sim_network_names_nodes_and_checks_links) {
    Network net;
    const int a = net.add("turbulence");
    const int b = net.add("turbulence");
    CHECK_EQ(net.node(a)->name, std::string("turbulence1"));
    CHECK_EQ(net.node(b)->name, std::string("turbulence2"));
    CHECK_EQ(net.add("no_such_node"), 0);

    std::string error;
    CHECK(!net.rename(a, "2fast", &error));
    CHECK(!net.rename(a, "turbulence2", &error));
    CHECK(net.rename(a, "whirls", &error));
    CHECK(net.named("whirls") != nullptr);

    const int solver = net.add("pyro_solver");
    const int look = net.add("volume_look");
    // A force goes into Forces, not into Sources, and not into a look.
    CHECK(!net.connect(a, "force", solver, "sources", &error));
    CHECK(error.find("force") != std::string::npos);
    CHECK(!net.connect(a, "force", look, "gas", &error));
    CHECK(!net.connect(a, "nothing", solver, "forces", &error));
    // Forces takes any number, in order; Gas takes one, the last.
    CHECK(net.connect(a, "force", solver, "forces"));
    CHECK(net.connect(b, "force", solver, "forces"));
    CHECK(!net.connect(b, "force", solver, "forces", &error));  // already there
    const std::vector<Link> forces = net.linksInto(solver, "forces");
    CHECK_EQ(forces.size(), size_t(2));
    CHECK_EQ(forces[0].from, a);
    const int other = net.add("pyro_solver");
    CHECK(net.connect(solver, "gas", look, "gas"));
    CHECK(net.connect(other, "gas", look, "gas"));
    const std::vector<Link> gas = net.linksInto(look, "gas");
    CHECK_EQ(gas.size(), size_t(1));
    CHECK_EQ(gas[0].from, other);

    // Removing a node takes its links with it.
    CHECK(net.remove(a));
    CHECK_EQ(net.linksInto(solver, "forces").size(), size_t(1));
    CHECK(!net.remove(a));
}

TEST(sim_network_parameters_keep_their_limits) {
    Network net;
    const int s = net.add("pyro_source");
    const uint64_t before = net.revision();
    std::string error;
    CHECK(net.setParam(s, "center", "0.5 1 -0.25", &error));
    CHECK(net.param(s, "center") == (ParamValue{0.5f, 1.0f, -0.25f}));
    CHECK(net.revision() > before);
    CHECK(net.setParam(s, "motion", "circle", &error));
    CHECK_EQ(net.value(s, "motion"), 1.0f);
    CHECK(!net.setParam(s, "motion", "zigzag", &error));
    CHECK(error.find("sway") != std::string::npos);
    CHECK(!net.setParam(s, "fuel", "lots", &error));
    CHECK(!net.setParam(s, "no_such_param", "1", &error));

    // Below the limit: kept at it. Past the slider: fine.
    CHECK(net.setParam(s, "fuel", "-3"));
    CHECK_EQ(net.value(s, "fuel"), 0.0f);
    CHECK(net.setParam(s, "fuel", "400"));
    CHECK_EQ(net.value(s, "fuel"), 400.0f);
    CHECK(net.setParam(s, "flicker", "7"));
    CHECK_EQ(net.value(s, "flicker"), 2.0f);
    // A whole number is whole; one number sets all three of a vector.
    CHECK(net.setParam(s, "seed", "3.7"));
    CHECK_EQ(net.value(s, "seed"), 4.0f);
    CHECK(net.setParam(s, "velocity", "2"));
    CHECK(net.param(s, "velocity") == (ParamValue{2.0f, 2.0f, 2.0f}));

    // Numbers as people write them; the same with every standard library.
    CHECK(net.setParam(s, "fuel", "+2.5"));
    CHECK_EQ(net.value(s, "fuel"), 2.5f);
    CHECK(net.setParam(s, "fuel", ".5"));
    CHECK_EQ(net.value(s, "fuel"), 0.5f);
    CHECK(net.setParam(s, "fuel", "1e1"));
    CHECK_EQ(net.value(s, "fuel"), 10.0f);
    for (const char* bad : {"", "1.5x", "0x10", "nan", "inf", "1e999", "- 1", "1..2"})
        CHECK(!net.setParam(s, "fuel", bad, &error));
    CHECK_EQ(net.value(s, "fuel"), 10.0f);

    // Only what differs from the default is stored.
    CHECK(!net.isDefault(s, "fuel"));
    CHECK(net.setParam(s, "fuel", "0"));
    CHECK(net.isDefault(s, "fuel"));
    CHECK(net.resetParam(s, "velocity"));
    CHECK(net.param(s, "velocity") == (ParamValue{0.0f, 0.5f, 0.0f}));

    // Moving a node is no edit.
    const uint64_t r = net.revision();
    net.node(s)->x = 120.0f;
    CHECK_EQ(net.revision(), r);
}

TEST(sim_network_files_read_back_the_same) {
    int ids[4];
    Network net = chain(ids);
    const int wind = net.add("wind", 0, 300);
    net.setParam(wind, "direction", "0 0 1");
    net.setParam(wind, "gusts", "0.5");
    net.connect(wind, "force", ids[1], "forces");
    net.setBypass(wind, true);
    net.setParam(ids[0], "motion", "sway");
    net.setParam(ids[2], "floor", "off");
    net.rename(ids[0], "vent");

    const std::string text = net.save();
    CHECK(text.rfind("pgsim 1\n", 0) == 0);
    CHECK(text.find("node 1 pyro_source 2 vent 0 0\n") != std::string::npos);
    CHECK(text.find("  param motion sway\n") != std::string::npos);
    CHECK(text.find("  param floor off\n") != std::string::npos);
    CHECK(text.find("  bypass\n") != std::string::npos);
    CHECK(text.find("link 1.source -> 2.sources\n") != std::string::npos);
    // Defaults are not written.
    CHECK(text.find("param radius") == std::string::npos);

    Network back;
    std::string error;
    std::vector<std::string> warnings;
    CHECK(Network::load(text, back, error, &warnings));
    CHECK(warnings.empty());
    CHECK_EQ(back.save(), text);
    CHECK(back.compile().scene == net.compile().scene);
    CHECK(back.compile().look == net.compile().look);
    // New nodes do not take the ids of the loaded ones.
    CHECK(back.add("drag") > wind);
}

TEST(sim_network_load_keeps_what_it_can) {
    const char* text =
        "pgsim 1\n"
        "# a comment\n"
        "node 1 sphere_source 1 fire 0 0\n"
        "  param fuel 10\n"
        "  param sparkle 3          # no such parameter\n"
        "  param motion hop         # no such choice\n"
        "node 2 plasma_field 3 zap 0 200\n"
        "  param power 9\n"
        "node 3 pyro_solver 1 solver 300 0\n"
        "node 4 pyro_solver 1 solver 300 300\n"
        "link 1.source -> 3.sources\n"
        "link 1.source -> 3.forces\n"
        "link 2.field -> 3.forces\n"
        "link 9.source -> 3.sources\n";
    Network net;
    std::string error;
    std::vector<std::string> warnings;
    CHECK(Network::load(text, net, error, &warnings));
    CHECK_EQ(net.nodes().size(), size_t(4));
    CHECK_EQ(net.value(1, "fuel"), 10.0f);
    CHECK_EQ(net.value(1, "motion"), 0.0f);
    CHECK_EQ(net.node(4)->name, std::string("solver1"));  // two were called solver
    // sparkle, hop, the second solver's name, the source into forces, node 9
    CHECK_EQ(warnings.size(), size_t(5));
    // The unknown node and its link survive a save, for a program that knows them.
    const std::string again = net.save();
    CHECK(again.find("node 2 plasma_field 3 zap 0 200\n  param power 9 0 0\n") != std::string::npos);
    CHECK(again.find("link 2.field -> 3.forces") != std::string::npos);
    CHECK(mentions(net.compile(), 2, "Unknown node type"));

    Network untouched = net;
    CHECK(!Network::load("pgsim 99\n", untouched, error));
    CHECK(error.find("newer") != std::string::npos);
    CHECK(!Network::load("pgshadergraph 1\n", untouched, error));
    CHECK(!Network::load("pgsim 1\nnode 1 wind 1 w 0\n", untouched, error));
    CHECK(error.find("line 2") != std::string::npos);
    CHECK(!Network::load("pgsim 1\nnode 1 wind 1 w 0 0\nnode 1 drag 1 d 0 0\n", untouched, error));
    CHECK(!Network::load("pgsim 1\n  param fuel 1\n", untouched, error));
    CHECK(!Network::load("", untouched, error));
    CHECK_EQ(untouched.save(), net.save());  // a failed load leaves it as it was
}

TEST(sim_network_compiles_to_the_scene_and_the_look) {
    int ids[4];
    Network net = chain(ids);
    net.setParam(ids[0], "center", "0.1 0.2 0.3");
    net.setParam(ids[0], "motion", "circle");
    net.setParam(ids[1], "fps", "24");
    net.setParam(ids[1], "resolution", "40");
    net.setParam(ids[1], "closed_floor", "off");
    net.setParam(ids[2], "light_elevation", "90");
    net.setParam(ids[3], "frames", "48");
    const int box = net.add("object");
    net.setParam(box, "shape", "box");
    net.setParam(box, "center", "0 0.6 0");
    net.connect(box, "collider", ids[1], "colliders");
    const int vortex = net.add("vortex");
    net.setParam(vortex, "lift", "0.7");
    net.setParam(vortex, "mask", "smoke");
    net.connect(vortex, "force", ids[1], "forces");
    const int drag = net.add("drag");
    net.connect(drag, "force", ids[1], "forces");

    Compiled c = net.compile();
    CHECK(c.ok);
    CHECK(!c.errors());
    CHECK_EQ(c.frames, 48);
    CHECK_EQ(c.solver, ids[1]);
    const Scene& s = c.scene;
    CHECK_EQ(s.solver.resolution, 40);
    CHECK(!s.solver.closedFloor);
    CHECK(std::fabs(s.solver.timeStep - 1.0f / 24.0f) < 1e-7f);
    CHECK_EQ(s.emitters.size(), size_t(1));
    CHECK(s.emitters[0].center == Vec3(0.1f, 0.2f, 0.3f));
    CHECK(s.emitters[0].motion == Motion::Circle);
    CHECK_EQ(s.emitters[0].smoke, 4.0f);
    CHECK_EQ(s.emitters[0].node, ids[0]);
    CHECK_EQ(s.forces.size(), size_t(2));
    CHECK(s.forces[0].kind == ForceKind::Vortex);  // in the order they were linked
    CHECK(s.forces[0].mask == Mask::Smoke);
    CHECK_EQ(s.forces[0].lift, 0.7f);
    CHECK(s.forces[1].kind == ForceKind::Drag);
    CHECK_EQ(s.colliders.size(), size_t(1));
    CHECK(s.colliders[0].shape == Shape::Box);
    CHECK(std::fabs(c.look.lightDirection().y - 1.0f) < 1e-6f);
    CHECK(c.isActive(vortex) && c.isActive(box) && c.isActive(ids[3]));

    // Bypassed: out of the scene, and dimmed -- a node that feeds nothing too.
    net.setBypass(vortex, true);
    const int loose = net.add("turbulence");
    c = net.compile();
    CHECK_EQ(c.scene.forces.size(), size_t(1));
    CHECK(!c.isActive(vortex));
    CHECK(!c.isActive(loose));
}

TEST(sim_network_says_what_is_missing) {
    Network net;
    Compiled c = net.compile();
    CHECK(!c.ok);
    CHECK(mentions(c, 0, "No Output"));

    int ids[4];
    net = chain(ids);
    CHECK(net.compile().ok);
    CHECK(net.compile().problems.empty());

    net.disconnect(net.linksInto(ids[3], "look")[0]);
    c = net.compile();
    CHECK(!c.ok);
    CHECK(mentions(c, ids[3], "Volume Look"));

    net.connect(ids[2], "look", ids[3], "look");
    net.disconnect(net.linksInto(ids[2], "gas")[0]);
    c = net.compile();
    CHECK(!c.ok);
    CHECK(mentions(c, ids[2], "Pyro Solver"));

    net.connect(ids[1], "gas", ids[2], "gas");
    net.setParam(ids[0], "center", "5 0 0");
    net.setParam(ids[0], "smoke", "0");
    c = net.compile();
    CHECK(c.ok);
    CHECK(!c.errors());
    CHECK(mentions(c, ids[0], "Outside"));
    CHECK(mentions(c, ids[0], "Adds nothing"));

    net.setBypass(ids[0], true);
    CHECK(mentions(net.compile(), ids[1], "No sources"));

    net.setBypass(ids[0], false);
    net.setParam(ids[0], "center", "0 0.6 0");
    const int ball = net.add("object");
    net.setParam(ball, "center", "0 0.6 0");
    net.connect(ball, "collider", ids[1], "colliders");
    CHECK(mentions(net.compile(), ids[0], "Inside a collider"));

    const int second = net.add("output");
    CHECK(mentions(net.compile(), second, "Another Output"));
}

TEST(sim_network_examples_match_the_presets) {
    // The campfire and the column of smoke the solver's tests use are the
    // networks the program comes with.
    Network fire, smoke;
    CHECK(Network::example("campfire", fire));
    CHECK(Network::example("smoke", smoke));
    CHECK(withoutNodes(fire.compile().scene) == Scene::fire());
    CHECK(withoutNodes(smoke.compile().scene) == Scene::smoke());
}

TEST(sim_network_examples_all_run) {
    CHECK(Network::exampleNames().size() >= 2);
    for (const std::string& name : Network::exampleNames()) {
        Network net;
        std::string error;
        std::vector<std::string> warnings;
        CHECK(Network::load(Network::exampleText(name), net, error, &warnings));
        if (!warnings.empty()) ::testing::fail(__FILE__, __LINE__, name + ": " + warnings[0]);
        const Compiled c = net.compile(PG_SIM_EXAMPLES_DIR);  // where their meshes are
        CHECK(c.ok);
        for (const Problem& p : c.problems) ::testing::fail(__FILE__, __LINE__, name + ": " + p.message);
        Scene scene = c.scene;
        scene.solver.resolution = 16;
        PyroSolver sim(scene);
        for (int f = 0; f < 3; ++f) sim.step();
        CHECK(std::isfinite(sim.density().sum()));
    }
}

TEST(sim_network_files_of_earlier_versions_load_upgraded) {
    // Sphere and box sources and colliders, as version 1 wrote them: they
    // come in as Pyro Sources and Objects, where they were, as big.
    const char* text =
        "pgsim 1\n"
        "node 1 sphere_source 1 fire 0 0\n"
        "  param radius 0.15\n"
        "  param fuel 12\n"
        "  param motion circle\n"
        "node 2 box_source 1 logs 0 100\n"  // the box's size was 0.2 0.1 0.2 by default
        "  param smoke 3\n"
        "node 3 sphere_collider 1 ball 0 200\n"  // by default at 0 0.6 0, 0.15 in radius
        "node 4 box_collider 1 wall 0 300\n"
        "  param center 0.4 0.5 0\n"
        "  param size 0.1 1 0.8\n"
        "  bypass\n"
        "node 5 pyro_solver 1 solver 300 0\n"
        "node 6 volume_look 1 look 500 0\n"
        "node 7 output 1 output 700 0\n"
        "link 1.source -> 5.sources\n"
        "link 2.source -> 5.sources\n"
        "link 3.collider -> 5.colliders\n"
        "link 4.collider -> 5.colliders\n"
        "link 5.gas -> 6.gas\n"
        "link 6.look -> 7.look\n";
    Network net;
    std::string error;
    std::vector<std::string> warnings;
    CHECK(Network::load(text, net, error, &warnings));
    CHECK(warnings.empty());
    CHECK_EQ(net.node(1)->type, std::string("pyro_source"));
    CHECK_EQ(net.node(1)->version, findNodeType("pyro_source")->version);
    CHECK(net.param(1, "size") == (ParamValue{0.3f, 0.3f, 0.3f}));
    CHECK_EQ(net.value(1, "shape"), 0.0f);
    CHECK_EQ(net.value(1, "fuel"), 12.0f);
    CHECK_EQ(net.value(1, "motion"), 1.0f);
    CHECK_EQ(net.node(2)->type, std::string("pyro_source"));
    CHECK_EQ(net.value(2, "shape"), 1.0f);
    CHECK(net.param(2, "size") == (ParamValue{0.2f, 0.1f, 0.2f}));
    CHECK_EQ(net.node(3)->type, std::string("object"));
    CHECK(net.param(3, "center") == (ParamValue{0.0f, 0.6f, 0.0f}));
    CHECK(net.isDefault(3, "size"));  // 0.3 across, as a new object
    CHECK_EQ(net.value(4, "shape"), 1.0f);
    CHECK(net.param(4, "size") == (ParamValue{0.1f, 1.0f, 0.8f}));
    CHECK(net.node(4)->bypass);
    CHECK_EQ(net.linksInto(5, "sources").size(), size_t(2));
    CHECK_EQ(net.linksInto(5, "colliders").size(), size_t(2));

    const Compiled c = net.compile();
    CHECK(c.ok);
    CHECK_EQ(c.scene.emitters.size(), size_t(2));
    CHECK(c.scene.emitters[0].size == Vec3(0.3f));
    CHECK(c.scene.emitters[1].shape == Shape::Box);
    CHECK_EQ(c.scene.colliders.size(), size_t(1));  // the wall is bypassed
    CHECK(c.scene.colliders[0].center == Vec3(0.0f, 0.6f, 0.0f));
    CHECK_EQ(c.solids.size(), size_t(1));

    // Saved, it is a file of the new types -- which reads back the same.
    const std::string saved = net.save();
    CHECK(saved.find("node 1 pyro_source 2 fire 0 0\n") != std::string::npos);
    CHECK(saved.find("node 3 object 1 ball 0 200\n  param center 0 0.6 0\n") != std::string::npos);
    CHECK(saved.find("sphere_") == std::string::npos && saved.find("box_") == std::string::npos);
    Network again;
    CHECK(Network::load(saved, again, error, &warnings));
    CHECK_EQ(again.save(), saved);
}

TEST(sim_network_objects_are_in_the_scene_linked_or_not) {
    int ids[4];
    Network net = chain(ids);
    const int rock = net.add("object", 0, 300);
    net.setParam(rock, "shape", "cone");
    net.setParam(rock, "rotation", "0 45 10");
    net.setParam(rock, "color", "0.6 0.3 0.2");
    Compiled c = net.compile();
    // Drawn, and not dimmed, though nothing collides with it.
    CHECK_EQ(c.solids.size(), size_t(1));
    CHECK(c.solids[0].body.shape == Shape::Cone);
    CHECK(c.solids[0].body.rotation == Vec3(0.0f, 45.0f, 10.0f));
    CHECK(c.solids[0].color == Vec3(0.6f, 0.3f, 0.2f));
    CHECK_EQ(c.solids[0].body.node, rock);
    CHECK(c.scene.colliders.empty());
    CHECK(c.isActive(rock));

    // Linked into the solver's Colliders: the gas goes round it.
    CHECK(net.connect(rock, "collider", ids[1], "colliders"));
    c = net.compile();
    CHECK_EQ(c.scene.colliders.size(), size_t(1));
    CHECK(c.scene.colliders[0] == c.solids[0].body);

    // A new colour is no new scene: nothing is simulated again.
    const Scene before = c.scene;
    net.setParam(rock, "color", "0.1 0.5 0.9");
    CHECK(net.compile().scene == before);
    CHECK(!(net.compile().solids == c.solids));

    // Bypassed: gone from the scene and the picture.
    net.setBypass(rock, true);
    c = net.compile();
    CHECK(c.solids.empty() && c.scene.colliders.empty() && !c.isActive(rock));

    // A turned source turns its jet.
    net.setParam(ids[0], "rotation", "0 0 90");
    net.setParam(ids[0], "velocity", "0 1 0");
    const Emitter e = net.compile().scene.emitters[0];
    const Vec3 jet = e.shapeAt(0.0f).turn().apply(e.velocity);
    CHECK(std::fabs(jet.x + 1.0f) < 1e-6f && std::fabs(jet.y) < 1e-6f);
}

TEST(sim_network_examples_are_in_the_current_format) {
    // An example is what saving it writes, comments aside: the files show
    // the format of this version, parameters in the order of their tables.
    for (const std::string& name : Network::exampleNames()) {
        const std::string text = Network::exampleText(name);
        std::string bare;
        size_t pos = 0;
        while (pos < text.size()) {
            const size_t end = std::min(text.find('\n', pos), text.size());
            const std::string line = text.substr(pos, end - pos);
            if (line.rfind("#", 0) != 0) bare += line + "\n";
            pos = end + 1;
        }
        Network net;
        std::string error;
        CHECK(Network::load(text, net, error));
        if (net.save() != bare) ::testing::fail(__FILE__, __LINE__, name + " is not as saving writes it:\n" + net.save());
    }
}

TEST(sim_network_objects_can_be_meshes_from_files) {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "pg_test_network_mesh";
    fs::create_directories(dir / "models dir");
    {
        std::ofstream out(dir / "models dir" / "tetra.obj", std::ios::binary);
        out << "v 0 0 0\nv 1 0 0\nv 0 1 0\nv 0 0 1\nf 1 3 2\nf 1 2 4\nf 1 4 3\nf 2 3 4\n";
    }
    int ids[4];
    Network net = chain(ids);
    const int rock = net.add("object");
    CHECK(net.setParam(rock, "shape", "mesh"));
    // A path with a space, set as the command line would: in quotes.
    std::string error;
    CHECK(net.setParam(rock, "file", "\"models dir/tetra.obj\"", &error));
    CHECK_EQ(net.text(rock, "file"), std::string("models dir/tetra.obj"));
    CHECK(!net.isDefault(rock, "file"));
    CHECK(net.connect(rock, "collider", ids[1], "colliders"));

    // Read from the network's folder.
    Compiled c = net.compile(dir.string());
    CHECK_EQ(c.solids.size(), size_t(1));
    CHECK(c.solids[0].body.shape == Shape::Mesh && c.solids[0].body.mesh);
    CHECK_EQ(c.solids[0].body.mesh->mesh().triangles.size(), size_t(4));
    CHECK(c.scene.colliders[0].mesh == c.solids[0].body.mesh);  // one mesh, read once
    CHECK(!mentions(c, rock, "mesh"));
    // From elsewhere the file is not there: said, and its box stands in.
    c = net.compile((dir / "elsewhere").string());
    CHECK(mentions(c, rock, "cannot be read"));
    CHECK(!c.solids[0].body.mesh && c.solids[0].body.instance().shape() == Shape::Box);

    // Saved in quotes, read back -- a '#' in a path is no comment.
    net.setText(rock, "file", "odd \"name\" #1.obj");
    const std::string text = net.save();
    CHECK(text.find("  param file \"odd \\\"name\\\" #1.obj\"\n") != std::string::npos);
    Network back;
    std::vector<std::string> warnings;
    CHECK(Network::load(text + "# the end\n", back, error, &warnings));
    CHECK(warnings.empty());
    CHECK_EQ(back.text(rock, "file"), std::string("odd \"name\" #1.obj"));
    CHECK_EQ(back.save(), text);
    // An empty path is the default, and a mesh without one is said so.
    CHECK(net.resetParam(rock, "file"));
    CHECK(net.isDefault(rock, "file") && net.text(rock, "file").empty());
    CHECK(mentions(net.compile(), rock, "choose its OBJ file"));
    CHECK(!net.setParam(rock, "file", "\"not closed", &error));
    fs::remove_all(dir);
}
