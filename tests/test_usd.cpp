//
// USD out (src/pg/io/Usda.h, src/pg/sim/UsdExport.h): values written the
// way USD writes them, a stage prim by prim with its time samples; geometry
// to .usda by its extension; a shot -- the bodies of an RBD Solver moved
// and turned exactly as the pieces are posed, the gas in VDB files beside
// the stage, the camera's lens in tenths of a unit, the sun where the look
// has it, and what does not change written once.
//
#include "pg/core/CookEngine.h"
#include "pg/core/Graph.h"
#include "pg/io/Export.h"
#include "pg/io/Usda.h"
#include "pg/nodes/Nodes.h"
#include "pg/sim/Network.h"
#include "pg/sim/Rigid.h"
#include "pg/sim/Shape.h"
#include "pg/sim/UsdExport.h"
#include "pg/sim/WaterMesh.h"
#include "pg/sim/World.h"

#include "test_framework.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <list>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace pg;
namespace fs = std::filesystem;
namespace usda = pg::io::usda;

namespace {

/// A folder of its own under the system's temporary one, gone afterwards.
struct TempFolder {
    fs::path path;
    explicit TempFolder(const std::string& name) {
        std::random_device rd;
        path = fs::temp_directory_path() / ("pg_test_" + name + "_" + std::to_string(rd()));
        fs::create_directories(path);
    }
    ~TempFolder() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    std::string operator/(const std::string& name) const { return (path / name).string(); }
};

std::string fileText(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool near(const Vec3& a, const Vec3& b, float eps) { return length(a - b) <= eps; }

/// The prim at `path` -- names from the root -- or null.
const usda::Prim* find(const usda::Stage& s, const std::vector<std::string>& path) {
    const std::list<usda::Prim>* level = &s.prims;
    const usda::Prim* at = nullptr;
    for (const std::string& name : path) {
        at = nullptr;
        for (const usda::Prim& p : *level) {
            if (p.name == name) at = &p;
        }
        if (!at) return nullptr;
        level = &at->children;
    }
    return at;
}

const usda::Attribute* attribute(const usda::Prim& p, const std::string& name) {
    for (const usda::Attribute& a : p.attributes) {
        if (a.name == name) return &a;
    }
    return nullptr;
}

/// Its value at `time`: the sample there, else the default.
std::string valueAt(const usda::Attribute& a, double time) {
    for (const auto& [t, v] : a.samples) {
        if (t == time) return v;
    }
    return a.value;
}

/// "(1, 2, 3)" and "[(1, 2, 3), (4, 5, 6)]" back to vectors; "(w, x, y, z)"
/// to a quaternion of x, y, z, w.
Vec3 parseTuple(const std::string& s) {
    Vec3 v;
    std::sscanf(s.c_str(), " (%f, %f, %f)", &v.x, &v.y, &v.z);
    return v;
}

std::vector<Vec3> parseTuples(const std::string& s) {
    std::vector<Vec3> out;
    for (size_t at = s.find('('); at != std::string::npos; at = s.find('(', at + 1)) out.push_back(parseTuple(s.substr(at)));
    return out;
}

Vec4 parseQuat(const std::string& s) {
    Vec4 q;
    std::sscanf(s.c_str(), " (%f, %f, %f, %f)", &q.w, &q.x, &q.y, &q.z);
    return q;
}

/// A box of `size` at `center`, cut into `count` Voronoi pieces.
GeometryPtr fracturedBox(Vec3 center, Vec3 size, int count) {
    registerBuiltinNodes();
    Graph g;
    pg::Node* box = g.create("box", "box");
    box->setInt("divisions", 1);
    box->setVec3("size", size);
    box->setVec3("center", center);
    pg::Node* fracture = g.create("voronoifracture", "fracture");
    fracture->setInt("count", count);
    fracture->setInt("seed", 3);
    fracture->setInput(0, box);
    CookEngine engine;
    return engine.cook(*fracture, CookContext{});
}

/// A unit quad, a line of three points and two loose points -- sized,
/// coloured.
Geometry sampler() {
    Geometry geo;
    geo.addPoints(9);
    auto P = geo.positionsForWrite();
    const Vec3 at[9] = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {2, 0, 0}, {2, 1, 0}, {2, 2, 0}, {3, 0, 0}, {3, 1, 0}};
    for (int i = 0; i < 9; ++i) P[static_cast<size_t>(i)] = at[i];
    const uint32_t quad[4] = {0, 1, 2, 3}, line[3] = {4, 5, 6};
    geo.addPrimitive(quad, true);
    geo.addPrimitive(line, false);
    auto pscale = geo.points().create("pscale", AttrType::Float).write<float>();
    pscale[7] = 0.25f;
    pscale[8] = 0.5f;
    return geo;
}

}  // namespace

TEST(usda_values_are_written_the_way_usd_writes_them) {
    CHECK_EQ(usda::number(0.1f), std::string("0.1"));
    CHECK_EQ(usda::number(1.0f), std::string("1"));
    CHECK_EQ(usda::number(-0.0f), std::string("0"));
    CHECK_EQ(usda::number(std::numeric_limits<float>::quiet_NaN()), std::string("0"));
    CHECK_EQ(usda::tuple(Vec3(1.0f, 0.5f, -2.0f)), std::string("(1, 0.5, -2)"));
    // USD writes a quaternion's real part first.
    CHECK_EQ(usda::quat(Vec4(0.1f, 0.2f, 0.3f, 0.9f)), std::string("(0.9, 0.1, 0.2, 0.3)"));
    CHECK_EQ(usda::quoted("a \"b\" \\c"), std::string("\"a \\\"b\\\" \\\\c\""));
    CHECK_EQ(usda::identifier("3 dogs!"), std::string("_3_dogs_"));
    CHECK_EQ(usda::identifier(""), std::string("_"));
    CHECK_EQ(usda::identifier("body_12"), std::string("body_12"));
    CHECK_EQ(usda::tuples({}), std::string("[]"));
    const std::vector<int32_t> ints = {1, -2};
    CHECK_EQ(usda::integers(ints), std::string("[1, -2]"));
    const std::vector<float> floats = {0.25f};
    CHECK_EQ(usda::numbers(floats), std::string("[0.25]"));
    CHECK_EQ(usda::tokens({"a", "b"}), std::string("[\"a\", \"b\"]"));
    CHECK_EQ(usda::asset("./x.vdb"), std::string("@./x.vdb@"));
    usda::Bounds b;
    CHECK_EQ(b.extent(), std::string("[(0, 0, 0), (0, 0, 0)]"));
    b.grow(Vec3(1, 2, 3));
    b.grow(Vec3(-1, 0, 5));
    CHECK_EQ(b.extent(0.5f), std::string("[(-1.5, -0.5, 2.5), (1.5, 2.5, 5.5)]"));
    // A value a frame: once when they are alike, else a sample each.
    usda::Prim p("Xform", "x");
    usda::animate(p, "float", "same", {{1, "2"}, {2, "2"}});
    usda::animate(p, "float", "moving", {{1, "2"}, {2, "3"}});
    CHECK_EQ(p.attributes[0].value, std::string("2"));
    CHECK(p.attributes[0].samples.empty());
    CHECK(p.attributes[1].value.empty());
    CHECK_EQ(p.attributes[1].samples.size(), size_t(2));
}

TEST(usda_a_stage_is_written_prim_by_prim_with_its_samples) {
    usda::Stage s;
    s.metadata = {{"upAxis", usda::quoted("Y")}};
    usda::Prim& root = s.prims.emplace_back("Xform", "World");
    root.set("double3", "xformOp:translate", "(1, 2, 3)");
    root.set("float", "size", "").samples = {{1, "0.5"}, {2, "1"}};
    root.setUniform("token[]", "xformOpOrder", usda::tokens({"xformOp:translate"}));
    usda::Prim& dots = root.child("Points", "dots");
    dots.metadata.push_back("prepend apiSchemas = [\"MaterialBindingAPI\"]");
    dots.set("color3f[]", "primvars:displayColor", "[(1, 0, 0)]").metadata = usda::interpolation("constant");
    dots.relate("material:binding", "</World/Looks/surface>");
    CHECK_EQ(s.text(), std::string("#usda 1.0\n"
                                   "(\n"
                                   "    upAxis = \"Y\"\n"
                                   ")\n"
                                   "\n"
                                   "def Xform \"World\"\n"
                                   "{\n"
                                   "    double3 xformOp:translate = (1, 2, 3)\n"
                                   "    float size.timeSamples = {\n"
                                   "        1: 0.5,\n"
                                   "        2: 1,\n"
                                   "    }\n"
                                   "    uniform token[] xformOpOrder = [\"xformOp:translate\"]\n"
                                   "\n"
                                   "    def Points \"dots\" (\n"
                                   "        prepend apiSchemas = [\"MaterialBindingAPI\"]\n"
                                   "    )\n"
                                   "    {\n"
                                   "        color3f[] primvars:displayColor = [(1, 0, 0)] (\n"
                                   "            interpolation = \"constant\"\n"
                                   "        )\n"
                                   "        rel material:binding = </World/Looks/surface>\n"
                                   "    }\n"
                                   "}\n"));
}

TEST(usd_geometry_goes_to_a_stage_by_its_extension) {
    TempFolder dir("usd_geometry");
    const Geometry geo = sampler();
    std::string error;
    CHECK(io::writeGeometry(geo, dir / "thing.usda", error));
    const std::string text = fileText(dir / "thing.usda");
    CHECK(text.rfind("#usda 1.0\n", 0) == 0);
    CHECK(text.find("defaultPrim = \"thing\"") != std::string::npos);
    CHECK(text.find("metersPerUnit = 1") != std::string::npos);
    CHECK(text.find("upAxis = \"Y\"") != std::string::npos);
    // The quad a Mesh of polygons as they are; the line linear curves; the
    // loose points as wide as twice their pscale.
    CHECK(text.find("def Mesh \"mesh\"") != std::string::npos);
    CHECK(text.find("int[] faceVertexCounts = [4]") != std::string::npos);
    CHECK(text.find("int[] faceVertexIndices = [0, 1, 2, 3]") != std::string::npos);
    CHECK(text.find("uniform token subdivisionScheme = \"none\"") != std::string::npos);
    CHECK(text.find("def BasisCurves \"curves\"") != std::string::npos);
    CHECK(text.find("int[] curveVertexCounts = [3]") != std::string::npos);
    CHECK(text.find("def Points \"points\"") != std::string::npos);
    CHECK(text.find("float[] widths = [0.5, 1]") != std::string::npos);
    bool listed = false;
    for (const char* const* e = io::geometryExtensions(); *e; ++e) listed = listed || std::string(*e) == ".usda";
    CHECK(listed);
    CHECK(sim::isUsdPath("a/b.USDA"));
    CHECK(sim::isUsdPath("shot.usd"));
    CHECK(!sim::isUsdPath("shot.usdc"));
    CHECK(!sim::isUsdPath("shot.obj"));
}

TEST(usd_export_moves_and_turns_each_body_as_the_pieces_are_posed) {
    CHECK(sim::rigidAvailable());
    sim::RigidScene scene;
    scene.pieces = fracturedBox(Vec3(0.0f, 2.0f, 0.0f), Vec3(1.0f, 1.0f, 1.0f), 6);
    scene.solver.glue = 0.0f;  // loose: they fall and tumble
    sim::RigidSolver solver(scene);
    CHECK(solver.error().empty());
    TempFolder dir("usd_bodies");
    sim::UsdExport usd(dir / "shot.usda");
    std::string error;
    sim::Frame last;
    for (int f = 1; f <= 20; ++f) {
        solver.step();
        sim::Frame frame;
        frame.number = f;
        frame.rigid = solver.capture();
        CHECK(usd.add(frame, nullptr, nullptr, sim::Look(), error));
        last = frame;
    }
    const std::shared_ptr<const sim::RigidLayout> layout = sim::rigidLayout(*scene.pieces, "piece");
    CHECK_EQ(usd.bodies(), layout->bodies);
    CHECK_EQ(usd.frames(), 20);
    const usda::Stage s = usd.stage();
    // As the look draws them: the faces the fracture cut have points of their own.
    const sim::Look look;
    const GeometryPtr drawn = sim::drawnPieces(last.rigid, look.piecesColor, look.piecesInside, look.insideGroup, look.rebarColor);
    const auto Q = drawn->positions();
    for (int b = 0; b < layout->bodies; ++b) {
        char name[16];
        std::snprintf(name, sizeof name, "body_%04d", b);
        const usda::Prim* body = find(s, {"World", "pieces", name});
        CHECK(body != nullptr);
        if (!body) continue;
        const usda::Attribute* translate = attribute(*body, "xformOp:translate");
        const usda::Attribute* orient = attribute(*body, "xformOp:orient");
        const usda::Attribute* order = attribute(*body, "xformOpOrder");
        CHECK(translate && orient && order);
        if (!translate || !orient || !order) continue;
        CHECK_EQ(translate->samples.size(), size_t(20));  // falling: a place each frame
        CHECK(order->uniform);
        CHECK_EQ(order->value, std::string("[\"xformOp:translate\", \"xformOp:orient\"]"));
        const usda::Prim* mesh = find(s, {"World", "pieces", name, "mesh"});
        CHECK(mesh != nullptr);
        if (!mesh) continue;
        const std::vector<Vec3> local = parseTuples(attribute(*mesh, "points")->value);
        // The mesh's points are the body's as they are drawn, in the order
        // its faces first use them; moved and turned they are where the
        // pieces are posed.
        std::vector<uint32_t> source;
        std::set<uint32_t> seen;
        for (const uint32_t prim : layout->prims[static_cast<size_t>(b)]) {
            for (const uint32_t p : drawn->primitivePoints(prim)) {
                if (seen.insert(p).second) source.push_back(p);
            }
        }
        CHECK_EQ(local.size(), source.size());
        sim::RigidPose pose;
        pose.position = parseTuple(valueAt(*translate, 20));
        pose.rotation = parseQuat(valueAt(*orient, 20));
        for (size_t i = 0; i < local.size() && i < source.size(); ++i) CHECK(near(pose.apply(local[i]), Q[source[i]], 1e-4f));
    }
}

TEST(usd_export_the_grit_carries_its_numbers_and_velocities) {
    // A block blown to dust throws grit: each frame's bits -- with the
    // numbers and velocities the frame has for them -- in the frame's layer,
    // written as it comes; the stage takes them from there.
    CHECK(sim::rigidAvailable());
    Geometry block = *fracturedBox(Vec3(0.0f, 1.0f, 0.0f), Vec3(1.0f, 1.0f, 1.0f), 1);
    {
        auto r = block.primitives().create("release", AttrType::Float).write<float>();
        std::fill(r.begin(), r.end(), 0.1f);
        auto v = block.primitives().create("vanish", AttrType::Int).write<int32_t>();
        std::fill(v.begin(), v.end(), 1);
    }
    sim::RigidScene scene;
    scene.pieces = std::make_shared<Geometry>(block);
    sim::RigidSolver solver(scene);
    TempFolder dir("usd_grit");
    sim::UsdExport usd(dir / "shot.usda");
    std::string error;
    sim::Frame last;
    int firstGrit = 0;
    for (int f = 1; f <= 12; ++f) {
        solver.step();
        sim::Frame frame;
        frame.number = f;
        frame.rigid = solver.capture();
        CHECK(usd.add(frame, nullptr, nullptr, sim::Look(), error));
        if (!frame.rigid.debris.empty()) {
            if (firstGrit == 0) firstGrit = f;
            CHECK(fs::exists(dir.path / "shot_frames" / ("shot.00" + std::string(f < 10 ? "0" : "") + std::to_string(f) + ".usda")));
        }
        last = frame;
    }
    const size_t bits = last.rigid.debris.size() / 4;
    CHECK(bits > 10);
    CHECK(firstGrit > 0);
    CHECK(usd.finish(error));
    CHECK(fs::exists(dir.path / "shot_frames" / "shot.manifest.usda"));
    const usda::Stage s = usd.stage();
    const usda::Prim* grit = find(s, {"World", "grit"});
    CHECK(grit != nullptr);
    if (!grit) return;
    // The stage names the layers, from the first frame with grit; before it,
    // the grit is not there.
    std::string clips;
    for (const std::string& m : grit->metadata) {
        if (m.rfind("clips", 0) == 0) clips = m;
    }
    CHECK(clips.find("@./shot_frames/shot.0012.usda@") != std::string::npos);
    CHECK(clips.find("string primPath = \"/World/grit\"") != std::string::npos);
    CHECK(clips.find("asset manifestAssetPath = @./shot_frames/shot.manifest.usda@") != std::string::npos);
    const usda::Attribute* ids = attribute(*grit, "ids");
    CHECK(ids && ids->type == "int64[]" && ids->value.empty());  // declared; the values are the layers'
    if (firstGrit > 1) CHECK(attribute(*grit, "visibility") != nullptr);
    // The layer of the last frame: its bits' numbers and velocities.
    const std::string layer = fileText(dir / "shot_frames/shot.0012.usda");
    CHECK(layer.find("over \"World\"") != std::string::npos);
    std::vector<int32_t> expected(last.rigid.debrisIds.begin(), last.rigid.debrisIds.end());
    const size_t idsAt = layer.find("int64[] ids.timeSamples = {");
    CHECK(idsAt != std::string::npos && layer.find("12: " + usda::integers(expected) + ",\n", idsAt) != std::string::npos);
    const size_t at = layer.find("vector3f[] velocities.timeSamples");
    CHECK(at != std::string::npos);
    if (at == std::string::npos) return;
    const size_t open = layer.find("12: ", at);
    const std::vector<Vec3> v = parseTuples(layer.substr(open, layer.find('\n', open) - open));
    CHECK_EQ(v.size(), bits);
    for (size_t i = 0; i < v.size() && i < bits; ++i) {
        const float* w = last.rigid.debrisVelocity.data() + 3 * i;
        CHECK(near(v[i], Vec3(w[0], w[1], w[2]), 1e-5f));
    }
}

TEST(usd_export_writes_the_gas_beside_the_stage_a_file_a_frame) {
    TempFolder dir("usd_gas");
    sim::Network net;
    CHECK(sim::Network::example("campfire", net));
    sim::Compiled c = net.compile(PG_SIM_EXAMPLES_DIR);
    CHECK(c.ok);
    c.world.gas.solver.resolution = 16;
    sim::WorldSolver solver(c.world);
    sim::UsdExport usd(dir / "shot.usda");
    std::string error;
    for (int i = 0; i < 3; ++i) {
        solver.step();
        const sim::Frame f = solver.capture();
        CHECK(usd.add(f, nullptr, c.hasCamera ? &c.camera : nullptr, c.look, error));
    }
    CHECK(usd.finish(error));
    CHECK_EQ(usd.gasFiles(), 3);
    CHECK_EQ(usd.frameFiles(), 0);  // smoke alone: nothing of it in layers
    for (const char* file : {"shot_gas.0001.vdb", "shot_gas.0002.vdb", "shot_gas.0003.vdb"}) {
        CHECK(fs::exists(dir.path / "shot_gas" / file));
    }
    const std::string text = fileText(dir / "shot.usda");
    CHECK(text.rfind("#usda 1.0\n", 0) == 0);
    CHECK(text.find("defaultPrim = \"World\"") != std::string::npos);
    CHECK(text.find("timeCodesPerSecond = 30") != std::string::npos);
    CHECK(text.find("endTimeCode = 3") != std::string::npos);
    CHECK(text.find("def Volume \"gas\"") != std::string::npos);
    CHECK(text.find("rel field:density = </World/gas/density>") != std::string::npos);
    CHECK(text.find("def OpenVDBAsset \"temperature\"") != std::string::npos);
    CHECK(text.find("3: @./shot_gas/shot_gas.0003.vdb@,") != std::string::npos);
}

TEST(usd_export_lens_in_tenths_of_a_unit_and_the_sun_where_the_look_has_it) {
    sim::Camera camera;
    camera.focal = 38.0f;
    camera.width = 1920;
    camera.height = 1080;
    camera.position = Vec3(1.0f, 2.0f, 3.0f);
    camera.rotation = Vec3(-10.0f, 30.0f, 5.0f);
    sim::Look look;
    look.lightAzimuth = 270.0f;
    look.lightElevation = 14.0f;
    look.exposure = 2.0f;
    sim::UsdExport usd("unused.usda", "geometry", 24.0f);
    std::string error;
    for (int f = 1; f <= 3; ++f) {
        sim::Frame frame;
        frame.number = f;
        CHECK(usd.add(frame, nullptr, &camera, look, error));
    }
    const usda::Stage s = usd.stage();
    const usda::Prim* cam = find(s, {"World", "camera"});
    CHECK(cam != nullptr);
    if (cam) {
        // 38 mm in tenths of a metre; 24 mm of film from top to bottom, and
        // across as the picture is wide; exposure in stops. All the same
        // every frame: written once.
        CHECK_EQ(attribute(*cam, "focalLength")->value, std::string("0.38"));
        CHECK(attribute(*cam, "focalLength")->samples.empty());
        CHECK_EQ(attribute(*cam, "verticalAperture")->value, std::string("0.24"));
        CHECK(std::fabs(std::stof(attribute(*cam, "horizontalAperture")->value) - 0.24f * 16.0f / 9.0f) < 1e-6f);
        CHECK_EQ(attribute(*cam, "exposure")->value, std::string("1"));
        CHECK(near(parseTuple(attribute(*cam, "xformOp:translate")->value), camera.position, 1e-6f));
        CHECK(near(parseTuple(attribute(*cam, "xformOp:rotateXYZ")->value), camera.rotation, 1e-5f));
    }
    // The sun shines along its -z: its +z is towards where the look has it.
    const usda::Prim* sun = find(s, {"World", "sun"});
    CHECK(sun != nullptr);
    if (sun) {
        const sim::Rotation r = sim::Rotation::fromEuler(parseTuple(attribute(*sun, "xformOp:rotateXYZ")->value));
        CHECK(near(r.z, look.lightDirection(), 1e-5f));
    }
    CHECK(find(s, {"World", "sky"}) != nullptr);
    CHECK(find(s, {"World", "ground"}) != nullptr);  // the look has a floor
    bool rate = false;
    for (const auto& [name, value] : s.metadata) rate = rate || (name == "timeCodesPerSecond" && value == "24");
    CHECK(rate);
}

TEST(usd_export_writes_what_does_not_change_once) {
    // The same geometry every frame: written once, in the stage. Geometry
    // that moves: in the layers of the frames it moves at -- the first one
    // too, once it is clear it moves -- and the stage takes it from there.
    TempFolder dir("usd_once");
    auto still = std::make_shared<Geometry>(sampler());
    sim::UsdExport same(dir / "same.usda", "thing");
    sim::UsdExport moving(dir / "moving.usda", "ground");  // a name another prim has: it gives way
    std::string error;
    for (int f = 1; f <= 4; ++f) {
        sim::Frame frame;
        frame.number = f;
        CHECK(same.add(frame, still, nullptr, sim::Look(), error));
        // Still for two frames, then rising.
        auto moved = std::make_shared<Geometry>(sampler());
        for (Vec3& p : moved->positionsForWrite()) p.y += 0.1f * static_cast<float>(std::max(f, 2));
        CHECK(moving.add(frame, moved, nullptr, sim::Look(), error));
        // The first frame is held back till the geometry is seen to move.
        CHECK_EQ(fs::exists(dir.path / "moving_frames" / "moving.0001.usda"), f >= 3);
    }
    CHECK(same.finish(error) && moving.finish(error));
    CHECK_EQ(same.frameFiles(), 0);
    CHECK(!fs::exists(dir.path / "same_frames"));
    const usda::Stage a = same.stage(), b = moving.stage();
    const usda::Prim* stillMesh = find(a, {"World", "thing", "mesh"});
    const usda::Prim* movingGeo = find(b, {"World", "ground_geometry"});
    const usda::Prim* movingMesh = find(b, {"World", "ground_geometry", "mesh"});
    CHECK(stillMesh && movingGeo && movingMesh);
    if (!stillMesh || !movingGeo || !movingMesh) return;
    CHECK(attribute(*stillMesh, "points")->samples.empty());
    CHECK(!attribute(*stillMesh, "points")->value.empty());
    // Frames 1 (as it first showed), 3 and 4 (as it moved): 2 was as 1.
    CHECK_EQ(moving.frameFiles(), 3);
    CHECK(!fs::exists(dir.path / "moving_frames" / "moving.0002.usda"));
    std::string clips;
    for (const std::string& m : movingGeo->metadata) {
        if (m.rfind("clips", 0) == 0) clips = m;
    }
    CHECK(clips.find("double2[] active = [(1, 0), (3, 1), (4, 2)]") != std::string::npos);
    CHECK(clips.find("double2[] times = [(1, 1), (3, 3), (4, 4)]") != std::string::npos);
    CHECK(attribute(*movingMesh, "points")->value.empty());  // declared; the values are the layers'
    CHECK(attribute(*movingMesh, "subdivisionScheme") != nullptr);
    const std::string three = fileText(dir / "moving_frames/moving.0003.usda");
    const size_t at = three.find("point3f[] points.timeSamples");
    CHECK(at != std::string::npos);
    if (at != std::string::npos) CHECK(std::fabs(parseTuples(three.substr(three.find("3: ", at)))[0].y - 0.3f) < 1e-6f);
    const std::string manifest = fileText(dir / "moving_frames/moving.manifest.usda");
    CHECK(manifest.find("over \"ground_geometry\"") != std::string::npos);
    CHECK(manifest.find("point3f[] points\n") != std::string::npos);
}

TEST(usd_export_water_and_rain_go_to_a_layer_a_frame) {
    // Rain on a pond: the water's surface and the drops, each frame in its
    // own layer -- written as the frame comes -- and the stage with their
    // materials, taking the values from the layers.
    TempFolder dir("usd_water");
    sim::Network net;
    CHECK(sim::Network::example("rain_pond", net));
    sim::Compiled c = net.compile(PG_SIM_EXAMPLES_DIR);
    CHECK(c.ok);
    c.world.water.solver.resolution = 16;
    sim::WorldSolver solver(c.world);
    sim::UsdExport usd(dir / "pond.usda", "geometry", 1.0f / c.world.timeStep);
    std::string error;
    sim::Frame last;
    for (int f = 1; f <= 3; ++f) {
        solver.step();
        last = solver.capture();
        CHECK(usd.add(last, nullptr, c.hasCamera ? &c.cameraAt(f) : nullptr, c.lookAt(f), error));
        CHECK(fs::exists(dir.path / "pond_frames" / ("pond.000" + std::to_string(f) + ".usda")));
    }
    CHECK(usd.finish(error));
    CHECK_EQ(usd.frameFiles(), 3);
    const usda::Stage s = usd.stage();
    const usda::Prim* water = find(s, {"World", "water"});
    const usda::Prim* drops = find(s, {"World", "rain", "drops"});
    const usda::Prim* droplets = find(s, {"World", "rain", "droplets"});
    CHECK(water && drops && droplets);
    CHECK(find(s, {"World", "Looks", "water"}) && find(s, {"World", "Looks", "rain"}));
    if (!water || !drops || !droplets) return;
    CHECK_EQ(water->type, std::string("Mesh"));
    CHECK_EQ(drops->type, std::string("Points"));
    for (const char* name : {"points", "normals", "velocities", "primvars:foam", "faceVertexCounts", "extent"}) {
        const usda::Attribute* a = attribute(*water, name);
        CHECK(a && a->value.empty());  // declared; the values are the layers'
    }
    CHECK_EQ(attribute(*water, "primvars:foam")->metadata, usda::interpolation("vertex"));
    CHECK_EQ(attribute(*drops, "widths")->value, std::string("[0.002]"));
    const std::string text = s.text();
    CHECK(text.find("string primPath = \"/World/water\"") != std::string::npos);
    CHECK(text.find("string primPath = \"/World/rain\"") != std::string::npos);
    CHECK(text.find("rel material:binding = </World/Looks/water>") != std::string::npos);
    // The last frame's layer: the surface as the frame gives it, the drops
    // with their numbers.
    const std::string layer = fileText(dir / "pond_frames/pond.0003.usda");
    const std::shared_ptr<Geometry> surface = sim::waterMesh(last.water, &last.rain);
    CHECK(surface->pointCount() > 100);
    const size_t at = layer.find("point3f[] points.timeSamples");
    CHECK(at != std::string::npos);
    if (at != std::string::npos) {
        const size_t open = layer.find("3: ", at);
        CHECK_EQ(parseTuples(layer.substr(open, layer.find('\n', open) - open)).size(), surface->pointCount());
    }
    CHECK(last.rain.dropCount() > 0);
    std::string ids = "[";
    for (size_t i = 0; i < last.rain.dropIds.size(); ++i) ids += (i ? ", " : "") + std::to_string(last.rain.dropIds[i]);
    CHECK(layer.find("3: " + ids + "],") != std::string::npos);
    const std::string manifest = fileText(dir / "pond_frames/pond.manifest.usda");
    CHECK(manifest.find("over \"water\"") != std::string::npos);
    CHECK(manifest.find("over \"droplets\"") != std::string::npos);
    CHECK(manifest.find("float[] primvars:foam\n") != std::string::npos);
}
