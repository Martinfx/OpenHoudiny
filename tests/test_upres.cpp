//
// Pyro Upres (src/pg/sim/Upres.h): the gas of a Pyro Solver again, on a
// finer grid -- that its grid is the solver's made finer, that its gas
// follows the solver's, that it whirls where the solver's flow swirls and
// stays calm where it is calm, that it keeps out of solids, and that it
// keeps invariant I5 -- the same bits on any thread count. And the lookups
// it reads the grids with: to the bit as sample().
//
#include "pg/core/Parallel.h"
#include "pg/sim/Cache.h"
#include "pg/sim/Frame.h"
#include "pg/sim/Pyro.h"
#include "pg/sim/Upres.h"
#include "pg/sim/World.h"

#include "test_framework.h"

#include <cmath>
#include <cstring>
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

/// A world of `scene`'s gas at `resolution`, made `scale` times finer.
World upresWorld(Scene scene, int resolution, int scale, float turbulence) {
    World w;
    w.hasGas = true;
    scene.solver.resolution = resolution;
    w.gas = scene;
    w.hasUpres = true;
    w.upres.scale = scale;
    w.upres.turbulence = turbulence;
    return w;
}

/// f(i, j, k, value) for every stored cell of a grid.
template <class F>
void eachCell(const SparseGrid& g, const F& f) {
    const Tiles& t = g.tiles();
    for (size_t s = 0; s < t.stored().size(); ++s) {
        int c[3];
        t.corner(t.stored()[s], c[0], c[1], c[2]);
        for (int z = 0; z < Tiles::kSide; ++z) {
            for (int y = 0; y < Tiles::kSide; ++y) {
                for (int x = 0; x < Tiles::kSide; ++x) {
                    f(c[0] + x, c[1] + y, c[2] + z, g.data()[s * Tiles::kCells + SparseGrid::local(x, y, z)]);
                }
            }
        }
    }
}

/// How much of a field there is: its sum times the volume of a cell.
double amount(const SparseGrid& g, const Domain& d) {
    return g.sum() * static_cast<double>(d.voxel) * d.voxel * d.voxel;
}

/// Centre of a field's mass along `axis`, world units.
double centreOf(const SparseGrid& g, const Domain& d, int axis) {
    double mass = 0.0, moment = 0.0;
    eachCell(g, [&](int i, int j, int k, float v) {
        const int at[3] = {i, j, k};
        mass += v;
        moment += v * (d.origin()[axis] + (static_cast<double>(at[axis]) + 0.5) * d.voxel);
    });
    return mass > 0.0 ? moment / mass : 0.0;
}

/// How much fine detail a field has: the mean square of its differences
/// from cell to cell, over the mean square of the field.
double detail(const SparseGrid& g) {
    double differences = 0.0, values = 0.0;
    eachCell(g, [&](int i, int j, int k, float v) {
        values += static_cast<double>(v) * v;
        if (i + 1 < g.nx()) differences += std::pow(g.at(i + 1, j, k) - v, 2.0);
        if (j + 1 < g.ny()) differences += std::pow(g.at(i, j + 1, k) - v, 2.0);
        if (k + 1 < g.nz()) differences += std::pow(g.at(i, j, k + 1) - v, 2.0);
    });
    return values > 0.0 ? differences / values : 0.0;
}

}  // namespace

TEST(upres_grid_is_the_solvers_made_finer) {
    Domain coarse;
    coarse.cells[0] = 48;
    coarse.cells[1] = 64;
    coarse.cells[2] = 40;
    coarse.voxel = 0.03f;
    for (int scale = 2; scale <= 4; ++scale) {
        UpresSettings u;
        u.scale = scale;
        const Domain d = u.domain(coarse);
        for (int a = 0; a < 3; ++a) {
            CHECK_EQ(d.cells[a], coarse.cells[a] * scale);
            CHECK_NEAR(d.size()[a], coarse.size()[a], 1e-5f);
        }
        CHECK_NEAR(d.voxel * static_cast<float>(scale), coarse.voxel, 1e-7f);
    }
    // Out of range: 2 to 4.
    UpresSettings u;
    u.scale = 9;
    CHECK_EQ(u.sanitized().scale, 4);
    u.scale = 0;
    CHECK_EQ(u.sanitized().scale, 2);
    // No finer than 2048 cells along the longest side -- 2 at least.
    u.scale = 4;
    coarse.cells[1] = 600;
    CHECK_EQ(u.domain(coarse).cells[1], 1800);
    coarse.cells[1] = 1024;
    CHECK_EQ(u.domain(coarse).cells[1], 2048);
    // Nothing that is not a number gets through.
    u.turbulence = std::nanf("");
    u.swirlSize = -1.0f;
    u.swirlLife = 0.0f;
    const UpresSettings safe = u.sanitized();
    CHECK_EQ(safe.turbulence, 1.0f);
    CHECK(safe.swirlSize > 0.0f);
    CHECK(safe.swirlLife > 0.0f);
}

TEST(upres_follows_the_solvers_gas) {
    // Without whirls, the fine smoke is the solver's carried finer: as much
    // of it, where the solver's is.
    WorldSolver world(upresWorld(Scene::smoke(), 32, 2, 0.0f));
    for (int f = 0; f < 30; ++f) world.step();
    const PyroSolver& gas = *world.gas();
    const UpresSolver& up = *world.upres();
    CHECK_EQ(up.domain().cells[0], 2 * gas.domain().cells[0]);
    CHECK_EQ(up.frame(), 30);
    CHECK_EQ(up.time(), gas.time());
    const double coarse = amount(gas.density(), gas.domain()), fine = amount(up.density(), up.domain());
    CHECK(coarse > 0.0);
    CHECK(fine > 0.85 * coarse && fine < 1.15 * coarse);
    for (int a = 0; a < 3; ++a) {
        CHECK(std::fabs(centreOf(up.density(), up.domain(), a) - centreOf(gas.density(), gas.domain(), a)) <
              0.75 * gas.domain().voxel);
    }
    // Sparse: the fine tiles round the smoke, not the whole grid.
    CHECK(up.activeCells() < up.domain().cellCount() / 2);
    // The frames keep it: its grid, its tiles.
    const Frame f = world.capture();
    CHECK(f.domain == up.domain());
    CHECK(!f.gasTiles.empty());
    CHECK_EQ(f.number, 30);
    double kept = 0.0;
    for (size_t c = 0; c < f.fields.size(); c += 3) kept += fromHalf(f.fields[c]);
    CHECK_NEAR(kept, up.density().sum(), 1e-3 * up.density().sum());
}

TEST(upres_whirls_where_the_flow_swirls) {
    // The whirls make the fire finer, cell to cell -- and leave as much of
    // it, where it was.
    auto run = [](float turbulence) {
        auto world = std::make_unique<WorldSolver>(upresWorld(Scene::fire(), 32, 2, turbulence));
        for (int f = 0; f < 40; ++f) world->step();
        return world;
    };
    const auto calm = run(0.0f);
    const auto whirled = run(1.0f);
    const UpresSolver& a = *calm->upres();
    const UpresSolver& b = *whirled->upres();
    CHECK(detail(b.density()) > 1.3 * detail(a.density()));
    CHECK(detail(b.flame()) > 1.3 * detail(a.flame()));
    const double before = amount(a.density(), a.domain()), after = amount(b.density(), b.domain());
    CHECK(after > 0.8 * before && after < 1.2 * before);
    CHECK(std::fabs(centreOf(b.density(), b.domain(), 1) - centreOf(a.density(), a.domain(), 1)) <
          2.0 * calm->gas()->domain().voxel);
    // The whirls themselves: there, and as strong as the flow's shear makes
    // them -- some centimetres a second on the whole, never faster than the
    // solver's flow at its fastest.
    double sum = 0.0, most = 0.0;
    size_t cells = 0;
    eachCell(b.density(), [&](int i, int j, int k, float) {
        const Vec3 t = b.turbulenceAt(i, j, k);
        const double s = std::sqrt(static_cast<double>(dot(t, t)));
        sum += s;
        most = std::max(most, s);
        ++cells;
    });
    CHECK(cells > 0);
    CHECK(sum / static_cast<double>(cells) > 0.01 && sum / static_cast<double>(cells) < 0.2);
    float fastest = 0.0f;
    for (int a = 0; a < 3; ++a) {
        for (const float v : whirled->gas()->velocity(a).values()) fastest = std::max(fastest, std::fabs(v));
    }
    CHECK(most < static_cast<double>(fastest));
}

TEST(upres_stays_calm_where_the_flow_is_calm) {
    // Smoke that does not move -- no lift, no push, no forces -- has no
    // swirl to whirl: with or without Turbulence, the same bits.
    Scene still = Scene::smoke();
    still.forces.clear();
    still.solver.buoyancy = 0.0f;
    still.solver.weight = 0.0f;
    still.solver.vorticity = 0.0f;
    still.emitters[0].velocity = Vec3();
    still.emitters[0].flicker = 0.0f;
    WorldSolver calm(upresWorld(still, 32, 2, 0.0f)), whirled(upresWorld(still, 32, 2, 2.0f));
    for (int f = 0; f < 12; ++f) {
        calm.step();
        whirled.step();
    }
    CHECK(whirled.upres()->density().sum() > 0.0);
    CHECK(sameBits(calm.upres()->density(), whirled.upres()->density()));
    const Vec3 t = whirled.upres()->turbulenceAt(32, 12, 32);
    CHECK_EQ(dot(t, t), 0.0f);
}

TEST(upres_keeps_the_gas_out_of_solids) {
    // A ball over the smoke: the fine smoke goes round it, none inside.
    Scene s = Scene::smoke();
    Collider ball;
    ball.shape = Shape::Sphere;
    ball.center = Vec3(0.0f, 0.55f, 0.0f);
    ball.size = Vec3(0.36f);
    s.colliders.push_back(ball);
    WorldSolver world(upresWorld(s, 32, 2, 1.0f));
    for (int f = 0; f < 36; ++f) world.step();
    const UpresSolver& up = *world.upres();
    const Domain& d = up.domain();
    double inside = 0.0, around = 0.0;
    size_t cellsInside = 0;
    eachCell(up.density(), [&](int i, int j, int k, float v) {
        const Vec3 p = d.origin() + Vec3(static_cast<float>(i) + 0.5f, static_cast<float>(j) + 0.5f,
                                         static_cast<float>(k) + 0.5f) * d.voxel;
        if (ball.contains(p)) {
            inside += v;
            ++cellsInside;
        } else if (p.y > 0.3f) {
            around += v;
        }
    });
    CHECK(cellsInside > 100);  // the ball's cells are worked on: the smoke reaches it
    CHECK_EQ(inside, 0.0);
    CHECK(around > 0.0);
}

TEST(upres_is_bitwise_identical_across_thread_counts) {
    // A moving source, a collider, the whirls.
    Scene s = Scene::fire();
    Emitter box = s.emitters[0];
    box.shape = Shape::Box;
    box.center = Vec3(0.2f, 0.1f, 0.1f);
    box.motion = Motion::Circle;
    box.smoke = 2.0f;
    s.emitters.push_back(box);
    Collider c;
    c.shape = Shape::Box;
    c.center = Vec3(-0.2f, 0.6f, 0.0f);
    s.colliders.push_back(c);
    ThreadCountGuard guard;
    auto run = [&](unsigned threads) {
        TaskPool::instance().setThreadCount(threads);
        auto world = std::make_unique<WorldSolver>(upresWorld(s, 24, 3, 1.5f));
        for (int f = 0; f < 10; ++f) world->step();
        return world;
    };
    const auto one = run(1);
    const auto four = run(4);
    const UpresSolver& a = *one->upres();
    const UpresSolver& b = *four->upres();
    CHECK(a.tiles() == b.tiles());
    CHECK(sameBits(a.density(), b.density()));
    CHECK(sameBits(a.temperature(), b.temperature()));
    CHECK(sameBits(a.fuel(), b.fuel()));
    CHECK(sameBits(a.flame(), b.flame()));
    CHECK(a.density().sum() > 0.0);
    CHECK(formatFrame(one->capture()) == formatFrame(four->capture()));
}

TEST(upres_lookups_sample_to_the_bit) {
    // One lookup, many grids of its tiles: each as sample() reads it --
    // inside a tile, across tiles, past the sides, by tiles not kept.
    const int n[3] = {20, 12, 17};
    std::vector<uint8_t> state(3 * 2 * 3, Tiles::Whole);
    state[4] = Tiles::Off;
    auto tiles = std::make_shared<const Tiles>(n[0], n[1], n[2], state, -1);
    SparseGrid a(tiles, 0.25f), b(tiles);
    uint32_t h = 777u;
    auto next = [&] {
        h = h * 1664525u + 1013904223u;
        return static_cast<float>(h >> 8) / static_cast<float>(1u << 24);
    };
    for (size_t i = 0; i < a.size(); ++i) {
        a.data()[i] = next() * 4.0f - 1.0f;
        b.data()[i] = next();
    }
    for (int t = 0; t < 4000; ++t) {
        const float x = next() * 24.0f - 2.0f, y = next() * 16.0f - 2.0f, z = next() * 21.0f - 2.0f;
        SparseGrid::Corners at;
        a.cornersAt(x, y, z, at);
        float lo0, hi0, lo1, hi1;
        const float s0 = a.sample(x, y, z, false, lo0, hi0);
        const float s1 = a.sampleAt(at, lo1, hi1);
        CHECK(s0 == s1 && lo0 == lo1 && hi0 == hi1);
        CHECK(b.sample(x, y, z) == b.sampleAt(at));
    }
}
