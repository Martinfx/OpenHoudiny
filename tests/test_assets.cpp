//
// Digital assets (pg/sim/Asset.h): a network packed into a node of its own,
// with the parameters it shows, used in another network and following its
// definition.
//
#include "pg/core/Parallel.h"
#include "pg/sim/Asset.h"
#include "pg/sim/GeometryGraph.h"
#include "pg/sim/Network.h"

#include "test_framework.h"

#include <cmath>
#include <map>
#include <cstdio>
#include <cstdlib>

using pg::GeometryPtr;
using pg::Vec3;
using namespace pg::sim;

namespace {

/// An asset `name`: what comes in, lifted by its "height" -- a wrangle's
/// ch("lift") promoted -- and a grid of "width" beside it, merged.
Network lifter(const std::string& name, int version, const std::string& extra = "") {
    Network def;
    const int in = def.add("asset_input");
    const int lift = def.add("point_wrangle");
    const int grid = def.add("grid");
    const int merged = def.add("merge");
    std::string error;
    def.connect(in, "geometry", lift, "geometry", &error);
    def.connect(lift, "geometry", merged, "geometry", &error);
    def.connect(grid, "geometry", merged, "geometry", &error);
    def.setText(lift, "snippet", "@P.y += ch(\"lift\");" + extra);
    def.setParam(lift, "lift", ParamValue{0.25f, 0.0f, 0.0f});
    def.setParam(grid, "rows", ParamValue{2.0f, 0.0f, 0.0f});
    def.setParam(grid, "cols", ParamValue{2.0f, 0.0f, 0.0f});
    def.setDisplay(merged);
    AssetInfo info;
    info.name = name;
    info.label = "Lifter";
    info.version = version;
    def.setAsset(info);
    def.promote(lift, "lift", true);
    def.promote(grid, "sizex", true);
    return def;
}

float highest(const GeometryPtr& g) {
    float y = -1e9f;
    for (const Vec3& p : g->positions()) y = std::max(y, p.y);
    return y;
}

}  // namespace

TEST(asset_an_instance_cooks_its_definition_with_its_parameters_and_inputs) {
    std::string error;
    CHECK(AssetLibrary::instance().add(lifter("lifter_a", 1), "", error));
    const NodeType* t = findNodeType("lifter_a");
    CHECK(t != nullptr);
    CHECK_EQ(std::string(t->category), std::string("Assets"));
    CHECK(t->param("lift") != nullptr);
    CHECK_EQ(t->param("lift")->value[0], 0.25f);  // the definition's value is the default
    CHECK(t->param("sizex") != nullptr);
    CHECK_EQ(t->inputs.size(), 1u);

    Network scene;
    const int box = scene.add("box");
    const int inst = scene.add("lifter_a");
    CHECK(inst != 0);
    CHECK(scene.connect(box, "geometry", inst, "geometry", &error));
    GeometryGraph geo;
    geo.sync(scene);
    GeometryPtr out = geo.cook(inst, 1);
    CHECK(geo.error(inst).empty());
    CHECK_EQ(out->primitiveCount(), 6u + 1u);  // the box and the grid's one quad
    CHECK_NEAR(highest(out), 1.25, 1e-5);

    CHECK(scene.setParam(inst, "lift", ParamValue{2.0f, 0.0f, 0.0f}));
    geo.sync(scene);
    CHECK_NEAR(highest(geo.cook(inst, 1)), 3.0, 1e-5);

    // A new definition: every instance follows it.
    CHECK(AssetLibrary::instance().add(lifter("lifter_a", 2, " @P.y += 10;"), "", error));
    geo.sync(scene);
    CHECK_NEAR(highest(geo.cook(inst, 1)), 13.0, 1e-5);
}

TEST(asset_definitions_go_to_files_and_carry_along) {
    std::string error;
    Network def = lifter("lifter_b", 3);
    const std::string text = def.save();
    CHECK(text.find("asset lifter_b 3 \"Lifter\"") != std::string::npos);
    CHECK(text.find("promote point_wrangle1 lift lift") != std::string::npos);
    Network back;
    CHECK(Network::load(text, back, error));
    CHECK_EQ(back.asset(), def.asset());

    // Renamed or removed inside, the promotion follows.
    const int wrangle = back.named("point_wrangle1")->id;
    CHECK(back.rename(wrangle, "raise"));
    CHECK_EQ(back.asset().promoted.front().node, std::string("raise"));
    CHECK(back.promotion(wrangle, "lift") != nullptr);
    CHECK(back.remove(wrangle));
    CHECK_EQ(back.asset().promoted.size(), 1u);

    // A network that uses an asset carries its definition.
    CHECK(AssetLibrary::instance().add(def, "", error));
    Network scene;
    const int inst = scene.add("lifter_b");
    CHECK(scene.setParam(inst, "lift", ParamValue{0.5f, 0.0f, 0.0f}));
    const std::string file = scene.save();
    CHECK(file.find("definition lifter_b\n| pgsim 1\n| asset lifter_b 3") != std::string::npos);
    Network again;
    std::vector<std::string> warnings;
    CHECK(Network::load(file, again, error, &warnings));
    CHECK(warnings.empty());
    CHECK_EQ(again.param(inst, "lift")[0], 0.5f);
    CHECK_EQ(again.save(), file);

    // What a file carries replaces an older definition, not a newer one.
    std::string older = file;
    const size_t at = older.find("| asset lifter_b 3");
    older.replace(at, 18, "| asset lifter_b 1");
    CHECK(Network::load(older, again, error));
    CHECK_EQ(AssetLibrary::instance().find("lifter_b")->version, 3);
    std::string newer = file;
    newer.replace(newer.find("| asset lifter_b 3"), 18, "| asset lifter_b 7");
    CHECK(Network::load(newer, again, error));
    CHECK_EQ(AssetLibrary::instance().find("lifter_b")->version, 7);
}

TEST(asset_errors_are_said) {
    std::string error;
    Network none = lifter("lifter_c", 1);
    none.setDisplay(0);
    CHECK(!AssetLibrary::instance().add(none, "", error));
    CHECK(error.find("display") != std::string::npos);
    CHECK(!AssetLibrary::instance().add(lifter("box", 1), "", error));
    CHECK(error.find("box") != std::string::npos);
    Network nameless = lifter("", 1);
    CHECK(!AssetLibrary::instance().add(nameless, "", error));

    // An asset with itself inside, or inside an asset it holds: refused, and
    // the definition before stays.
    Network selfish;
    const int grid = selfish.add("grid");
    selfish.setDisplay(grid);
    AssetInfo info;
    info.name = "selfish";
    selfish.setAsset(info);
    CHECK(AssetLibrary::instance().add(selfish, "", error));
    Network inside = selfish;
    info.version = 2;
    inside.setAsset(info);
    inside.setDisplay(inside.add("selfish"));
    CHECK(!AssetLibrary::instance().add(inside, "", error));
    CHECK(error.find("itself") != std::string::npos);
    CHECK_EQ(AssetLibrary::instance().find("selfish")->version, 1);

    Network outer;
    outer.setDisplay(outer.add("selfish"));
    AssetInfo holder;
    holder.name = "holder";
    outer.setAsset(holder);
    CHECK(AssetLibrary::instance().add(outer, "", error));
    Network around = selfish;
    around.setAsset(info);
    around.setDisplay(around.add("holder"));
    CHECK(!AssetLibrary::instance().add(around, "", error));
    CHECK(error.find("through holder") != std::string::npos);

    // An asset inside an asset cooks as any other node inside.
    Network scene;
    const int inst = scene.add("holder");
    GeometryGraph geo;
    geo.sync(scene);
    const size_t prims = geo.cook(inst, 1)->primitiveCount();
    CHECK(geo.error(inst).empty());
    GeometryGraph plain;
    plain.sync(selfish);
    CHECK(prims > 0);
    CHECK_EQ(prims, plain.cook(grid, 1)->primitiveCount());
}

TEST(asset_that_reads_the_frame_changes_with_it) {
    std::string error;
    CHECK(AssetLibrary::instance().add(lifter("lifter_d", 1, " @P.y += $F;"), "", error));
    CHECK(AssetLibrary::instance().varies("lifter_d"));
    CHECK(!AssetLibrary::instance().varies("lifter_a"));
    Network scene;
    const int box = scene.add("box");
    const int inst = scene.add("lifter_d");
    CHECK(scene.connect(box, "geometry", inst, "geometry", &error));
    GeometryGraph geo;
    geo.sync(scene);
    CHECK_NEAR(highest(geo.cook(inst, 1)), 2.25, 1e-5);
    CHECK_NEAR(highest(geo.cook(inst, 5)), 6.25, 1e-5);
    CHECK_NEAR(highest(geo.cook(inst, 1)), 2.25, 1e-5);
}

TEST(asset_made_of_chosen_nodes_takes_their_place) {
    std::string error;
    Network scene;
    const int box = scene.add("box");
    const int move = scene.add("transform", 200.0f, 0.0f);
    const int paint = scene.add("color", 400.0f, 0.0f);
    const int ball = scene.add("sphere", 400.0f, 150.0f);
    const int merged = scene.add("merge", 600.0f, 0.0f);
    CHECK(scene.connect(box, "geometry", move, "geometry", &error));
    CHECK(scene.connect(move, "geometry", paint, "geometry", &error));
    CHECK(scene.connect(paint, "geometry", merged, "geometry", &error));
    CHECK(scene.connect(ball, "geometry", merged, "geometry", &error));
    CHECK(scene.setParam(move, "t", ParamValue{0.0f, 2.0f, 0.0f}));
    scene.setDisplay(merged);
    GeometryGraph before;
    before.sync(scene);
    const GeometryPtr expected = before.cook(merged, 1);

    AssetInfo info;
    info.name = "raised_paint";
    info.label = "Raised Paint";
    Network def;
    const int inst = collapseToAsset(scene, {move, paint}, info, "", &def, error);
    CHECK(inst != 0);
    CHECK(scene.node(move) == nullptr);
    CHECK(scene.node(paint) == nullptr);
    CHECK_EQ(scene.node(inst)->type, std::string("raised_paint"));
    CHECK_EQ(scene.linksInto(inst, "geometry").size(), 1u);
    CHECK_EQ(scene.linksInto(inst, "geometry").front().from, box);
    CHECK_EQ(scene.linksInto(merged, "geometry").size(), 2u);
    CHECK_EQ(scene.displayed(), merged);
    CHECK_EQ(def.asset().name, std::string("raised_paint"));
    CHECK_EQ(def.nodes().size(), 3u);  // the two, and what comes in
    GeometryGraph after;
    after.sync(scene);
    const GeometryPtr got = after.cook(merged, 1);
    CHECK(after.error(inst).empty());
    CHECK_EQ(got->pointCount(), expected->pointCount());
    CHECK_NEAR(highest(got), highest(expected), 1e-5);

    // What cannot be one.
    Network other;
    const int a = other.add("box");
    const int b = other.add("transform");
    const int c = other.add("transform");
    const int m = other.add("merge");
    other.connect(a, "geometry", b, "geometry");
    other.connect(a, "geometry", c, "geometry");
    other.connect(b, "geometry", m, "geometry");
    other.connect(c, "geometry", m, "geometry");
    info.name = "twofold";
    CHECK_EQ(collapseToAsset(other, {b, c}, info, "", nullptr, error), 0);
    CHECK(error.find("more than one") != std::string::npos);
    const int smoke = other.add("pyro_source");
    CHECK_EQ(collapseToAsset(other, {a, smoke}, info, "", nullptr, error), 0);
    CHECK(error.find("geometry node") != std::string::npos);
    CHECK(other.node(a) != nullptr);
}

namespace pg::sim {
struct EmbeddedExample {
    const char* name;
    const char* text;
};
const std::vector<EmbeddedExample>& embeddedAssets();
}  // namespace pg::sim

TEST(asset_the_program_carries_load_and_cook) {
    // The program's own, not those of whoever runs the tests.
    unsetenv("PROTOTYPE_ASSETS");
    setenv("XDG_DATA_HOME", "/nonexistent/prototype-tests", 1);
    std::vector<std::string> errors;
    AssetLibrary::instance().loadDefaults(&errors);
    for (const std::string& e : errors) std::printf("        %s\n", e.c_str());
    CHECK(errors.empty());
    CHECK(!embeddedAssets().empty());
    for (const EmbeddedExample& e : embeddedAssets()) {
        Network net;
        std::string error;
        std::vector<std::string> warnings;
        CHECK(Network::load(e.text, net, error, &warnings));
        for (const std::string& w : warnings) std::printf("        %s: %s\n", e.name, w.c_str());
        CHECK(warnings.empty());
        if (net.save() != e.text) std::printf("        %s is written as:\n%s\n", e.name, net.save().c_str());
        CHECK_EQ(net.save(), std::string(e.text));  // as the program writes it
        const auto def = AssetLibrary::instance().find(net.asset().name);
        CHECK(def != nullptr);
        if (!def) continue;
        // An instance cooks without an error, and makes something -- a Line into what takes one.
        Network scene;
        const int line = scene.add("line");
        CHECK(scene.setParam(line, "points", ParamValue{5.0f, 0.0f, 0.0f}));
        const int inst = scene.add(def->name);
        if (!def->inputs.empty()) CHECK(scene.connect(line, "geometry", inst, "geometry", &error));
        GeometryGraph geo;
        geo.sync(scene);
        const GeometryPtr g = geo.cook(inst, 1);
        CHECK_EQ(geo.error(inst), std::string());
        CHECK(g->primitiveCount() > 0);
    }
}

TEST(asset_building_is_the_same_on_any_number_of_threads) {
    AssetLibrary::instance().loadDefaults();
    Network street;
    CHECK(Network::example("street", street));
    const unsigned saved = pg::TaskPool::instance().threadCount();
    uint64_t hashes[2] = {0, 0};
    for (int k = 0; k < 2; ++k) {
        pg::TaskPool::instance().setThreadCount(k == 0 ? 1u : 4u);
        GeometryGraph geo;
        geo.sync(street);
        const GeometryPtr g = geo.cook(street.displayed(), 1);
        CHECK(g && g->primitiveCount() > 2000);
        for (const Node& n : street.nodes()) CHECK_EQ(geo.error(n.id), std::string());
        hashes[k] = g ? g->hash() : 0;
    }
    pg::TaskPool::instance().setThreadCount(saved);
    CHECK_EQ(hashes[0], hashes[1]);

    // Its walls, windows, door and roof close round it: a solid, which a
    // fracture can cut.
    const auto def = AssetLibrary::instance().find("building");
    CHECK(def != nullptr);
    if (!def) return;
    GeometryGraph inside;
    inside.sync(*def->net);
    const GeometryPtr body = inside.cook(def->net->named("paint")->id, 1);
    std::map<std::pair<uint32_t, uint32_t>, int> edges;
    for (size_t p = 0; p < body->primitiveCount(); ++p) {
        const auto f = body->primitivePoints(p);
        for (size_t i = 0; i < f.size(); ++i) ++edges[{f[i], f[(i + 1) % f.size()]}];
    }
    bool closed = !edges.empty();
    for (const auto& [e, count] : edges) closed = closed && count == 1 && edges.count({e.second, e.first}) == 1;
    CHECK(closed);
    // More floors, more of it.
    CHECK(street.setParam(street.named("tower")->id, "floors", ParamValue{12.0f, 0.0f, 0.0f}));
    GeometryGraph taller;
    taller.sync(street);
    CHECK(taller.cook(street.displayed(), 1)->primitiveCount() > 2000u + 200u);
}
