//
// Grains (src/pg/sim/Grains.h): a block of sand falls and rests on the
// floor, the grains apart; poured, it piles up as steep as its friction
// holds; wet, it stands; it stays out of what it falls on and pushes a piece
// that gives; the wind blows it; a stream is poured where no grain is in
// the way; the grains as points; the same bits on one thread and on four,
// and a state saved goes on to the bit.
//
#include "pg/core/Half.h"
#include "pg/core/Parallel.h"
#include "pg/sim/Grains.h"
#include "pg/sim/State.h"

#include "test_framework.h"

#include <algorithm>
#include <cmath>
#include <set>

using namespace pg;
using namespace pg::sim;

namespace {

/// A block of nx x ny x nz points `spacing` apart, its bottom at `bottom`,
/// each a little off the lattice.
std::shared_ptr<Geometry> block(int nx, int ny, int nz, float spacing, float bottom) {
    auto g = std::make_shared<Geometry>();
    g->addPoints(static_cast<size_t>(nx * ny * nz));
    auto P = g->positionsForWrite();
    size_t k = 0;
    for (int y = 0; y < ny; ++y) {
        for (int z = 0; z < nz; ++z) {
            for (int x = 0; x < nx; ++x) {
                const float jx = static_cast<float>((k * 7919u) % 1000u) / 1000.0f - 0.5f;
                const float jz = static_cast<float>((k * 104729u) % 1000u) / 1000.0f - 0.5f;
                P[k++] = Vec3((static_cast<float>(x) - 0.5f * static_cast<float>(nx - 1) + 0.1f * jx) * spacing,
                              bottom + static_cast<float>(y) * spacing,
                              (static_cast<float>(z) - 0.5f * static_cast<float>(nz - 1) + 0.1f * jz) * spacing);
            }
        }
    }
    return g;
}

/// A disc of points `radius` grains across, at `height`, falling at 1 m/s:
/// a spout.
std::shared_ptr<Geometry> spout(float r, float height) {
    std::vector<Vec3> at;
    for (int z = -3; z <= 3; ++z) {
        for (int x = -3; x <= 3; ++x) {
            if (x * x + z * z <= 9) at.push_back(Vec3(static_cast<float>(x) * 2.4f * r, height, static_cast<float>(z) * 2.4f * r));
        }
    }
    auto g = std::make_shared<Geometry>();
    g->addPoints(at.size());
    auto P = g->positionsForWrite();
    std::copy(at.begin(), at.end(), P.begin());
    auto v = g->points().create("v", AttrType::Vec3).write<Vec3>();
    std::fill(v.begin(), v.end(), Vec3(0.0f, -1.0f, 0.0f));
    return g;
}

/// How far, at most and on average, the grains are into each other: a
/// share of the sum of their radii.
void overlap(const GrainSolver& s, float& worst, float& mean) {
    const auto& x = s.positions();
    const auto& r = s.radii();
    worst = 0.0f;
    double sum = 0.0;
    size_t pairs = 0;
    for (size_t i = 0; i < x.size(); ++i) {
        for (size_t j = i + 1; j < x.size(); ++j) {
            const float d = length(x[i] - x[j]), R = r[i] + r[j];
            if (d >= R) continue;
            worst = std::max(worst, (R - d) / R);
            sum += (R - d) / R;
            ++pairs;
        }
    }
    mean = pairs ? static_cast<float>(sum / static_cast<double>(pairs)) : 0.0f;
}

/// The slope of a pile round the y axis, degrees: its top over the radius
/// that holds nine tenths of its grains.
float pileAngle(const GrainSolver& s) {
    std::vector<float> out;
    float top = 0.0f;
    for (const Vec3& p : s.positions()) {
        const float d = std::sqrt(p.x * p.x + p.z * p.z);
        out.push_back(d);
        if (d < 0.03f) top = std::max(top, p.y);
    }
    std::sort(out.begin(), out.end());
    return std::atan2(top, out[static_cast<size_t>(0.9 * static_cast<double>(out.size()))]) * 57.29578f;
}

GrainScene poured(float friction) {
    GrainScene s;
    s.geometry = spout(0.01f, 0.4f);
    s.solver.radius = 0.01f;
    s.solver.friction = friction;
    s.solver.emitFrames = 100;
    return s;
}

}  // namespace

TEST(grains_fall_and_rest_on_the_floor_apart) {
    GrainScene scene;
    scene.geometry = block(8, 12, 8, 0.023f, 0.05f);
    scene.solver.radius = 0.01f;
    GrainSolver s(scene);
    CHECK_EQ(s.grainCount(), size_t(0));  // made in the first step
    for (int f = 0; f < 120; ++f) s.step();
    CHECK_EQ(s.grainCount(), size_t(768));
    float lowest = 1.0f, fastest = 0.0f;
    for (size_t i = 0; i < s.grainCount(); ++i) {
        lowest = std::min(lowest, s.positions()[i].y - s.radii()[i]);
        fastest = std::max(fastest, length(s.velocities()[i]));
    }
    CHECK(lowest > -1e-4f);   // on the floor, not in it
    CHECK(fastest < 0.05f);   // at rest
    float worst = 0.0f, mean = 0.0f;
    overlap(s, worst, mean);
    CHECK(mean < 0.04f);
    CHECK(worst < 0.3f);
    // Their sizes as asked: within a fifth of the radius either way.
    for (const float r : s.radii()) CHECK(r >= 0.0079f && r <= 0.0121f);
    // Sorted into a heap below where they started.
    float top = 0.0f;
    for (const Vec3& p : s.positions()) top = std::max(top, p.y);
    CHECK(top < 0.05f + 11 * 0.023f);
}

TEST(grains_pile_as_steep_as_their_friction_holds) {
    float angle[2] = {};
    size_t count[2] = {};
    const float friction[2] = {0.15f, 0.8f};
    for (int k = 0; k < 2; ++k) {
        GrainSolver s(poured(friction[k]));
        for (int f = 0; f < 200; ++f) s.step();
        angle[k] = pileAngle(s);
        count[k] = s.grainCount();
        // Settled -- but for a grain or two still rolling off its foot.
        size_t moving = 0;
        for (const Vec3& v : s.velocities()) moving += length(v) > 0.05f ? 1 : 0;
        CHECK(moving * 100 < s.grainCount());
    }
    std::printf("    pile: friction 0.15 %.1f degrees (%zu grains), 0.8 %.1f (%zu)\n", static_cast<double>(angle[0]),
                count[0], static_cast<double>(angle[1]), count[1]);
    CHECK(angle[1] > angle[0] + 8.0f);
    CHECK(angle[1] > 25.0f && angle[1] < 50.0f);
}

TEST(grains_wet_stand_as_a_wall) {
    // A column of sand: dry it slumps; wet it stands.
    float height[2] = {};
    for (int k = 0; k < 2; ++k) {
        GrainScene scene;
        scene.geometry = block(5, 16, 5, 0.021f, 0.011f);
        scene.solver.radius = 0.01f;
        scene.solver.sizeVariance = 0.1f;
        scene.solver.cohesion = k == 0 ? 0.0f : 1.0f;
        GrainSolver s(scene);
        for (int f = 0; f < 60; ++f) s.step();
        for (const Vec3& p : s.positions()) height[k] = std::max(height[k], p.y);
    }
    std::printf("    column: dry %.3f m, wet %.3f m\n", static_cast<double>(height[0]), static_cast<double>(height[1]));
    CHECK(height[1] > 0.25f);
    CHECK(height[1] > height[0] + 0.05f);
}

TEST(grains_stay_out_of_what_they_fall_on_and_push_a_piece) {
    GrainScene scene;
    scene.geometry = block(6, 6, 6, 0.024f, 0.5f);
    scene.solver.radius = 0.01f;
    Collider box;
    box.shape = Shape::Box;
    box.center = Vec3(0.0f, 0.2f, 0.0f);
    box.size = Vec3(0.5f, 0.2f, 0.5f);
    scene.colliders.push_back(box);
    // A piece of an RBD Solver beside it, that gives: the grains that land
    // on it push it down.
    Collider piece = box;
    piece.center = Vec3(0.0f, 0.4f, 0.0f);
    piece.size = Vec3(0.3f, 0.05f, 0.3f);
    piece.piece = 7;
    piece.mass = 0.5f;
    scene.colliders.push_back(piece);
    GrainSolver s(scene);
    bool pushed = false;
    for (int f = 0; f < 20; ++f) {
        s.step();
        for (const GrainSolver::Reaction& r : s.reactions()) {
            CHECK_EQ(r.piece, 7u);
            if (r.velocity.y < 0.0f) pushed = true;
        }
    }
    CHECK(pushed);
    const ShapeInstance shape = box.instance();
    for (size_t i = 0; i < s.grainCount(); ++i) CHECK(shape.distance(s.positions()[i]) > 0.5f * s.radii()[i]);
    // On the box, not through it to the floor.
    size_t onTop = 0;
    for (const Vec3& p : s.positions()) onTop += p.y > 0.29f ? 1 : 0;
    CHECK(onTop > s.grainCount() / 2);
}

TEST(grains_blown_by_the_wind) {
    GrainScene scene;
    scene.geometry = block(4, 4, 4, 0.03f, 1.0f);
    scene.solver.radius = 0.01f;
    scene.solver.airDrag = 3.0f;
    scene.solver.floor = false;
    Force wind;
    wind.kind = ForceKind::Wind;
    wind.speed = 8.0f;
    wind.direction = Vec3(1.0f, 0.0f, 0.0f);
    scene.forces.push_back(wind);
    GrainSolver s(scene);
    for (int f = 0; f < 10; ++f) s.step();
    float x = 0.0f;
    for (const Vec3& p : s.positions()) x += p.x;
    CHECK(x / static_cast<float>(s.grainCount()) > 0.2f);
}

TEST(grains_poured_where_none_is_in_the_way) {
    GrainSolver s(poured(0.6f));
    s.step();
    const size_t first = s.grainCount();
    CHECK_EQ(first, size_t(29));
    for (int f = 0; f < 20; ++f) s.step();
    CHECK(s.grainCount() > 10 * first);
    // None made inside another: none into one more than the solver lets them.
    float worst = 0.0f, mean = 0.0f;
    overlap(s, worst, mean);
    CHECK(worst < 0.5f);
    // Each its own number.
    std::set<uint32_t> ids(s.ids().begin(), s.ids().end());
    CHECK_EQ(ids.size(), s.grainCount());
    // No more than asked for.
    GrainScene few = poured(0.6f);
    few.solver.maxGrains = 100;
    GrainSolver capped(few);
    for (int f = 0; f < 20; ++f) capped.step();
    CHECK_EQ(capped.grainCount(), size_t(100));
}

TEST(grains_as_points) {
    GrainScene scene;
    scene.geometry = block(3, 2, 3, 0.03f, 0.2f);
    {
        auto cd = std::const_pointer_cast<Geometry>(scene.geometry)->points().create("Cd", AttrType::Vec3).write<Vec3>();
        for (auto& c : cd) c = Vec3(0.8f, 0.6f, 0.3f);
    }
    GrainSolver s(scene);
    s.step();
    const GrainFrame f = s.capture();
    CHECK(f.fits());
    CHECK_EQ(f.size(), size_t(18));
    const auto g = grainPoints(f, Vec3(1.0f, 0.0f, 0.0f));
    CHECK_EQ(g->pointCount(), size_t(18));
    const auto pscale = g->points().find("pscale")->read<float>();
    const auto cd = g->points().find("Cd")->read<Vec3>();
    const auto orient = g->points().find("orient")->read<Vec4>();
    const auto id = g->points().find("id")->read<int32_t>();
    CHECK(g->points().find("v") != nullptr);
    for (size_t i = 0; i < g->pointCount(); ++i) {
        CHECK_NEAR(pscale[i], s.radii()[i], 1e-5f);
        CHECK_NEAR(cd[i].x, 0.8f, 0.01f);
        CHECK_NEAR(orient[i].x * orient[i].x + orient[i].y * orient[i].y + orient[i].z * orient[i].z +
                       orient[i].w * orient[i].w,
                   1.0f, 1e-4f);
        CHECK_EQ(static_cast<uint32_t>(id[i]), s.ids()[i]);
    }
    // Without Cd: the look's colour.
    GrainFrame plain = f;
    plain.colors.clear();
    CHECK_NEAR(grainPoints(plain, Vec3(1.0f, 0.0f, 0.0f))->points().find("Cd")->read<Vec3>()[0].x, 1.0f, 1e-6f);
    // A frame that does not hold together gives none.
    GrainFrame broken = f;
    broken.radii.pop_back();
    CHECK(!broken.fits());
    CHECK_EQ(grainPoints(broken, Vec3())->pointCount(), size_t(0));
}

TEST(grains_the_same_on_any_threads_and_from_a_state) {
    GrainScene scene = poured(0.6f);
    scene.solver.cohesion = 0.3f;
    const unsigned saved = TaskPool::instance().threadCount();
    TaskPool::instance().setThreadCount(1);
    GrainSolver one(scene);
    for (int f = 0; f < 40; ++f) one.step();
    TaskPool::instance().setThreadCount(4);
    GrainSolver four(scene);
    for (int f = 0; f < 20; ++f) four.step();
    StateWriter out;
    four.saveState(out);
    GrainSolver later(scene);
    StateReader in(out.bytes());
    CHECK(later.loadState(in));
    for (int f = 0; f < 20; ++f) {
        four.step();
        later.step();
    }
    TaskPool::instance().setThreadCount(saved);
    CHECK(one.positions() == four.positions());
    CHECK(one.ids() == four.ids());
    CHECK(later.positions() == four.positions());
    CHECK(later.velocities() == four.velocities());
    CHECK(later.ids() == four.ids());
    // A state cut short, or of no grains that fit, is refused.
    const std::string bytes = out.take();
    StateReader cut(std::string_view(bytes).substr(0, bytes.size() / 2));
    GrainSolver no(scene);
    CHECK(!no.loadState(cut));
}
