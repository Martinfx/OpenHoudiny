//
// Animation (src/pg/sim/Network.h): keys on the parameters of a network, the
// network taken frame by frame, and solids that move -- the gas and the
// water they push take their motion on.
//
#include "pg/sim/GeometryGraph.h"
#include "pg/sim/Network.h"
#include "pg/sim/World.h"

#include "test_framework.h"

#include <cmath>
#include <string>

using namespace pg;
using namespace pg::sim;

namespace {

bool near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }

bool mentions(const Compiled& c, int node, const std::string& words) {
    for (const Problem& p : c.problems) {
        if (p.node == node && p.message.find(words) != std::string::npos) return true;
    }
    return false;
}

sim::ParamValue one(float x) { return {x, 0.0f, 0.0f}; }

}  // namespace

TEST(anim_keys_go_through_their_values) {
    std::vector<Key> keys = {{1.0f, one(0.0f), Interp::Linear}, {11.0f, one(10.0f), Interp::Step}, {21.0f, one(0.0f)}};
    // Held before the first key and after the last.
    CHECK_EQ(evaluate(keys, -5.0f, ParamKind::Float)[0], 0.0f);
    CHECK_EQ(evaluate(keys, 50.0f, ParamKind::Float)[0], 0.0f);
    // Linear, then a step that holds up to the next key.
    CHECK(near(evaluate(keys, 6.0f, ParamKind::Float)[0], 5.0f));
    CHECK_EQ(evaluate(keys, 11.0f, ParamKind::Float)[0], 10.0f);
    CHECK_EQ(evaluate(keys, 20.9f, ParamKind::Float)[0], 10.0f);
    CHECK_EQ(evaluate(keys, 21.0f, ParamKind::Float)[0], 0.0f);
    // Smooth: through each key, easing in and out, never past them.
    keys = {{1.0f, one(0.0f)}, {11.0f, one(10.0f)}, {21.0f, one(12.0f)}, {31.0f, one(0.0f)}};
    CHECK_EQ(evaluate(keys, 11.0f, ParamKind::Float)[0], 10.0f);
    CHECK_EQ(evaluate(keys, 21.0f, ParamKind::Float)[0], 12.0f);
    CHECK(evaluate(keys, 2.0f, ParamKind::Float)[0] < 0.5f);  // eases out of the first key
    float last = -1.0f;
    for (float f = 1.0f; f <= 21.0f; f += 0.25f) {
        const float v = evaluate(keys, f, ParamKind::Float)[0];
        CHECK(v >= last - 1e-5f);  // rises all the way: no dip
        CHECK(v <= 12.0f + 1e-5f);  // no overshoot past the top
        last = v;
    }
    // Vectors, each part; ints whole; toggles and choices step.
    keys = {{1.0f, {0.0f, 1.0f, 2.0f}, Interp::Linear}, {3.0f, {2.0f, 1.0f, 0.0f}}};
    const sim::ParamValue mid = evaluate(keys, 2.0f, ParamKind::Vector);
    CHECK(near(mid[0], 1.0f) && near(mid[1], 1.0f) && near(mid[2], 1.0f));
    keys = {{1.0f, one(0.0f), Interp::Linear}, {3.0f, one(3.0f)}};
    CHECK_EQ(evaluate(keys, 1.4f, ParamKind::Int)[0], 1.0f);
    CHECK_EQ(evaluate(keys, 2.9f, ParamKind::Toggle)[0], 0.0f);
    CHECK_EQ(evaluate(keys, 3.0f, ParamKind::Choice)[0], 3.0f);
}

TEST(anim_keys_are_kept_in_the_network_and_its_file) {
    Network net;
    const int rock = net.add("object");
    const uint64_t before = net.revision();
    CHECK(net.setKey(rock, "center", 1.0f, {-0.5f, 0.3f, 0.0f}, Interp::Linear));
    CHECK(net.revision() != before);
    CHECK(net.setKey(rock, "center", 31.0f, {0.5f, 0.3f, 0.0f}));
    CHECK(net.setKey(rock, "center", 16.0f, {0.0f, 0.8f, 0.0f}));
    CHECK(net.animated(rock, "center") && !net.animated(rock, "size"));
    CHECK(!net.isDefault(rock, "center"));
    CHECK_EQ(net.keys(rock, "center")->size(), size_t(3));
    CHECK_EQ(net.keys(rock, "center")->at(1).frame, 16.0f);  // in the order of their frames
    CHECK(near(net.valueAt(rock, "center", 16.0f)[1], 0.8f));
    CHECK(near(net.valueAt(rock, "center", 8.5f)[0], -0.25f));  // linear from the first key
    // Kept within the parameter's limits; not for text.
    CHECK(net.setKey(rock, "size", 5.0f, {-3.0f, 1.0f, 1.0f}));
    CHECK(net.keys(rock, "size")->front().value[0] >= 0.0f);
    CHECK(!net.setKey(rock, "file", 1.0f, one(1.0f)));
    CHECK(!net.setKey(rock, "nothing", 1.0f, one(1.0f)));
    // A key at a frame that has one replaces it.
    CHECK(net.setKey(rock, "center", 16.0f, {0.0f, 1.0f, 0.0f}));
    CHECK_EQ(net.keys(rock, "center")->size(), size_t(3));
    // Editing at a frame: a key there when animated, else the value.
    CHECK(net.setParamAt(rock, "center", 20.0f, {0.2f, 0.9f, 0.0f}));
    CHECK_EQ(net.keys(rock, "center")->size(), size_t(4));
    CHECK(net.setParamAt(rock, "color", 20.0f, {1.0f, 0.0f, 0.0f}));
    CHECK(!net.animated(rock, "color") && net.param(rock, "color")[0] == 1.0f);
    const std::vector<float> frames = net.keyFrames(rock);
    CHECK_EQ(frames.size(), size_t(5));  // 1, 5, 16, 20, 31
    CHECK_EQ(frames.front(), 1.0f);

    // Saved, read back, the same.
    const std::string text = net.save();
    CHECK(text.find("  key center 1 linear -0.5 0.3 0\n") != std::string::npos);
    CHECK(text.find("  key center 31 smooth 0.5 0.3 0\n") != std::string::npos);
    Network back;
    std::string error;
    std::vector<std::string> warnings;
    CHECK(Network::load(text, back, error, &warnings));
    CHECK(warnings.empty());
    CHECK(*back.keys(rock, "center") == *net.keys(rock, "center"));
    CHECK_EQ(back.save(), text);
    // What does not fit is dropped and said.
    Network odd;
    CHECK(Network::load(text + "node 9 object 1 other 0 0\n  key center x smooth 0 0 0\n  key nothing 1 smooth 1\n"
                               "  key center 2 bouncy 0 0 0\n",
                        odd, error, &warnings));
    CHECK_EQ(warnings.size(), size_t(3));
    CHECK(!odd.animated(9, "center"));

    // Taking keys off: the last one leaves its value; clearing keeps the
    // value at a frame; resetting goes back to the default.
    CHECK(net.removeKey(rock, "size", 5.0f));
    CHECK(!net.animated(rock, "size"));
    CHECK(net.clearKeys(rock, "center", 16.0f));
    CHECK(!net.animated(rock, "center") && near(net.param(rock, "center")[1], 1.0f));
    CHECK(net.setKey(rock, "center", 1.0f, one(0.1f)));
    CHECK(net.resetParam(rock, "center"));
    CHECK(net.isDefault(rock, "center") && !net.anyAnimated());
}

TEST(anim_the_network_is_taken_frame_by_frame) {
    Network net;
    const int smoke = net.add("pyro_source");
    CHECK(net.setParam(smoke, "smoke", "4"));
    const int ball = net.add("object");
    const int solver = net.add("pyro_solver");
    const int look = net.add("volume_look");
    const int out = net.add("output");
    CHECK(net.setParam(out, "frames", "31"));
    CHECK(net.connect(smoke, "source", solver, "sources"));
    CHECK(net.connect(ball, "collider", solver, "colliders"));
    CHECK(net.connect(solver, "gas", look, "gas"));
    CHECK(net.connect(look, "look", out, "look"));
    // Nothing animated: one world, no poses.
    Compiled c = net.compile();
    CHECK(c.ok && c.world.animation.empty() && c.poses.empty());
    CHECK(&c.worldAt(10) == &c.world);

    // The ball crosses the domain in a second, turning a quarter round y;
    // the source's smoke fades; the look darkens.
    CHECK(net.setKey(ball, "center", 1.0f, {-0.5f, 0.6f, 0.0f}, Interp::Linear));
    CHECK(net.setKey(ball, "center", 31.0f, {0.5f, 0.6f, 0.0f}, Interp::Linear));
    CHECK(net.setKey(ball, "rotation", 1.0f, {0.0f, 0.0f, 0.0f}, Interp::Linear));
    CHECK(net.setKey(ball, "rotation", 31.0f, {0.0f, 90.0f, 0.0f}, Interp::Linear));
    CHECK(net.setKey(smoke, "smoke", 1.0f, one(4.0f), Interp::Linear));
    CHECK(net.setKey(smoke, "smoke", 31.0f, one(0.0f), Interp::Linear));
    CHECK(net.setKey(look, "smoke_density", 1.0f, one(10.0f), Interp::Linear));
    CHECK(net.setKey(look, "smoke_density", 31.0f, one(40.0f), Interp::Linear));
    c = net.compile();
    CHECK(c.ok);
    CHECK(c.problems.empty());
    CHECK(!c.world.animation.empty());
    CHECK_EQ(c.world.animation.frames->size(), size_t(31));
    CHECK_EQ(c.poses.size(), size_t(31));
    const World& mid = c.worldAt(16);
    CHECK_EQ(mid.gas.colliders.size(), size_t(1));
    const Collider& body = mid.gas.colliders[0];
    CHECK(near(body.center.x, 0.0f, 1e-4f));
    CHECK(near(body.velocity.x, 1.0f, 1e-3f) && near(body.velocity.y, 0.0f));  // a metre in 30 frames at 30 fps
    CHECK(near(body.spin.y, 3.14159265f / 2.0f, 1e-3f) && near(body.spin.x, 0.0f, 1e-4f));
    CHECK(near(c.worldAt(1).gas.colliders[0].velocity.x, 1.0f, 1e-3f));  // the first frame moves as the second
    CHECK(near(mid.gas.emitters[0].smoke, 2.0f, 1e-4f));
    CHECK(near(c.lookAt(16).smokeDensity, 25.0f, 1e-3f));
    CHECK(near(c.solidsAt(16)[0].body.center.x, 0.0f, 1e-4f));
    CHECK(near(c.solidsAt(31)[0].body.center.x, 0.5f, 1e-4f));
    CHECK(c.world.gas.colliders[0].center == c.worldAt(1).gas.colliders[0].center);
    // The look alone animated: the world is the same all along.
    {
        Network still = net;
        for (const char* p : {"center", "rotation"}) CHECK(still.clearKeys(ball, p));
        CHECK(still.clearKeys(smoke, "smoke"));
        const Compiled looks = still.compile();
        CHECK(looks.world.animation.empty() && !looks.poses.empty());
        CHECK(near(looks.lookAt(31).smokeDensity, 40.0f, 1e-3f));
    }
    // Compiled again, the same world: a runner would not start again.
    CHECK(net.compile().world == c.world);
    // Another key: another world.
    CHECK(net.setKey(ball, "center", 16.0f, {0.0f, 0.9f, 0.0f}));
    CHECK(!(net.compile().world == c.world));

    // What cannot change as it runs is frame 1's, and said so.
    CHECK(net.setKey(solver, "resolution", 1.0f, one(32.0f)));
    CHECK(net.setKey(solver, "resolution", 31.0f, one(64.0f)));
    c = net.compile();
    CHECK(mentions(c, solver, "cannot change"));
    CHECK_EQ(c.worldAt(31).gas.solver.resolution, 32);
}

TEST(anim_a_moving_solid_pushes_the_gas) {
    // Still air, no source: a ball moving along x, and one standing.
    auto run = [](float speed) {
        Network net;
        const int ball = net.add("object");
        CHECK(net.setParam(ball, "size", "0.3 0.3 0.3"));
        const int solver = net.add("pyro_solver");
        CHECK(net.setParam(solver, "size", "1.6 1 1"));
        CHECK(net.setParam(solver, "resolution", "32"));
        const int source = net.add("pyro_source");
        CHECK(net.setParam(source, "center", "0 0.1 0.4"));
        CHECK(net.setParam(source, "size", "0.05 0.05 0.05"));
        CHECK(net.setParam(source, "smoke", "0.1"));
        CHECK(net.setParam(source, "velocity", "0 0 0"));
        const int look = net.add("volume_look");
        const int out = net.add("output");
        CHECK(net.setParam(out, "frames", "20"));
        CHECK(net.connect(source, "source", solver, "sources"));
        CHECK(net.connect(ball, "collider", solver, "colliders"));
        CHECK(net.connect(solver, "gas", look, "gas"));
        CHECK(net.connect(look, "look", out, "look"));
        CHECK(net.setKey(ball, "center", 1.0f, {-0.4f, 0.5f, 0.0f}, Interp::Linear));
        CHECK(net.setKey(ball, "center", 21.0f, {-0.4f + speed * 20.0f / 30.0f, 0.5f, 0.0f}, Interp::Linear));
        const Compiled c = net.compile();
        CHECK(c.ok);
        WorldSolver sim(c.world);
        for (int f = 0; f < 10; ++f) sim.step();
        // The gas just ahead of the ball, where it is now.
        const float x = -0.4f + speed * 10.0f / 30.0f + 0.22f;
        const PyroSolver& gas = *sim.gas();
        const Domain& d = gas.domain();
        float v[3];
        gas.velocityAt((x - d.origin().x) / d.voxel, (0.5f - d.origin().y) / d.voxel, (0.0f - d.origin().z) / d.voxel, v);
        // The ball, where the solver has it: a cell inside it is solid.
        const int i = static_cast<int>((x - 0.22f - d.origin().x) / d.voxel), j = static_cast<int>(0.5f / d.voxel);
        const int k = static_cast<int>((0.0f - d.origin().z) / d.voxel);
        CHECK(gas.solid().at(i, j, k) > 0.5f);
        return v[0];
    };
    const float pushed = run(1.5f), still = run(0.0f);
    CHECK(std::fabs(still) < 0.05f);
    CHECK(pushed > 0.4f);
}

TEST(anim_a_moving_solid_pushes_the_water) {
    // A paddle sweeps through a pool along x: the water in front of it heaps up.
    Network net;
    const int pool = net.add("water_source");
    CHECK(net.setParam(pool, "center", "0 0.1 0"));
    CHECK(net.setParam(pool, "size", "1.6 0.2 0.6"));
    const int paddle = net.add("object");
    CHECK(net.setParam(paddle, "shape", "box"));
    CHECK(net.setParam(paddle, "size", "0.1 0.5 0.6"));
    const int solver = net.add("liquid_solver");
    CHECK(net.setParam(solver, "size", "1.6 0.6 0.6"));
    CHECK(net.setParam(solver, "resolution", "32"));
    const int look = net.add("water_look");
    const int out = net.add("output");
    CHECK(net.setParam(out, "frames", "30"));
    CHECK(net.connect(pool, "water", solver, "sources"));
    CHECK(net.connect(paddle, "collider", solver, "colliders"));
    CHECK(net.connect(solver, "liquid", look, "liquid"));
    CHECK(net.connect(look, "look", out, "look"));
    CHECK(net.setKey(paddle, "center", 1.0f, {-0.6f, 0.25f, 0.0f}, Interp::Linear));
    CHECK(net.setKey(paddle, "center", 31.0f, {0.3f, 0.25f, 0.0f}, Interp::Linear));
    const Compiled c = net.compile();
    CHECK(c.ok);
    WorldSolver sim(c.world);
    for (int f = 0; f < 15; ++f) sim.step();
    const LiquidSolver& water = *sim.water();
    const float x = c.worldAt(15).water.colliders[0].center.x;
    // How high the water stands at a point: the highest cell with water under it.
    auto height = [&](float at) {
        float top = 0.0f;
        for (float y = 0.0f; y < 0.6f; y += 0.01f) {
            if (water.distanceToSurface(Vec3(at, y, 0.0f)) < 0.0f) top = y;
        }
        return top;
    };
    const float ahead = height(x + 0.15f), behind = height(x - 0.15f);
    CHECK(ahead > behind + 0.03f);
    // The water ahead moves with the paddle.
    CHECK(water.velocityAt(Vec3(x + 0.1f, 0.1f, 0.0f)).x > 0.3f);
    CHECK(std::isfinite(water.maxSpeed()));
}

TEST(anim_geometry_parameters_are_animated_too) {
    Network net;
    const int box = net.add("box");
    CHECK(net.setKey(box, "size", 1.0f, {1.0f, 1.0f, 1.0f}, Interp::Linear));
    CHECK(net.setKey(box, "size", 11.0f, {3.0f, 1.0f, 1.0f}, Interp::Linear));
    const int sphere = net.add("sphere");
    CHECK(net.setKey(sphere, "rows", 1.0f, one(4.0f), Interp::Linear));
    CHECK(net.setKey(sphere, "rows", 11.0f, one(8.0f), Interp::Linear));
    GeometryGraph g;
    g.engine().setParallelBranches(false);
    g.sync(net);
    auto width = [&](int frame) {
        const GeometryPtr geo = g.cook(box, frame);
        float lo = 1e30f, hi = -1e30f;
        for (const Vec3& p : geo->positions()) {
            lo = std::min(lo, p.x);
            hi = std::max(hi, p.x);
        }
        return hi - lo;
    };
    CHECK(near(width(1), 1.0f) && near(width(6), 2.0f) && near(width(11), 3.0f) && near(width(20), 3.0f));
    // Cooked once a frame: back to a frame, from the cache.
    const uint64_t cooks = g.coreNode(box)->cookCount();
    width(6);
    CHECK_EQ(g.coreNode(box)->cookCount(), cooks);
    // Ints too.
    CHECK_EQ(g.cook(sphere, 1)->pointCount(), size_t(3 * 24 + 2));
    CHECK_EQ(g.cook(sphere, 11)->pointCount(), size_t(7 * 24 + 2));
    // Keys changed: the new ones.
    CHECK(net.setKey(box, "size", 11.0f, {5.0f, 1.0f, 1.0f}, Interp::Linear));
    g.sync(net);
    CHECK(near(width(11), 5.0f));
    CHECK(net.clearKeys(box, "size", 1.0f));
    g.sync(net);
    CHECK(near(width(11), 1.0f));
}
