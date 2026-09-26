//
// The smoke and fire solver (src/pg/sim): that its parts do what the physics
// says, and that it keeps invariant I5 -- the same bits on any thread count.
//
#include "pg/core/Parallel.h"
#include "pg/sim/Pyro.h"

#include "test_framework.h"

#include <cmath>
#include <cstring>

using namespace pg;
using namespace pg::sim;

namespace {

struct ThreadCountGuard {
    unsigned saved = TaskPool::instance().threadCount();
    ~ThreadCountGuard() { TaskPool::instance().setThreadCount(saved); }
};

bool sameBits(const Grid& a, const Grid& b) {
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}

/// Height of the centre of mass of a field, in domain units.
double centreHeight(const Grid& g, float cellSize) {
    double mass = 0.0, moment = 0.0;
    for (int k = 0; k < g.nz(); ++k) {
        for (int j = 0; j < g.ny(); ++j) {
            for (int i = 0; i < g.nx(); ++i) {
                mass += g.at(i, j, k);
                moment += g.at(i, j, k) * (j + 0.5) * cellSize;
            }
        }
    }
    return mass > 0.0 ? moment / mass : 0.0;
}

PyroSettings small(PyroSettings s, int resolution) {
    s.resolution = resolution;
    return s;
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

TEST(pyro_projection_makes_the_flow_divergence_free) {
    PyroSettings s = small(PyroSettings::smoke(), 16);
    s.pressureCycles = 10;
    PyroSolver sim(s);
    // A flow that is anything but incompressible: sources and sinks everywhere.
    for (int a = 0; a < 3; ++a) {
        Grid& v = sim.velocity(a);
        for (int k = 0; k < v.nz(); ++k) {
            for (int j = 0; j < v.ny(); ++j) {
                for (int i = 0; i < v.nx(); ++i) {
                    v.at(i, j, k) = std::sin(0.7f * i + 1.3f * a) * std::cos(0.5f * j) + 0.3f * std::sin(0.9f * k);
                }
            }
        }
    }
    const double before = sim.meanDivergence();
    sim.project();
    const double after = sim.meanDivergence();
    CHECK(before > 1.0);
    CHECK(after < before * 1e-3);
}

TEST(pyro_projection_converges_fast_with_the_default_cycles) {
    // The default budget, from a cold start: nearly all of the divergence goes
    // in one step, and the pressure carried over finishes the rest later.
    PyroSolver sim(small(PyroSettings::smoke(), 32));
    Grid& v = sim.velocity(1);
    for (int k = 0; k < v.nz(); ++k) {
        for (int j = 0; j < v.ny(); ++j) {
            for (int i = 0; i < v.nx(); ++i) {
                const float dx = (i - 16.0f) / 6.0f, dy = (j - 16.0f) / 6.0f, dz = (k - 16.0f) / 6.0f;
                v.at(i, j, k) = std::exp(-(dx * dx + dy * dy + dz * dz));  // an upward jet
            }
        }
    }
    const double before = sim.meanDivergence();
    sim.project();
    CHECK(sim.meanDivergence() < before * 0.03);
}

TEST(pyro_hot_smoke_rises) {
    PyroSolver sim(small(PyroSettings::smoke(), 24));
    const float source = sim.settings().sourceHeight;
    for (int f = 0; f < 15; ++f) sim.step();
    const double early = centreHeight(sim.density(), sim.cellSize());
    for (int f = 0; f < 15; ++f) sim.step();
    const double late = centreHeight(sim.density(), sim.cellSize());
    CHECK(sim.density().sum() > 0.0);
    CHECK(early > source);
    CHECK(late > early + 0.05);
    // And above the source the gas flows up.
    float v[3];
    sim.velocityAt(12.0f, 12.0f, 12.0f, v);
    CHECK(v[1] > 0.1f);
}

TEST(pyro_fire_burns_fuel_into_heat_and_soot) {
    PyroSettings s = small(PyroSettings::fire(), 24);
    PyroSolver sim(s);
    CHECK_EQ(s.smokeRate, 0.0f);  // the fire's smoke is all soot
    for (int f = 0; f < 20; ++f) sim.step();
    CHECK(sim.temperature().max() > 0.5f);
    CHECK(sim.density().sum() > 0.0);
    // Hotter than the source alone could make it: the fuel burns.
    PyroSettings noFuel = s;
    noFuel.fuelRate = 0.0f;
    PyroSolver cold(noFuel);
    for (int f = 0; f < 20; ++f) cold.step();
    CHECK(sim.temperature().sum() > 3.0 * cold.temperature().sum());
    CHECK_EQ(cold.density().sum(), 0.0);
}

TEST(pyro_is_bitwise_identical_across_thread_counts) {
    ThreadCountGuard guard;
    auto run = [](unsigned threads) {
        TaskPool::instance().setThreadCount(threads);
        PyroSolver sim(small(PyroSettings::fire(), 20));
        for (int f = 0; f < 12; ++f) sim.step();
        return sim;
    };
    const PyroSolver one = run(1);
    const PyroSolver four = run(4);
    CHECK(sameBits(one.density(), four.density()));
    CHECK(sameBits(one.temperature(), four.temperature()));
    CHECK(sameBits(one.fuel(), four.fuel()));
    CHECK(sameBits(one.flame(), four.flame()));
    for (int a = 0; a < 3; ++a) CHECK(sameBits(one.velocity(a), four.velocity(a)));
    CHECK(one.density().sum() > 0.0);
}

TEST(pyro_new_resolution_resets_the_domain) {
    PyroSolver sim(small(PyroSettings::smoke(), 16));
    for (int f = 0; f < 5; ++f) sim.step();
    CHECK(sim.density().sum() > 0.0);
    PyroSettings s = sim.settings();
    s.buoyancy = 2.0f;  // same resolution: carries on
    sim.setSettings(s);
    CHECK(sim.density().sum() > 0.0);
    s.resolution = 20;  // rounded up to a multiple of 8, for multigrid
    sim.setSettings(s);
    CHECK_EQ(sim.frame(), 0);
    CHECK_EQ(sim.density().nx(), 24);
    CHECK_EQ(sim.density().ny(), 36);
    CHECK_EQ(sim.velocity(0).nx(), 25);  // one more face than cells
    CHECK_EQ(sim.velocity(1).ny(), 37);
    CHECK_EQ(sim.density().sum(), 0.0);
}

TEST(pyro_multigrid_converges_at_any_resolution) {
    // What makes multigrid worth it: each V-cycle removes most of the error,
    // however fine the grid -- plain sweeps get slower the finer it is.
    for (const int n : {16, 32, 64}) {
        Grid b(n, n / 2 * 3, n), p(n, n / 2 * 3, n);
        for (int k = 0; k < b.nz(); ++k) {
            for (int j = 0; j < b.ny(); ++j) {
                for (int i = 0; i < b.nx(); ++i) b.at(i, j, k) = std::sin(0.37f * i * i + 1.1f * j) * std::cos(0.23f * k * j);
            }
        }
        const float h = 1.0f / static_cast<float>(n);
        PoissonSolver solver;
        solver.solve(p, b, h, 1);
        const double first = PoissonSolver::residual(p, b, h);
        solver.solve(p, b, h, 2);
        CHECK(PoissonSolver::residual(p, b, h) < first * 0.03);  // < 0.17 per cycle
        CHECK(solver.levels() >= 3);
    }
}

TEST(pyro_smoke_shadows_what_is_behind_it) {
    Grid density(8, 8, 8);
    for (int k = 0; k < 8; ++k) {
        for (int i = 0; i < 8; ++i) density.at(i, 5, k) = 4.0f;  // a slab at y = 5
    }
    const float up[3] = {0.0f, 1.0f, 0.0f};
    const Grid light = lightTransmittance(density, up, 0.5f, 1);
    CHECK(light.at(4, 1, 4) < 0.2f);   // below the slab: in its shadow
    CHECK(light.at(4, 7, 4) > 0.99f);  // above it: nothing between it and the light
    const Grid empty = lightTransmittance(Grid(8, 8, 8), up, 0.5f, 2);
    CHECK_EQ(empty.nx(), 4);
    CHECK_NEAR(empty.at(1, 1, 1), 1.0, 1e-6);
}
