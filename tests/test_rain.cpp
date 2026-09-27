//
// Rain (src/pg/sim/Rain.h): the air below the cloud full of drops from the
// start, and as many landing as the rate says; drops fall at their speed and
// slant with the wind, whose gusts travel with it; they land on objects and
// splash off them, ring the water they fall in, start and stop on time; and
// invariant I5 -- the same bits on any thread count.
//
#include "pg/core/Parallel.h"
#include "pg/sim/Frame.h"
#include "pg/sim/Liquid.h"
#include "pg/sim/Rain.h"
#include "pg/sim/Shared.h"
#include "pg/sim/World.h"

#include "test_framework.h"

#include <cmath>
#include <map>
#include <set>
#include <cstring>
#include <memory>

using namespace pg;
using namespace pg::sim;

namespace {

struct ThreadCountGuard {
    unsigned saved = TaskPool::instance().threadCount();
    ~ThreadCountGuard() { TaskPool::instance().setThreadCount(saved); }
};

/// Rain from a cloud `size` wide at `height`, `rate` drops a second on each
/// square metre.
RainScene cloud(float height, Vec3 size, float rate) {
    RainScene s;
    s.rain.center = Vec3(0.0f, height, 0.0f);
    s.rain.size = size;
    s.rain.rate = rate;
    return s;
}

Force wind(Vec3 direction, float speed, float gusts = 0.0f) {
    Force f;
    f.kind = ForceKind::Wind;
    f.direction = direction;
    f.speed = speed;
    f.gusts = gusts;
    return f;
}

/// A pool: water `depth` deep filling a tank `size` wide.
LiquidScene pool(Vec3 size, float depth, int resolution) {
    LiquidScene s;
    s.solver.size = size;
    s.solver.resolution = resolution;
    WaterSource w;
    w.shape = Shape::Box;
    w.center = Vec3(0.0f, 0.5f * depth, 0.0f);
    w.size = Vec3(size.x, depth, size.z);
    s.sources.push_back(w);
    return s;
}

bool same(const std::vector<RainParticle>& a, const std::vector<RainParticle>& b) {
    return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(RainParticle)) == 0);
}

}  // namespace

TEST(rain_fills_the_air_below_the_cloud_and_keeps_its_rate) {
    RainScene s = cloud(2.1f, Vec3(2.0f, 0.2f, 2.0f), 500.0f);
    s.rain.splash = 0.0f;
    RainSolver rain(s);
    rain.step();
    // The first step: the drops that fall in the 0.3 s a drop takes to come
    // down -- rain from before it began -- and this step's.
    const double dt = s.rain.timeStep;
    const double expected = 500.0 * 4.0 * (dt + 2.1 / 7.0);
    const double made = static_cast<double>(rain.drops().size()) + rain.lastLandings();
    CHECK(std::fabs(made - expected) <= 1.0);
    // Spread all the way down, not bunched under the cloud.
    size_t low = 0, high = 0;
    for (const RainParticle& d : rain.drops()) {
        CHECK(d.position.y > 0.0f && d.position.y < 2.2f);
        low += d.position.y < 0.4f;
        high += d.position.y > 1.6f;
    }
    CHECK(low > rain.drops().size() / 10 && high > rain.drops().size() / 10);

    // From then on, as many land a step as the rate says: 500 a second on
    // each of the 4 square metres.
    for (int f = 0; f < 30; ++f) rain.step();
    int landed = 0;
    for (int f = 0; f < 30; ++f) {
        rain.step();
        landed += rain.lastLandings();
    }
    const double perStep = 500.0 * 4.0 * dt;
    CHECK(std::fabs(landed / 30.0 - perStep) < 0.02 * perStep);
    CHECK(rain.droplets().empty());  // splash 0
}

TEST(rain_falls_at_its_speed_and_slants_with_the_wind) {
    RainScene s = cloud(3.0f, Vec3(2.0f, 0.2f, 2.0f), 300.0f);
    s.forces.push_back(wind(Vec3(1.0f, 0.0f, 0.0f), 3.0f));
    RainSolver rain(s);
    for (int f = 0; f < 20; ++f) rain.step();
    CHECK(rain.drops().size() > 400);  // 300 a second on 4 m^2, 0.43 s in the air
    Vec3 mean;
    double lowX = 0.0, highX = 0.0, lowY = 0.0, highY = 0.0;
    int lows = 0, highs = 0;
    for (const RainParticle& d : rain.drops()) {
        mean += d.velocity;
        if (d.position.y < 1.0f) {
            lowX += d.position.x;
            lowY += d.position.y;
            ++lows;
        } else if (d.position.y > 2.0f) {
            highX += d.position.x;
            highY += d.position.y;
            ++highs;
        }
    }
    mean = mean * (1.0f / static_cast<float>(rain.drops().size()));
    CHECK(std::fabs(mean.x - 3.0f) < 0.03f);
    CHECK(std::fabs(mean.y + 7.0f) < 0.07f);
    CHECK(std::fabs(mean.z) < 0.01f);
    // Downwind the lower they are: 3 m sideways for each 7 m down.
    CHECK(lows > 50 && highs > 50);
    const double drift = lowX / lows - highX / highs, expected = 3.0 / 7.0 * (highY / highs - lowY / lows);
    CHECK(std::fabs(drift - expected) < 0.15);
}

TEST(rain_wind_gusts_travel_with_the_wind) {
    const Force w = wind(Vec3(1.0f, 0.0f, 1.0f), 4.0f, 0.6f);
    const Vec3 d = normalize(w.direction);
    // A gust is carried along: what blows here now blows 2 m downwind
    // half a second later.
    for (const float t : {0.0f, 0.7f, 3.1f}) {
        for (const Vec3& p : {Vec3(0.0f, 1.0f, 0.0f), Vec3(-1.5f, 0.3f, 0.8f)}) {
            const Vec3 now = detail::windAt(w, t, 7u, p);
            const Vec3 later = detail::windAt(w, t + 0.5f, 7u, p + d * (w.speed * 0.5f));
            CHECK(length(now - later) < 1e-3f * w.speed);
            CHECK(std::fabs(dot(normalize(now), d) - 1.0f) < 1e-5f);  // gusts change its speed, not its way
        }
    }
    // And it gusts: in one place, now harder, now softer -- within 1 +- gusts.
    float lo = 1e9f, hi = 0.0f;
    for (int i = 0; i < 400; ++i) {
        const float speed = length(detail::windAt(w, 0.05f * static_cast<float>(i), 7u, Vec3(0.0f, 1.0f, 0.0f)));
        lo = std::min(lo, speed);
        hi = std::max(hi, speed);
    }
    CHECK(hi - lo > 0.3f * w.speed);
    CHECK(lo >= 0.4f * w.speed - 1e-4f && hi <= 1.6f * w.speed + 1e-4f);
    // Without gusts, steady.
    const Force steady = wind(Vec3(0.0f, 0.0f, 2.0f), 5.0f);
    CHECK(length(detail::windAt(steady, 1.3f, 7u, Vec3(4.0f, 1.0f, 2.0f)) - Vec3(0.0f, 0.0f, 5.0f)) < 1e-5f);
}

TEST(rain_lands_on_objects_and_splashes_off_them) {
    RainScene s = cloud(2.0f, Vec3(1.0f, 0.1f, 1.0f), 2000.0f);
    Collider table;
    table.shape = Shape::Box;
    table.center = Vec3(0.0f, 0.5f, 0.0f);
    table.size = Vec3(0.6f, 1.0f, 0.6f);
    s.colliders.push_back(table);
    RainSolver rain(s);
    int landed = 0;
    bool thrownUp = false;
    for (int f = 0; f < 30; ++f) {
        rain.step();
        landed += rain.lastLandings();
        for (const RainParticle& d : rain.drops()) CHECK(!table.contains(d.position));
        for (const RainParticle& d : rain.droplets()) {
            CHECK(d.age < d.life);
            thrownUp = thrownUp || d.position.y > 1.02f;  // up off the table's top
        }
    }
    CHECK(landed > 1000);
    CHECK(!rain.droplets().empty());
    CHECK(thrownUp);
    // Nothing is thrown up with splash 0.
    s.rain.splash = 0.0f;
    RainSolver dry(s);
    for (int f = 0; f < 10; ++f) dry.step();
    CHECK(dry.droplets().empty());
}

TEST(rain_drops_and_droplets_keep_their_numbers) {
    // Drops fall onto a table and splash: each drop keeps its number while
    // it falls -- near where it was a frame before -- and the droplets have
    // numbers of their own; the frame keeps them.
    RainScene s = cloud(2.0f, Vec3(1.0f, 0.1f, 1.0f), 2000.0f);
    Collider table;
    table.shape = Shape::Box;
    table.center = Vec3(0.0f, 0.5f, 0.0f);
    table.size = Vec3(0.6f, 1.0f, 0.6f);
    s.colliders.push_back(table);
    RainSolver rain(s);
    std::map<uint32_t, Vec3> before;
    size_t followed = 0;
    for (int f = 0; f < 20; ++f) {
        rain.step();
        std::map<uint32_t, Vec3> now;
        for (const RainParticle& d : rain.drops()) {
            CHECK(now.emplace(d.id, d.position).second);  // one of each number
            const auto was = before.find(d.id);
            if (was != before.end()) {
                CHECK(length(d.position - was->second) < 0.5f);  // a frame of falling
                ++followed;
            }
        }
        std::set<uint32_t> splashes;
        for (const RainParticle& d : rain.droplets()) CHECK(splashes.insert(d.id).second);
        before = std::move(now);
    }
    CHECK(followed > 1000);
    CHECK(!rain.droplets().empty());
    const RainFrame frame = capture(rain);
    CHECK_EQ(frame.dropIds.size(), frame.dropCount());
    CHECK_EQ(frame.dropletIds.size(), frame.dropletCount());
    CHECK_EQ(frame.dropIds[0], rain.drops()[0].id);
}

TEST(rain_rings_the_water_it_falls_in) {
    World w;
    w.hasWater = true;
    w.water = pool(Vec3(1.0f, 0.5f, 1.0f), 0.2f, 16);
    w.hasRain = true;
    w.rain = cloud(1.5f, Vec3(1.2f, 0.1f, 1.2f), 1500.0f);
    WorldSolver sim(w);
    int splashes = 0;
    for (int f = 0; f < 10; ++f) {
        sim.step();
        splashes += sim.rain()->lastSplashes();
        // None goes on under the surface.
        for (const RainParticle& d : sim.rain()->drops()) CHECK(sim.water()->distanceToSurface(d.position) >= 0.0f);
    }
    // 1500 a second on the 1 m^2 of water, for a third of a second.
    CHECK(splashes > 400 && splashes < 600);
    const Ripples& r = sim.rain()->ripples();
    CHECK(!r.empty());
    CHECK_EQ(r.height.size(), static_cast<size_t>(r.nx) * static_cast<size_t>(r.nz));
    float deepest = 0.0f;
    for (const float h : r.height) {
        CHECK(std::isfinite(h));
        deepest = std::max(deepest, std::fabs(h));
    }
    CHECK(deepest > 1e-5f && deepest < 0.05f);  // rings, not waves
    // The frame carries the drops and the ripples.
    const Frame f = sim.capture();
    CHECK(!f.rain.empty());
    CHECK_EQ(f.rain.dropCount(), sim.rain()->drops().size());
    CHECK_EQ(f.rain.ripples.size(), r.height.size());
    CHECK_EQ(f.rain.rippleCells[0], r.nx);
    // Without water, no ripples.
    RainSolver dry(w.rain);
    dry.step();
    CHECK(dry.ripples().empty());
}

TEST(rain_starts_and_stops_on_time) {
    RainScene s = cloud(2.0f, Vec3(1.0f, 0.2f, 1.0f), 1000.0f);
    s.rain.start = 0.5f;
    s.rain.end = 1.0f;
    RainSolver rain(s);
    int landed = 0;
    for (int f = 0; f < 15; ++f) {
        rain.step();
        CHECK(rain.drops().empty());
    }
    rain.step();
    // Rain that starts late starts at the cloud: none fallen far yet.
    CHECK(!rain.drops().empty());
    for (const RainParticle& d : rain.drops()) CHECK(d.position.y > 1.5f);
    for (int f = 0; f < 45; ++f) {
        rain.step();
        landed += rain.lastLandings();
    }
    // Stopped, and every drop down: 1000 a second on a square metre for half a second.
    CHECK(rain.drops().empty());
    CHECK(std::abs(landed - 500) <= 1);
}

TEST(rain_is_bitwise_identical_across_thread_counts) {
    ThreadCountGuard guard;
    World w;
    w.hasWater = true;
    w.water = pool(Vec3(1.0f, 0.5f, 1.0f), 0.2f, 16);
    w.hasRain = true;
    w.rain = cloud(1.5f, Vec3(2.0f, 0.1f, 2.0f), 1200.0f);
    w.rain.forces.push_back(wind(Vec3(1.0f, 0.0f, 0.4f), 2.0f, 0.5f));
    Force shake;
    shake.kind = ForceKind::Turbulence;
    shake.strength = 3.0f;
    shake.scale = 0.2f;
    w.rain.forces.push_back(shake);
    Collider rock;
    rock.center = Vec3(0.7f, 0.2f, 0.0f);
    rock.size = Vec3(0.4f);
    w.rain.colliders.push_back(rock);
    auto run = [&](unsigned threads) {
        TaskPool::instance().setThreadCount(threads);
        auto sim = std::make_unique<WorldSolver>(w);
        for (int f = 0; f < 8; ++f) sim->step();
        return sim;
    };
    const auto one = run(1), four = run(4);
    const RainSolver& a = *one->rain();
    const RainSolver& b = *four->rain();
    CHECK(same(a.drops(), b.drops()));
    CHECK(same(a.droplets(), b.droplets()));
    CHECK(!a.droplets().empty());
    CHECK(a.ripples() == b.ripples());
}

TEST(rain_settings_out_of_range_are_made_safe) {
    RainScene s;
    s.rain.center = Vec3(std::nanf(""), 3.0f, 0.0f);
    s.rain.size = Vec3(-1.0f, 0.0f, 1e9f);
    s.rain.rate = std::nanf("");
    s.rain.speed = -4.0f;
    s.rain.splash = 1e9f;
    s.rain.ripples = -1.0f;
    s.rain.timeStep = 0.0f;
    s.forces.push_back(wind(Vec3(0.0f), std::nanf("")));
    const RainScene safe = s.sanitized();
    const RainSettings& r = safe.rain;
    CHECK(std::isfinite(r.center.x));
    CHECK(r.size.x > 0.0f && r.size.y > 0.0f && r.size.z <= 100.0f);
    CHECK(std::isfinite(r.rate) && r.rate >= 0.0f && r.rate <= 20000.0f);
    CHECK(r.speed > 0.0f);
    CHECK(r.splash <= 20.0f && r.ripples >= 0.0f);
    CHECK(r.timeStep > 0.0f);
    CHECK(std::isfinite(safe.forces[0].speed));
    // And it runs.
    RainSolver rain(s);
    rain.step();
    for (const RainParticle& d : rain.drops()) CHECK(std::isfinite(d.position.x) && std::isfinite(d.velocity.y));
}
