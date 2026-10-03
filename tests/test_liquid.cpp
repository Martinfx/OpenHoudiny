//
// Water (src/pg/sim/Liquid.h, FreeSurface.h): the pressure of a free surface
// solved to its tolerance, and in pockets shut in by solids with more flowing
// in than out; still water stays still with the pressure of its
// depth; a dam breaks and the water keeps its volume; a ball of water falls
// as anything does; sources fill and pour; the water stays out of solids and
// goes where the sides are open; frames hold its surface; and invariant I5 --
// the same bits on any thread count.
//
#include "pg/core/Parallel.h"
#include "pg/sim/FreeSurface.h"
#include "pg/sim/Frame.h"
#include "pg/sim/Liquid.h"
#include "pg/sim/WaterMesh.h"

#include "test_framework.h"

#include <cmath>
#include <map>
#include <set>
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

/// The cells, surface and faces of a grid of n cells a side whose cells
/// below `level` are liquid: walls round it, open at the top.
struct Pool {
    std::vector<uint8_t> cells;
    Grid phi;
    Grid open[3];
};

Pool poolOf(int n, float level) {
    Pool p;
    p.cells.resize(static_cast<size_t>(n) * n * n);
    p.phi = Grid(n, n, n);
    for (int k = 0; k < n; ++k) {
        for (int j = 0; j < n; ++j) {
            for (int i = 0; i < n; ++i) {
                const float d = static_cast<float>(j) + 0.5f - level;
                p.phi.at(i, j, k) = d;
                p.cells[p.phi.index(i, j, k)] = d < 0.0f ? FreeSurfaceSolver::Liquid : FreeSurfaceSolver::Air;
            }
        }
    }
    for (int a = 0; a < 3; ++a) {
        Grid& open = p.open[a];
        open = Grid(n + (a == 0), n + (a == 1), n + (a == 2), 1.0f);
        for (int k = 0; k < open.nz(); ++k) {
            for (int j = 0; j < open.ny(); ++j) {
                for (int i = 0; i < open.nx(); ++i) {
                    const int f = a == 0 ? i : a == 1 ? j : k;
                    if (f == 0 || (a != 1 && f == n)) open.at(i, j, k) = 0.0f;  // floor and walls; the top is open
                }
            }
        }
    }
    return p;
}

/// The pressure system of poolOf(n, level).
void pool(FreeSurfaceSolver& solver, int n, float level) {
    const Pool p = poolOf(n, level);
    solver.setSystem(p.cells, p.open, p.phi);
}

/// Closes the faces round the cells from `lo` up to `hi`: what is in them
/// is shut in, as by solids round it.
void wallIn(Pool& pool, const int lo[3], const int hi[3]) {
    for (int a = 0; a < 3; ++a) {
        for (int k = lo[2]; k < hi[2] + (a == 2); ++k) {
            for (int j = lo[1]; j < hi[1] + (a == 1); ++j) {
                for (int i = lo[0]; i < hi[0] + (a == 0); ++i) {
                    const int f = a == 0 ? i : a == 1 ? j : k;
                    if (f == lo[a] || f == hi[a]) pool.open[a].at(i, j, k) = 0.0f;
                }
            }
        }
    }
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

TEST(liquid_pressure_of_a_pocket_shut_in_by_solids_stays_finite) {
    // The pool of 32 cells a side, 20 deep, with water shut in it: a box of
    // 48 cells and one of 2 -- water trapped under a crate, and between a
    // crate and the floor -- and a single cell with no open face, which
    // takes no part. More flows into the pockets than out of them, as when
    // a crate moves in: no pressure solves that, and the conjugate gradients
    // went off to 1e15 (the flood with the crates blew up so). Now what
    // flows in squeezes a pocket alike in all its cells, its pressure keeps
    // the level it came with, and the rest of the pool is solved as ever.
    ThreadCountGuard guard;
    const int n = 32;
    Pool water = poolOf(n, 19.7f);
    const int boxLo[3] = {4, 2, 4}, boxHi[3] = {8, 5, 8};
    const int gapLo[3] = {20, 0, 20}, gapHi[3] = {22, 1, 21};
    const int cellLo[3] = {26, 5, 26}, cellHi[3] = {27, 6, 27};
    wallIn(water, boxLo, boxHi);
    wallIn(water, gapLo, gapHi);
    wallIn(water, cellLo, cellHi);
    auto in = [](const int lo[3], const int hi[3], int i, int j, int k) {
        return i >= lo[0] && i < hi[0] && j >= lo[1] && j < hi[1] && k >= lo[2] && k < hi[2];
    };
    std::mt19937 rng(11);
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    Grid rhs(n, n, n), start(n, n, n);
    for (int k = 0; k < n; ++k) {
        for (int j = 0; j < 20; ++j) {
            for (int i = 0; i < n; ++i) {
                float& b = rhs.at(i, j, k);
                b = u(rng);
                if (in(boxLo, boxHi, i, j, k)) {
                    b += 0.5f;  // 24 more in than out
                    start.at(i, j, k) = 5.0f;
                } else if (in(gapLo, gapHi, i, j, k)) {
                    b += 0.8f;
                    start.at(i, j, k) = -2.0f;
                }
            }
        }
    }
    Grid p[2];
    for (int t = 0; t < 2; ++t) {
        TaskPool::instance().setThreadCount(t == 0 ? 1u : 4u);
        FreeSurfaceSolver solver;
        solver.setSystem(water.cells, water.open, water.phi);
        CHECK_EQ(solver.pockets(), size_t(2));
        CHECK_EQ(solver.unknowns(), size_t(n * n * 20 - 1));
        p[t] = start;
        const int iterations = solver.solve(p[t], rhs, 1e-5f, 100);
        CHECK(iterations > 0 && iterations <= 30);
        CHECK(solver.residual() <= 1e-5);
        // A p is the right-hand side, less each pocket's mean in it.
        Grid ap(n, n, n);
        solver.apply(p[t], ap);
        double inflow[2] = {0.0, 0.0}, level[2] = {0.0, 0.0};
        int cells[2] = {0, 0};
        for (int k = 0; k < n; ++k) {
            for (int j = 0; j < 20; ++j) {
                for (int i = 0; i < n; ++i) {
                    const int q = in(boxLo, boxHi, i, j, k) ? 0 : in(gapLo, gapHi, i, j, k) ? 1 : -1;
                    if (q < 0) continue;
                    inflow[q] += rhs.at(i, j, k);
                    level[q] += p[t].at(i, j, k);
                    ++cells[q];
                }
            }
        }
        double worst = 0.0, largest = 0.0, highest = 0.0;
        for (int k = 0; k < n; ++k) {
            for (int j = 0; j < 20; ++j) {
                for (int i = 0; i < n; ++i) {
                    if (in(cellLo, cellHi, i, j, k)) continue;
                    const int q = in(boxLo, boxHi, i, j, k) ? 0 : in(gapLo, gapHi, i, j, k) ? 1 : -1;
                    const double b = rhs.at(i, j, k) - (q < 0 ? 0.0 : inflow[q] / cells[q]);
                    worst = std::max(worst, std::fabs(b - ap.at(i, j, k)));
                    largest = std::max(largest, std::fabs(b));
                    highest = std::max(highest, static_cast<double>(std::fabs(p[t].at(i, j, k))));
                }
            }
        }
        CHECK(worst <= 2e-5 * largest);
        CHECK(std::isfinite(highest) && highest < 100.0);
        // The levels the pockets came with.
        CHECK(std::fabs(level[0] / cells[0] - 5.0) < 1e-4);
        CHECK(std::fabs(level[1] / cells[1] + 2.0) < 1e-4);
        // The cell with no open face holds no pressure.
        CHECK_EQ(p[t].at(cellLo[0], cellLo[1], cellLo[2]), 0.0f);
    }
    // Invariant I5: the same bits on one thread and on four.
    CHECK(std::memcmp(p[0].data(), p[1].data(), p[0].size() * sizeof(float)) == 0);
}

TEST(liquid_pressure_on_the_tiles_round_the_water_is_that_of_the_whole_grid) {
    // A pool in a corner of a closed tank, a solid block far from it: kept
    // in the tiles round the pool -- the block in its own, on every grid of
    // the multigrid -- the pressure is that of the whole grid, to the bit.
    const int n[3] = {64, 32, 48};
    const float far = 3.0f;
    auto inBlock = [](int i, int j, int k) { return i >= 40 && i < 56 && j < 16 && k >= 30 && k < 46; };
    // The tiles kept: the pool's and those round them.
    const int tn[3] = {n[0] / 8, n[1] / 8, n[2] / 8};
    std::vector<uint8_t> state(static_cast<size_t>(tn[0] * tn[1] * tn[2]), Tiles::Off);
    for (int c = 0; c < tn[2]; ++c) {
        for (int b = 0; b < tn[1]; ++b) {
            for (int a = 0; a < tn[0]; ++a) {
                if (a <= 3 && b <= 2 && c <= 3) state[static_cast<size_t>(a + tn[0] * (b + tn[1] * c))] = Tiles::Whole;
            }
        }
    }
    const auto tiles = std::make_shared<const Tiles>(n[0], n[1], n[2], state, -1);
    // The whole grid: the pool, air -- as far as the background past the
    // kept tiles -- the block, walls round the sides and the floor.
    std::vector<uint8_t> cells(static_cast<size_t>(n[0] * n[1] * n[2]), FreeSurfaceSolver::Air);
    Grid phi(n[0], n[1], n[2], far), open[3];
    for (int k = 0; k < n[2]; ++k) {
        for (int j = 0; j < n[1]; ++j) {
            for (int i = 0; i < n[0]; ++i) {
                const size_t c = phi.index(i, j, k);
                if (inBlock(i, j, k)) cells[c] = FreeSurfaceSolver::Solid;
                if (!tiles->has(i, j, k)) continue;
                const float d = std::max({static_cast<float>(i) - 19.5f, static_cast<float>(j) - 9.6f,
                                          static_cast<float>(k) - 19.5f});
                phi.at(i, j, k) = d;
                if (d < 0.0f) cells[c] = FreeSurfaceSolver::Liquid;
            }
        }
    }
    for (int a = 0; a < 3; ++a) {
        open[a] = Grid(n[0] + (a == 0), n[1] + (a == 1), n[2] + (a == 2), 1.0f);
        for (int k = 0; k < open[a].nz(); ++k) {
            for (int j = 0; j < open[a].ny(); ++j) {
                for (int i = 0; i < open[a].nx(); ++i) {
                    const int f = a == 0 ? i : a == 1 ? j : k;
                    const bool side = f == 0 || (f == n[a] && a != 1);
                    // The block's faces closed, a sliver of those round it.
                    const bool block = inBlock(i, j, k) || inBlock(i - (a == 0), j - (a == 1), k - (a == 2));
                    if (side || block) open[a].at(i, j, k) = 0.0f;
                    else if (inBlock(i + 1, j, k) || inBlock(i, j + 1, k)) open[a].at(i, j, k) = 0.4f;
                }
            }
        }
    }
    std::mt19937 rng(3);
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    Grid rhs(n[0], n[1], n[2]), whole(n[0], n[1], n[2]);
    for (size_t c = 0; c < rhs.size(); ++c) {
        if (cells[c] == FreeSurfaceSolver::Liquid) rhs.data()[c] = u(rng);
    }
    FreeSurfaceSolver dense;
    dense.setSystem(cells, open, phi);
    const int denseIterations = dense.solve(whole, rhs, 1e-5f, 100);
    CHECK(denseIterations > 0);

    // The same on the kept tiles: the walls as walls, the block on its own.
    const bool closed[6] = {true, true, true, false, true, true};
    SolidLevels solids(n[0], n[1], n[2], closed);
    std::vector<uint8_t> blockState(state.size(), Tiles::Off);
    for (int c = 3; c <= 5; ++c) {
        for (int b = 0; b <= 2; ++b) {
            for (int a = 4; a <= 7; ++a) blockState[static_cast<size_t>(a + tn[0] * (b + tn[1] * c))] = Tiles::Whole;
        }
    }
    const auto blockTiles = std::make_shared<const Tiles>(n[0], n[1], n[2], blockState, -1);
    std::vector<uint8_t> solid(blockTiles->stored().size() * Tiles::kCells, 0);
    forEachCounted(*blockTiles, [&](int i, int j, int k, size_t c) { solid[c] = inBlock(i, j, k) ? 1 : 0; });
    SparseGrid faces[3];
    for (int a = 0; a < 3; ++a) {
        faces[a] = SparseGrid(Tiles::faces(*blockTiles, a), 1.0f);
        forEachCounted(faces[a].tiles(), [&](int i, int j, int k, size_t f) { faces[a].data()[f] = open[a].at(i, j, k); });
    }
    solids.set(blockTiles, solid, faces);
    CHECK(!solids.solid(0, 1, 1, 1) && solids.solid(0, 45, 5, 35) && solids.solid(2, 11, 1, 9));
    CHECK_EQ(solids.open(0, 1, 3, 0, 3), 0.0f);  // the floor
    CHECK_EQ(solids.open(0, 1, 3, n[1], 3), 1.0f);  // the sky
    std::vector<uint8_t> kept(tiles->stored().size() * Tiles::kCells, FreeSurfaceSolver::Air);
    SparseGrid distance(tiles, far), b(tiles), p(tiles);
    forEachCounted(*tiles, [&](int i, int j, int k, size_t c) {
        kept[c] = cells[phi.index(i, j, k)];
        distance.data()[c] = phi.at(i, j, k);
        b.data()[c] = rhs.at(i, j, k);
    });
    FreeSurfaceSolver sparse;
    sparse.setSystem(tiles, kept, distance, solids);
    CHECK_EQ(sparse.levels(), dense.levels());
    CHECK_EQ(sparse.unknowns(), dense.unknowns());
    CHECK_EQ(sparse.solve(p, b, 1e-5f, 100), denseIterations);
    CHECK_EQ(sparse.residual(), dense.residual());
    int differ = 0;
    for (int k = 0; k < n[2]; ++k) {
        for (int j = 0; j < n[1]; ++j) {
            for (int i = 0; i < n[0]; ++i) {
                if (p.at(i, j, k) != whole.at(i, j, k)) ++differ;
            }
        }
    }
    CHECK_EQ(differ, 0);
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
    const SparseGrid& p = sim.pressure();
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
    CHECK_EQ(sim.open(1, mid, 4, mid), 0.0f);
    CHECK_EQ(sim.open(1, 1, 20, 1), 1.0f);
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

TEST(liquid_particles_keep_their_numbers_as_they_are_sorted_and_lost) {
    // A hose pours out over an open side: particles are made, sorted by cell
    // every step and lost. Each keeps its number -- near where it was the
    // frame before -- and a number is never given again.
    WaterSource hose = block(Vec3(0.3f, 0.2f, 0.0f), Vec3(0.2f, 0.2f, 0.2f));
    hose.mode = WaterMode::Flow;
    hose.velocity = Vec3(2.0f, 0.0f, 0.0f);
    LiquidScene s = tank(Vec3(1.0f, 1.0f, 0.5f), 24, hose);
    s.solver.closedSides = false;
    LiquidSolver sim(s);
    std::map<uint32_t, Vec3> before;
    std::set<uint32_t> gone;
    size_t followed = 0;
    bool lost = false;
    for (int f = 0; f < 30; ++f) {
        sim.step();
        const std::vector<uint32_t>& ids = sim.ids();
        CHECK_EQ(ids.size(), sim.particleCount());
        std::map<uint32_t, Vec3> now;
        for (size_t p = 0; p < ids.size(); ++p) {
            const Vec3 at = sim.positions()[p];
            CHECK(now.emplace(ids[p], at).second);  // one of each number
            CHECK(!gone.count(ids[p]));
            const auto was = before.find(ids[p]);
            if (was != before.end()) {
                CHECK(length(at - was->second) < 0.2f);  // a frame of flow: centimetres
                ++followed;
            }
        }
        for (const auto& [id, at] : before) {
            if (!now.count(id)) gone.insert(id);
        }
        lost = lost || !gone.empty();
        before = std::move(now);
    }
    CHECK(followed > 1000);
    CHECK(lost);
    // The frame keeps them, a number a particle.
    const WaterFrame w = capture(sim, true);
    CHECK(w.ids == sim.ids());
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
    CHECK(f.bytes() == f.cells.size() + f.flow.size() * sizeof(uint16_t));
}

TEST(liquid_frames_hold_how_fast_the_water_goes) {
    // A dam breaks: the frame's flow is the solver's velocity at the cells'
    // centres -- the mean of their faces' -- near the water, fast where it
    // falls; nothing far above it.
    LiquidScene s = tank(Vec3(1.0f, 2.0f, 1.0f), 32, block(Vec3(-0.25f, 0.25f, 0.0f), Vec3(0.5f, 0.5f, 1.0f)));
    LiquidSolver sim(s);
    for (int i = 0; i < 5; ++i) sim.step();
    const WaterFrame f = capture(sim);
    const Domain d = f.flowDomain();
    CHECK(d == sim.domain());
    // Sparse, the solver's tiles round the water alone.
    CHECK(f.hasFlow());
    CHECK(!f.flowTiles.empty());
    CHECK_EQ(f.flow.size(), 3 * Tiles::kCells * f.flowTiles.size());
    CHECK(f.flowTiles.size() < sim.tiles().tileCount());
    float fastest = 0.0f, worst = 0.0f;
    for (int k = 0; k < d.cells[2]; ++k) {
        for (int j = 0; j < d.cells[1]; ++j) {
            for (int i = 0; i < d.cells[0]; ++i) {
                const Vec3 centre = d.origin() + Vec3((static_cast<float>(i) + 0.5f) * d.voxel,
                                                      (static_cast<float>(j) + 0.5f) * d.voxel,
                                                      (static_cast<float>(k) + 0.5f) * d.voxel);
                const Vec3 mean(0.5f * (sim.velocity(0).at(i, j, k) + sim.velocity(0).at(i + 1, j, k)),
                                0.5f * (sim.velocity(1).at(i, j, k) + sim.velocity(1).at(i, j + 1, k)),
                                0.5f * (sim.velocity(2).at(i, j, k) + sim.velocity(2).at(i, j, k + 1)));
                const Vec3 v = f.flowAt(centre);
                // Near the surface -- within the band the frame's distance
                // has -- or in the water: the solver's.
                bool near = false;
                for (int q = 0; q < 8; ++q) {
                    near = near || f.distance(2 * i + (q & 1), 2 * j + ((q >> 1) & 1), 2 * k + ((q >> 2) & 1)) < f.band;
                }
                if (near) worst = std::max(worst, length(v - mean) / std::max(1.0f, length(mean)));
                fastest = std::max(fastest, length(v));
                if (j == d.cells[1] - 1) CHECK(v == Vec3());  // far above the water: still
            }
        }
    }
    CHECK(worst < 2e-3f);  // as a half float holds it
    CHECK(fastest > 0.5f);
    // Between the centres it blends; beyond the grid, the nearest cell's.
    const Vec3 a = f.flowAt(Vec3(-0.2f, 0.1f, 0.03f)), b = f.flowAt(Vec3(-0.2f, 0.1f, 0.09f));
    const Vec3 between = f.flowAt(Vec3(-0.2f, 0.1f, 0.06f));
    CHECK(length(between - (a + b) * 0.5f) < 0.5f * length(a - b) + 1e-3f);
    CHECK(f.flowAt(Vec3(-5.0f, 0.03f, 0.03f)) == f.flowAt(Vec3(-0.49f, 0.03f, 0.03f)));
}

TEST(liquid_wind_carries_spray_and_leaves_a_pond_level) {
    // A breeze over a pond, a few drops falling through it: the drops go
    // with the wind, the pond does not heap up against the far wall --
    // air, a thousandth as heavy as water, pushes on its surface alone.
    LiquidScene s = tank(Vec3(1.5f, 1.5f, 1.0f), 24, block(Vec3(0.0f, 0.1f, 0.0f), Vec3(1.5f, 0.2f, 1.0f)));
    WaterSource drops = block(Vec3(-0.4f, 1.3f, 0.0f), Vec3(0.08f, 0.08f, 0.08f));
    s.sources.push_back(drops);
    Force wind;
    wind.kind = ForceKind::Wind;
    wind.speed = 3.0f;
    wind.strength = 2.0f;
    s.forces.push_back(wind);
    LiquidSolver sim(s);
    // Falling: the drops take the wind's speed, as fast as it pulls.
    for (int f = 0; f < 12; ++f) sim.step();
    float spraySpeed = 0.0f;
    int spray = 0;
    for (size_t i = 0; i < sim.particleCount(); ++i) {
        if (sim.positions()[i].y < 0.4f) continue;
        spraySpeed += sim.velocities()[i].x;
        ++spray;
    }
    CHECK(spray > 0);
    const float pulled = wind.speed * (1.0f - std::exp(-wind.strength * 12.0f / 30.0f));
    if (spray > 0) CHECK(spraySpeed / static_cast<float>(spray) > 0.8f * pulled);
    // Seconds on: the pond as deep at its two ends -- the mean height of the
    // water within 20 cm of each wall, half its depth.
    for (int f = 12; f < 60; ++f) sim.step();
    double upwind = 0.0, downwind = 0.0;
    int up = 0, down = 0;
    for (const Vec3& p : sim.positions()) {
        if (p.y > 0.4f) continue;
        if (p.x < -0.55f) upwind += p.y, ++up;
        if (p.x > 0.55f) downwind += p.y, ++down;
    }
    CHECK(up > 0 && down > 0);
    upwind /= std::max(up, 1);
    downwind /= std::max(down, 1);
    CHECK(std::fabs(upwind - 0.1) < 0.01);
    CHECK(std::fabs(downwind - upwind) < 0.005);  // level: not the 25 cm a pushed pond heaps up
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

TEST(liquid_sparse_is_the_dense_water_to_the_bit) {
    // A dam in one end of a tall tank, a hose that pours later, a ball the
    // water flows past as it moves, a box away from the water, a wind and
    // whirls: kept in the tiles round the water, it is the water of every
    // tile -- particles, pressure, surface, level and frames alike.
    LiquidScene s = tank(Vec3(2.0f, 1.5f, 1.0f), 48, block(Vec3(-0.7f, 0.3f, 0.0f), Vec3(0.6f, 0.6f, 1.0f)));
    WaterSource hose = block(Vec3(0.6f, 1.1f, 0.2f), Vec3(0.1f, 0.1f, 0.1f));
    hose.shape = Shape::Cylinder;
    hose.mode = WaterMode::Flow;
    hose.velocity = Vec3(0.0f, -1.0f, 0.0f);
    hose.start = 0.1f;
    hose.end = 0.3f;
    s.sources.push_back(hose);
    Collider ball;
    ball.center = Vec3(-0.45f, 0.15f, 0.0f);
    ball.size = Vec3(0.2f);
    ball.velocity = Vec3(0.5f, 0.0f, 0.0f);
    s.colliders.push_back(ball);
    Collider box;
    box.shape = Shape::Box;
    box.center = Vec3(0.75f, 0.15f, -0.3f);
    box.size = Vec3(0.3f, 0.3f, 0.3f);
    s.colliders.push_back(box);
    Force wind;
    wind.kind = ForceKind::Wind;
    wind.speed = 3.0f;
    s.forces.push_back(wind);
    Force whirl;
    whirl.kind = ForceKind::Turbulence;
    whirl.strength = 1.0f;
    s.forces.push_back(whirl);
    LiquidScene all = s;
    all.solver.sparse = false;
    LiquidSolver sparse(s), dense(all);
    CHECK(dense.tiles().all());
    size_t fewest = dense.tiles().stored().size();
    for (int f = 0; f < 12; ++f) {
        sparse.step();
        dense.step();
        fewest = std::min(fewest, sparse.tiles().stored().size());
    }
    CHECK(fewest < dense.tiles().stored().size() / 2);  // the air above the dam is not kept
    const size_t n = sparse.particleCount();
    CHECK_EQ(n, dense.particleCount());
    CHECK(n > 0);
    CHECK(std::memcmp(sparse.positions().data(), dense.positions().data(), n * sizeof(Vec3)) == 0);
    CHECK(std::memcmp(sparse.velocities().data(), dense.velocities().data(), n * sizeof(Vec3)) == 0);
    CHECK(std::memcmp(sparse.foam().data(), dense.foam().data(), n * sizeof(float)) == 0);
    CHECK(sparse.ids() == dense.ids());
    // The grids, where the water is, and the surface everywhere -- as far as
    // the particles reach where no tile is kept.
    const Domain& d = sparse.domain();
    int differ = 0;
    for (int k = 0; k < d.cells[2]; ++k) {
        for (int j = 0; j < d.cells[1]; ++j) {
            for (int i = 0; i < d.cells[0]; ++i) {
                if (sparse.surface().at(i, j, k) != dense.surface().at(i, j, k)) ++differ;
                if (sparse.pressure().at(i, j, k) != dense.pressure().at(i, j, k)) ++differ;
            }
        }
    }
    CHECK_EQ(differ, 0);
    for (size_t p = 0; p < n; p += 7) {
        const Vec3& x = sparse.positions()[p];
        CHECK(sparse.velocityAt(x) == dense.velocityAt(x));
        CHECK_EQ(sparse.distanceToSurface(x + Vec3(0.0f, 0.3f, 0.0f)), dense.distanceToSurface(x + Vec3(0.0f, 0.3f, 0.0f)));
    }
    CHECK(sparse.waterLevel().height == dense.waterLevel().height);
    // The frames: the sparse one's tiles, the rest far air, still -- every
    // cell as the dense one's; and so the surface made of them.
    const WaterFrame a = capture(sparse), b = capture(dense);
    CHECK(!a.tiles.empty() && !a.flowTiles.empty());
    CHECK(b.tiles.empty() && b.flowTiles.empty());
    CHECK(a.cells.size() < b.cells.size() / 2);
    std::vector<uint8_t> cells;
    std::vector<uint16_t> flow;
    CHECK(a.denseCells(cells) == b.cells);
    CHECK(a.denseFlow(flow) == b.flow);
    CHECK_EQ(waterMesh(a)->hash(), waterMesh(b)->hash());
    CHECK(waterMesh(a)->pointCount() > 1000);
}

TEST(liquid_keeps_the_tiles_round_the_water_and_its_sources) {
    // A pool in a corner of a big tank, a fill source high above it that
    // starts later: the tiles kept are those of the water and the ones round
    // them -- not the air above it -- and the source's once it is about to
    // pour.
    WaterSource pool = block(Vec3(-1.6f, 0.15f, -0.6f), Vec3(0.5f, 0.3f, 0.5f));
    LiquidScene s = tank(Vec3(4.0f, 2.0f, 2.0f), 128, pool);
    WaterSource late = block(Vec3(1.5f, 1.6f, 0.6f), Vec3(0.2f, 0.2f, 0.2f));
    late.start = 0.5f;
    s.sources.push_back(late);
    LiquidSolver sim(s);
    // Before the first step: the pool's source.
    CHECK(sim.tiles().stored().size() > 0);
    CHECK(sim.tiles().stored().size() < sim.tiles().tileCount() / 10);
    sim.step();
    // Every particle in a kept tile, and every tile round its tile too.
    const Domain& d = sim.domain();
    const Vec3 o = d.origin();
    int outside = 0;
    for (const Vec3& p : sim.positions()) {
        const int c[3] = {static_cast<int>((p.x - o.x) / d.voxel), static_cast<int>((p.y - o.y) / d.voxel),
                          static_cast<int>((p.z - o.z) / d.voxel)};
        for (int q = 0; q < 27; ++q) {
            const int x = c[0] + 8 * (q % 3 - 1), y = c[1] + 8 * (q / 3 % 3 - 1), z = c[2] + 8 * (q / 9 - 1);
            if (x < 0 || y < 0 || z < 0 || x >= d.cells[0] || y >= d.cells[1] || z >= d.cells[2]) continue;
            if (!sim.tiles().has(x, y, z)) ++outside;
        }
    }
    CHECK_EQ(outside, 0);
    // The far corner and the air above the pool: not kept.
    CHECK(!sim.tiles().has(d.cells[0] - 1, d.cells[1] - 1, d.cells[2] - 1));
    CHECK(!sim.tiles().has(4, d.cells[1] - 4, 4));
    // The late source: kept from the step before it pours, and filled.
    const int si = static_cast<int>((1.5f - o.x) / d.voxel), sj = static_cast<int>((1.6f - o.y) / d.voxel),
              sk = static_cast<int>((0.6f - o.z) / d.voxel);
    CHECK(!sim.tiles().has(si, sj, sk));
    const size_t before = sim.particleCount();
    while (sim.time() < 0.55f) sim.step();  // a step from 0.5 on: it pours
    CHECK(sim.particleCount() > before);
    CHECK(sim.tiles().has(si, sj - 8, sk));  // its water, falling
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
    CHECK(safe.solver.resolution <= 1024);
    CHECK(safe.solver.timeStep > 0.0f);
    CHECK(safe.solver.flip >= 0.0f && safe.solver.flip <= 1.0f);
    CHECK(safe.solver.size.x > 0.0f && safe.solver.size.z <= 1000.0f);
    CHECK(safe.sources[0].size.x > 0.0f);
    CHECK(std::isfinite(safe.sources[0].velocity.x) && safe.sources[0].velocity.y <= 1000.0f);
    // And it runs -- at a resolution a test can afford.
    s.solver.resolution = 48;
    LiquidSolver sim(s);
    sim.step();
    CHECK(std::isfinite(sim.maxSpeed()));
}
