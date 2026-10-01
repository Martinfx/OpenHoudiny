//
// The particles of a frame as the renderers draw them (render/Particles.h):
// the grit's chips -- closed, flat-faced, a unit from their middle to their
// farthest corner, a dozen shapes of stone and half a dozen slivers of glass
// -- placed on the loose points alone, as big as their pscale, turned by
// their orient, each its shape by its id; the rain's streaks, as long as a
// drop falls in a share of a frame, water seen as much of the time as the
// look says, casting no shadow; what the rain falls on wet. Both renderers
// draw them: the chips seen and in shadow below them, the rain seen against
// the dark, the wet floor darker.
//
#include "pg/core/Geometry.h"
#include "pg/render/Cycles.h"
#include "pg/render/Particles.h"
#include "pg/render/PathTracer.h"
#include "pg/sim/Frame.h"

#include "test_framework.h"

#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <map>
#include <set>

using namespace pg;
using namespace pg::render;

namespace {

/// A quad on the floor, a metre across, and loose points over it: the grit
/// of drawnPieces -- pscale, orient, id, glass and Cd -- the third without a
/// size, the fifth of glass, the second turned a quarter round y.
GeometryPtr gritted(bool reversed = false) {
    auto geo = std::make_shared<Geometry>();
    geo->addPoints(10);
    auto size = geo->points().create("pscale", AttrType::Float).write<float>();
    auto orient = geo->points().create("orient", AttrType::Vec4).write<Vec4>();
    auto id = geo->points().create("id", AttrType::Int).write<int32_t>();
    auto glass = geo->points().create("glass", AttrType::Int).write<int32_t>();
    auto cd = geo->points().create("Cd", AttrType::Vec3).write<Vec3>();
    auto P = geo->positionsForWrite();
    const Vec3 quad[4] = {{-0.5f, 0.0f, -0.5f}, {-0.5f, 0.0f, 0.5f}, {0.5f, 0.0f, 0.5f}, {0.5f, 0.0f, -0.5f}};
    for (int i = 0; i < 4; ++i) {
        P[static_cast<size_t>(i)] = quad[i];
        size[static_cast<size_t>(i)] = 0.5f;  // a size -- but not loose
    }
    const float sizes[6] = {0.05f, 0.1f, 0.0f, 0.2f, 0.03f, 0.08f};
    const float s = std::sqrt(0.5f);
    for (int k = 0; k < 6; ++k) {
        const size_t i = 4 + static_cast<size_t>(reversed ? 5 - k : k);
        P[i] = Vec3(0.1f * static_cast<float>(k), 0.5f, 0.0f);
        size[i] = sizes[k];
        orient[i] = k == 1 ? Vec4(0.0f, s, 0.0f, s) : Vec4(0.0f, 0.0f, 0.0f, 1.0f);
        id[i] = 100 + k;
        glass[i] = k == 4 ? 1 : 0;
        cd[i] = k == 4 ? Vec3(0.86f, 0.94f, 0.92f) : Vec3(0.5f, 0.4f, 0.3f);
    }
    const uint32_t face[4] = {0, 1, 2, 3};
    geo->addPrimitive(face, true);
    return geo;
}

/// Rain of `drops` drops falling at `fall` m/s through the box from `lo` to
/// `hi`, spread evenly (a lattice), a frame of 1/30 s.
std::shared_ptr<sim::Frame> raining(const Vec3& lo, const Vec3& hi, int drops, float fall = 8.0f) {
    auto f = std::make_shared<sim::Frame>();
    f->rain.timeStep = 1.0f / 30.0f;
    const int side = std::max(1, static_cast<int>(std::cbrt(static_cast<float>(drops))));
    uint32_t n = 0;
    for (int k = 0; k < side; ++k) {
        for (int j = 0; j < side; ++j) {
            for (int i = 0; i < side; ++i) {
                const Vec3 t((static_cast<float>(i) + 0.5f) / static_cast<float>(side),
                             (static_cast<float>(j) + 0.37f) / static_cast<float>(side),
                             (static_cast<float>(k) + 0.71f) / static_cast<float>(side));
                const Vec3 p = lo + (hi - lo) * t;
                f->rain.drops.insert(f->rain.drops.end(), {p.x, p.y, p.z, 0.0f, -fall, 0.0f});
                f->rain.dropIds.push_back(n++);
            }
        }
    }
    return f;
}

Image traced(const std::shared_ptr<const Scene>& scene, int width, int height, int samples) {
    Settings s;
    s.width = width;
    s.height = height;
    s.samples = samples;
    s.denoise = false;
    PathTracer t;
    t.setSettings(s);
    t.setScene(scene);
    while (!t.done()) t.pass();
    return t.beauty();
}

/// The mean of a channel over the pixels round where the camera sees `p`.
double meanAt(const Image& img, int c, const sim::Camera& cam, const Vec3& p) {
    const Vec3 d = p - cam.position;
    const float z = dot(d, normalize(cam.forward()));
    const float tanY = std::tan(0.5f * cam.fovY() * 3.14159265f / 180.0f), tanX = tanY * img.width / img.height;
    const int px = static_cast<int>((dot(d, normalize(cam.right())) / z / tanX + 1.0f) * 0.5f * img.width);
    const int py = static_cast<int>((1.0f - dot(d, normalize(cam.up())) / z / tanY) * 0.5f * img.height);
    double sum = 0.0;
    int count = 0;
    for (int y = std::max(py - 1, 0); y <= std::min(py + 1, img.height - 1); ++y) {
        for (int x = std::max(px - 1, 0); x <= std::min(px + 1, img.width - 1); ++x) {
            sum += img.pixels[3 * (static_cast<size_t>(y) * img.width + x) + c];
            ++count;
        }
    }
    return sum / count;
}

/// The mean of a channel over the rows from `top` to `bottom` (shares of the height).
double meanRows(const Image& img, int c, float top, float bottom) {
    double sum = 0.0;
    size_t count = 0;
    for (int y = static_cast<int>(top * img.height); y < static_cast<int>(bottom * img.height); ++y) {
        for (int x = 0; x < img.width; ++x) {
            sum += img.pixels[3 * (static_cast<size_t>(y) * img.width + x) + c];
            ++count;
        }
    }
    return count ? sum / static_cast<double>(count) : 0.0;
}

/// A big chip of red stone hanging over the floor, the sun straight above;
/// seen from the side. Without the chip: the floor alone.
SceneInput chipOverFloor(bool chip, int width, int height) {
    auto geo = std::make_shared<Geometry>();
    if (chip) {
        geo->addPoints(1);
        geo->positionsForWrite()[0] = Vec3(0.0f, 0.6f, 0.0f);
        geo->points().create("pscale", AttrType::Float).write<float>()[0] = 0.3f;
        geo->points().create("Cd", AttrType::Vec3).write<Vec3>()[0] = Vec3(0.8f, 0.15f, 0.1f);
    }
    SceneInput in;
    in.bodies = geo;
    in.look.lightElevation = 89.0f;
    in.look.groundColor = Vec3(0.5f);
    in.camera = sim::Camera::lookingAt(Vec3(2.5f, 1.0f, 0.0f), Vec3(0.0f, 0.3f, 0.0f));
    in.camera.width = width;
    in.camera.height = height;
    return in;
}

/// The floor seen from above at a slant, the rain far over it -- out of the
/// picture -- wetting it as wet as `wetness`.
SceneInput wetFloor(float wetness, int width, int height) {
    SceneInput in;
    in.frame = raining(Vec3(-3.0f, 50.0f, -3.0f), Vec3(3.0f, 51.0f, 3.0f), 64);
    in.look.lightElevation = 70.0f;
    in.look.groundColor = Vec3(0.5f);
    in.look.wetness = wetness;
    in.camera = sim::Camera::lookingAt(Vec3(0.0f, 2.0f, 2.0f), Vec3(0.0f, 0.0f, 0.0f));
    in.camera.width = width;
    in.camera.height = height;
    return in;
}

}  // namespace

TEST(particles_chips_are_closed_flat_faced_and_a_unit_across) {
    std::set<long> volumes;
    for (int glass = 0; glass < 2; ++glass) {
        const size_t shapes = glass ? kSliverShapes : kChipShapes;
        for (size_t k = 0; k < shapes; ++k) {
            const auto m = chipMesh(k, glass != 0, defaultRayEngine());
            CHECK(m && m->count() >= 8);  // a sliver of three sides: two of them, three quads
            if (!m || m->count() == 0) continue;
            CHECK(chipMesh(k, glass != 0, defaultRayEngine()) == m);  // made once
            double volume = 0.0;
            float far = 0.0f, lo = 1e9f, hi = -1e9f;
            bool outward = true, flat = true;
            for (size_t t = 0; t < m->count(); ++t) {
                const Vec3 a = m->v0[t], b = a + m->e1[t], c = a + m->e2[t];
                volume += static_cast<double>(dot(a, cross(b, c))) / 6.0;
                for (const Vec3& p : {a, b, c}) {
                    far = std::max(far, length(p));
                    lo = std::min(lo, p.y);
                    hi = std::max(hi, p.y);
                }
                const Vec3 n = normalize(cross(m->e1[t], m->e2[t]));
                outward = outward && dot(n, a + b + c) > 0.0f;
                for (int corner = 0; corner < 3; ++corner) flat = flat && dot(m->normals[3 * t + corner], n) > 0.999f;
            }
            CHECK(outward);
            CHECK(flat);
            CHECK(volume > 0.0);
            CHECK_NEAR(far, 1.0f, 1e-3f);
            CHECK_EQ(m->materials.size(), 1u);
            if (glass) {
                CHECK(m->materials[0].kind == Material::Kind::Glass);
                CHECK(hi - lo < 0.25f);  // a sliver: as thin as a pane
            } else {
                CHECK(m->materials[0].kind == Material::Kind::Surface);
                CHECK(m->materials[0].preset == MaterialPreset::BrokenConcrete);
                CHECK(volume > 0.15);
            }
            volumes.insert(std::lround(volume * 1e4));
        }
    }
    CHECK_EQ(volumes.size(), kChipShapes + kSliverShapes);  // each a shape of its own
}

TEST(particles_grit_is_placed_as_chips_on_the_loose_points) {
    Scene scene;
    scene.engine = defaultRayEngine();
    const GeometryPtr geo = gritted();
    CHECK_EQ(placeChips(*geo, scene), 5u);
    CHECK_EQ(scene.placed.size(), 5u);
    const auto P = geo->positions();
    const auto size = geo->points().find("pscale")->read<float>();
    const auto id = geo->points().find("id")->read<int32_t>();
    std::map<int32_t, const Mesh*> shapeOf;
    for (const Placed& p : scene.placed) {
        // On a loose point of a size -- not the quad's, not the one without.
        size_t i = 0;
        while (i < P.size() && length(P[i] - p.at) > 1e-6f) ++i;
        CHECK(i >= 4 && i < P.size());
        if (i < 4 || i >= P.size()) continue;
        CHECK_NEAR(p.scale, size[i], 1e-6f);
        const Mesh& m = *scene.meshes[p.mesh];
        shapeOf[id[i]] = &m;
        if (id[i] == 104) {
            CHECK(m.materials[0].kind == Material::Kind::Glass);
            CHECK(length(p.tint - Vec3(0.86f, 0.94f, 0.92f)) < 1e-5f);
        } else {
            CHECK(m.materials[0].kind == Material::Kind::Surface);
            // A shade of its colour of its own: lighter or darker, greyer.
            CHECK(p.tint.x > 0.3f * 0.5f && p.tint.x < 1.3f * 0.5f);
        }
        // Turned by its orient: the second a quarter round y.
        const Vec3 x = p.axes * Vec3(1.0f, 0.0f, 0.0f);
        if (id[i] == 101) {
            CHECK(length(x - Vec3(0.0f, 0.0f, -1.0f)) < 1e-4f);
        } else {
            CHECK(length(x - Vec3(1.0f, 0.0f, 0.0f)) < 1e-4f);
        }
    }
    // Its shape goes by its id, wherever the point is among the others.
    Scene again;
    again.engine = scene.engine;
    const GeometryPtr other = gritted(true);
    CHECK_EQ(placeChips(*other, again), 5u);
    const auto otherId = other->points().find("id")->read<int32_t>();
    const auto otherP = other->positions();
    for (const Placed& p : again.placed) {
        size_t i = 0;
        while (i < otherP.size() && length(otherP[i] - p.at) > 1e-6f) ++i;
        if (i < otherP.size()) CHECK(again.meshes[p.mesh].get() == shapeOf[otherId[i]]);
    }
    // Through the scene builder: the quad, and a chip on each grit point.
    SceneInput in;
    in.bodies = geo;
    SceneBuilder builder;
    CHECK_EQ(builder.build(in)->placed.size(), 6u);
}

TEST(particles_rain_is_streaks_as_long_as_the_drops_fall) {
    sim::RainFrame rain;
    rain.timeStep = 1.0f / 30.0f;
    rain.drops = {0.0f, 2.0f, 0.0f, 0.0f, -8.0f, 0.0f};
    rain.dropIds = {7};
    sim::Look look;
    look.rainStreak = 0.5f;
    look.rainOpacity = 0.4f;
    const auto one = rainMesh(rain, look);
    CHECK(one && one->count() == 8);
    if (!one) return;
    // From just ahead of the drop back up to where it was half a frame ago.
    CHECK_NEAR(one->box.lo.y, 2.0f - 0.00125f, 1e-4f);
    CHECK_NEAR(one->box.hi.y, 2.0f + 8.0f * 0.5f / 30.0f, 1e-4f);
    CHECK(one->box.hi.x - one->box.lo.x < 0.003f);
    CHECK_EQ(one->materials.size(), 1u);
    const Material& m = one->materials[0];
    CHECK(m.kind == Material::Kind::Rain);
    CHECK_NEAR(m.opacity, 0.4f, 1e-6f);
    CHECK_NEAR(m.ior, 1.33f, 1e-6f);
    CHECK(!one->shadows);
    CHECK(!one->clear);
    // A droplet of a splash: half as thick, its streak 0.6 as long.
    rain.droplets = {0.5f, 0.1f, 0.0f, 0.0f, 1.5f, 0.0f};
    rain.dropletIds = {1u << 30};
    const auto two = rainMesh(rain, look);
    CHECK(two && two->count() == 16);
    if (two) CHECK_NEAR(two->box.lo.y, 0.1f - 1.5f * 0.6f * 0.5f / 30.0f, 1e-4f);
    // One just in front of the lens: left out.
    const Vec3 eye(0.0f, 2.2f, 0.1f);
    const auto seen = rainMesh(rain, look, defaultRayEngine(), &eye);
    CHECK(seen && seen->count() == 8);
    // No rain: none.
    CHECK(!rainMesh(sim::RainFrame(), look));
}

TEST(particles_rain_wets_what_it_falls_on_and_casts_no_shadow) {
    SceneInput in;
    in.frame = raining(Vec3(-1.0f, 0.5f, -0.5f), Vec3(1.0f, 1.5f, 0.5f), 1000);
    in.look.wetness = 0.7f;
    in.look.lightElevation = 89.0f;
    SceneBuilder builder;
    const auto scene = builder.build(in);
    CHECK_NEAR(scene->wetness, 0.7f, 1e-6f);
    const Vec3 up(0.0f, 1.0f, 0.0f), side(1.0f, 0.0f, 0.0f);
    CHECK_NEAR(scene->wetAt(Vec3(0.0f, 0.0f, 0.0f), up), 0.7f, 1e-5f);
    CHECK_EQ(scene->wetAt(Vec3(0.0f, 0.0f, 0.0f), side), 0.0f);
    CHECK_EQ(scene->wetAt(Vec3(3.0f, 0.0f, 0.0f), up), 0.0f);
    const float edge = scene->wetAt(Vec3(scene->wetHi.x + 0.15f, 0.0f, 0.0f), up);
    CHECK(edge > 0.0f && edge < 0.7f);
    // The streaks are there, but the sun gets through them all.
    bool streaks = false;
    for (const auto& m : scene->meshes) streaks = streaks || (!m->materials.empty() && m->materials[0].kind == Material::Kind::Rain);
    CHECK(streaks);
    const Vec3 through = scene->transmittance(Vec3(0.0f, 0.01f, 0.0f), normalize(scene->sunDirection), 1e30f);
    CHECK(length(through - Vec3(1.0f)) < 1e-6f);
    // Without rain, nothing is wet.
    SceneInput dry;
    CHECK_EQ(builder.build(dry)->wetness, 0.0f);
}

TEST(render_grit_chips_are_seen_and_shade_the_floor) {
    const int w = 48, h = 32;
    SceneBuilder builder;
    const SceneInput with = chipOverFloor(true, w, h), without = chipOverFloor(false, w, h);
    const Image a = traced(builder.build(with), w, h, 24), b = traced(builder.build(without), w, h, 24);
    const sim::Camera& cam = with.camera;
    const Vec3 under(0.0f, 0.0f, 0.0f), chip(0.0f, 0.6f, 0.0f);
    const double shade = meanAt(a, 1, cam, under), lit = meanAt(b, 1, cam, under);
    const double red = meanAt(a, 0, cam, chip), green = meanAt(a, 1, cam, chip);
    std::printf("  floor under the chip %.3f (%.3f without); the chip %.3f red, %.3f green\n", shade, lit, red, green);
    CHECK(shade < 0.5 * lit);
    CHECK(red > 2.0 * green);
}

TEST(render_rain_is_seen_against_the_dark_and_wets_the_floor) {
    const int w = 48, h = 32;
    SceneBuilder builder;
    // Rain between the eye and the studio's dark backdrop: the streaks bend
    // and send back the sky's light -- brighter than the backdrop.
    SceneInput in;
    in.look.wetness = 0.0f;
    in.look.rainOpacity = 0.6f;
    in.look.floor = false;
    in.camera = sim::Camera::lookingAt(Vec3(0.0f, 1.0f, 4.0f), Vec3(0.0f, 1.0f, 0.0f));
    in.camera.width = w;
    in.camera.height = h;
    const Image dark = traced(builder.build(in), w, h, 16);
    in.frame = raining(Vec3(-1.5f, 0.0f, -1.0f), Vec3(1.5f, 2.0f, 1.0f), 8000);
    const Image rain = traced(builder.build(in), w, h, 16);
    const double before = meanRows(dark, 1, 0.1f, 0.9f), after = meanRows(rain, 1, 0.1f, 0.9f);
    std::printf("  the backdrop %.4f; through the rain %.4f\n", before, after);
    CHECK(after > 1.1 * before);
    // The floor where it rains: darker -- a film of water on it, shining.
    const Image dry = traced(builder.build(wetFloor(0.0f, w, h)), w, h, 24);
    const Image wet = traced(builder.build(wetFloor(1.0f, w, h)), w, h, 24);
    const double d = meanRows(dry, 1, 0.3f, 0.9f), wv = meanRows(wet, 1, 0.3f, 0.9f);
    std::printf("  the floor dry %.3f, wet %.3f\n", d, wv);
    CHECK(wv < 0.85 * d);
}

TEST(render_cycles_draws_the_grit_and_the_wet) {
    if (!cyclesAvailable()) return;
    const int w = 48, h = 32;
    SceneBuilder builder;
    auto cycles = [&](const SceneInput& in) {
        Settings s;
        s.width = w;
        s.height = h;
        s.samples = 16;
        s.denoise = false;
        s.sky = Settings::Sky::Look;
        CyclesRender r;
        r.start(builder.build(in), s);
        r.wait();
        CHECK(r.error().empty());
        return r.beauty();
    };
    const SceneInput with = chipOverFloor(true, w, h);
    const Image a = cycles(with), b = cycles(chipOverFloor(false, w, h));
    const double shade = meanAt(a, 1, with.camera, Vec3(0.0f)), lit = meanAt(b, 1, with.camera, Vec3(0.0f));
    const double red = meanAt(a, 0, with.camera, Vec3(0.0f, 0.6f, 0.0f)),
                 green = meanAt(a, 1, with.camera, Vec3(0.0f, 0.6f, 0.0f));
    const Image dry = cycles(wetFloor(0.0f, w, h)), wet = cycles(wetFloor(1.0f, w, h));
    const double d = meanRows(dry, 1, 0.3f, 0.9f), wv = meanRows(wet, 1, 0.3f, 0.9f);
    std::printf("  floor under the chip %.3f (%.3f without), the chip %.3f red %.3f green; floor dry %.3f, wet %.3f\n",
                shade, lit, red, green, d, wv);
    CHECK(shade < 0.5 * lit);
    CHECK(red > 2.0 * green);
    CHECK(wv < 0.85 * d);
}
