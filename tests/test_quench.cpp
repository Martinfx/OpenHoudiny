//
// Water and fire (PyroSolver::setWater, World): water in the gas cools it,
// soaks the fuel and makes steam -- a field of its own, which rises and
// thins out; a source it falls on soaks and stays out; without water, or with Quench 0, nothing changes, to the bit; a
// downpour puts out a campfire, a bucket of water douses it. Rain fills the
// water it falls into as fast as Fill says, and fast drops land in shallow
// water rather than through it.
//
#include "pg/core/Parallel.h"
#include "pg/sim/Frame.h"
#include "pg/sim/Liquid.h"
#include "pg/sim/Network.h"
#include "pg/sim/Pyro.h"
#include "pg/sim/Rain.h"
#include "pg/sim/World.h"

#include "test_framework.h"

#include <cmath>
#include <cstring>

using namespace pg;
using namespace pg::sim;

namespace {

/// The campfire at a coarse grid: quick to step.
Scene smallFire() {
    Scene s = Scene::fire();
    s.solver.resolution = 48;
    return s;
}

/// Points 4 to a cell over a box: the water of a Liquid Solver at h.
PyroSolver::Water waterIn(const Vec3& lo, const Vec3& hi, float h) {
    PyroSolver::Water w;
    w.particleVolume = 0.125f * h * h * h;
    for (float z = lo.z; z < hi.z; z += 0.5f * h) {
        for (float y = lo.y; y < hi.y; y += 0.5f * h) {
            for (float x = lo.x; x < hi.x; x += 0.5f * h) w.particles.emplace_back(x, y, z);
        }
    }
    return w;
}

World exampleWorld(const char* name) {
    Network net;
    std::string error;
    CHECK(Network::load(Network::exampleText(name), net, error));
    const Compiled c = net.compile(PG_SIM_EXAMPLES_DIR);
    CHECK(c.ok);
    return c.world;
}

bool sameBits(const SparseGrid& a, const SparseGrid& b) {
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}

}  // namespace

TEST(water_in_the_gas_cools_it_soaks_the_fuel_and_makes_steam) {
    PyroSolver dry(smallFire()), wet(smallFire());
    for (int f = 0; f < 20; ++f) {
        dry.step();
        wet.step();
    }
    // Water over the fire and a little round it.
    const PyroSolver::Water water = waterIn(Vec3(-0.2f, 0.0f, -0.2f), Vec3(0.2f, 0.5f, 0.2f), 0.02f);
    for (int f = 0; f < 3; ++f) {
        dry.step();
        wet.setWater(water);
        wet.step();
    }
    CHECK(wet.wetCells() > 1000);
    CHECK(wet.temperature().sum() < 0.6 * dry.temperature().sum());
    CHECK(wet.flame().sum() < 0.1 * dry.flame().sum());
    CHECK(wet.fuel().sum() < 0.1 * dry.fuel().sum());
    // The heat it took is steam: a field of its own, not smoke.
    CHECK(wet.steamy());
    CHECK(!dry.steamy());
    CHECK(wet.steam().sum() > 0.3 * (dry.temperature().sum() - wet.temperature().sum()));
    CHECK(wet.density().sum() < 1.05 * dry.density().sum());
    // ... which a frame of it holds; one of the dry fire none.
    const Frame wetFrame = capture(wet), dryFrame = capture(dry);
    CHECK(!wetFrame.steam.empty() && wetFrame.steamFits());
    CHECK(dryFrame.steam.empty());
    // The source under it is soaked: it gives a share of what it did.
    CHECK_EQ(wet.soaked().size(), size_t(1));
    CHECK(wet.soaked()[0] > 1.0f);
    CHECK(dry.soaked().empty());
}

TEST(steam_rises_and_thins_out) {
    // A douse of the fire, then none: the steam it made goes up -- lighter
    // than the air -- and fades as Steam Fade says; with Steam 0 the water
    // makes none.
    PyroSolver fire(smallFire());
    for (int f = 0; f < 20; ++f) fire.step();
    const PyroSolver::Water water = waterIn(Vec3(-0.2f, 0.0f, -0.2f), Vec3(0.2f, 0.4f, 0.2f), 0.02f);
    for (int f = 0; f < 2; ++f) {
        fire.setWater(water);
        fire.step();
    }
    auto heightOf = [&](const SparseGrid& g) {
        double sum = 0.0, weighted = 0.0;
        for (int k = 0; k < fire.nz(); ++k) {
            for (int j = 0; j < fire.ny(); ++j) {
                for (int i = 0; i < fire.nx(); ++i) {
                    const double v = g.at(i, j, k);
                    sum += v;
                    weighted += v * fire.worldAt(0.0f, static_cast<float>(j) + 0.5f, 0.0f).y;
                }
            }
        }
        return sum > 0.0 ? weighted / sum : 0.0;
    };
    const double made = fire.steam().sum(), low = heightOf(fire.steam());
    CHECK(made > 0.0);
    for (int f = 0; f < 30; ++f) {
        fire.setWater(PyroSolver::Water());
        fire.step();
    }
    // A second on: well up, and thinned -- by some half (e^-0.7), less what
    // carrying it on a grid this coarse makes of it.
    CHECK(heightOf(fire.steam()) > low + 0.5);
    CHECK(fire.steam().sum() < 0.7 * made);
    CHECK(fire.steam().sum() > 0.05 * made);
    // Steam 0: the water cools as before, and makes no steam.
    Scene none = smallFire();
    none.solver.steam = 0.0f;
    PyroSolver plain(none);
    for (int f = 0; f < 20; ++f) plain.step();
    for (int f = 0; f < 2; ++f) {
        plain.setWater(water);
        plain.step();
    }
    CHECK(!plain.steamy());
    CHECK_EQ(plain.steam().sum(), 0.0);
}

TEST(the_fire_boils_away_the_water_in_it) {
    // A handful of water dropped into the flames of a burning fire -- too
    // little to put it out: much of it boils away on the way down; with
    // Evaporate 0 none does. The drops of a downpour through it, the same.
    // The same on one thread and on four.
    auto world = [](float evaporate) {
        World w;
        w.hasGas = true;
        w.gas = smallFire();
        w.gas.solver.evaporate = evaporate;
        w.hasWater = true;
        w.water.solver.size = Vec3(1.0f, 1.2f, 1.0f);
        w.water.solver.resolution = 32;
        WaterSource blob;
        blob.center = Vec3(0.0f, 0.55f, 0.0f);
        blob.size = Vec3(0.08f, 0.08f, 0.08f);
        blob.start = 0.8f;  // when the fire is burning
        w.water.sources.push_back(blob);
        w.keepParticles = true;
        return w;
    };
    auto run = [](const World& w, int frames) {
        WorldSolver s(w);
        for (int f = 0; f < frames; ++f) s.step();
        return s.capture();
    };
    const Frame boiled = run(world(1.0f), 45), kept = run(world(0.0f), 45);
    CHECK(kept.water.particles > 50);
    CHECK(boiled.water.particles < kept.water.particles * 7 / 10);
    // The same particles boil away on one thread as on many.
    const unsigned saved = TaskPool::instance().threadCount();
    TaskPool::instance().setThreadCount(1);
    const Frame one = run(world(1.0f), 45);
    TaskPool::instance().setThreadCount(saved);
    CHECK_EQ(one.water.particles, boiled.water.particles);
    CHECK(one.water.positions == boiled.water.positions);
    // Rain -- drops that fall through the flames in a twentieth of a second:
    // few boil away, more as Evaporate is higher.
    auto rainy = [](float evaporate) {
        World w;
        w.hasGas = true;
        w.gas = smallFire();
        w.gas.solver.evaporate = evaporate;
        w.gas.solver.quench = 0.0f;  // the fire burns on: only the boiling counts
        w.hasRain = true;
        w.rain.rain.center = Vec3(0.0f, 1.6f, 0.0f);
        w.rain.rain.size = Vec3(0.3f, 0.1f, 0.3f);
        w.rain.rain.rate = 3000.0f;
        return w;
    };
    // ... those that got through the flames: low over the fire.
    auto under = [](const Frame& f) {
        size_t n = 0;
        for (size_t d = 0; d < f.rain.dropCount(); ++d) {
            const float* q = f.rain.drops.data() + 6 * d;
            n += q[1] < 0.35f && std::fabs(q[0]) < 0.15f && std::fabs(q[2]) < 0.15f ? 1 : 0;
        }
        return n;
    };
    size_t dry = 0, wet = 0;
    WorldSolver boiling(rainy(8.0f)), raining(rainy(0.0f));
    for (int f = 0; f < 60; ++f) {
        boiling.step();
        raining.step();
        if (f >= 20) {
            dry += under(boiling.capture());
            wet += under(raining.capture());
        }
    }
    CHECK(wet > 50);
    CHECK(dry < wet * 8 / 10);
}

TEST(a_soaked_fire_stays_out_when_the_water_has_gone) {
    PyroSolver fire(smallFire()), doused(smallFire());
    for (int f = 0; f < 10; ++f) {
        fire.step();
        doused.step();
    }
    const PyroSolver::Water water = waterIn(Vec3(-0.15f, 0.0f, -0.15f), Vec3(0.15f, 0.3f, 0.15f), 0.02f);
    for (int f = 0; f < 10; ++f) {
        fire.step();
        doused.setWater(water);
        doused.step();
    }
    // The water gone, the fire burns on, the soaked one does not catch again.
    for (int f = 0; f < 20; ++f) {
        fire.step();
        doused.setWater(PyroSolver::Water());
        doused.step();
    }
    CHECK_EQ(doused.wetCells(), size_t(0));
    CHECK(doused.flame().sum() < 0.05 * fire.flame().sum());
}

TEST(no_water_or_quench_0_change_nothing_to_the_bit) {
    Scene off = smallFire();
    off.solver.quench = 0.0f;
    PyroSolver plain(smallFire()), empty(smallFire()), unquenched(off);
    const PyroSolver::Water water = waterIn(Vec3(-0.2f, 0.0f, -0.2f), Vec3(0.2f, 0.5f, 0.2f), 0.02f);
    for (int f = 0; f < 12; ++f) {
        plain.step();
        empty.setWater(PyroSolver::Water());
        empty.step();
        unquenched.setWater(water);
        unquenched.step();
    }
    for (const PyroSolver* s : {&empty, &unquenched}) {
        CHECK(sameBits(plain.density(), s->density()));
        CHECK(sameBits(plain.temperature(), s->temperature()));
        CHECK(sameBits(plain.flame(), s->flame()));
    }
    // And a world with gas and nothing wet in it is the gas alone.
    World w;
    w.hasGas = true;
    w.gas = smallFire();
    World rained = w;
    rained.hasRain = true;
    rained.rain.rain.center = Vec3(3.0f, 2.5f, 0.0f);  // beside the gas: no drop gets into it
    rained.rain.rain.size = Vec3(0.5f, 0.2f, 0.5f);
    WorldSolver a(w), b(rained);
    for (int f = 0; f < 10; ++f) {
        a.step();
        b.step();
    }
    CHECK(sameBits(a.gas()->density(), b.gas()->density()));
}

TEST(a_downpour_puts_out_a_campfire_and_a_bucket_douses_it) {
    // The campfire in the rain, against the same fire without it.
    const World rain = preview(exampleWorld("campfire_rain"), 0.5f);
    World dry = rain;
    dry.hasRain = false;
    WorldSolver rained(rain), burning(dry);
    for (int f = 0; f < 150; ++f) {
        rained.step();
        burning.step();
    }
    CHECK(rained.gas()->soaked()[0] > 2.0f);
    CHECK(rained.gas()->flame().sum() < 0.2 * burning.gas()->flame().sum());
    // A ball of water dropped on it: out in a few frames.
    const World douse = preview(exampleWorld("fire_douse"), 0.5f);
    WorldSolver doused(douse);
    double before = 0.0;
    for (int f = 1; f <= 90; ++f) {
        doused.step();
        if (f == 45) before = doused.gas()->flame().sum();
    }
    CHECK(before > 0.0);
    CHECK(doused.gas()->flame().sum() < 0.1 * before);
}

TEST(rain_fills_the_water_as_fast_as_fill_says) {
    World w;
    w.hasWater = true;
    w.water.solver.size = Vec3(1.0f, 0.4f, 1.0f);
    w.water.solver.resolution = 40;
    WaterSource pool;
    pool.center = Vec3(0.0f, 0.03f, 0.0f);
    pool.size = Vec3(1.0f, 0.06f, 1.0f);
    w.water.sources.push_back(pool);
    w.hasRain = true;
    w.rain.rain.center = Vec3(0.0f, 1.0f, 0.0f);
    w.rain.rain.size = Vec3(0.8f, 0.2f, 0.8f);
    w.rain.rain.rate = 1500.0f;
    w.rain.rain.fill = 20.0f;
    World dry = w;
    dry.rain.rain.fill = 0.0f;
    WorldSolver filling(w), raining(dry);
    for (int f = 0; f < 15; ++f) {
        filling.step();
        raining.step();
    }
    const double start = filling.water()->volume(), dryStart = raining.water()->volume();
    const int frames = 45;
    for (int f = 0; f < frames; ++f) {
        filling.step();
        raining.step();
    }
    // 20 mm a second over the 0.64 m^2 under the cloud, in litres.
    const double expected = 20.0 * 0.64 * frames / 30.0;
    const double added = filling.water()->volume() - start;
    CHECK(added > 0.8 * expected);
    CHECK(added < 1.1 * expected);
    // Without Fill the drops ring the water and add nothing.
    CHECK(std::fabs(raining.water()->volume() - dryStart) < 1e-9);
    // The water rose: on top, not packed into the cells it filled.
    const WaterLevel before = raining.water()->waterLevel(), after = filling.water()->waterLevel();
    CHECK(after.at(0.0f, 0.0f) - before.at(0.0f, 0.0f) > 0.5f * 0.02f * frames / 30.0f);
}

TEST(fast_drops_land_in_shallow_water_not_through_it) {
    World w;
    w.hasWater = true;
    w.water.solver.size = Vec3(1.0f, 0.3f, 1.0f);
    w.water.solver.resolution = 32;
    WaterSource pool;
    pool.center = Vec3(0.0f, 0.03f, 0.0f);
    pool.size = Vec3(1.0f, 0.06f, 1.0f);  // a drop falls further than that in a frame
    w.water.sources.push_back(pool);
    w.hasRain = true;
    w.rain.rain.center = Vec3(0.0f, 1.5f, 0.0f);
    w.rain.rain.size = Vec3(0.6f, 0.2f, 0.6f);
    w.rain.rain.speed = 9.0f;
    WorldSolver s(w);
    int water = 0, solid = 0;
    for (int f = 0; f < 30; ++f) {
        s.step();
        water += s.rain()->lastSplashes();
        solid += s.rain()->lastLandings();
    }
    CHECK(water > 100);
    CHECK(solid * 20 < water);
}

TEST(network_quench_steam_and_fill_reach_the_solvers) {
    Network net;
    std::string error;
    CHECK(Network::load(Network::exampleText("campfire_rain"), net, error));
    for (const Node& n : std::vector<Node>(net.nodes())) {
        if (n.type == "pyro_solver") {
            net.setParam(n.id, "quench", {2.0f, 0.0f, 0.0f});
            net.setParam(n.id, "steam", {0.5f, 0.0f, 0.0f});
            net.setParam(n.id, "steam_lift", {2.5f, 0.0f, 0.0f});
            net.setParam(n.id, "steam_fade", {0.25f, 0.0f, 0.0f});
            net.setParam(n.id, "evaporate", {3.0f, 0.0f, 0.0f});
        }
        if (n.type == "volume_look") {
            net.setParam(n.id, "steam_color", {0.8f, 0.85f, 0.9f});
            net.setParam(n.id, "steam_density", {12.0f, 0.0f, 0.0f});
        }
        if (n.type == "rain") net.setParam(n.id, "fill", {12.0f, 0.0f, 0.0f});
    }
    const Compiled c = net.compile(PG_SIM_EXAMPLES_DIR);
    CHECK(c.ok);
    CHECK_EQ(c.world.gas.solver.quench, 2.0f);
    CHECK_EQ(c.world.gas.solver.steam, 0.5f);
    CHECK_EQ(c.world.gas.solver.steamLift, 2.5f);
    CHECK_EQ(c.world.gas.solver.steamFade, 0.25f);
    CHECK_EQ(c.world.gas.solver.evaporate, 3.0f);
    CHECK_EQ(c.lookAt(1).steamColor.y, 0.85f);
    CHECK_EQ(c.lookAt(1).steamDensity, 12.0f);
    CHECK_EQ(c.world.rain.rain.fill, 12.0f);
    // Saved and read again, they stay.
    Network again;
    CHECK(Network::load(net.save(), again, error));
    const Compiled d = again.compile(PG_SIM_EXAMPLES_DIR);
    CHECK_EQ(d.world.gas.solver.quench, 2.0f);
    CHECK_EQ(d.world.gas.solver.steamFade, 0.25f);
    CHECK_EQ(d.world.gas.solver.evaporate, 3.0f);
    CHECK_EQ(d.lookAt(1).steamDensity, 12.0f);
    CHECK_EQ(d.world.rain.rain.fill, 12.0f);
    // Their defaults: water puts fire out and the fire boils it, rain does
    // not fill; the steam white.
    const World plain = exampleWorld("rain_pond");
    CHECK_EQ(plain.rain.rain.fill, 0.0f);
    const World campfire = exampleWorld("campfire");
    CHECK_EQ(campfire.gas.solver.quench, 1.0f);
    CHECK_EQ(campfire.gas.solver.steam, 1.0f);
    CHECK_EQ(campfire.gas.solver.steamLift, 1.5f);
    CHECK_EQ(campfire.gas.solver.steamFade, 0.7f);
    CHECK_EQ(campfire.gas.solver.evaporate, 1.0f);
    CHECK(Look().steamColor.x > 0.9f && Look().steamDensity == 8.0f);
}
