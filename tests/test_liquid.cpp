//
// Water (src/pg/sim/Liquid.h, FreeSurface.h): the pressure of a free surface
// solved to its tolerance; still water stays still with the pressure of its
// depth; a dam breaks and the water keeps its volume; a ball of water falls
// as anything does; sources fill and pour; the water stays out of solids and
// goes where the sides are open; frames hold its surface; and invariant I5 --
// the same bits on any thread count.
//
#include "pg/core/Parallel.h"
#include "pg/sim/FreeSurface.h"
#include "pg/sim/Frame.h"
#include "pg/sim/Liquid.h"

#include "test_framework.h"

#include <cmath>
#include <cstring>
#include <random>

using namespace pg;
using namespace pg::sim;

namespace {

struct ThreadCountGuard {
    unsigned saved = TaskPool::instance().threadCount();
    ~ThreadCountGuard() { TaskPool::instance().setThreadCount(saved); }
};

/// A tank `size` wide, high and deep at `resolution`, with one source.
LiquidScene tank(Vec3 size, int resolution, const WaterSource& source) {
    LiquidScene s;
    s.solver.size = size;
    s.solver.resolution = resolution;
    s.sources.push_back(source);
    return s;
}

WaterSource block(Vec3 center, Vec3 size) {
    WaterSource w;
    w.shape = Shape::Box;
    w.center = center;
    w.size = size;
    return w;
}

Vec3 centreOfMass(const LiquidSolver& sim) {
    Vec3 sum;
    for (const Vec3& p : sim.positions()) sum += p;
    return sum * (1.0f / static_cast<float>(std::max<size_t>(sim.particleCount(), 1)));
}

/// The pressure system of a grid of n cells a side whose cells below
/// `level` are liquid: walls round it, open at the top.
void pool(FreeSurfaceSolver& solver, int n, float level) {
    std::vector<uint8_t> cells(static_cast<size_t>(n) * n * n);
    Grid phi(n, n, n);
    for (int k = 0; k < n; ++k) {
        for (int j = 0; j < n; ++j) {
            for (int i = 0; i < n; ++i) {
                const float d = static_cast<float>(j) + 0.5f - level;
                phi.at(i, j, k) = d;
                cells[phi.index(i, j, k)] = d < 0.0f ? FreeSurfaceSolver::Liquid : FreeSurfaceSolver::Air;
            }
        }
    }
    Grid open[3];
    for (int a = 0; a < 3; ++a) {
        open[a] = Grid(n + (a == 0), n + (a == 1), n + (a == 2), 1.0f);
        for (int k = 0; k < open[a].nz(); ++k) {
            for (int j = 0; j < open[a].ny(); ++j) {
                for (int i = 0; i < open[a].nx(); ++i) {
                    const int f = a == 0 ? i : a == 1 ? j : k;
                    if (f == 0 || (a != 1 && f == n)) open[a].at(i, j, k) = 0.0f;  // floor and walls; the top is open
                }
            }
        }
    }
    solver.setSystem(cells, open, phi);
}

}  // namespace

TEST(liquid_free_surface_pressure_converges) {
    FreeSurfaceSolver solver;
    pool(solver, 32, 19.7f);
    CHECK_EQ(solver.unknowns(), size_t(32 * 32 * 20));
    CHECK(solver.levels() >= 3);
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    Grid rhs(32, 32, 32), p(32, 32, 32);
    for (size_t c = 0; c < rhs.size(); ++c) rhs.data()[c] = u(rng);
    const int iterations = solver.solve(p, rhs, 1e-5f, 100);
    // Multigrid makes the conjugate gradients converge in a few steps.
    CHECK(iterations > 0 && iterations <= 30);
    CHECK(solver.residual() <= 1e-5);
    // The residual, worked out again from the operator.
    Grid ap(32, 32, 32);
    solver.apply(p, ap);
    double worst = 0.0, largest = 0.0;
    for (int k = 0; k < 32; ++k) {
        for (int j = 0; j < 20; ++j) {
            for (int i = 0; i < 32; ++i) {
                worst = std::max(worst, static_cast<double>(std::fabs(rhs.at(i, j, k) - ap.at(i, j, k))));
                largest = std::max(largest, static_cast<double>(std::fabs(rhs.at(i, j, k))));
            }
        }
    }
    CHECK(worst <= 2e-5 * largest);
    // Air holds no pressure.
    CHECK_EQ(p.at(5, 25, 5), 0.0f);
    // The surface is where theta says: a surface 0.2 of a cell above the
    // last row of centres weighs that face 1 / 0.2.
    CHECK(std::fabs(FreeSurfaceSolver::surfaceFraction(-0.2f, 0.8f) - 0.2f) < 1e-6f);
    CHECK_EQ(FreeSurfaceSolver::surfaceFraction(-0.001f, 1.0f), FreeSurfaceSolver::kMinTheta);
}

TEST(liquid_still_water_stays_still) {
    // A tank 0.3 m full -- the water from wall to wall: a domain has a
    // multiple of 8 cells along each side, and 24 fit 1 m -- after two
    // seconds it has hardly moved, and the pressure at the bottom is that of
    // the water above it.
    LiquidScene s = tank(Vec3(1.0f, 1.0f, 1.0f), 24, block(Vec3(0.0f, 0.15f, 0.0f), Vec3(1.0f, 0.3f, 1.0f)));
    LiquidSolver sim(s);
    sim.step();
    const size_t particles = sim.particleCount();
    CHECK(particles > 0);
    const float h = sim.cellSize();
    // p = g * depth, per unit density, at the centre of the bottom cell.
    const Grid& p = sim.pressure();
    const int column = sim.domain().cells[0] / 2;
    const float bottom = p.at(column, 0, column);
    const float want = 9.81f * (0.3f - 0.5f * h);
    if (std::fabs(bottom - want) > 0.15f * want) {
        ::testing::fail(__FILE__, __LINE__, "pressure at the bottom " + std::to_string(bottom) + ", not " + std::to_string(want));
    }
    float top = 0.0f;
    for (const Vec3& x : sim.positions()) top = std::max(top, x.y);
    for (int f = 0; f < 60; ++f) sim.step();
    CHECK_EQ(sim.particleCount(), particles);
    CHECK(sim.maxSpeed() < 0.15f);
    float after = 0.0f;
    for (const Vec3& x : sim.positions()) after = std::max(after, x.y);
    CHECK(std::fabs(after - top) < 1.5f * h);
}

TEST(liquid_a_dam_breaks_and_keeps_its_volume) {
    // A block of water in the corner of a closed tank, tall enough that
    // nothing splashes out.
    LiquidScene s = tank(Vec3(2.0f, 1.6f, 0.5f), 32, block(Vec3(-0.7f, 0.3f, 0.0f), Vec3(0.6f, 0.6f, 0.5f)));
    LiquidSolver sim(s);
    sim.step();
    const size_t particles = sim.particleCount();
    const Vec3 start = centreOfMass(sim);
    float front = -10.0f;
    for (int f = 0; f < 25; ++f) sim.step();
    for (const Vec3& x : sim.positions()) front = std::max(front, x.x);
    // It ran across the floor to the far wall, and lost nothing on the way.
    CHECK(front > 0.9f);
    CHECK_EQ(sim.particleCount(), particles);
    CHECK(centreOfMass(sim).x > start.x + 0.3f);
    CHECK(centreOfMass(sim).y < start.y);
    const Vec3 lo = sim.domain().origin(), hi = lo + sim.domain().size();
    for (const Vec3& x : sim.positions()) {
        if (x.y < lo.y || x.x < lo.x || x.x > hi.x || x.z < lo.z || x.z > hi.z) {
            ::testing::fail(__FILE__, __LINE__, "a particle left the tank");
            break;
        }
    }
    CHECK(sim.lastSubsteps() >= 1);
    CHECK(sim.lastIterations() > 0);
}

TEST(liquid_a_ball_of_water_falls_freely) {
    // Nothing touches it: it gains g t of speed, and its centre falls
    // g t^2 / 2 -- give or take the g t dt / 2 of a step that moves with the
    // speed at its end.
    WaterSource ball;
    ball.shape = Shape::Sphere;
    ball.center = Vec3(0.0f, 1.2f, 0.0f);
    ball.size = Vec3(0.3f);
    LiquidScene s = tank(Vec3(1.0f, 1.6f, 1.0f), 32, ball);
    s.solver.timeStep = 0.02f;
    LiquidSolver sim(s);
    sim.step();  // the first step fills it -- and it falls from then
    const float y0 = centreOfMass(sim).y, t0 = sim.time();
    for (int f = 0; f < 15; ++f) sim.step();
    const float t = sim.time() - t0;
    const float fell = y0 - centreOfMass(sim).y;
    const float want = 0.5f * 9.81f * (t * t + 2.0f * t0 * t);  // it was already falling for t0
    CHECK(std::fabs(fell - want) < 0.5f * 9.81f * sim.time() * s.solver.timeStep + 0.02f * want);
    float speed = 0.0f;
    for (const Vec3& v : sim.velocities()) speed += v.y;
    speed /= static_cast<float>(sim.particleCount());
    CHECK(std::fabs(speed + 9.81f * sim.time()) < 0.01f * 9.81f * sim.time());
}

TEST(liquid_sources_fill_once_and_pour_while_on) {
    // A fill source fills its shape once: eight particles to a cell.
    LiquidScene s = tank(Vec3(1.0f, 1.0f, 1.0f), 16, block(Vec3(0.0f, 0.25f, 0.0f), Vec3(0.5f, 0.5f, 0.5f)));
    LiquidSolver fill(s);
    fill.step();
    const double cells = 0.5 * 0.5 * 0.5 / std::pow(static_cast<double>(fill.cellSize()), 3.0);
    CHECK(std::fabs(static_cast<double>(fill.particleCount()) - 8.0 * cells) < 0.1 * 8.0 * cells);
    const size_t filled = fill.particleCount();
    fill.step();
    CHECK_EQ(fill.particleCount(), filled);
    CHECK(std::fabs(fill.volume() - 125.0) < 15.0);  // litres

    // A flow pours while it is on, then stops.
    WaterSource hose = block(Vec3(-0.35f, 0.7f, 0.0f), Vec3(0.1f, 0.1f, 0.1f));
    hose.shape = Shape::Cylinder;
    hose.mode = WaterMode::Flow;
    hose.velocity = Vec3(0.0f, -1.5f, 0.0f);  // along its own y: turned 90 degrees about z, it points +x
    hose.rotation = Vec3(0.0f, 0.0f, 90.0f);
    hose.end = 0.3f;
    LiquidSolver pour(tank(Vec3(1.0f, 1.0f, 1.0f), 16, hose));
    size_t before = 0;
    for (int f = 0; f < 9; ++f) {
        pour.step();
        CHECK(pour.particleCount() > before);  // more every frame while it is on
        before = pour.particleCount();
    }
    // The jet went the way the source points.
    float reach = -10.0f;
    for (const Vec3& x : pour.positions()) reach = std::max(reach, x.x);
    CHECK(reach > -0.2f);
    for (int f = 0; f < 10; ++f) pour.step();
    const size_t off = pour.particleCount();
    pour.step();
    CHECK_EQ(pour.particleCount(), off);
}

TEST(liquid_water_stays_out_of_solids) {
    // Water poured onto a box: it runs off, none gets in.
    LiquidScene s = tank(Vec3(1.0f, 1.0f, 1.0f), 24, block(Vec3(0.0f, 0.75f, 0.0f), Vec3(0.3f, 0.2f, 0.3f)));
    Collider box;
    box.shape = Shape::Box;
    box.center = Vec3(0.0f, 0.2f, 0.0f);
    box.size = Vec3(0.4f, 0.4f, 0.4f);
    box.rotation = Vec3(0.0f, 20.0f, 0.0f);
    s.colliders.push_back(box);
    LiquidSolver sim(s);
    // The faces inside the box are closed; those far from it open.
    const int mid = sim.domain().cells[0] / 2;
    CHECK_EQ(sim.open(1).at(mid, 4, mid), 0.0f);
    CHECK_EQ(sim.open(1).at(1, 20, 1), 1.0f);
    for (int f = 0; f < 30; ++f) sim.step();
    const float h = sim.cellSize();
    int inside = 0, beside = 0;
    for (const Vec3& x : sim.positions()) {
        if (sim.solidDistance(x) < -0.25f * h) ++inside;
        if (x.y < 0.1f) ++beside;
    }
    CHECK_EQ(inside, 0);
    CHECK(beside > 100);  // it ran down to the floor round the box
}

TEST(liquid_open_sides_let_the_water_go) {
    WaterSource hose = block(Vec3(0.3f, 0.2f, 0.0f), Vec3(0.2f, 0.2f, 0.2f));
    hose.mode = WaterMode::Flow;
    hose.velocity = Vec3(2.0f, 0.0f, 0.0f);
    LiquidScene s = tank(Vec3(1.0f, 1.0f, 0.5f), 24, hose);
    s.solver.closedSides = false;
    s.sources[0].end = 0.2f;
    LiquidSolver sim(s);
    size_t most = 0;
    for (int f = 0; f < 30; ++f) {
        sim.step();
        most = std::max(most, sim.particleCount());
    }
    // It poured, ran out over the side and is gone.
    CHECK(most > 100);
    CHECK(sim.particleCount() < most / 4);
}

TEST(liquid_frames_hold_the_surface) {
    LiquidScene s = tank(Vec3(1.0f, 1.0f, 1.0f), 16, block(Vec3(0.0f, 0.25f, 0.0f), Vec3(1.0f, 0.5f, 1.0f)));
    LiquidSolver sim(s);
    sim.step();
    const WaterFrame f = capture(sim);
    CHECK(!f.empty());
    CHECK_EQ(f.domain.cells[0], 2 * sim.domain().cells[0]);
    CHECK_EQ(f.cells.size(), 2 * f.domain.cellCount());
    CHECK_EQ(f.particles, sim.particleCount());
    // In the water below, out of it above; the distance a distance.
    const int mid = f.domain.cells[0] / 2;
    const float inside = f.distance(mid, 4, mid), above = f.distance(mid, 26, mid);
    CHECK(inside < 0.0f && above > 0.0f);
    CHECK(inside >= -f.band && above <= f.band);
    const float cell = f.domain.voxel;
    for (int j = 14; j < 18; ++j) {
        const float step = f.distance(mid, j + 1, mid) - f.distance(mid, j, mid);
        CHECK(step > 0.5f * cell && step < 1.2f * cell);  // rising by about a cell a cell
    }
    CHECK(f.foam(mid, 4, mid) >= 0.0f && f.foam(mid, 4, mid) <= 1.0f);
    CHECK(f.bytes() == f.cells.size());
}

TEST(liquid_is_bitwise_identical_across_thread_counts) {
    ThreadCountGuard guard;
    LiquidScene s = tank(Vec3(1.2f, 1.0f, 0.6f), 24, block(Vec3(-0.35f, 0.25f, 0.0f), Vec3(0.5f, 0.5f, 0.6f)));
    Collider ball;
    ball.center = Vec3(0.25f, 0.1f, 0.0f);
    ball.size = Vec3(0.2f);
    s.colliders.push_back(ball);
    Force whirl;
    whirl.kind = ForceKind::Turbulence;
    whirl.strength = 2.0f;
    s.forces.push_back(whirl);
    auto run = [&](unsigned threads) {
        TaskPool::instance().setThreadCount(threads);
        LiquidSolver sim(s);
        for (int f = 0; f < 8; ++f) sim.step();
        return sim;
    };
    const LiquidSolver one = run(1), four = run(4);
    CHECK_EQ(one.particleCount(), four.particleCount());
    CHECK(std::memcmp(one.positions().data(), four.positions().data(), one.particleCount() * sizeof(Vec3)) == 0);
    CHECK(std::memcmp(one.velocities().data(), four.velocities().data(), one.particleCount() * sizeof(Vec3)) == 0);
    CHECK(std::memcmp(one.pressure().data(), four.pressure().data(), one.pressure().size() * sizeof(float)) == 0);
}

TEST(liquid_scenes_out_of_range_are_made_safe) {
    LiquidScene s = LiquidScene::damBreak();
    s.solver.resolution = 100000;
    s.solver.timeStep = -1.0f;
    s.solver.flip = std::nanf("");
    s.solver.size = Vec3(-1.0f, 0.0f, 1e9f);
    s.sources[0].size = Vec3(0.0f);
    s.sources[0].velocity = Vec3(std::nanf(""), 1e30f, 0.0f);
    const LiquidScene safe = s.sanitized();
    CHECK(safe.solver.resolution <= 256);
    CHECK(safe.solver.timeStep > 0.0f);
    CHECK(safe.solver.flip >= 0.0f && safe.solver.flip <= 1.0f);
    CHECK(safe.solver.size.x > 0.0f && safe.solver.size.z <= 1000.0f);
    CHECK(safe.sources[0].size.x > 0.0f);
    CHECK(std::isfinite(safe.sources[0].velocity.x) && safe.sources[0].velocity.y <= 1000.0f);
    // And it runs.
    LiquidSolver sim(s);
    sim.step();
    CHECK(std::isfinite(sim.maxSpeed()));
}
