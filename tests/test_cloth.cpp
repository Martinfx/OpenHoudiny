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
#include "pg/sim/Cloth.h"
#include "pg/sim/State.h"

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
