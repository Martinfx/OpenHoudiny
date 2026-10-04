//
// The grit of an RBD Solver as grains (RigidScene::gritIntoGrains): a block
// dropped hard breaks, and the grit its breaks throw is the Grain Solver's
// as soon as it is out of the pieces -- it lands, on the floor and on the
// grains before it, and piles up; the solver's frames hold none of it. The
// grains take the colour of the cracks. The same on one thread and on
// four, and from a checkpoint; linked in the network, the RBD Solver's
// Rigid into the Grain Solver's Grit.
//
#include "pg/core/CookEngine.h"
#include "pg/core/Graph.h"
#include "pg/core/Half.h"
#include "pg/core/Parallel.h"
#include "pg/nodes/Nodes.h"
#include "pg/sim/Network.h"
#include "pg/sim/World.h"

#include "test_framework.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace pg;
using namespace pg::sim;

namespace {

/// A closed box of `size` at `center`, one piece.
GeometryPtr block(Vec3 center, Vec3 size) {
    registerBuiltinNodes();
    Graph g;
    pg::Node* box = g.create("box", "box");
    box->setInt("divisions", 1);
    box->setVec3("size", size);
    box->setVec3("center", center);
    CookEngine engine;
    return engine.cook(*box, CookContext{});
}

/// A block of concrete dropped from 4 m, breaking where it lands and
/// throwing much grit; the grit the grains' when `grains`.
World dropped(bool grains) {
    World w;
    w.hasRigid = true;
    RigidScene& r = w.rigid;
    r.pieces = block(Vec3(0.0f, 4.25f, 0.0f), Vec3(0.8f, 0.5f, 0.5f));
    r.solver.fracture = 5e4f;
    r.solver.fracturePieces = 10;
    r.solver.fractureDepth = 1;
    r.solver.glue = 0.0f;
    r.solver.substeps = 4;
    r.solver.debris = 4.0f;
    if (grains) {
        r.gritIntoGrains = true;
        r.intoGrains = true;
        r.gritColor = Vec3(0.5f, 0.45f, 0.4f);
        w.hasGrains = true;
        w.grains.solver.friction = 0.8f;
    }
    return w;
}

}  // namespace

TEST(grit_grains_a_block_breaks_and_its_grit_piles_up_as_grains) {
    if (!rigidAvailable()) return;
    WorldSolver solver(dropped(true));
    // How fast the grains went, at any frame: put out of the rubble they
    // were thrown into, frame after frame, none went faster than the block
    // fell (8.9 m/s) -- the fastest a piece knocks one -- and the half
    // metre a second more a grain may come off what it touches. (Their
    // speed built up in substeps, they went at up to 15.)
    float fastestEver = 0.0f, gritEver = 0.0f;
    for (int f = 0; f < 60; ++f) {
        solver.step();
        const Frame now = solver.capture();
        for (size_t i = 0; i < now.grains.size(); ++i) {
            const uint16_t* v = now.grains.velocities.data() + 3 * i;
            fastestEver = std::max(fastestEver, length(Vec3(fromHalf(v[0]), fromHalf(v[1]), fromHalf(v[2]))));
        }
        for (const RigidBit& b : solver.rigid()->thrown()) gritEver = std::max(gritEver, length(b.velocity));
    }
    std::printf("    grit: thrown at up to %.2f m/s, grains at up to %.2f\n", static_cast<double>(gritEver),
                static_cast<double>(fastestEver));
    CHECK(gritEver > 0.0f);
    CHECK(fastestEver < 9.4f);
    const Frame frame = solver.capture();
    // The grit is the grains' -- the pieces' solver keeps only what has not
    // come out of the pieces yet.
    CHECK(!frame.rigid.shatters.empty());
    const GrainFrame& g = frame.grains;
    CHECK(g.fits());
    CHECK(g.size() > 40);
    CHECK(frame.rigid.debris.size() / 4 < g.size() / 10);
    // In the colour of the cracks.
    CHECK_EQ(g.colors.size(), 3 * g.size());
    CHECK_EQ(static_cast<int>(g.colors[0]), static_cast<int>(std::lround(0.5f * 255.0f)));
    // Down -- on the floor, none in it, on the pieces and on each other:
    // some higher than a grain lying on the floor -- and most at rest; what
    // the falling pieces flung still slides, none flies off.
    int piled = 0, still = 0;
    float lowest = 1e30f, fastest = 0.0f;
    for (size_t i = 0; i < g.size(); ++i) {
        const float r = fromHalf(g.radii[i]);
        const Vec3 v(fromHalf(g.velocities[3 * i]), fromHalf(g.velocities[3 * i + 1]), fromHalf(g.velocities[3 * i + 2]));
        CHECK(std::isfinite(g.positions[i].y));
        lowest = std::min(lowest, g.positions[i].y - r);
        fastest = std::max(fastest, length(v));
        piled += g.positions[i].y > 1.6f * r ? 1 : 0;
        still += length(v) < 0.5f ? 1 : 0;
    }
    std::printf("    grit: %zu grains, %d still, %d piled, fastest %.2f m/s, lowest %.4f m\n", g.size(), still, piled,
                static_cast<double>(fastest), static_cast<double>(lowest));
    CHECK(lowest > -0.005f);
    CHECK(fastest < 8.0f);
    CHECK(still * 5 > static_cast<int>(4 * g.size()));
    CHECK(piled > 0);
    // Without the link, the grit stays the solver's and there are no grains.
    WorldSolver alone(dropped(false));
    for (int f = 0; f < 60; ++f) alone.step();
    const Frame plain = alone.capture();
    CHECK(!plain.rigid.debris.empty());
    CHECK(plain.grains.empty());
}

TEST(grit_grains_the_same_on_any_threads_and_from_a_checkpoint) {
    if (!rigidAvailable()) return;
    const World world = dropped(true);
    const unsigned saved = TaskPool::instance().threadCount();
    TaskPool::instance().setThreadCount(1);
    WorldSolver one(world);
    for (int f = 0; f < 40; ++f) one.step();
    TaskPool::instance().setThreadCount(4);
    WorldSolver four(world);
    for (int f = 0; f < 25; ++f) four.step();
    const std::string state = four.saveState();
    WorldSolver later(world);
    std::string error;
    CHECK(later.loadState(state, error));
    for (int f = 0; f < 15; ++f) {
        four.step();
        later.step();
    }
    TaskPool::instance().setThreadCount(saved);
    const Frame a = one.capture(), b = four.capture(), c = later.capture();
    CHECK(a.grains.size() > 0);
    CHECK(a.grains.positions == b.grains.positions);
    CHECK(a.grains.ids == b.grains.ids);
    CHECK(c.grains.positions == b.grains.positions);
    CHECK(c.grains.velocities == b.grains.velocities);
    CHECK(c.grains.ids == b.grains.ids);
    CHECK(c.rigid.poses == b.rigid.poses);
}

TEST(grit_grains_network_links_the_rbd_solvers_rigid_into_grit) {
    Network net;
    const int block = net.add("box", 0, 0);
    const int rbd = net.add("rbd_solver", 300, 0);
    const int grains = net.add("grain_solver", 600, 0);
    const int out = net.add("output", 900, 0);
    CHECK(net.connect(block, "geometry", rbd, "pieces"));
    CHECK(net.connect(rbd, "rigid", grains, "grit"));
    CHECK(net.connect(grains, "look", out, "look"));
    CHECK(net.setParam(rbd, "inside_color", sim::ParamValue{0.5f, 0.4f, 0.3f}));
    CHECK(net.setParam(grains, "color", sim::ParamValue{0.2f, 0.3f, 0.4f}));
    const Compiled c = net.compile();
    CHECK(c.ok);
    CHECK(c.world.hasRigid && c.world.hasGrains);
    CHECK(c.world.rigid.gritIntoGrains);
    CHECK(c.world.rigid.intoGrains);  // the grit lands on the pieces
    CHECK(std::fabs(c.world.rigid.gritColor.x - 0.45f) < 1e-6f);
    CHECK(std::fabs(c.world.grains.color.z - 0.4f) < 1e-6f);
    // Grit and no points: grains enough, no warning.
    for (const Problem& p : c.problems) CHECK(p.message.find("No grains") == std::string::npos);
}
