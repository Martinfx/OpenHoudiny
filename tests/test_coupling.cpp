//
// The pieces in the water and the gas (RigidSolver::feel, WorldSolver): wood
// floats as deep as it is heavy and rights itself, concrete sinks; the water
// carries a piece along with it; the gas's wind carries the grit, the water
// holds grit up and back; a checkpoint of a world whose water pushes its
// pieces goes on to the bit; with the fluids' shares at 0, the pieces fall
// as if there were no water.
//
#include "pg/core/CookEngine.h"
#include "pg/core/Graph.h"
#include "pg/nodes/Nodes.h"
#include "pg/sim/Cache.h"
#include "pg/sim/Rigid.h"
#include "pg/sim/World.h"

#include "test_framework.h"

#include <cmath>

using namespace pg;
using namespace pg::sim;

namespace {

Geometry boxPiece(Vec3 center, Vec3 size, int32_t piece) {
    registerBuiltinNodes();
    Graph g;
    pg::Node* box = g.create("box", "box");
    box->setInt("divisions", 1);
    box->setVec3("size", size);
    box->setVec3("center", center);
    CookEngine engine;
    Geometry out = *engine.cook(*box, CookContext{});
    auto a = out.primitives().create("piece", AttrType::Int).write<int32_t>();
    std::fill(a.begin(), a.end(), piece);
    return out;
}

/// Water as far as one looks, its surface at `level`, flowing at `flow`.
RigidFluids sea(float level, Vec3 flow = Vec3()) {
    RigidFluids f;
    f.waterLevel = [level](float, float) { return level; };
    f.waterVelocity = [flow](const Vec3&) { return flow; };
    f.waterCell = 0.1f;
    return f;
}

/// Steps `solver` `frames` times, the fluids feeling it before each step.
void stepIn(RigidSolver& solver, const RigidFluids& fluids, int frames) {
    for (int i = 0; i < frames; ++i) {
        solver.setFlow(solver.feel(fluids));
        solver.step();
    }
}

/// A tank of water 0.6 m deep and a box of 40 cm, of `density`, above it.
World pool(float density, int resolution = 32) {
    World w;
    w.hasWater = true;
    w.water.solver.size = Vec3(2.0f, 1.2f, 2.0f);
    w.water.solver.resolution = resolution;
    WaterSource s;
    s.center = Vec3(0.0f, 0.3f, 0.0f);
    s.size = Vec3(2.0f, 0.6f, 2.0f);
    w.water.sources = {s};
    w.hasRigid = true;
    w.rigid.pieces = std::make_shared<Geometry>(boxPiece(Vec3(0.0f, 1.0f, 0.0f), Vec3(0.4f, 0.4f, 0.4f), 0));
    w.rigid.solver.density = density;
    w.rigid.intoWater = true;
    return w;
}

}  // namespace

TEST(coupling_wood_floats_as_deep_as_it_is_heavy) {
    // A board 80 x 20 x 40 cm let go on a still sea whose surface is at 1 m:
    // it sinks until it pushes aside its weight of water -- half its height
    // at 500 kg/m^3, a quarter at 250 -- and stays there.
    for (const float density : {500.0f, 250.0f}) {
        RigidScene scene;
        scene.pieces = std::make_shared<Geometry>(boxPiece(Vec3(0.0f, 1.3f, 0.0f), Vec3(0.8f, 0.2f, 0.4f), 0));
        scene.solver.density = density;
        scene.solver.floor = false;
        RigidSolver solver(scene);
        stepIn(solver, sea(1.0f), 150);
        const RigidFrame f = solver.capture();
        const Vec3 middle = f.poses[0].apply(Vec3(0.0f, 1.3f, 0.0f));
        const float draft = 1.0f - (middle.y - 0.1f);
        CHECK_NEAR(draft, 0.2f * density / 1000.0f, 0.01f);
        CHECK(length(f.poses[0].velocity) < 0.02f);
    }
    // Heavier than water: down it goes -- slower than through the air.
    RigidScene stone;
    stone.pieces = std::make_shared<Geometry>(boxPiece(Vec3(0.0f, 0.8f, 0.0f), Vec3(0.4f, 0.4f, 0.4f), 0));
    stone.solver.density = 2400.0f;
    stone.solver.floor = false;
    RigidSolver sinking(stone), falling(stone);
    stepIn(sinking, sea(1.0f), 30);
    for (int i = 0; i < 30; ++i) falling.step();
    const float inWater = sinking.capture().poses[0].velocity.y, inAir = falling.capture().poses[0].velocity.y;
    CHECK(inWater < -0.5f);
    CHECK(inWater > 0.5f * inAir);
}

TEST(coupling_a_tilted_board_rights_itself) {
    // Let go at 30 degrees, a floating board turns flat: the water holds up
    // the side that is deeper harder.
    RigidScene scene;
    Geometry board = boxPiece(Vec3(0.0f, 1.0f, 0.0f), Vec3(0.8f, 0.2f, 0.4f), 0);
    const float a = 30.0f * 3.14159265f / 180.0f;
    for (Vec3& p : board.positionsForWrite()) {
        const Vec3 d = p - Vec3(0.0f, 1.0f, 0.0f);
        p = Vec3(0.0f, 1.0f, 0.0f) + Vec3(d.x * std::cos(a) - d.y * std::sin(a), d.x * std::sin(a) + d.y * std::cos(a), d.z);
    }
    scene.pieces = std::make_shared<Geometry>(board);
    scene.solver.density = 500.0f;
    scene.solver.floor = false;
    RigidSolver solver(scene);
    stepIn(solver, sea(1.0f), 240);
    const RigidFrame frame = solver.capture();
    const RigidPose& pose = frame.poses[0];
    // The board's own up, as it rests now: turned back by the 30 degrees.
    const Vec3 up = pose.apply(Vec3(-std::sin(a), std::cos(a), 0.0f)) - pose.apply(Vec3());
    CHECK(up.y > 0.99f);
}

TEST(coupling_the_water_carries_a_piece_along) {
    // As heavy as water, in a current of 2 m/s: it takes on the current.
    RigidScene scene;
    scene.pieces = std::make_shared<Geometry>(boxPiece(Vec3(0.0f, 0.0f, 0.0f), Vec3(0.3f, 0.3f, 0.3f), 0));
    scene.solver.density = 1000.0f;
    scene.solver.floor = false;
    RigidSolver solver(scene);
    stepIn(solver, sea(5.0f, Vec3(2.0f, 0.0f, 0.0f)), 60);
    const Vec3 v = solver.capture().poses[0].velocity;
    CHECK_NEAR(v.x, 2.0f, 0.1f);
    CHECK(std::fabs(v.y) < 0.1f);
    // No drag: the current passes it by.
    scene.solver.waterDrag = 0.0f;
    RigidSolver still(scene);
    stepIn(still, sea(5.0f, Vec3(2.0f, 0.0f, 0.0f)), 60);
    CHECK(std::fabs(still.capture().poses[0].velocity.x) < 1e-3f);
}

TEST(coupling_the_gas_blows_the_grit_and_the_water_holds_it) {
    // A block blown to dust throws its grit about; a blast of 20 m/s along x
    // takes it along -- stones of a centimetre or two, not far in a few
    // tenths of a second -- still air holds it back.
    Geometry block = boxPiece(Vec3(0.0f, 1.0f, 0.0f), Vec3(1.0f, 1.0f, 1.0f), 0);
    {
        auto r = block.primitives().create("release", AttrType::Float).write<float>();
        std::fill(r.begin(), r.end(), 0.05f);
        auto v = block.primitives().create("vanish", AttrType::Int).write<int32_t>();
        std::fill(v.begin(), v.end(), 1);
    }
    RigidScene scene;
    scene.pieces = std::make_shared<Geometry>(block);
    RigidFluids wind;
    wind.airVelocity = [](const Vec3&) { return Vec3(20.0f, 0.0f, 0.0f); };
    auto meanOf = [](const RigidFrame& f, int axis) {
        double sum = 0.0;
        size_t n = 0;
        for (size_t i = 0; i + 2 < f.debrisVelocity.size(); i += 3, ++n) sum += f.debrisVelocity[i + static_cast<size_t>(axis)];
        return n ? sum / static_cast<double>(n) : 0.0;
    };
    RigidSolver blown(scene), still(scene);
    stepIn(blown, wind, 12);
    for (int i = 0; i < 12; ++i) still.step();
    const RigidFrame a = blown.capture(), b = still.capture();
    CHECK(!a.debris.empty());
    CHECK_EQ(a.debris.size(), b.debris.size());
    CHECK(meanOf(a, 0) > meanOf(b, 0) + 0.5);
    // Air Drag 0: the wind does not reach it -- as in still air, to the bit.
    scene.solver.airDrag = 0.0f;
    RigidSolver calm(scene);
    stepIn(calm, wind, 12);
    CHECK(calm.capture().debrisVelocity == b.debrisVelocity);
    // In water it sinks, slowly -- a stone of 3 cm settles at some 0.8 m/s,
    // one of 5 at 1.1, as gravel does -- where the air lets it fall at 6 m/s
    // by now.
    scene.solver.airDrag = 1.0f;
    RigidSolver drowned(scene);
    stepIn(drowned, sea(10.0f), 20);
    const RigidFrame d = drowned.capture();
    CHECK(!d.debris.empty());
    for (size_t i = 0; i + 2 < d.debrisVelocity.size(); i += 3) {
        const Vec3 v(d.debrisVelocity[i], d.debrisVelocity[i + 1], d.debrisVelocity[i + 2]);
        CHECK(length(v) < 1.5f);
    }
}

TEST(coupling_wood_floats_in_the_tank_and_concrete_lies_on_its_bottom) {
    // The whole loop: the box drops into the water of a tank, the water goes
    // round it -- it is a collider of the water -- and holds it up.
    {
        WorldSolver solver(pool(500.0f));
        for (int i = 0; i < 120; ++i) solver.step();
        const RigidFrame f = solver.capture().rigid;
        const Vec3 middle = f.poses[0].apply(Vec3(0.0f, 1.0f, 0.0f));
        const float level = solver.water()->waterLevel().at(0.75f, 0.75f);
        CHECK(level > 0.55f && level < 0.7f);
        CHECK_NEAR(level - (middle.y - 0.2f), 0.2f, 0.07f);  // half its height under
    }
    {
        WorldSolver solver(pool(2400.0f));
        for (int i = 0; i < 90; ++i) solver.step();
        const Vec3 middle = solver.capture().rigid.poses[0].apply(Vec3(0.0f, 1.0f, 0.0f));
        CHECK_NEAR(middle.y, 0.2f, 0.03f);
    }
}

TEST(coupling_checkpoint_steps_the_pieces_again_with_what_the_water_did) {
    const World w = pool(500.0f, 24);
    WorldSolver straight(w);
    for (int f = 0; f < 20; ++f) straight.step();
    const std::string state = straight.saveState();
    WorldSolver resumed(w);
    std::string error;
    CHECK(resumed.loadState(state, error));
    CHECK_EQ(error, std::string());
    for (int f = 0; f < 10; ++f) {
        straight.step();
        resumed.step();
        CHECK(formatFrame(straight.capture()) == formatFrame(resumed.capture()));
    }
    // Into a world whose water does not push its pieces, it does not go.
    World off = w;
    off.rigid.solver.buoyancy = 0.0f;
    off.rigid.solver.waterDrag = 0.0f;
    WorldSolver other(off);
    CHECK(!other.loadState(state, error));
    CHECK(error.find("push") != std::string::npos);
}

TEST(coupling_off_the_pieces_fall_as_if_there_were_no_water) {
    World w = pool(500.0f, 24);
    w.rigid.solver.buoyancy = 0.0f;
    w.rigid.solver.waterDrag = 0.0f;
    World dry = w;
    dry.hasWater = false;
    dry.rigid.intoWater = false;
    WorldSolver wet(w), plain(dry);
    for (int f = 0; f < 30; ++f) {
        wet.step();
        plain.step();
    }
    CHECK(wet.capture().rigid.poses == plain.capture().rigid.poses);
}
