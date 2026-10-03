//
// Cloth, ropes and soft bodies (src/pg/sim/Cloth.h): a rope keeps its
// length and swings down; a sheet drapes over a ball and rests on it,
// nothing inside it; pinned corners stay where they are; a balloon keeps its
// volume; the wind lifts a flag; the same bits on one thread and on four,
// and a state saved goes on to the bit.
//
#include "pg/core/CookEngine.h"
#include "pg/core/Graph.h"
#include "pg/core/Parallel.h"
#include "pg/nodes/Nodes.h"
#include "pg/sim/Cache.h"
#include "pg/sim/Cloth.h"
#include "pg/sim/Network.h"
#include "pg/sim/State.h"
#include "pg/sim/World.h"

#include "test_framework.h"

#include <cmath>

using namespace pg;
using namespace pg::sim;

namespace {

/// A sheet of nx x nz quads, `size` across, level at `center`.
std::shared_ptr<Geometry> sheet(int nx, int nz, Vec3 size, Vec3 center) {
    auto g = std::make_shared<Geometry>();
    g->addPoints(static_cast<size_t>((nx + 1) * (nz + 1)));
    auto P = g->positionsForWrite();
    for (int k = 0; k <= nz; ++k) {
        for (int i = 0; i <= nx; ++i) {
            P[static_cast<size_t>(i + (nx + 1) * k)] =
                center + Vec3(size.x * (static_cast<float>(i) / nx - 0.5f), 0.0f, size.z * (static_cast<float>(k) / nz - 0.5f));
        }
    }
    for (int k = 0; k < nz; ++k) {
        for (int i = 0; i < nx; ++i) {
            const uint32_t a = static_cast<uint32_t>(i + (nx + 1) * k);
            const uint32_t q[4] = {a, a + 1, a + 1 + static_cast<uint32_t>(nx + 1), a + static_cast<uint32_t>(nx + 1)};
            g->addPrimitive(q, true);
        }
    }
    return g;
}

/// A rope of `count` segments from a to b.
std::shared_ptr<Geometry> rope(Vec3 a, Vec3 b, int count) {
    auto g = std::make_shared<Geometry>();
    g->addPoints(static_cast<size_t>(count + 1));
    auto P = g->positionsForWrite();
    std::vector<uint32_t> line;
    for (int i = 0; i <= count; ++i) {
        P[static_cast<size_t>(i)] = a + (b - a) * (static_cast<float>(i) / count);
        line.push_back(static_cast<uint32_t>(i));
    }
    g->addPrimitive(line, false);
    return g;
}

void pin(Geometry& g, std::initializer_list<uint32_t> points) {
    auto p = g.points().create("pin", AttrType::Int).write<int32_t>();
    for (const uint32_t i : points) p[i] = 1;
}

ClothScene sceneOf(std::shared_ptr<Geometry> g) {
    ClothScene s;
    s.geometry = std::move(g);
    return s;
}

float lowest(const std::vector<Vec3>& x) {
    float y = 1e30f;
    for (const Vec3& p : x) y = std::min(y, p.y);
    return y;
}

}  // namespace

TEST(cloth_a_rope_swings_down_and_keeps_its_length) {
    auto g = rope(Vec3(0.0f, 2.0f, 0.0f), Vec3(1.0f, 2.0f, 0.0f), 20);
    pin(*g, {0});
    ClothScene s = sceneOf(g);
    s.solver.damping = 3.0f;  // a heavy rope in thick air: it comes to rest
    ClothSolver solver(s);
    CHECK_EQ(solver.stretchCount(), 20u);
    CHECK_EQ(solver.bendCount(), 19u);
    for (int f = 0; f < 150; ++f) solver.step();
    const auto& x = solver.positions();
    CHECK(x[0] == Vec3(0.0f, 2.0f, 0.0f));
    float len = 0.0f;
    for (size_t i = 0; i + 1 < x.size(); ++i) len += length(x[i + 1] - x[i]);
    CHECK_NEAR(len, 1.0f, 0.02f);
    // Hanging down, nearly still.
    CHECK_NEAR(x.back().y, 1.0f, 0.05f);
    CHECK(std::fabs(x.back().x) < 0.1f);
}

TEST(cloth_drapes_over_a_ball_and_rests_on_it) {
    auto g = sheet(30, 30, Vec3(1.2f, 0.0f, 1.2f), Vec3(0.0f, 1.2f, 0.0f));
    ClothScene s = sceneOf(g);
    Collider ball;
    ball.shape = Shape::Sphere;
    ball.center = Vec3(0.0f, 0.6f, 0.0f);
    ball.size = Vec3(0.6f, 0.6f, 0.6f);
    s.colliders = {ball};
    ClothSolver solver(s);
    for (int f = 0; f < 150; ++f) solver.step();
    const auto& x = solver.positions();
    const Vec3 middle = x[static_cast<size_t>(15 + 31 * 15)];
    CHECK_NEAR(middle.y, 0.9f + s.solver.thickness, 0.02f);
    for (const Vec3& p : x) CHECK(length(p - ball.center) > 0.3f);
    // The corners hang down past the ball, onto the floor.
    CHECK(lowest(x) < 0.3f);
    float fastest = 0.0f;
    for (const Vec3& v : solver.velocities()) fastest = std::max(fastest, length(v));
    CHECK(fastest < 0.3f);
}

TEST(cloth_pinned_corners_stay_and_the_rest_hangs) {
    auto g = sheet(20, 20, Vec3(1.0f, 0.0f, 1.0f), Vec3(0.0f, 1.5f, 0.0f));
    pin(*g, {0, 20});
    ClothScene s = sceneOf(g);
    s.solver.damping = 2.0f;
    ClothSolver solver(s);
    for (int f = 0; f < 150; ++f) solver.step();
    const auto& x = solver.positions();
    CHECK(x[0] == g->positions()[0]);
    CHECK(x[20] == g->positions()[20]);
    // The far edge swung down under the pinned one.
    CHECK(x[20 + 21 * 20].y < 0.8f);
    CHECK(x[20 + 21 * 20].y > 0.3f);
}

TEST(cloth_a_balloon_keeps_its_volume) {
    registerBuiltinNodes();
    Graph graph;
    pg::Node* box = graph.create("box", "box");
    box->setInt("divisions", 6);
    box->setVec3("size", Vec3(0.5f, 0.5f, 0.5f));
    box->setVec3("center", Vec3(0.0f, 1.0f, 0.0f));
    CookEngine engine;
    auto g = std::make_shared<Geometry>(*engine.cook(*box, CookContext{}));
    ClothScene s = sceneOf(g);
    s.solver.pressure = 1.0f;
    ClothSolver solver(s);
    CHECK_EQ(solver.balloonCount(), 1u);
    const float rest = solver.balloonRestVolume(0);
    CHECK_NEAR(std::fabs(rest), 0.125f, 1e-3f);
    for (int f = 0; f < 60; ++f) solver.step();
    CHECK_NEAR(solver.balloonVolume(0) / rest, 1.0f, 0.05f);
    CHECK(lowest(solver.positions()) > 0.0f);
    // Without pressure it is cloth: it sags onto the floor, its volume gone.
    s.solver.pressure = 0.0f;
    ClothSolver flat(s);
    CHECK_EQ(flat.balloonCount(), 0u);
}

TEST(cloth_the_wind_lifts_a_flag) {
    // A flag 1 x 0.6 m on a pole along y, its edge at x = 0 pinned.
    auto g = std::make_shared<Geometry>();
    const int nx = 20, ny = 12;
    g->addPoints(static_cast<size_t>((nx + 1) * (ny + 1)));
    {
        auto P = g->positionsForWrite();
        for (int j = 0; j <= ny; ++j) {
            for (int i = 0; i <= nx; ++i) P[static_cast<size_t>(i + (nx + 1) * j)] = Vec3(i * 1.0f / nx, 2.0f + j * 0.6f / ny, 0.0f);
        }
    }
    for (int j = 0; j < ny; ++j) {
        for (int i = 0; i < nx; ++i) {
            const uint32_t a = static_cast<uint32_t>(i + (nx + 1) * j);
            const uint32_t q[4] = {a, a + 1, a + 1 + static_cast<uint32_t>(nx + 1), a + static_cast<uint32_t>(nx + 1)};
            g->addPrimitive(q, true);
        }
    }
    {
        auto p = g->points().create("pin", AttrType::Int).write<int32_t>();
        for (int j = 0; j <= ny; ++j) p[static_cast<size_t>((nx + 1) * j)] = 1;
    }
    ClothScene still = sceneOf(g);
    ClothScene windy = still;
    Force wind;
    wind.kind = ForceKind::Wind;
    wind.speed = 12.0f;
    wind.direction = Vec3(1.0f, 0.0f, 0.2f);
    windy.forces = {wind};
    ClothSolver hanging(still), flying(windy);
    for (int f = 0; f < 60; ++f) {
        hanging.step();
        flying.step();
    }
    auto meanY = [](const std::vector<Vec3>& x) {
        double s = 0.0;
        for (const Vec3& p : x) s += p.y;
        return s / static_cast<double>(x.size());
    };
    const Vec3 tipHang = hanging.positions()[nx], tipFly = flying.positions()[nx];
    CHECK(meanY(flying.positions()) > meanY(hanging.positions()) + 0.1);
    CHECK(tipFly.x > 0.6f);
}

TEST(cloth_is_the_same_on_one_thread_and_on_four_and_from_a_state) {
    auto g = sheet(24, 24, Vec3(1.0f, 0.0f, 1.0f), Vec3(0.0f, 1.0f, 0.0f));
    ClothScene s = sceneOf(g);
    Collider ball;
    ball.center = Vec3(0.1f, 0.5f, 0.0f);
    ball.size = Vec3(0.5f, 0.5f, 0.5f);
    s.colliders = {ball};
    const unsigned saved = TaskPool::instance().threadCount();
    TaskPool::instance().setThreadCount(1);
    ClothSolver one(s);
    for (int f = 0; f < 30; ++f) one.step();
    TaskPool::instance().setThreadCount(4);
    ClothSolver four(s);
    for (int f = 0; f < 15; ++f) four.step();
    StateWriter out;
    four.saveState(out);
    ClothSolver later(s);
    StateReader in(out.bytes());
    CHECK(later.loadState(in));
    for (int f = 0; f < 15; ++f) {
        four.step();
        later.step();
    }
    TaskPool::instance().setThreadCount(saved);
    CHECK(one.positions() == four.positions());
    CHECK(later.positions() == four.positions());
    CHECK(later.velocities() == four.velocities());
}

TEST(cloth_a_soft_ball_stays_round_where_its_points_crowd) {
    // Round the poles of a sphere its points are nearer each other than the
    // cloth is thick: they are not pushed apart for that.
    registerBuiltinNodes();
    Graph graph;
    pg::Node* ball = graph.create("sphere", "sphere");
    ball->setFloat("radius", 0.17f);
    ball->setVec3("center", Vec3(0.0f, 2.1f, 0.0f));
    CookEngine engine;
    auto g = std::make_shared<Geometry>(*engine.cook(*ball, CookContext{}));
    ClothScene s = sceneOf(g);
    s.solver.pressure = 1.0f;
    s.solver.thickness = 0.02f;
    s.solver.airDrag = 0.0f;
    ClothSolver solver(s);
    for (int f = 0; f < 12; ++f) solver.step();
    const auto& x = solver.positions();
    Vec3 c;
    for (const Vec3& p : x) c += p;
    c = c * (1.0f / static_cast<float>(x.size()));
    CHECK_NEAR(x.front().y - c.y, 0.17f, 0.005f);  // the north pole
    CHECK_NEAR(c.y - x.back().y, 0.17f, 0.005f);   // the south pole
    CHECK_NEAR(solver.balloonVolume(0) / solver.balloonRestVolume(0), 1.0f, 0.01f);
}

TEST(cloth_a_sheet_falls_slower_through_still_air) {
    auto fallen = [](float drag) {
        ClothScene s = sceneOf(sheet(10, 10, Vec3(1.0f, 0.0f, 1.0f), Vec3(0.0f, 5.0f, 0.0f)));
        s.solver.airDrag = drag;
        ClothSolver solver(s);
        for (int f = 0; f < 15; ++f) solver.step();
        return 5.0f - solver.positions()[60].y;
    };
    const float free = fallen(0.0f), held = fallen(1.0f);
    CHECK(free > 1.1f);           // half a second of free fall: 1.2 m, less damping
    CHECK(held < 0.7f * free);    // flat through the air: a couple of metres a second
    CHECK(held > 0.3f);
}

TEST(cloth_from_the_network_through_the_cache_and_a_checkpoint) {
    // The tablecloth example: a grid and a sphere merged, one Cloth Solver,
    // the table its colliders.
    Network net;
    CHECK(Network::example("tablecloth", net));
    const Compiled c = net.compile();
    CHECK(c.ok);
    CHECK(c.world.hasCloth);
    CHECK(!c.clothMoves);
    CHECK_EQ(c.world.cloth.colliders.size(), 6u);
    CHECK_NEAR(c.world.cloth.solver.pressure, 1.0f, 1e-6f);
    WorldSolver straight(c.world);
    CHECK(straight.cloth() != nullptr);
    CHECK_EQ(straight.cloth()->balloonCount(), 1u);  // the ball; the cloth is open
    for (int f = 0; f < 20; ++f) straight.step();

    // A frame written and read back: the same points, the geometry adopted.
    const Frame frame = straight.capture();
    CHECK_EQ(frame.cloth.positions.size(), c.world.cloth.geometry->pointCount());
    Frame back;
    std::string error;
    CHECK(parseFrame(formatFrame(frame), back, error));
    CHECK(back.cloth.geometry == nullptr);
    adoptCloth(back, c.world.cloth);
    CHECK(back.cloth.geometry == c.world.cloth.geometry);
    CHECK(back.cloth.positions == frame.cloth.positions);
    CHECK(back.cloth.velocities == frame.cloth.velocities);
    CHECK(posedCloth(back.cloth) != nullptr);

    // A checkpoint goes on to the bit.
    const std::string state = straight.saveState();
    WorldSolver resumed(c.world);
    CHECK(resumed.loadState(state, error));
    for (int f = 0; f < 5; ++f) {
        straight.step();
        resumed.step();
        CHECK(straight.cloth()->positions() == resumed.cloth()->positions());
    }
}

namespace {

/// A curtain of `nx` x `ny` quads, 1 m wide and high, from y = 1 to 2 in the
/// plane z = 0, hung by its top row; `weight` kg on each of the middle
/// points of its bottom row.
std::shared_ptr<Geometry> curtain(int nx, int ny, float weight) {
    auto g = sheet(nx, ny, Vec3(1.0f, 0.0f, 1.0f), Vec3());
    auto P = g->positionsForWrite();
    for (Vec3& p : P) p = Vec3(p.x, 1.5f - p.z, 0.0f);
    auto pin = g->points().create("pin", AttrType::Int).write<int32_t>();
    auto mass = g->points().create("mass", AttrType::Float).write<float>();
    for (size_t i = 0; i < P.size(); ++i) {
        pin[i] = P[i].y > 1.999f ? 1 : 0;
        const bool middle = std::fabs(P[i].x) < 0.15f && P[i].y < 1.001f;
        mass[i] = middle ? weight : 0.3f / static_cast<float>((nx + 1) * (ny + 1));
    }
    return g;
}

}  // namespace

TEST(cloth_tears_where_it_is_pulled_too_far) {
    // A curtain with weights sewn in the middle of its hem: it holds them
    // when it does not tear; torn, it opens and they fall to the floor.
    auto run = [](float tear) {
        ClothScene s = sceneOf(curtain(20, 20, 0.2f));
        s.solver.tear = tear;
        auto solver = std::make_unique<ClothSolver>(s);
        for (int f = 0; f < 45; ++f) solver->step();
        return solver;
    };
    const auto whole = run(0.0f), torn = run(0.3f);
    CHECK_EQ(whole->tornPoints(), 0u);
    CHECK(lowest(whole->positions()) > 0.5f);
    CHECK(torn->tornPoints() > 0u);
    CHECK(lowest(torn->positions()) < 0.05f);
    // Drawn: the points split off with their own's attributes, every face
    // there still, each on the points it is on now.
    const ClothFrame frame = torn->capture();
    CHECK(frame.torn());
    CHECK_EQ(frame.copies.size(), torn->tornPoints());
    const auto g = posedCloth(frame);
    CHECK(g != nullptr);
    CHECK_EQ(g->pointCount(), frame.positions.size());
    CHECK_EQ(g->primitiveCount(), 400u);
    const auto P = g->positions();
    const AttributeArray* mass = g->points().find("mass");
    CHECK(mass != nullptr);
    for (size_t k = 0; k < frame.copies.size(); ++k) {
        const size_t i = 441 + k;
        CHECK(P[i] == frame.positions[i]);
        CHECK(mass->read<float>()[i] == mass->read<float>()[frame.copies[k]]);
    }
    // A face on a split point is not on the point it was split off.
    size_t moved = 0;
    for (size_t v = 0; v < frame.corners.size(); ++v) moved += frame.corners[v] >= 441;
    CHECK(moved > 0);
}

TEST(cloth_a_border_pulled_too_far_holds_and_is_not_torn_again_and_again) {
    // One quad hung by its top corners, a heavy weight on each of the
    // others: its sides stretch past where they would tear. They are edges
    // of one face -- the border -- and torn they would open nothing. They
    // hold, and the links are not made again substep after substep, as they
    // were: on the lips of a torn tarp, that took more than all the rest.
    auto g = sheet(1, 1, Vec3(1.0f, 0.0f, 1.0f), Vec3());
    for (Vec3& p : g->positionsForWrite()) p = Vec3(p.x, 1.5f - p.z, 0.0f);
    pin(*g, {0, 1});
    auto mass = g->points().create("mass", AttrType::Float).write<float>();
    mass[0] = mass[1] = 0.1f;
    mass[2] = mass[3] = 500.0f;
    ClothScene s = sceneOf(g);
    s.solver.tear = 0.1f;
    ClothSolver solver(s);
    float longest = 0.0f;
    for (int f = 0; f < 24; ++f) {
        solver.step();
        const std::vector<Vec3>& x = solver.positions();
        longest = std::max({longest, length(x[2] - x[0]), length(x[3] - x[1])});
    }
    if (!(longest > 1.1f)) ::testing::fail(__FILE__, __LINE__, "the sides stretched to " + std::to_string(longest) + " only");
    CHECK_EQ(solver.tornPoints(), 0u);
    CHECK_EQ(solver.relinks(), 0u);
}

TEST(cloth_a_rope_parts_and_a_balloon_bursts) {
    // A rope with a weight at its end, pinned at its top: torn, it parts in
    // two lines, and the weight falls.
    auto g = rope(Vec3(0.0f, 2.0f, 0.0f), Vec3(0.0f, 1.0f, 0.0f), 20);
    pin(*g, {0});
    auto mass = g->points().create("mass", AttrType::Float).write<float>();
    std::fill(mass.begin(), mass.end(), 0.01f);
    mass[20] = 5.0f;
    ClothScene s = sceneOf(g);
    s.solver.tear = 0.2f;
    ClothSolver r(s);
    for (int f = 0; f < 30; ++f) r.step();
    CHECK(r.positions()[20].y < 0.1f);
    const auto parted = posedCloth(r.capture());
    CHECK(parted != nullptr);
    CHECK(parted->primitiveCount() >= 2u);
    CHECK(r.capture().copies.empty());  // a rope parts without splitting its points

    // A ball blown up to three times its volume: its edges tear, the ball is
    // open, and no balloon any more.
    registerBuiltinNodes();
    Graph graph;
    pg::Node* ball = graph.create("sphere", "sphere");
    ball->setFloat("radius", 0.3f);
    ball->setVec3("center", Vec3(0.0f, 1.0f, 0.0f));
    CookEngine engine;
    ClothScene b = sceneOf(std::make_shared<Geometry>(*engine.cook(*ball, CookContext{})));
    b.solver.pressure = 3.0f;
    b.solver.tear = 0.2f;
    b.solver.selfCollision = false;
    ClothSolver balloon(b);
    CHECK_EQ(balloon.balloonCount(), 1u);
    for (int f = 0; f < 30 && balloon.balloonCount() > 0; ++f) balloon.step();
    CHECK_EQ(balloon.balloonCount(), 0u);
    CHECK(balloon.tornPoints() > 0u);
}

TEST(cloth_torn_goes_through_the_cache_and_a_state_to_the_bit) {
    ClothScene s = sceneOf(curtain(20, 20, 0.2f));
    s.solver.tear = 0.3f;
    const unsigned saved = TaskPool::instance().threadCount();
    TaskPool::instance().setThreadCount(1);
    ClothSolver one(s);
    for (int f = 0; f < 45; ++f) one.step();
    TaskPool::instance().setThreadCount(4);
    ClothSolver four(s);
    for (int f = 0; f < 30; ++f) four.step();
    CHECK(four.tornPoints() > 0u);
    StateWriter out;
    four.saveState(out);
    ClothSolver later(s);
    StateReader in(out.bytes());
    CHECK(later.loadState(in));
    CHECK_EQ(later.tornPoints(), four.tornPoints());
    for (int f = 0; f < 15; ++f) {
        four.step();
        later.step();
    }
    TaskPool::instance().setThreadCount(saved);
    CHECK(one.positions() == four.positions());
    CHECK(later.positions() == four.positions());

    // A frame of it written, read back, and given its geometry again.
    Frame frame;
    frame.cloth = four.capture();
    Frame back;
    std::string error;
    CHECK(parseFrame(formatFrame(frame), back, error));
    CHECK(back.cloth.copies == frame.cloth.copies);
    CHECK(back.cloth.corners == frame.cloth.corners);
    ClothScene scene = s;
    adoptCloth(back, scene);
    CHECK(back.cloth.geometry == s.geometry);
    const auto a = posedCloth(frame.cloth), b = posedCloth(back.cloth);
    CHECK(a && b && a->pointCount() == b->pointCount() && a->primitiveCount() == b->primitiveCount());
}

TEST(cloth_catches_the_crates_and_the_block_tears_it) {
    // The tarp example: three wooden crates dropped on a tarp laced to a
    // frame, then a block of concrete. The tarp holds the crates up -- they
    // feel it -- and the block goes through.
    Network net;
    CHECK(Network::example("tarp", net));
    const Compiled c = net.compile();
    CHECK(c.ok);
    CHECK(c.world.rigid.intoCloth);
    WorldSolver w(c.world);
    auto heights = [&] {
        std::vector<float> y;
        for (const Collider& o : w.cloth()->scene().colliders) {
            if (o.piece >= 0) y.push_back(o.center.y);
        }
        return y;
    };
    bool pushed = false;
    for (int f = 0; f < 26; ++f) {
        w.step();
        pushed = pushed || !w.cloth()->reactions().empty();
    }
    CHECK(pushed);
    CHECK_EQ(w.cloth()->tornPoints(), 0u);  // the crates alone do not tear it
    std::vector<float> y = heights();
    CHECK_EQ(y.size(), 4u);
    for (int k = 0; k < 3; ++k) CHECK(y[static_cast<size_t>(k)] > 0.7f);  // on the tarp, not through it
    const std::string state = w.saveState();
    for (int f = 0; f < 30; ++f) w.step();
    CHECK(w.cloth()->tornPoints() > 0u);
    y = heights();
    CHECK(y[3] < 0.5f);  // the block through it, on the floor
    // A checkpoint goes on as the cloth pushed the pieces.
    WorldSolver resumed(c.world);
    std::string error;
    CHECK(resumed.loadState(state, error));
    for (int f = 0; f < 30; ++f) resumed.step();
    CHECK(resumed.cloth()->positions() == w.cloth()->positions());
}


