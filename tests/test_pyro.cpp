//
// The smoke and fire solver (src/pg/sim): that its parts do what the physics
// says, and that it keeps invariant I5 -- the same bits on any thread count.
//
#include "pg/core/Parallel.h"
#include "pg/sim/Frame.h"
#include "pg/sim/Pyro.h"

#include "test_framework.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

using namespace pg;
using namespace pg::sim;

namespace {

struct ThreadCountGuard {
    unsigned saved = TaskPool::instance().threadCount();
    ~ThreadCountGuard() { TaskPool::instance().setThreadCount(saved); }
};

bool sameBits(const SparseGrid& a, const SparseGrid& b) {
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}

/// Centre of mass of a field along `axis`, world units.
double centreOf(const PyroSolver& sim, const SparseGrid& g, int axis) {
    double mass = 0.0, moment = 0.0;
    for (int k = 0; k < g.nz(); ++k) {
        for (int j = 0; j < g.ny(); ++j) {
            for (int i = 0; i < g.nx(); ++i) {
                const Vec3 p = sim.worldAt(i + 0.5f, j + 0.5f, k + 0.5f);
                mass += g.at(i, j, k);
                moment += g.at(i, j, k) * p[axis];
            }
        }
    }
    return mass > 0.0 ? moment / mass : 0.0;
}

/// A scene at `resolution` cells along its longest side.
Scene small(Scene s, int resolution) {
    s.solver.resolution = resolution;
    return s;
}

/// Some flow that is anything but incompressible: sources and sinks everywhere.
void stir(PyroSolver& sim) {
    for (int a = 0; a < 3; ++a) {
        SparseGrid& v = sim.velocity(a);
        for (int k = 0; k < v.nz(); ++k) {
            for (int j = 0; j < v.ny(); ++j) {
                for (int i = 0; i < v.nx(); ++i) {
                    v.ref(i, j, k) = std::sin(0.7f * i + 1.3f * a) * std::cos(0.5f * j) + 0.3f * std::sin(0.9f * k);
                }
            }
        }
    }
}

bool finite(const PyroSolver& sim) {
    for (const SparseGrid* g : {&sim.density(), &sim.temperature(), &sim.fuel(), &sim.flame(), &sim.velocity(0),
                          &sim.velocity(1), &sim.velocity(2)}) {
        for (const float v : g->values()) {
            if (!std::isfinite(v)) return false;
        }
    }
    return true;
}

}  // namespace

TEST(pyro_grid_samples_between_cell_centres) {
    Grid g(2, 1, 1);
    g.at(0, 0, 0) = 0.0f;
    g.at(1, 0, 0) = 1.0f;
    CHECK_NEAR(g.sample(0.5f, 0.5f, 0.5f), 0.0, 1e-6);  // centre of cell 0
    CHECK_NEAR(g.sample(1.5f, 0.5f, 0.5f), 1.0, 1e-6);  // centre of cell 1
    CHECK_NEAR(g.sample(1.0f, 0.5f, 0.5f), 0.5, 1e-6);  // the face between them
    // Outside, the border repeats -- or is empty, for what flows in from outside.
    CHECK_NEAR(g.sample(2.5f, 0.5f, 0.5f), 1.0, 1e-6);
    CHECK_NEAR(g.sample(2.5f, 0.5f, 0.5f, true), 0.0, 1e-6);
    float lo = 0, hi = 0;
    g.sample(1.2f, 0.5f, 0.5f, false, lo, hi);
    CHECK_NEAR(lo, 0.0, 1e-6);
    CHECK_NEAR(hi, 1.0, 1e-6);
}

TEST(pyro_domain_is_whole_multiples_of_8_cells) {
    SolverSettings s;
    s.size = Vec3(1.0f, 1.5f, 1.0f);
    s.resolution = 96;  // along the longest side
    Domain d = s.domain();
    CHECK_EQ(d.cells[0], 64);
    CHECK_EQ(d.cells[1], 96);
    CHECK_EQ(d.cells[2], 64);
    s.size = Vec3(1.2f, 1.0f, 0.5f);
    s.resolution = 40;
    d = s.domain();
    for (int a = 0; a < 3; ++a) {
        CHECK_EQ(d.cells[a] % 8, 0);
        CHECK(d.size()[a] >= s.size[a] - 1e-4f);  // rounded up: at least what was asked
    }
    CHECK_NEAR(d.origin().x, -0.5 * d.size().x, 1e-6);
    CHECK_NEAR(d.origin().y, 0.0, 1e-6);  // standing on the floor
}

TEST(pyro_projection_makes_the_flow_divergence_free) {
    Scene s = small(Scene::smoke(), 24);
    s.solver.pressureCycles = 10;
    s.solver.sparse = false;  // every cell stirred
    PyroSolver sim(s);
    stir(sim);
    const double before = sim.meanDivergence();
    sim.project();
    CHECK(before > 1.0);
    CHECK(sim.meanDivergence() < before * 1e-3);
}

TEST(pyro_projection_converges_fast_with_the_default_cycles) {
    // The default budget, from a cold start: nearly all of the divergence goes
    // in one step, and the pressure carried over finishes the rest later.
    Scene s = small(Scene::smoke(), 48);
    s.solver.sparse = false;  // the jet set on every face
    PyroSolver sim(s);
    SparseGrid& v = sim.velocity(1);
    for (int k = 0; k < v.nz(); ++k) {
        for (int j = 0; j < v.ny(); ++j) {
            for (int i = 0; i < v.nx(); ++i) {
                const float dx = (i - 16.0f) / 6.0f, dy = (j - 16.0f) / 6.0f, dz = (k - 16.0f) / 6.0f;
                v.ref(i, j, k) = std::exp(-(dx * dx + dy * dy + dz * dz));  // an upward jet
            }
        }
    }
    const double before = sim.meanDivergence();
    sim.project();
    CHECK(sim.meanDivergence() < before * 0.03);
}

TEST(pyro_multigrid_converges_at_any_resolution) {
    // What makes multigrid worth it: each V-cycle removes most of the error,
    // however fine the grid -- plain sweeps get slower the finer it is. With a
    // wall and with solids too.
    for (const int n : {16, 32, 64}) {
        for (int variant = 0; variant < 3; ++variant) {
            SparseGrid b(n, n / 2 * 3, n), p(n, n / 2 * 3, n), solid(n, n / 2 * 3, n);
            for (int k = 0; k < b.nz(); ++k) {
                for (int j = 0; j < b.ny(); ++j) {
                    for (int i = 0; i < b.nx(); ++i) {
                        b.ref(i, j, k) = std::sin(0.37f * i * i + 1.1f * j) * std::cos(0.23f * k * j);
                        const float dx = i - n * 0.5f, dy = j - n * 0.6f, dz = k - n * 0.5f;
                        if (dx * dx + dy * dy + dz * dz < n * n / 25.0f) solid.ref(i, j, k) = 1.0f;
                    }
                }
            }
            PoissonBoundary boundary;
            boundary.closed[2] = variant >= 1;              // a floor
            boundary.solid = variant == 2 ? &solid : nullptr;  // a ball
            if (variant == 2) {
                for (size_t c = 0; c < b.size(); ++c) {
                    if (solid.data()[c] > 0.5f) b.data()[c] = 0.0f;
                }
            }
            const float h = 1.0f / static_cast<float>(n);
            PoissonSolver solver;
            solver.setBoundary(boundary);
            solver.solve(p, b, h, 1);
            const double first = solver.residual(p, b, h);
            solver.solve(p, b, h, 2);
            CHECK(solver.residual(p, b, h) < first * (variant == 2 ? 0.1 : 0.03));
            CHECK(solver.levels() >= 3);
        }
    }
}

TEST(pyro_hot_smoke_rises) {
    PyroSolver sim(small(Scene::smoke(), 36));
    const float source = sim.scene().emitters[0].center.y;
    for (int f = 0; f < 15; ++f) sim.step();
    const double early = centreOf(sim, sim.density(), 1);
    for (int f = 0; f < 15; ++f) sim.step();
    const double late = centreOf(sim, sim.density(), 1);
    CHECK(sim.density().sum() > 0.0);
    CHECK(early > source);
    CHECK(late > early + 0.05);
    // And above the source, half a metre up, the gas flows up.
    float v[3];
    sim.velocityAt(sim.nx() * 0.5f, 0.5f / sim.cellSize(), sim.nz() * 0.5f, v);
    CHECK(v[1] > 0.1f);
}

TEST(pyro_fire_burns_fuel_into_heat_and_soot) {
    Scene s = small(Scene::fire(), 36);
    PyroSolver sim(s);
    CHECK_EQ(s.emitters[0].smoke, 0.0f);  // the fire's smoke is all soot
    for (int f = 0; f < 20; ++f) sim.step();
    CHECK(sim.temperature().max() > 0.5f);
    CHECK(sim.density().sum() > 0.0);
    // Hotter than the source alone could make it: the fuel burns.
    Scene noFuel = s;
    noFuel.emitters[0].fuel = 0.0f;
    PyroSolver cold(noFuel);
    for (int f = 0; f < 20; ++f) cold.step();
    CHECK(sim.temperature().sum() > 3.0 * cold.temperature().sum());
    CHECK_EQ(cold.density().sum(), 0.0);
}

TEST(pyro_sources_emit_only_in_their_time) {
    Scene s = small(Scene::fire(), 24);
    s.emitters[0].start = 0.2f;
    s.emitters[0].end = 0.4f;
    PyroSolver sim(s);
    for (int f = 0; f < 5; ++f) sim.step();  // 0 .. 0.167 s
    CHECK_EQ(sim.fuel().sum(), 0.0);
    for (int f = 0; f < 6; ++f) sim.step();  // into the window
    CHECK(sim.flame().sum() > 0.0);
    for (int f = 0; f < 30; ++f) sim.step();  // long after: all burnt
    CHECK(sim.fuel().max() < 1e-3f);
}

TEST(pyro_moving_sources_follow_their_path) {
    Emitter e;
    e.center = Vec3(0.0f, 0.2f, 0.0f);
    e.motion = Motion::Circle;
    e.motionSize = 0.3f;
    e.motionPeriod = 2.0f;
    CHECK_NEAR(e.centerAt(0.0f).x, 0.3, 1e-6);
    CHECK_NEAR(e.centerAt(0.5f).z, 0.3, 1e-5);  // a quarter round
    CHECK_NEAR(length(e.motionVelocityAt(0.7f)), 0.3 * 3.14159265 * 2.0 / 2.0, 1e-4);
    e.motion = Motion::Sway;
    CHECK_NEAR(e.centerAt(0.5f).x, 0.3, 1e-5);
    CHECK_NEAR(e.centerAt(1.0f).x, 0.0, 1e-5);
}

TEST(pyro_box_sources_fill_a_box) {
    Scene s = small(Scene::smoke(), 32);
    s.forces.clear();
    s.emitters[0].shape = Shape::Box;
    s.emitters[0].center = Vec3(0.2f, 0.3f, 0.0f);
    s.emitters[0].size = Vec3(0.3f, 0.2f, 0.2f);
    s.emitters[0].velocity = Vec3();
    PyroSolver sim(s);
    sim.updateTiles(0.1f);  // the source's tiles taken on, as a step does first
    sim.emit(0.1f);
    const SparseGrid& d = sim.density();
    for (int k = 0; k < d.nz(); ++k) {
        for (int j = 0; j < d.ny(); ++j) {
            for (int i = 0; i < d.nx(); ++i) {
                if (d.at(i, j, k) <= 0.0f) continue;
                const Vec3 p = sim.worldAt(i + 0.5f, j + 0.5f, k + 0.5f);
                CHECK(std::fabs(p.x - 0.2f) < 0.15f && std::fabs(p.y - 0.3f) < 0.1f && std::fabs(p.z) < 0.1f);
            }
        }
    }
    CHECK(d.sum() > 0.0);
}

TEST(pyro_colliders_keep_the_gas_out_and_the_flow_around) {
    Scene s = small(Scene::smoke(), 36);
    Collider ball;
    ball.center = Vec3(0.0f, 0.55f, 0.0f);
    ball.size = Vec3(0.3f);
    s.colliders.push_back(ball);
    PyroSolver sim(s);
    for (int f = 0; f < 40; ++f) sim.step();
    const SparseGrid& solid = sim.solid();
    CHECK(solid.sum() > 10.0);
    double inside = 0.0;
    for (size_t c = 0; c < solid.size(); ++c) {
        if (solid.data()[c] > 0.5f) inside += sim.density().data()[c];
    }
    CHECK_EQ(inside, 0.0);
    // No flow into it: every face of a solid cell is still.
    for (int k = 0; k < sim.nz(); ++k) {
        for (int j = 0; j < sim.ny(); ++j) {
            for (int i = 0; i < sim.nx(); ++i) {
                if (solid.at(i, j, k) < 0.5f) continue;
                CHECK_EQ(sim.velocity(0).at(i, j, k), 0.0f);
                CHECK_EQ(sim.velocity(0).at(i + 1, j, k), 0.0f);
                CHECK_EQ(sim.velocity(1).at(i, j, k), 0.0f);
                CHECK_EQ(sim.velocity(1).at(i, j + 1, k), 0.0f);
            }
        }
    }
    // The smoke gets past it, around the sides.
    CHECK(centreOf(sim, sim.density(), 1) > 0.3);
    CHECK(sim.meanDivergence() < 0.05);
}

TEST(pyro_a_closed_floor_lets_nothing_through) {
    Scene s = small(Scene::smoke(), 24);
    s.solver.closedFloor = true;
    s.solver.sparse = false;  // every cell stirred
    PyroSolver sim(s);
    stir(sim);
    sim.project();
    for (int k = 0; k < sim.nz(); ++k) {
        for (int i = 0; i < sim.nx(); ++i) CHECK_EQ(sim.velocity(1).at(i, 0, k), 0.0f);
    }
    CHECK(sim.meanDivergence() < 1.0);
}

TEST(pyro_wind_blows_the_smoke_downwind) {
    Scene s = small(Scene::smoke(), 32);
    Force wind;
    wind.kind = ForceKind::Wind;
    wind.direction = Vec3(1.0f, 0.0f, 0.0f);
    wind.speed = 0.6f;
    wind.strength = 3.0f;
    s.forces.push_back(wind);
    PyroSolver sim(s);
    for (int f = 0; f < 30; ++f) sim.step();
    CHECK(centreOf(sim, sim.density(), 0) > 0.1);
    Scene calm = small(Scene::smoke(), 32);
    PyroSolver still(calm);
    for (int f = 0; f < 30; ++f) still.step();
    CHECK(std::fabs(centreOf(still, still.density(), 0)) < 0.05);
}

TEST(pyro_a_vortex_spins_the_gas_around_its_axis) {
    Scene s = small(Scene::smoke(), 24);
    s.emitters.clear();
    s.forces.clear();
    Force vortex;
    vortex.kind = ForceKind::Vortex;
    vortex.direction = Vec3(0.0f, 1.0f, 0.0f);
    vortex.center = Vec3(0.0f, 0.75f, 0.0f);
    vortex.radius = 0.4f;
    vortex.speed = 1.0f;
    vortex.strength = 2.0f;
    s.forces.push_back(vortex);
    s.solver.sparse = false;  // no gas: sparse, there would be no air to spin
    PyroSolver sim(s);
    sim.addForces(0.1f);
    // Counter-clockwise seen from above (+y): at +x the gas moves towards -z.
    float v[3];
    const float cx = sim.nx() * 0.5f + 0.2f / sim.cellSize(), cy = 0.75f / sim.cellSize(), cz = sim.nz() * 0.5f;
    sim.velocityAt(cx, cy, cz, v);
    CHECK(v[2] < -0.01f);
    CHECK(std::fabs(v[0]) < 0.01f);
    CHECK(std::fabs(v[1]) < 1e-6f);

    // Lift carries the gas along the axis, suction draws it in.
    s.forces[0].lift = 0.5f;
    s.forces[0].suction = 0.3f;
    PyroSolver drawn(s);
    drawn.addForces(0.1f);
    drawn.velocityAt(cx, cy, cz, v);
    CHECK(v[1] > 0.01f);
    CHECK(v[0] < -0.01f);

    // A height keeps it to a part of the axis.
    s.forces[0].height = 0.3f;
    PyroSolver short_(s);
    short_.addForces(0.1f);
    short_.velocityAt(cx, 0.2f / short_.cellSize(), cz, v);
    CHECK(std::fabs(v[0]) + std::fabs(v[1]) + std::fabs(v[2]) < 1e-6f);
}

TEST(pyro_a_vortex_through_the_open_top_stays_bounded) {
    // Its low pressure sucks air in along the axis. Air that comes in from
    // outside comes in still -- carried in at the speed at the side, it fed
    // on itself and grew without end.
    for (const bool floor : {true, false}) {
        Scene s = small(Scene::smoke(), 32);
        s.emitters.clear();
        s.forces.clear();
        s.solver.closedFloor = floor;
        Force vortex;
        vortex.kind = ForceKind::Vortex;
        vortex.direction = Vec3(0.0f, 1.0f, 0.0f);
        vortex.center = Vec3(0.0f, 0.7f, 0.0f);
        vortex.radius = 0.45f;
        vortex.speed = 1.2f;
        vortex.strength = 2.0f;
        s.forces.push_back(vortex);
        PyroSolver sim(s);
        for (int f = 0; f < 90; ++f) sim.step();
        float fastest = 0.0f;
        for (int a = 0; a < 3; ++a) {
            for (const float x : sim.velocity(a).values()) fastest = std::max(fastest, std::fabs(x));
        }
        CHECK(fastest < 1.5f);  // it spins at 1.2 at most
    }
}

TEST(pyro_is_bitwise_identical_across_thread_counts) {
    // Everything at once: two sources, one moving, every force, a solid.
    Scene s = small(Scene::fire(), 32);
    Emitter box = s.emitters[0];
    box.shape = Shape::Box;
    box.center = Vec3(0.2f, 0.1f, 0.1f);
    box.motion = Motion::Circle;
    box.smoke = 2.0f;
    s.emitters.push_back(box);
    for (const ForceKind k : {ForceKind::Wind, ForceKind::Vortex, ForceKind::Attractor, ForceKind::Drag}) {
        Force f;
        f.kind = k;
        f.strength = k == ForceKind::Drag ? 0.2f : 1.0f;
        s.forces.push_back(f);
    }
    Collider c;
    c.shape = Shape::Box;
    c.center = Vec3(-0.2f, 0.6f, 0.0f);
    s.colliders.push_back(c);

    ThreadCountGuard guard;
    // Sparse -- the tiles taken on and let go the same way too -- and dense.
    for (const bool sparse : {true, false}) {
        s.solver.sparse = sparse;
        auto run = [&](unsigned threads) {
            TaskPool::instance().setThreadCount(threads);
            PyroSolver sim(s);
            for (int f = 0; f < 12; ++f) sim.step();
            return sim;
        };
        const PyroSolver one = run(1);
        const PyroSolver four = run(4);
        CHECK(one.tiles() == four.tiles());
        CHECK(sameBits(one.density(), four.density()));
        CHECK(sameBits(one.temperature(), four.temperature()));
        CHECK(sameBits(one.fuel(), four.fuel()));
        CHECK(sameBits(one.flame(), four.flame()));
        for (int a = 0; a < 3; ++a) CHECK(sameBits(one.velocity(a), four.velocity(a)));
        CHECK(one.density().sum() > 0.0);
        CHECK(finite(one));
    }
}

TEST(pyro_a_new_domain_resets_the_simulation) {
    PyroSolver sim(small(Scene::smoke(), 24));
    for (int f = 0; f < 5; ++f) sim.step();
    CHECK(sim.density().sum() > 0.0);
    Scene s = sim.scene();
    s.solver.buoyancy = 2.0f;  // same domain: carries on
    sim.setScene(s);
    CHECK(sim.density().sum() > 0.0);
    CHECK_EQ(sim.frame(), 5);
    s.solver.resolution = 30;  // 20 x 30 x 20 cells, rounded up to multiples of 8
    sim.setScene(s);
    CHECK_EQ(sim.frame(), 0);
    CHECK_EQ(sim.density().nx(), 24);
    CHECK_EQ(sim.density().ny(), 32);
    CHECK_EQ(sim.velocity(0).nx(), 25);  // one more face than cells
    CHECK_EQ(sim.velocity(1).ny(), 33);
    CHECK_EQ(sim.density().sum(), 0.0);
}

TEST(pyro_scenes_out_of_range_are_made_safe) {
    Scene s = small(Scene::fire(), 16);
    s.solver.timeStep = 0.0f;  // expansion is burnt fuel / time step
    s.solver.cooling = std::nanf("");
    s.solver.substeps = 1000;
    s.solver.resolution = 1 << 30;
    s.emitters[0].size = Vec3(-1.0f);
    s.emitters[0].fuel = -5.0f;
    Force f;
    f.kind = ForceKind::Wind;
    f.direction = Vec3();  // no direction at all
    f.strength = std::nanf("");
    s.forces.push_back(f);
    const Scene safe = s.sanitized();
    CHECK(safe.solver.timeStep > 0.0f);
    CHECK_EQ(safe.solver.cooling, SolverSettings{}.cooling);
    CHECK_EQ(safe.solver.substeps, 16);
    CHECK_EQ(safe.solver.resolution, 1024);
    CHECK(safe.emitters[0].size.x > 0.0f);
    CHECK_EQ(safe.emitters[0].fuel, 0.0f);
    CHECK(length(safe.forces[1].direction) > 0.0f);

    s.solver.resolution = 16;
    s.solver.substeps = 1;
    PyroSolver sim(s);  // takes the scene sanitized
    for (int frame = 0; frame < 3; ++frame) sim.step();
    CHECK(finite(sim));
}

TEST(pyro_smoke_shadows_what_is_behind_it) {
    SparseGrid density(8, 8, 8);
    for (int k = 0; k < 8; ++k) {
        for (int i = 0; i < 8; ++i) density.ref(i, 5, k) = 4.0f;  // a slab at y = 5
    }
    const float up[3] = {0.0f, 1.0f, 0.0f};
    const Grid light = lightTransmittance(density, up, 0.5f, 1);
    CHECK(light.at(4, 1, 4) < 0.2f);   // below the slab: in its shadow
    CHECK(light.at(4, 7, 4) > 0.99f);  // above it: nothing between it and the light
    const Grid empty = lightTransmittance(SparseGrid(8, 8, 8), up, 0.5f, 2);
    CHECK_EQ(empty.nx(), 4);
    CHECK_NEAR(empty.at(1, 1, 1), 1.0, 1e-6);
}

TEST(pyro_frames_keep_the_gas_as_half_floats) {
    // Exact where a half is exact; rounded to nearest even otherwise; the
    // ends of the range as IEEE 754 has them.
    for (const float v : {0.0f, 1.0f, -2.0f, 0.5f, 1024.0f, 65504.0f, 6.103515625e-05f, 5.9604645e-08f}) {
        CHECK_EQ(fromHalf(toHalf(v)), v);
    }
    CHECK_EQ(toHalf(1.0f), uint16_t(0x3C00));
    CHECK_EQ(toHalf(65520.0f), uint16_t(0x7C00));     // past the largest half: infinity
    CHECK_EQ(toHalf(1.0f + 1.0f / 2048.0f), uint16_t(0x3C00));  // halfway: to even
    CHECK_EQ(toHalf(1.0f + 3.0f / 2048.0f), uint16_t(0x3C02));
    CHECK(std::isnan(fromHalf(toHalf(std::nanf("")))));
    CHECK(std::fabs(fromHalf(toHalf(0.1f)) - 0.1f) < 1e-4f);

    PyroSolver sim(small(Scene::fire(), 24));
    for (int f = 0; f < 5; ++f) sim.step();
    const Frame frame = capture(sim);
    CHECK_EQ(frame.number, 5);
    // Sparse: the tiles with gas in them alone.
    CHECK(!frame.gasTiles.empty());
    CHECK_EQ(frame.fields.size(), 3 * 512 * frame.gasTiles.size());
    CHECK(frame.gasTiles.size() < sim.tiles().tileCount());
    // Every cell within half-float precision of the solver's.
    double worst = 0.0;
    for (int k = 0; k < sim.nz(); ++k) {
        for (int j = 0; j < sim.ny(); ++j) {
            for (int i = 0; i < sim.nx(); ++i) {
                const float want = sim.temperature().at(i, j, k);
                worst = std::max(worst, static_cast<double>(std::fabs(frame.at(1, i, j, k) - want) / std::max(1.0f, want)));
            }
        }
    }
    CHECK(worst < 1e-3);
}

TEST(pyro_a_swelling_source_pushes_the_gas_out) {
    // A source that swells -- the air a collapse squeezes out -- pushes the
    // gas away on all sides: out along the floor, where it cannot go down.
    // Cold smoke from the same source, not swelling, stays about where it
    // was let out.
    auto run = [](float expansion) {
        Scene s = small(Scene::smoke(), 32);
        s.forces.clear();
        s.solver.vorticity = 0.0f;
        Emitter& e = s.emitters[0];
        e.heat = 0.0f;
        e.flicker = 0.0f;
        e.velocity = Vec3();
        e.size = Vec3(0.5f);
        e.expansion = expansion;
        auto sim = std::make_unique<PyroSolver>(s);
        for (int f = 0; f < 20; ++f) sim->step();
        return sim;
    };
    // How far from the source's axis the smoke is, on average.
    auto reach = [](const PyroSolver& sim) {
        const SparseGrid& g = sim.density();
        double mass = 0.0, moment = 0.0;
        for (int k = 0; k < g.nz(); ++k) {
            for (int j = 0; j < g.ny(); ++j) {
                for (int i = 0; i < g.nx(); ++i) {
                    const Vec3 p = sim.worldAt(i + 0.5f, j + 0.5f, k + 0.5f);
                    mass += g.at(i, j, k);
                    moment += g.at(i, j, k) * std::hypot(p.x, p.z);
                }
            }
        }
        return mass > 0.0 ? moment / mass : 0.0;
    };
    const auto still = run(0.0f), swelling = run(6.0f);
    CHECK(still->density().sum() > 0.0 && swelling->density().sum() > 0.0);
    CHECK(reach(*swelling) > 1.5 * reach(*still));
    // Beside the source, at its height, the gas flows away from it.
    const PyroSolver& sim = *swelling;
    const float out = 0.4f / sim.cellSize(), y = 0.12f / sim.cellSize();
    float v[3];
    sim.velocityAt(sim.nx() * 0.5f + out, y, sim.nz() * 0.5f, v);
    CHECK(v[0] > 0.05f);
    sim.velocityAt(sim.nx() * 0.5f - out, y, sim.nz() * 0.5f, v);
    CHECK(v[0] < -0.05f);
    sim.velocityAt(sim.nx() * 0.5f, y, sim.nz() * 0.5f + out, v);
    CHECK(v[2] > 0.05f);
    CHECK(finite(sim));
}

TEST(pyro_burning_gas_thins_out_as_it_swells) {
    // A burst of fuel that swells a lot. The gas that swells carries its fuel
    // thinned out; kept as rich, that fuel burnt and swelled again, and the
    // fire filled the whole domain within a few frames.
    Scene s = small(Scene::fire(), 32);
    s.emitters[0].fuel = 60.0f;
    s.emitters[0].end = 0.2f;
    s.emitters[0].size = Vec3(0.24f);
    s.emitters[0].center = Vec3(0.0f, 0.3f, 0.0f);
    s.solver.expansion = 3.0f;
    s.solver.substeps = 2;
    PyroSolver sim(s);
    for (int f = 0; f < 9; ++f) sim.step();
    size_t burning = 0;
    for (const float v : sim.flame().values()) burning += v > 0.01f;
    CHECK(burning < sim.flame().size() / 5);
}

// --- sparse ------------------------------------------------------------------------

TEST(pyro_sparse_grids_sample_as_dense_grids_do) {
    // Every tile kept: the same numbers at every point, inside a tile, across
    // tiles and past the sides -- the bounds of the MacCormack limiter too.
    const int n[3] = {20, 12, 17};
    Grid dense(n[0], n[1], n[2]);
    SparseGrid sparse(n[0], n[1], n[2]);
    uint32_t h = 12345u;
    auto next = [&] {
        h = h * 1664525u + 1013904223u;
        return static_cast<float>(h >> 8) / static_cast<float>(1u << 24);
    };
    for (int k = 0; k < n[2]; ++k) {
        for (int j = 0; j < n[1]; ++j) {
            for (int i = 0; i < n[0]; ++i) dense.ref(i, j, k) = sparse.ref(i, j, k) = next() * 4.0f - 1.0f;
        }
    }
    for (int t = 0; t < 4000; ++t) {
        const float x = next() * 24.0f - 2.0f, y = next() * 16.0f - 2.0f, z = next() * 21.0f - 2.0f;
        for (const bool zero : {false, true}) {
            float lo0, hi0, lo1, hi1;
            const float a = dense.sample(x, y, z, zero, lo0, hi0), b = sparse.sample(x, y, z, zero, lo1, hi1);
            CHECK(a == b && lo0 == lo1 && hi0 == hi1);
        }
    }
    CHECK_EQ(dense.max(), sparse.max());
    CHECK_EQ(dense.sum(), sparse.sum());
    const Grid back = sparse.dense();
    CHECK(back.values() == dense.values());
}

TEST(pyro_sparse_grids_keep_only_their_tiles) {
    // 24 x 16 x 8 cells: 3 x 2 x 1 tiles, the first and the last kept.
    std::vector<uint8_t> state(6, Tiles::Off);
    state[0] = state[5] = Tiles::Whole;
    auto tiles = std::make_shared<const Tiles>(24, 16, 8, state, -1);
    CHECK_EQ(tiles->stored().size(), 2u);
    CHECK_EQ(tiles->activeCells(), 2u * 512u);
    SparseGrid g(tiles);
    CHECK_EQ(g.size(), 2u * 512u);
    g.ref(3, 4, 5) = 2.0f;    // tile 0
    g.ref(20, 12, 1) = 7.0f;  // tile 5
    CHECK(!g.stored(10, 3, 3));
    CHECK_EQ(g.at(10, 3, 3), 0.0f);  // not kept: still, empty air
    CHECK_EQ(g.max(), 7.0f);
    CHECK_EQ(g.sum(), 9.0);
    // Onto other tiles: what both keep stays, the rest starts at 0.
    std::vector<uint8_t> other(6, Tiles::Off);
    other[1] = other[5] = Tiles::Whole;
    g.retile(std::make_shared<const Tiles>(24, 16, 8, other, -1));
    CHECK(!g.stored(3, 4, 5));
    CHECK_EQ(g.at(20, 12, 1), 7.0f);
    CHECK_EQ(g.at(10, 3, 3), 0.0f);
    // The faces along x of tile 0's cells: its own, and the first layer of
    // the tile after it -- the far faces of its last cells.
    const std::shared_ptr<const Tiles> faces = Tiles::faces(*tiles, 0);
    CHECK_EQ(faces->nx(), 25);
    CHECK(faces->has(0, 0, 0) && faces->has(7, 0, 0) && faces->has(8, 0, 0));
    CHECK(!faces->has(9, 0, 0));
    CHECK(faces->has(16, 8, 0) && faces->has(24, 8, 0));  // tile 5's, and past the side
    CHECK_EQ(faces->activeCells(), 2u * 512u + 2u * 64u);
}

TEST(pyro_sparse_pressure_holds_zero_round_the_gas) {
    // A box of tiles in the middle of a larger grid: the equation solved in
    // it, the still air round it at p = 0 -- as a solve on the box alone
    // with open sides all round, give or take the half cell where the
    // pressure reaches 0.
    const int n = 48;
    std::vector<uint8_t> state(6 * 6 * 6, Tiles::Off);
    for (int c = 2; c < 4; ++c) {
        for (int b = 2; b < 4; ++b) {
            for (int a = 2; a < 4; ++a) state[static_cast<size_t>(a + 6 * (b + 6 * c))] = Tiles::Whole;
        }
    }
    auto tiles = std::make_shared<const Tiles>(n, n, n, state, -1);
    SparseGrid b(tiles), p(tiles);
    forEachCounted(*tiles, [&](int i, int j, int k, size_t c) {
        const float dx = i - 24.0f, dy = j - 24.0f, dz = k - 24.0f;
        b.data()[c] = std::exp(-(dx * dx + dy * dy + dz * dz) / 20.0f);
    });
    PoissonSolver solver;
    solver.setBoundary(PoissonBoundary{});
    const float h = 1.0f / n;
    solver.solve(p, b, h, 1);
    const double first = solver.residual(p, b, h);
    solver.solve(p, b, h, 4);
    CHECK(solver.residual(p, b, h) < first * 0.01);
    CHECK(p.at(24, 24, 24) < 0.0f);  // a source: the pressure dips round it
    CHECK_EQ(p.at(4, 4, 4), 0.0f);   // outside the tiles
}

TEST(pyro_sparse_smoke_stays_where_its_gas_is) {
    // A column of smoke in a wide domain, sparse and dense: most of the
    // domain never worked on, and the smoke where the dense solver has it.
    Scene s = small(Scene::smoke(), 64);
    s.solver.size = Vec3(4.0f, 1.5f, 4.0f);
    s.solver.sparse = false;
    PyroSolver dense(s);
    s.solver.sparse = true;
    PyroSolver sparse(s);
    size_t most = 0;
    for (int f = 0; f < 30; ++f) {
        dense.step();
        sparse.step();
        most = std::max(most, sparse.activeCells());
    }
    CHECK(most < sparse.domain().cellCount() / 3);
    const double a = dense.density().sum(), b = sparse.density().sum();
    CHECK(b > 0.99 * a && b < 1.01 * a);
    CHECK(std::fabs(centreOf(dense, dense.density(), 1) - centreOf(sparse, sparse.density(), 1)) < 0.01);
    // Where there is no gas left, the tiles go.
    Scene gone = s;
    gone.emitters.clear();
    PyroSolver empty(gone);
    for (int f = 0; f < 3; ++f) empty.step();
    CHECK_EQ(empty.activeCells(), 0u);
}
