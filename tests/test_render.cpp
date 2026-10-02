//
// The path tracer (render/): its hierarchies -- Embree's and our own -- meet
// what trying every triangle meets, and what each other meets, shadows
// through glass included; instances as the copies they stand for; a render
// the same however it is run; the sun lighting a floor as the viewport
// lights it; the denoiser nearer the converged picture than the noise, Open
// Image Denoise nearer still; the Output's settings; the files; the
// materials vegetation brings. And what moves: how fast, in the scene --
// each corner, each chip of grit -- and Cycles blurring it along its way
// while the shutter is open, a moving camera too. Over a plate: the light a
// picture shows got back through the view transforms; the CG over the plate
// in both renderers -- the plate as it went in where the CG changes nothing,
// darker in its shadow on a catcher, hidden behind a holdout, seen through
// glass -- and its passes in the EXR.
//
#include "pg/core/Grass.h"
#include "pg/core/Instances.h"
#include "pg/core/Tree.h"
#include "pg/io/Exr.h"
#include "pg/render/Cycles.h"
#include "pg/render/Denoise.h"
#include "pg/render/PathTracer.h"
#include "pg/render/Plate.h"
#include "pg/render/Save.h"
#include "pg/sim/Display.h"
#include "pg/sim/Network.h"

#include "test_framework.h"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <random>
#include <set>
#include <thread>

using namespace pg;
using namespace pg::render;
namespace fs = std::filesystem;

namespace {

/// A number in [-1, 1): the same with any standard library, as the
/// generator's numbers are (its distributions' are not).
float centred(std::mt19937& rng) { return static_cast<float>(rng() >> 8) * (2.0f / 16777216.0f) - 1.0f; }
/// Three, in this order -- a call's arguments are taken in no set order.
Vec3 centred3(std::mt19937& rng) {
    const float x = centred(rng);
    const float y = centred(rng);
    const float z = centred(rng);
    return {x, y, z};
}

/// `n` triangles strewn in a box 2 m wide, each some `size` across.
std::shared_ptr<Geometry> strewn(int n, uint32_t seed, float size = 0.3f) {
    std::mt19937 rng(seed);
    auto geo = std::make_shared<Geometry>();
    geo->addPoints(3 * static_cast<size_t>(n));
    auto P = geo->positionsForWrite();
    for (int t = 0; t < n; ++t) {
        const Vec3 c = centred3(rng) + Vec3(0.0f, 1.2f, 0.0f);
        for (int k = 0; k < 3; ++k) P[3 * t + k] = c + centred3(rng) * (0.5f * size);
        const uint32_t tri[3] = {static_cast<uint32_t>(3 * t), static_cast<uint32_t>(3 * t + 1), static_cast<uint32_t>(3 * t + 2)};
        geo->addPrimitive(tri, true);
    }
    return geo;
}

/// The nearest of `geo`'s triangles a ray meets, every one tried.
float bruteForce(const Geometry& geo, const Vec3& o, const Vec3& d) {
    float best = 1e30f;
    const auto P = geo.positions();
    for (size_t t = 0; t < geo.primitiveCount(); ++t) {
        const auto pts = geo.primitivePoints(t);
        const Vec3 a = P[pts[0]], e1 = P[pts[1]] - a, e2 = P[pts[2]] - a;
        const Vec3 p = cross(d, e2);
        const float det = dot(e1, p);
        if (std::fabs(det) < 1e-12f) continue;
        const Vec3 s = o - a;
        const float u = dot(s, p) / det;
        const Vec3 q = cross(s, e1);
        const float v = dot(d, q) / det;
        const float h = dot(e2, q) / det;
        if (u >= 0.0f && v >= 0.0f && u + v <= 1.0f && h > 0.0f) best = std::min(best, h);
    }
    return best;
}

sim::Look noFloor() {
    sim::Look k;
    k.floor = false;
    return k;
}

/// The engines this build has: ours, and Embree's when it is there.
std::vector<RayEngine> engines() {
    std::vector<RayEngine> e{RayEngine::Own};
    if (embreeAvailable()) e.push_back(RayEngine::Embree);
    return e;
}

SceneInput inputOf(GeometryPtr geo, const sim::Look& look, const sim::Camera& cam) {
    SceneInput in;
    in.geometry = std::move(geo);
    in.look = look;
    in.camera = cam;
    return in;
}

/// A small picture of triangles over the floor: many small ones in the
/// sun -- or a few large under an overcast sky, broad surfaces in each
/// other's soft shadows: the light a sky gives, noisy with few samples.
std::shared_ptr<const Scene> smallScene(int width, int height, bool overcast = false,
                                        RayEngine engine = defaultRayEngine()) {
    sim::Camera cam = sim::Camera::lookingAt(Vec3(1.6f, 1.9f, 2.0f), Vec3(0.0f, 1.0f, 0.0f));
    cam.width = width;
    cam.height = height;
    sim::Look look;
    if (overcast) {
        look.lightIntensity = 0.0f;
        look.skyIntensity = 1.0f;
        look.groundColor = Vec3(0.5f, 0.5f, 0.5f);
    }
    SceneBuilder builder(engine);
    return builder.build(inputOf(overcast ? strewn(12, 3, 1.6f) : strewn(300, 3), look, cam));
}

}  // namespace

TEST(render_bvh_meets_what_every_triangle_meets) {
    const auto geo = strewn(400, 11);
    for (const RayEngine engine : engines()) {
        SceneBuilder builder(engine);
        const auto scene = builder.build(inputOf(geo, noFloor(), sim::Camera()));
        CHECK(scene->engine == engine);
        CHECK_EQ(scene->embree != nullptr, engine == RayEngine::Embree);
        std::mt19937 rng(5);
        int hits = 0;
        for (int r = 0; r < 3000; ++r) {
            const Vec3 o = centred3(rng) * 3.0f + Vec3(0.0f, 1.2f, 0.0f);
            const Vec3 target = centred3(rng) * 0.8f + Vec3(0.0f, 1.2f, 0.0f);
            const Vec3 d = normalize(target - o);
            const float expected = bruteForce(*geo, o, d);
            Hit hit;
            const bool met = scene->intersect(o, d, 1e30f, 0.5f, hit);
            CHECK_EQ(met, expected < 1e29f);
            if (met) {
                ++hits;
                CHECK_NEAR(hit.t, expected, 1e-4f * std::max(1.0f, expected));
            }
            // A shadow ray stops at the same place.
            const Vec3 through = scene->transmittance(o, d, 1e30f);
            CHECK_EQ(through.x + through.y + through.z == 0.0f, expected < 1e29f);
        }
        std::printf("  %s: %d of 3000 met\n", rayEngineName(engine).c_str(), hits);
        CHECK(hits > 1000);
    }
}

TEST(render_instances_meet_rays_as_their_copies) {
    // Prototypes placed turned, sized and tinted; the same unpacked.
    auto proto = strewn(30, 2);
    auto geo = std::make_shared<Geometry>();
    geo->addPrototype(proto);
    const int n = 25;
    geo->addPoints(n);
    auto P = geo->positionsForWrite();
    auto k = geo->points().create("instance", AttrType::Int).write<int32_t>();
    auto orient = geo->points().create("orient", AttrType::Vec4).write<Vec4>();
    auto pscale = geo->points().create("pscale", AttrType::Float).write<float>();
    for (int i = 0; i < n; ++i) {
        P[i] = Vec3(static_cast<float>(i % 5) * 3.0f - 6.0f, 0.0f, static_cast<float>(i / 5) * 3.0f - 6.0f);
        k[i] = 0;
        const float a = 0.7f * static_cast<float>(i);
        orient[i] = Vec4(0.0f, std::sin(0.5f * a), 0.0f, std::cos(0.5f * a));
        pscale[i] = 0.6f + 0.05f * static_cast<float>(i);
    }
    const auto copies = unpackInstances(*geo);
    for (const RayEngine engine : engines()) {
        SceneBuilder builder(engine);
        const auto a = builder.build(inputOf(geo, noFloor(), sim::Camera()));
        const auto b = builder.build(inputOf(copies, noFloor(), sim::Camera()));
        CHECK_EQ(a->meshes.size(), 1u);  // the prototype once
        CHECK_EQ(a->placed.size(), static_cast<size_t>(n));
        std::mt19937 rng(9);
        int hits = 0;
        for (int r = 0; r < 2000; ++r) {
            // At one of them, from anywhere round.
            const Vec3 at = P[static_cast<size_t>(r % n)] + centred3(rng) * 0.6f + Vec3(0.0f, 1.0f, 0.0f);
            const Vec3 w = centred3(rng);
            const Vec3 o = at + normalize(Vec3(w.x, 0.3f + std::fabs(w.y), w.z)) * 8.0f;
            const Vec3 d = normalize(at - o);
            Hit ha, hb;
            const bool ma = a->intersect(o, d, 1e30f, 0.5f, ha), mb = b->intersect(o, d, 1e30f, 0.5f, hb);
            CHECK_EQ(ma, mb);
            if (ma && mb) {
                ++hits;
                CHECK_NEAR(ha.t, hb.t, 1e-3f);
                CHECK(length(ha.face - hb.face) < 1e-3f || length(ha.face + hb.face) < 1e-3f);
                CHECK(length(ha.color - hb.color) < 1e-4f);  // tinted as the copy
            }
        }
        std::printf("  %s: %d of 2000 met\n", rayEngineName(engine).c_str(), hits);
        CHECK(hits > 100);
    }
}

TEST(render_embree_meets_what_our_bvh_meets) {
    if (!embreeAvailable()) {
        std::printf("  no Embree in this build\n");
        return;
    }
    // Triangles of the geometry itself -- a quarter of them glass -- a
    // prototype placed turned, sized and tinted, a ball and a box; the
    // floor.
    auto geo = strewn(300, 17, 0.5f);
    auto glass = geo->primitives().create("glass", AttrType::Float).write<float>();
    for (size_t p = 0; p < glass.size(); p += 4) glass[p] = 1.0f;
    geo->addPrototype(strewn(40, 4));
    const size_t first = geo->pointCount();
    const int n = 30;
    geo->addPoints(n);
    auto P = geo->positionsForWrite();
    auto k = geo->points().create("instance", AttrType::Int).write<int32_t>();
    auto orient = geo->points().create("orient", AttrType::Vec4).write<Vec4>();
    auto pscale = geo->points().create("pscale", AttrType::Float).write<float>();
    auto tint = geo->points().create("tint", AttrType::Vec3).write<Vec3>();
    for (size_t pt = 0; pt < first; ++pt) k[pt] = -1;  // the triangles' own corners stand for nothing
    for (int i = 0; i < n; ++i) {
        const size_t pt = first + static_cast<size_t>(i);
        P[pt] = Vec3(static_cast<float>(i % 6) * 1.5f - 4.0f, -0.6f, static_cast<float>(i / 6) * 1.5f - 3.0f);
        k[pt] = 0;
        const float a = 1.3f * static_cast<float>(i);
        orient[pt] = Vec4(0.0f, std::sin(0.5f * a), 0.0f, std::cos(0.5f * a));
        pscale[pt] = 0.5f + 0.04f * static_cast<float>(i);
        tint[pt] = Vec3(1.0f, 0.5f + 0.01f * static_cast<float>(i), 0.8f);
    }
    SceneInput in = inputOf(geo, sim::Look(), sim::Camera());
    sim::Solid ball, box;
    ball.body.shape = sim::Shape::Sphere;
    ball.body.center = Vec3(0.5f, 1.0f, -0.5f);
    ball.body.size = Vec3(0.6f, 0.6f, 0.6f);
    box.body.shape = sim::Shape::Box;
    box.body.center = Vec3(-1.0f, 0.8f, 0.6f);
    box.body.rotation = Vec3(10.0f, 30.0f, 0.0f);
    box.body.size = Vec3(0.5f, 0.4f, 0.7f);
    in.solids = {ball, box};
    SceneBuilder ours(RayEngine::Own), embree(RayEngine::Embree);
    const auto a = ours.build(in), b = embree.build(in);
    CHECK(a->embree == nullptr && b->embree != nullptr);
    CHECK_EQ(b->meshes.size(), 2u);  // the geometry's own, the prototype
    CHECK_EQ(b->placed.size(), static_cast<size_t>(n) + 1);
    CHECK(b->embree->anyClear());
    std::mt19937 rng(23);
    int hits = 0, solids = 0, floors = 0, glassy = 0, shaded = 0;
    for (int r = 0; r < 4000; ++r) {
        const Vec3 o = centred3(rng) * 4.0f + Vec3(0.0f, 2.5f, 0.0f);
        const Vec3 target = centred3(rng) * Vec3(4.0f, 1.0f, 4.0f) + Vec3(0.0f, 0.4f, 0.0f);
        const Vec3 d = normalize(target - o);
        Hit ha, hb;
        const bool ma = a->intersect(o, d, 1e30f, 0.5f, ha), mb = b->intersect(o, d, 1e30f, 0.5f, hb);
        CHECK_EQ(ma, mb);
        if (ma && mb) {
            ++hits;
            CHECK_NEAR(ha.t, hb.t, 1e-4f * std::max(1.0f, ha.t));
            CHECK(length(ha.face - hb.face) < 1e-3f);
            CHECK(length(ha.normal - hb.normal) < 1e-3f);
            CHECK(length(ha.color - hb.color) < 1e-4f);
            CHECK(ha.material && hb.material && *ha.material == *hb.material);
            CHECK_EQ(ha.floor, hb.floor);
            solids += ha.material == &a->solidMaterial;
            floors += ha.floor;
            glassy += ha.material->kind == Material::Kind::Glass;
        }
        // Shadows: blocked alike, tinted alike through the glass.
        const Vec3 sun = normalize(Vec3(0.3f, 1.0f, 0.2f) + centred3(rng) * 0.3f);
        const Vec3 from = ma ? ha.position + ha.face * (dot(ha.face, sun) > 0.0f ? 1e-3f : -1e-3f) : o;
        const Vec3 ta = a->transmittance(from, sun, 1e30f), tb = b->transmittance(from, sun, 1e30f);
        CHECK(length(ta - tb) < 1e-4f);
        shaded += ta.x < 1.0f;
    }
    std::printf("  %d of 4000 met (%d solids, %d floor, %d glass); %d shadows\n", hits, solids, floors, glassy, shaded);
    CHECK(hits > 2000 && solids > 20 && floors > 100 && glassy > 20 && shaded > 200);
}

TEST(render_is_the_same_however_it_is_run) {
    std::vector<Image> depths;
    for (const RayEngine engine : engines()) {
        Settings s;
        s.width = 64;
        s.height = 40;
        s.samples = 3;
        s.denoise = false;
        // Each with a scene of its own: built again, the hierarchies the same.
        PathTracer a, b;
        a.setSettings(s);
        b.setSettings(s);
        a.setScene(smallScene(64, 40, false, engine));
        b.setScene(smallScene(64, 40, false, engine));
        for (int i = 0; i < 3; ++i) {
            CHECK(a.pass());
            CHECK(b.pass());
        }
        CHECK(a.done());
        const Image ia = a.beauty(), ib = b.beauty();
        CHECK(ia.pixels == ib.pixels);
        depths.push_back(a.depth());
        // Another seed: another noise.
        s.seed = 7;
        b.setSettings(s);
        for (int i = 0; i < 3; ++i) b.pass();
        CHECK(!(b.beauty().pixels == ia.pixels));
        // Stopped on the way: no sample added.
        std::atomic<bool> stop{true};
        PathTracer c;
        c.setSettings(s);
        c.setScene(smallScene(64, 40, false, engine));
        CHECK(!c.pass(&stop));
        CHECK_EQ(c.samples(), 0);
    }
    if (depths.size() == 2) {
        // The engines see the same: how far what each pixel first meets is.
        int same = 0, seen = 0;
        for (size_t p = 0; p < depths[0].pixels.size(); ++p) {
            const float za = depths[0].pixels[p], zb = depths[1].pixels[p];
            if (!std::isfinite(za) && !std::isfinite(zb)) continue;
            ++seen;
            same += std::isfinite(za) && std::isfinite(zb) && std::fabs(za - zb) <= 1e-4f * za;
        }
        std::printf("  depth the same in %d of %d pixels\n", same, seen);
        CHECK(seen > 1000 && same >= seen - seen / 100);
    }
}

TEST(render_sun_lights_the_floor_as_the_viewport_does) {
    // A white floor seen from straight above, the sun alone: albedo times
    // the sun's light times the cosine -- as the viewport shades it.
    sim::Look look;
    look.skyIntensity = 0.0f;
    look.groundColor = Vec3(0.5f, 0.5f, 0.5f);
    look.lightElevation = 50.0f;
    sim::Camera cam = sim::Camera::lookingAt(Vec3(0.0f, 3.0f, 0.001f), Vec3(0.0f, 0.0f, 0.0f));
    cam.width = 16;
    cam.height = 16;
    cam.focal = 200.0f;
    SceneBuilder builder;
    Settings s;
    s.width = 16;
    s.height = 16;
    s.samples = 64;
    s.denoise = false;
    PathTracer t;
    t.setSettings(s);
    t.setScene(builder.build(inputOf(nullptr, look, cam)));
    while (!t.done()) t.pass();
    const Image img = t.beauty();
    double sum = 0.0;
    for (size_t p = 0; p < img.pixels.size(); p += 3) sum += img.pixels[p + 1];
    const double mean = sum / static_cast<double>(img.pixels.size() / 3);
    const double expected = 0.5 * look.lightColor.y * look.lightIntensity * std::sin(50.0 * 3.14159265 / 180.0);
    std::printf("  floor %.4f, expected %.4f\n", mean, expected);
    CHECK(std::fabs(mean - expected) < 0.12 * expected);
}

TEST(render_denoiser_comes_nearer_the_converged_picture) {
    Settings s;
    s.width = 80;
    s.height = 50;
    s.denoise = false;
    s.samples = 256;
    PathTracer reference;
    reference.setSettings(s);
    reference.setScene(smallScene(80, 50, true));
    while (!reference.done()) reference.pass();
    const Image truth = reference.beauty();
    s.samples = 2;
    s.seed = 3;
    PathTracer noisy;
    noisy.setSettings(s);
    noisy.setScene(smallScene(80, 50, true));
    while (!noisy.done()) noisy.pass();
    auto error = [&](const Image& img) {
        double e = 0.0;
        for (size_t i = 0; i < img.pixels.size(); ++i) {
            const double a = std::min(img.pixels[i], 4.0f), b = std::min(truth.pixels[i], 4.0f);
            e += (a - b) * (a - b);
        }
        return std::sqrt(e / static_cast<double>(img.pixels.size()));
    };
    const double raw = error(noisy.beauty()), clean = error(noisy.denoised());
    if (const char* dump = std::getenv("PG_RENDER_DUMP")) {
        std::string e;
        savePicture(reference, std::string(dump) + "/ref.png", false, "", e);
        savePicture(noisy, std::string(dump) + "/raw.png", false, "", e);
        savePicture(noisy, std::string(dump) + "/den.png", true, "", e);
    }
    std::printf("  rms %.4f raw, %.4f denoised\n", raw, clean);
    CHECK(clean < 0.6 * raw);
}

TEST(render_open_image_denoise_comes_nearer_than_our_filter) {
    if (!oidnAvailable()) return;
    // A few samples under an overcast sky -- the noise of the light, not
    // of the edges -- the noise taken out by Open Image Denoise and by our
    // own filter, against many.
    Settings s;
    s.width = 80;
    s.height = 50;
    s.denoise = false;
    s.samples = 512;
    PathTracer reference;
    reference.setSettings(s);
    reference.setScene(smallScene(80, 50, true));
    while (!reference.done()) reference.pass();
    const Image truth = reference.beauty();
    s.samples = 4;
    s.seed = 5;
    PathTracer t;
    t.setSettings(s);
    t.setScene(smallScene(80, 50, true));
    while (!t.done()) t.pass();
    const Image noisy = t.beauty(), albedo = t.albedo(), normal = t.normal();
    auto error = [&](const Image& img) {
        double e = 0.0;
        for (size_t i = 0; i < img.pixels.size(); ++i) {
            const double a = std::min(img.pixels[i], 4.0f), b = std::min(truth.pixels[i], 4.0f);
            e += (a - b) * (a - b);
        }
        return std::sqrt(e / static_cast<double>(img.pixels.size()));
    };
    Image a, b;
    std::string why;
    CHECK(oidnDenoise(noisy, albedo, normal, a, why));
    CHECK(why.empty());
    CHECK(oidnDenoise(noisy, albedo, normal, b, why));
    CHECK(a.pixels == b.pixels);  // the same picture, the same again
    const double raw = error(noisy), oidn = error(a);
    const double own = error(denoise(noisy, albedo, normal, t.depth(), std::vector<float>(80 * 50, 0.01f)));
    std::printf("  rms %.4f raw, %.4f our filter, %.4f %s\n", raw, own, oidn, oidnVersion().c_str());
    CHECK(oidn < own && own < raw);
    // denoised() takes it, unless told otherwise.
    if (defaultDenoiser() == Denoiser::Oidn) CHECK(t.denoised().pixels == a.pixels);
    // Pictures of different sizes: refused, with why.
    CHECK(!oidnDenoise(noisy, Image(), normal, a, why) && !why.empty());
}

TEST(render_settings_come_from_the_output) {
    sim::Network net;
    const int out = net.add("output");
    CHECK(net.setParam(out, "render_samples", "300"));
    CHECK(net.setParam(out, "render_bounces", "7"));
    CHECK(net.setParam(out, "render_denoise", "0"));
    CHECK(net.setParam(out, "render_fstop", "2.8"));
    CHECK(net.setParam(out, "render_focus", "4.5"));
    CHECK(net.setParam(out, "render_sun_angle", "2"));
    CHECK(net.setParam(out, "render_motion_blur", "0.25"));
    const sim::Compiled c = net.compile();
    CHECK_EQ(c.render.samples, 300);
    CHECK_EQ(c.render.bounces, 7);
    CHECK(!c.render.denoise);
    CHECK_NEAR(c.render.fstop, 2.8f, 1e-5f);
    CHECK_NEAR(c.render.focus, 4.5f, 1e-5f);
    CHECK_NEAR(c.render.sunAngle, 2.0f, 1e-5f);
    CHECK_NEAR(c.render.shutter, 0.25f, 1e-6f);
    // Without them set: the defaults, saved and read back the same -- the
    // shutter open half a frame.
    CHECK_NEAR(Settings().shutter, 0.5f, 1e-6f);
    sim::Network plain;
    plain.add("output");
    CHECK(plain.compile().render == Settings());
}

TEST(render_scene_carries_how_fast_what_moves_goes) {
    // Triangles whose points go as fast as they are far from the origin,
    // that way: what each corner goes at -- in the order the triangles are
    // in the hierarchy's leaves -- tells where it is.
    auto geo = strewn(40, 5);
    const std::vector<Vec3> P(geo->positions().begin(), geo->positions().end());
    {
        auto v = geo->points().create("v", AttrType::Vec3).write<Vec3>();
        std::copy(P.begin(), P.end(), v.begin());
    }
    const sim::ShadedTriangles tris = sim::shadedTriangles(*geo);
    CHECK_EQ(tris.velocities.size(), tris.positions.size());
    for (size_t i = 0; i < tris.positions.size(); ++i) CHECK(length(tris.velocities[i] - tris.positions[i]) < 1e-6f);
    sim::Camera cam = sim::Camera::lookingAt(Vec3(1.6f, 1.9f, 2.0f), Vec3(0.0f, 1.0f, 0.0f));
    for (const RayEngine engine : engines()) {
        SceneBuilder builder(engine);
        const auto scene = builder.build(inputOf(geo, noFloor(), cam));
        CHECK_EQ(scene->meshes.size(), 1u);
        const Mesh& m = *scene->meshes[0];
        CHECK_EQ(m.velocity.size(), 3 * m.count());
        float off = 0.0f;
        for (size_t t = 0; t < m.count() && m.velocity.size() == 3 * m.count(); ++t) {
            const Vec3 corner[3] = {m.v0[t], m.v0[t] + m.e1[t], m.v0[t] + m.e2[t]};
            for (size_t c = 0; c < 3; ++c) off = std::max(off, length(m.velocity[3 * t + c] - corner[c]));
        }
        CHECK(off < 1e-5f);
    }
    // Standing: none.
    CHECK(SceneBuilder().build(inputOf(strewn(4, 5), noFloor(), cam))->meshes[0]->velocity.empty());
    // A chip of grit flies as its point does.
    auto grit = std::make_shared<Geometry>();
    grit->addPoints(1);
    grit->positionsForWrite()[0] = Vec3(0.0f, 1.0f, 0.0f);
    grit->points().create("pscale", AttrType::Float).write<float>()[0] = 0.05f;
    grit->points().create("v", AttrType::Vec3).write<Vec3>()[0] = Vec3(3.0f, 1.0f, -2.0f);
    SceneInput in = inputOf(nullptr, noFloor(), cam);
    in.bodies = grit;
    const auto chips = SceneBuilder().build(in);
    CHECK_EQ(chips->placed.size(), 1u);
    if (!chips->placed.empty()) CHECK(length(chips->placed[0].velocity - Vec3(3.0f, 1.0f, -2.0f)) < 1e-6f);

    // The camera between two frames: halfway along, its lens halfway, turned
    // the shorter way round -- from 170 degrees to -170 through 180, not 0.
    sim::Camera a, b;
    a.position = Vec3(0.0f, 1.0f, 0.0f);
    a.rotation = Vec3(0.0f, 170.0f, 0.0f);
    a.focal = 30.0f;
    b.position = Vec3(2.0f, 1.0f, 0.0f);
    b.rotation = Vec3(0.0f, -170.0f, 0.0f);
    b.focal = 50.0f;
    const sim::Camera half = a.toward(b, 0.5f);
    CHECK(length(half.position - Vec3(1.0f, 1.0f, 0.0f)) < 1e-5f);
    CHECK_NEAR(half.focal, 40.0f, 1e-4f);
    CHECK(dot(half.forward(), normalize(a.forward() + b.forward())) > 0.9999f);
    CHECK(length(a.toward(b, 0.0f).forward() - a.forward()) < 1e-5f);
    CHECK(length(a.toward(b, 1.0f).forward() - b.forward()) < 1e-5f);
}

TEST(render_saves_png_and_exr_with_passes) {
    Settings s;
    s.width = 32;
    s.height = 20;
    s.samples = 2;
    PathTracer t;
    t.setSettings(s);
    std::string error;
    const fs::path dir = fs::temp_directory_path() / "pg_render_test";
    fs::create_directories(dir);
    CHECK(!savePicture(t, (dir / "none.png").string(), true, "", error));  // nothing rendered
    t.setScene(smallScene(32, 20));
    while (!t.done()) t.pass();
    CHECK(savePicture(t, (dir / "a.png").string(), true, "", error));
    CHECK(fs::file_size(dir / "a.png") > 100);
    CHECK(savePicture(t, (dir / "a.exr").string(), false, "test", error));
    io::ExrImage exr;
    CHECK(io::readExr((dir / "a.exr").string(), exr, error));
    CHECK_EQ(exr.width, 32);
    CHECK_EQ(exr.height, 20);
    std::set<std::string> names;
    for (const io::ExrChannel& c : exr.channels) names.insert(c.name);
    for (const char* n : {"R", "G", "B", "A", "Z", "albedo.R", "albedo.G", "albedo.B", "N.X", "N.Y", "N.Z"}) {
        CHECK(names.count(n) == 1);
    }
    fs::remove_all(dir);
}

TEST(render_vegetation_lets_light_through) {
    // Grass and leaves carry a translucency; bark none.
    const Geometry clump = growGrassClump(GrassSettings(), 4);
    const AttributeArray* t = clump.primitives().find("translucency");
    CHECK(t && t->type() == AttrType::Float);
    for (const float v : t->read<float>()) CHECK_NEAR(v, 0.35f, 1e-6f);
    const TreeSettings ts;
    Geometry tree;
    meshTree(growTree(ts, Vec3(), 1.0f, 2), ts, 0, tree);
    const AttributeArray* tt = tree.primitives().find("translucency");
    const AttributeArray* level = tree.primitives().find("level");
    CHECK(tt && level);
    int leaves = 0;
    for (size_t p = 0; p < tree.primitiveCount(); ++p) {
        const bool leaf = level->read<int32_t>()[p] < 0;
        leaves += leaf;
        CHECK_NEAR(tt->read<float>()[p], leaf ? 0.4f : 0.0f, 1e-6f);
    }
    CHECK(leaves > 0);
    // ... which the scene makes its material.
    const auto mesh = meshOf(clump);
    CHECK_EQ(mesh->materials.size(), 1u);
    CHECK_NEAR(mesh->materials[0].translucency, 0.35f, 0.01f);
}

namespace {

/// The mean of each block of `n` x `n` pixels' brightness: a picture as
/// its shapes, its noise averaged away.
std::vector<double> blocks(const Image& img, int n) {
    std::vector<double> out;
    for (int by = 0; by + n <= img.height; by += n) {
        for (int bx = 0; bx + n <= img.width; bx += n) {
            double sum = 0.0;
            for (int y = by; y < by + n; ++y) {
                for (int x = bx; x < bx + n; ++x) {
                    const float* p = &img.pixels[(static_cast<size_t>(y) * static_cast<size_t>(img.width) + static_cast<size_t>(x)) * 3];
                    sum += 0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2];
                }
            }
            out.push_back(sum / (n * n));
        }
    }
    return out;
}

Image cyclesRender(const std::shared_ptr<const Scene>& scene, const Settings& s) {
    CyclesRender render;
    render.start(scene, s);
    render.wait();
    CHECK(render.error().empty());
    return render.beauty();
}

}  // namespace

TEST(render_cycles_lights_a_floor_as_the_sun_and_the_sky_do) {
    if (!cyclesAvailable()) return;
    // A grey floor seen from above, under a white sky, then under the sun
    // alone: as bright as a matte floor is -- the light as strong as ours,
    // from where ours comes from.
    for (const bool sun : {false, true}) {
        sim::Camera cam = sim::Camera::lookingAt(Vec3(0.0f, 3.0f, 0.01f), Vec3(0.0f, 0.0f, 0.0f));
        cam.width = 32;
        cam.height = 32;
        sim::Look look;
        look.lightIntensity = sun ? 2.2f : 0.0f;
        look.lightElevation = 60.0f;
        look.lightColor = Vec3(1.0f, 1.0f, 1.0f);
        look.skyIntensity = sun ? 0.0f : 1.0f;
        look.skyColor = Vec3(1.0f, 1.0f, 1.0f);
        look.groundColor = Vec3(0.5f, 0.5f, 0.5f);
        SceneBuilder builder;
        const auto scene = builder.build(inputOf(strewn(1, 1, 0.1f), look, cam));
        Settings s;
        s.width = 32;
        s.height = 32;
        s.samples = 64;
        s.denoise = false;
        s.sky = Settings::Sky::Look;
        s.detail = 0.0f;
        const Image cycles = cyclesRender(scene, s);
        CHECK_EQ(cycles.pixels.size(), 3u * 32u * 32u);
        double mean = 0.0;
        for (size_t p = 0; p < 32u * 32u; ++p) mean += cycles.pixels[3 * p + 1];
        mean /= 1024.0;
        const double matte = sun ? 0.5 * 2.2 * std::sin(60.0 * 3.14159265358979 / 180.0) : 0.5;
        std::printf("  the floor under the %s: %.3f, a matte one %.3f\n", sun ? "sun" : "sky", mean, matte);
        CHECK(std::fabs(mean / matte - 1.0) < 0.05);
    }
}

TEST(render_cycles_shows_what_the_path_tracer_does) {
    if (!cyclesAvailable()) return;
    // The same scene -- the sun, the sky, the floor, triangles strewn on
    // it -- through both: the same picture, but for the noise and how each
    // makes a surface reflect (Cycles' reflections lose no light where the
    // surface is rough; ours, some).
    Settings s;
    s.width = 96;
    s.height = 60;
    s.samples = 256;
    s.denoise = false;
    s.sky = Settings::Sky::Look;
    s.detail = 0.0f;
    const auto scene = smallScene(96, 60);
    PathTracer t;
    t.setSettings(s);
    t.setScene(scene);
    while (!t.done()) t.pass();
    const Image ours = t.beauty();
    const Image cycles = cyclesRender(scene, s);
    CHECK_EQ(cycles.width, 96);
    CHECK_EQ(cycles.height, 60);
    const std::vector<double> a = blocks(ours, 12), b = blocks(cycles, 12);
    CHECK_EQ(a.size(), b.size());
    double meanA = 0.0, meanB = 0.0, off = 0.0;
    for (size_t i = 0; i < a.size(); ++i) {
        meanA += a[i];
        meanB += b[i];
    }
    meanA /= static_cast<double>(a.size());
    meanB /= static_cast<double>(b.size());
    for (size_t i = 0; i < a.size(); ++i) off += std::fabs(a[i] / meanA - b[i] / meanB);
    off /= static_cast<double>(a.size());
    std::printf("  mean brightness %.4f ours, %.4f %s; blocks off by %.3f\n", meanA, meanB, cyclesVersion().c_str(), off);
    // About as bright, and the same shapes where they are: the camera,
    // the scene the right way up, the shadows where the sun casts them.
    CHECK(std::fabs(meanB / meanA - 1.0) < 0.25);
    CHECK(off < 0.08);
}

TEST(render_cycles_is_the_same_twice_and_its_passes_are_ours) {
    if (!cyclesAvailable()) return;
    Settings s;
    s.width = 64;
    s.height = 40;
    s.samples = 16;
    s.denoise = true;
    s.sky = Settings::Sky::Look;
    s.detail = 0.0f;
    // A few large triangles: few pixels on their edges, where a pixel's
    // samples see different things.
    const auto scene = smallScene(64, 40, true);
    CyclesRender a;
    a.start(scene, s);
    a.wait();
    const Image first = a.beauty();
    CHECK(cyclesRender(scene, s).pixels == first.pixels);  // the same picture, the same again
    // What the pixels see, in our axes, as the path tracer has it: the
    // normals turned back to Y up, the depth along the ray, infinite for
    // the sky.
    const Image n = a.normal(), z = a.depth(), albedo = a.albedo();
    CHECK_EQ(n.width, 64);
    CHECK_EQ(z.width, 64);
    CHECK_EQ(albedo.width, 64);
    PathTracer t;
    t.setSettings(s);
    t.setScene(scene);
    while (!t.done()) t.pass();
    const Image ourN = t.normal(), ourZ = t.depth();
    int both = 0, sky = 0, ourSky = 0, alike = 0, near = 0;
    for (size_t p = 0; p < 64u * 40u; ++p) {
        sky += !std::isfinite(z.pixels[p]);
        ourSky += !std::isfinite(ourZ.pixels[p]);
        if (!std::isfinite(z.pixels[p]) || !std::isfinite(ourZ.pixels[p])) continue;
        ++both;
        const Vec3 cn(n.pixels[3 * p], n.pixels[3 * p + 1], n.pixels[3 * p + 2]);
        const Vec3 on(ourN.pixels[3 * p], ourN.pixels[3 * p + 1], ourN.pixels[3 * p + 2]);
        // Cycles turns a normal toward the camera; ours stays on the side
        // the triangle was made with.
        alike += std::fabs(dot(cn, on)) > 0.99f * length(cn) * length(on);
        near += std::fabs(z.pixels[p] / ourZ.pixels[p] - 1.0f) < 0.03f;
    }
    std::printf("  of %d pixels both see, normals alike in %d, as far in %d; sky %d pixels against %d\n", both, alike,
                near, sky, ourSky);
    // But where a pixel's samples see different things: an edge, the floor
    // far away -- Cycles' depth is its first sample's, ours their mean.
    CHECK(both > 1000);
    CHECK(alike > both * 17 / 20);
    CHECK(near > both * 17 / 20);
    CHECK(std::abs(sky - ourSky) < 64 * 40 / 50);
    // Saved: the PNG as the path tracer's is, the EXR with its passes.
    const fs::path dir = fs::temp_directory_path() / "pg_test_cycles";
    fs::create_directories(dir);
    Rendered r;
    r.beauty = first;
    r.albedo = albedo;
    r.normal = n;
    r.depth = z;
    std::string error;
    CHECK(savePicture(r, (dir / "c.png").string(), "", error));
    CHECK(savePicture(r, (dir / "c.exr").string(), "cycles", error));
    io::ExrImage exr;
    CHECK(io::readExr((dir / "c.exr").string(), exr, error));
    std::set<std::string> names;
    for (const auto& c : exr.channels) names.insert(c.name);
    for (const char* want : {"R", "G", "B", "A", "Z", "albedo.R", "N.Y"}) CHECK(names.count(want));
    fs::remove_all(dir);
}

TEST(render_cycles_for_the_render_tab_shows_pictures_and_goes_on_after_a_stop) {
    if (!cyclesAvailable()) return;
    // As the Render tab has it: pictures as the samples add up -- the first
    // of fewer, larger pixels -- then, stopped (another renderer chosen)
    // and asked again, it renders again.
    Settings s;
    s.width = 96;
    s.height = 60;
    s.samples = 32;
    const auto scene = smallScene(96, 60);
    CyclesRender r(true);
    auto picture = [&](Image& out) {
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        while (std::chrono::steady_clock::now() < until) {
            if (r.takePicture(out)) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    };
    r.start(scene, s);
    Image first;
    CHECK(picture(first));
    CHECK(first.width > 0 && first.width <= 96 && first.height * 96 == first.width * 60);
    r.cancel();
    CHECK(r.done());
    r.start(scene, s);
    Image again;
    CHECK(picture(again));
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (!r.done() && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(r.done());
    CHECK_EQ(r.samples(), 32);
    CHECK(r.error().empty());
    // The end: the whole picture.
    Image last = again;
    while (r.takePicture(last)) {
    }
    CHECK_EQ(r.beauty().width, 96);
    CHECK_EQ(r.beauty().height, 60);
}

TEST(render_agx_shows_middle_grey_as_blender_does_and_bright_colours_going_white) {
    using V = Settings::View;
    // Middle grey half way up, black black, a light 16 times as bright as
    // white nearly white -- and in between, the brighter the lighter.
    const Vec3 grey = shown(Vec3(0.18f, 0.18f, 0.18f), V::AgX);
    std::printf("  AgX: middle grey %.3f, black %.3f, 16 %.3f\n", grey.y, shown(Vec3(), V::AgX).y,
                shown(Vec3(16.0f, 16.0f, 16.0f), V::AgX).y);
    CHECK(std::fabs(grey.y - 0.5f) < 0.03f);
    CHECK(std::fabs(grey.x - grey.y) < 0.01f && std::fabs(grey.z - grey.y) < 0.01f);
    CHECK(shown(Vec3(), V::AgX).y < 0.02f);
    CHECK(shown(Vec3(16.0f, 16.0f, 16.0f), V::AgX).y > 0.95f);
    float last = -1.0f;
    for (float v = 0.001f; v < 64.0f; v *= 1.5f) {
        const float s = shown(Vec3(v, v, v), V::AgX).y;
        CHECK(s >= last);
        last = s;
    }
    // A very bright red: towards white, as on film -- the viewport's ACES,
    // channel by channel, keeps it red.
    const Vec3 red(20.0f, 0.05f, 0.05f);
    std::printf("  a bright red: AgX %.2f %.2f %.2f, ACES %.2f %.2f %.2f\n", shown(red, V::AgX).x, shown(red, V::AgX).y,
                shown(red, V::AgX).z, shown(red, V::Aces).x, shown(red, V::Aces).y, shown(red, V::Aces).z);
    CHECK(shown(red, V::AgX).y > 0.5f && shown(red, V::AgX).z > 0.5f);
    CHECK(shown(red, V::Aces).y < 0.4f);
    // Punchy: darker below the middle, as bright at white, more colour.
    const Vec3 punchy = shown(Vec3(0.18f, 0.18f, 0.18f), V::AgXPunchy);
    const Vec3 tint = shown(Vec3(0.3f, 0.18f, 0.1f), V::AgXPunchy), plain = shown(Vec3(0.3f, 0.18f, 0.1f), V::AgX);
    std::printf("  Punchy: middle grey %.3f; an orange %.2f %.2f %.2f (AgX %.2f %.2f %.2f)\n", punchy.y, tint.x, tint.y,
                tint.z, plain.x, plain.y, plain.z);
    CHECK(punchy.y < grey.y - 0.05f && punchy.y > 0.3f);
    CHECK(tint.x - tint.z > plain.x - plain.z);
    CHECK(shown(Vec3(16.0f, 16.0f, 16.0f), V::AgXPunchy).y > 0.95f);
}

TEST(render_cycles_lights_a_day_under_a_physical_sky) {
    if (!cyclesAvailable()) return;
    // A grey floor from above in Nishita's day: the sun as bright as the
    // look's and of its colour, the blue sky adding a little -- more of it,
    // and bluer, when the sun is low; the sky overhead blue.
    auto floorLight = [](float elevation, const Vec3& colour, bool up) {
        sim::Camera cam = up ? sim::Camera::lookingAt(Vec3(0.0f, 1.0f, 0.0f), Vec3(0.01f, 5.0f, 0.0f))
                             : sim::Camera::lookingAt(Vec3(0.0f, 3.0f, 0.01f), Vec3(0.0f, 0.0f, 0.0f));
        cam.width = 32;
        cam.height = 32;
        sim::Look look;
        look.lightIntensity = 2.2f;
        look.lightElevation = elevation;
        look.lightColor = colour;
        look.groundColor = Vec3(0.5f, 0.5f, 0.5f);
        look.skyBehind = true;
        SceneBuilder builder;
        const auto scene = builder.build(inputOf(strewn(1, 1, 0.01f), look, cam));
        Settings s;
        s.width = 32;
        s.height = 32;
        s.samples = 64;
        s.denoise = true;
        s.sky = Settings::Sky::Physical;
        s.detail = 0.0f;
        const Image img = cyclesRender(scene, s);
        Vec3 mean;
        for (size_t p = 0; p < 32u * 32u; ++p) mean += Vec3(img.pixels[3 * p], img.pixels[3 * p + 1], img.pixels[3 * p + 2]);
        return mean / 1024.0f;
    };
    const Vec3 white(1.0f, 1.0f, 1.0f), warm(1.0f, 0.8f, 0.6f);
    const Vec3 high = floorLight(45.0f, white, false), low = floorLight(10.0f, white, false),
               golden = floorLight(45.0f, warm, false), sky = floorLight(45.0f, white, true);
    const float sun = 0.5f * 2.2f * std::sin(45.0f * 3.14159265f / 180.0f);
    const float bright = 0.2126f * high.x + 0.7152f * high.y + 0.0722f * high.z;
    std::printf("  the floor in the day: %.3f (the sun alone %.3f); red over blue %.2f, the sun low %.2f, the sun "
                "golden %.2f; the sky overhead %.2f %.2f %.2f\n",
                bright, sun, high.x / high.z, low.x / low.z, golden.x / golden.z, sky.x, sky.y, sky.z);
    CHECK(bright > 1.0f * sun && bright < 1.6f * sun);
    CHECK(high.x / high.z < 1.0f && high.x / high.z > 0.7f);
    CHECK(low.x / low.z < high.x / high.z);
    CHECK(golden.x / golden.z > 1.3f);
    CHECK(sky.z > 1.2f * sky.x);
}

TEST(render_cycles_surface_detail_makes_a_flat_surface_uneven) {
    if (!cyclesAvailable()) return;
    // The floor from above under an even sky -- some 40 m of it, its
    // patches several -- flat without the detail, as bright all over;
    // uneven with it, about as bright on the whole.
    sim::Camera cam = sim::Camera::lookingAt(Vec3(0.0f, 60.0f, 0.01f), Vec3(0.0f, 0.0f, 0.0f));
    cam.width = 48;
    cam.height = 48;
    sim::Look look;
    look.lightIntensity = 0.0f;
    look.skyIntensity = 1.0f;
    look.skyColor = Vec3(1.0f, 1.0f, 1.0f);
    look.groundColor = Vec3(0.5f, 0.5f, 0.5f);
    SceneBuilder builder;
    const auto scene = builder.build(inputOf(strewn(1, 1, 0.01f), look, cam));
    auto spread = [&](float detail, double& mean) {
        Settings s;
        s.width = 48;
        s.height = 48;
        s.samples = 32;
        s.denoise = true;
        s.sky = Settings::Sky::Look;
        s.detail = detail;
        const Image img = cyclesRender(scene, s);
        double sum = 0.0, square = 0.0;
        for (size_t p = 0; p < 48u * 48u; ++p) {
            sum += img.pixels[3 * p + 1];
            square += double(img.pixels[3 * p + 1]) * img.pixels[3 * p + 1];
        }
        mean = sum / (48.0 * 48.0);
        return std::sqrt(std::max(square / (48.0 * 48.0) - mean * mean, 0.0)) / mean;
    };
    double flatMean = 0.0, detailMean = 0.0;
    const double flat = spread(0.0f, flatMean), uneven = spread(1.0f, detailMean);
    std::printf("  the floor: %.3f flat, varying %.3f; with detail %.3f, varying %.3f\n", flatMean, flat, detailMean,
                uneven);
    CHECK(uneven > 3.0 * flat && uneven > 0.03);
    CHECK(std::fabs(detailMean / flatMean - 1.0) < 0.1);
}

TEST(render_cycles_lights_the_scene_with_a_sky_picture) {
    if (!cyclesAvailable()) return;
    // A picture all round: a blue sky over brown ground, a quarter of the
    // sky five times as bright. The floor lit by the sky alone as a matte
    // floor is under an even sky that colour; twice as bright with Sky
    // Strength 2; seen straight up, the sky's blue; turned, the bright
    // quarter goes round.
    const std::string file = (std::filesystem::temp_directory_path() / "pg_test_sky.exr").string();
    {
        io::ExrImage sky;
        sky.width = 64;
        sky.height = 32;
        const char* names[3] = {"R", "G", "B"};
        const float above[3] = {0.2f, 0.35f, 0.9f}, below[3] = {0.3f, 0.2f, 0.1f};
        for (int c = 0; c < 3; ++c) {
            io::ExrChannel ch;
            ch.name = names[c];
            ch.half = false;
            for (int y = 0; y < 32; ++y) {
                for (int x = 0; x < 64; ++x) {
                    const bool bright = y < 16 && x < 16;
                    ch.values.push_back(y < 16 ? above[c] * (bright ? 5.0f : 1.0f) : below[c]);
                }
            }
            sky.channels.push_back(std::move(ch));
        }
        std::string error;
        CHECK(io::writeExr(sky, file, error));
    }
    auto render = [&](const sim::Camera& c, float rotation, float strength, bool behind) {
        sim::Camera cam = c;
        cam.width = 32;
        cam.height = 32;
        sim::Look look;
        look.lightIntensity = 0.0f;
        look.groundColor = Vec3(0.5f, 0.5f, 0.5f);
        look.skyBehind = behind;
        SceneBuilder builder;
        const auto scene = builder.build(inputOf(strewn(1, 1, 0.01f), look, cam));
        Settings s;
        s.width = 32;
        s.height = 32;
        s.samples = 64;
        s.denoise = true;
        s.sky = Settings::Sky::Image;
        s.skyImage = file;
        s.skyRotation = rotation;
        s.skyStrength = strength;
        s.detail = 0.0f;
        const Image img = cyclesRender(scene, s);
        Vec3 mean;
        for (size_t p = 0; p < 32u * 32u; ++p) mean += Vec3(img.pixels[3 * p], img.pixels[3 * p + 1], img.pixels[3 * p + 2]);
        return mean / 1024.0f;
    };
    const sim::Camera down = sim::Camera::lookingAt(Vec3(0.0f, 3.0f, 0.01f), Vec3(0.0f, 0.0f, 0.0f));
    const sim::Camera up = sim::Camera::lookingAt(Vec3(0.0f, 1.0f, 0.0f), Vec3(0.01f, 5.0f, 0.0f));
    const Vec3 floor = render(down, 0.0f, 1.0f, false), twice = render(down, 0.0f, 2.0f, false);
    const Vec3 overhead = render(up, 0.0f, 1.0f, true);
    // The sky's light on the floor, the bright quarter's share with it.
    const Vec3 sky = Vec3(0.2f, 0.35f, 0.9f) * 2.0f;
    std::printf("  under the picture: the floor %.3f %.3f %.3f (an even sky %.3f %.3f %.3f), twice %.2f; overhead %.2f "
                "%.2f %.2f\n",
                floor.x, floor.y, floor.z, 0.5f * sky.x, 0.5f * sky.y, 0.5f * sky.z, twice.z / floor.z, overhead.x,
                overhead.y, overhead.z);
    CHECK(std::fabs(floor.z / (0.5f * sky.z) - 1.0f) < 0.25f);
    CHECK(floor.z > 2.0f * floor.x);
    CHECK(std::fabs(twice.z / floor.z - 2.0f) < 0.15f);
    CHECK(overhead.z > 2.0f * overhead.x);
    // Looking out at the horizon, the picture turned a quarter at a time.
    float least = 1e30f, most = 0.0f;
    for (int k = 0; k < 4; ++k) {
        const Vec3 m = render(sim::Camera::lookingAt(Vec3(0.0f, 1.0f, 0.0f), Vec3(5.0f, 1.8f, 0.0f)), 90.0f * k, 1.0f, true);
        least = std::min(least, m.z);
        most = std::max(most, m.z);
    }
    std::printf("  turned round: the view's blue %.3f to %.3f\n", least, most);
    CHECK(most > 2.0f * least);
    std::filesystem::remove(file);
}

TEST(render_cycles_clouds_cover_the_sky_and_drift_on_the_wind) {
    if (!cyclesAvailable()) return;
    // Looking up into Nishita's sky: clear, blue; clouded, whiter and
    // brighter; later, the wind has moved the clouds.
    auto render = [](float clouds, float time) {
        sim::Camera cam = sim::Camera::lookingAt(Vec3(0.0f, 1.0f, 0.0f), Vec3(0.3f, 5.0f, 0.0f));
        cam.width = 32;
        cam.height = 32;
        sim::Look look;
        look.lightIntensity = 2.2f;
        look.lightElevation = 45.0f;
        look.skyBehind = true;
        SceneInput in = inputOf(strewn(1, 1, 0.01f), look, cam);
        in.time = time;
        SceneBuilder builder;
        const auto scene = builder.build(in);
        Settings s;
        s.width = 32;
        s.height = 32;
        s.samples = 16;
        s.denoise = true;
        s.sky = Settings::Sky::Physical;
        s.clouds = clouds;
        s.cloudSize = 0.5f;
        s.cloudWind = 20.0f;
        s.detail = 0.0f;
        return cyclesRender(scene, s);
    };
    auto mean = [](const Image& img) {
        Vec3 m;
        for (size_t p = 0; p < 32u * 32u; ++p) m += Vec3(img.pixels[3 * p], img.pixels[3 * p + 1], img.pixels[3 * p + 2]);
        return m / 1024.0f;
    };
    const Vec3 clear = mean(render(0.0f, 0.0f)), cloudy = mean(render(0.8f, 0.0f));
    const Image now = render(0.5f, 0.0f), later = render(0.5f, 300.0f);
    double moved = 0.0;
    for (size_t i = 0; i < now.pixels.size(); ++i) moved += std::fabs(now.pixels[i] - later.pixels[i]);
    moved /= static_cast<double>(now.pixels.size());
    std::printf("  overhead: clear %.3f %.3f %.3f, clouded %.3f %.3f %.3f; the wind moved them by %.3f a pixel\n", clear.x,
                clear.y, clear.z, cloudy.x, cloudy.y, cloudy.z, moved);
    CHECK(clear.z > 1.5f * clear.x);
    CHECK(cloudy.z / cloudy.x < 0.8f * clear.z / clear.x);
    CHECK(cloudy.y > clear.y);
    CHECK(moved > 0.02 * clear.z);
}

TEST(render_cycles_blurs_what_moves_while_the_shutter_is_open) {
    if (!cyclesAvailable()) return;
    // A white square 30 cm across, 3 m in front of the camera, flying
    // sideways at 24 m/s: the shutter open half a frame of 1/24 s, it goes
    // 25 cm while it is open -- seen smeared along its way, wider than it
    // is, fainter; the light it sends the same. A chip of grit flying so
    // the same; the square standing while the camera goes by the same.
    const int w = 96, h = 64;
    const sim::Camera cam = [&] {
        sim::Camera c = sim::Camera::lookingAt(Vec3(0.0f, 1.0f, 3.0f), Vec3(0.0f, 1.0f, 0.0f));
        c.width = w;
        c.height = h;
        return c;
    }();
    sim::Look look = noFloor();
    look.lightIntensity = 0.0f;
    look.skyIntensity = 1.0f;
    look.skyColor = Vec3(1.0f);
    const Vec3 fast(24.0f, 0.0f, 0.0f);
    auto square = [](const Vec3& v) {
        auto geo = std::make_shared<Geometry>();
        geo->addPoints(4);
        auto P = geo->positionsForWrite();
        P[0] = Vec3(-0.15f, 0.85f, 0.0f);
        P[1] = Vec3(0.15f, 0.85f, 0.0f);
        P[2] = Vec3(0.15f, 1.15f, 0.0f);
        P[3] = Vec3(-0.15f, 1.15f, 0.0f);
        const uint32_t quad[4] = {0, 1, 2, 3};
        geo->addPrimitive(quad, true);
        auto cd = geo->points().create("Cd", AttrType::Vec3).write<Vec3>();
        std::fill(cd.begin(), cd.end(), Vec3(0.9f));
        if (v != Vec3(0.0f)) {
            auto vel = geo->points().create("v", AttrType::Vec3).write<Vec3>();
            std::fill(vel.begin(), vel.end(), v);
        }
        return geo;
    };
    auto chip = [](const Vec3& v) {
        auto geo = std::make_shared<Geometry>();
        geo->addPoints(1);
        geo->positionsForWrite()[0] = Vec3(0.0f, 1.0f, 0.0f);
        geo->points().create("pscale", AttrType::Float).write<float>()[0] = 0.15f;
        geo->points().create("Cd", AttrType::Vec3).write<Vec3>()[0] = Vec3(0.9f);
        geo->points().create("v", AttrType::Vec3).write<Vec3>()[0] = v;
        return geo;
    };
    SceneBuilder builder;
    auto render = [&](const SceneInput& in, float shutter) {
        Settings s;
        s.width = w;
        s.height = h;
        s.samples = 32;
        s.denoise = false;
        s.sky = Settings::Sky::Look;
        s.detail = 0.0f;
        s.shutter = shutter;
        return cyclesRender(builder.build(in), s);
    };
    auto input = [&](GeometryPtr geo, GeometryPtr bodies) {
        SceneInput in = inputOf(std::move(geo), look, cam);
        in.bodies = std::move(bodies);
        in.frameTime = 1.0f / 24.0f;
        return in;
    };
    // Across the middle rows: how much brighter than the backdrop each
    // column is; then how many columns are so much brighter, the most, and
    // all of it.
    auto across = [&](const Image& img) {
        std::vector<double> e(static_cast<size_t>(w), 0.0);
        for (int y = h / 2 - 3; y <= h / 2 + 3; ++y) {
            for (int x = 0; x < w; ++x) {
                const float* p = &img.pixels[3 * (static_cast<size_t>(y) * w + static_cast<size_t>(x))];
                e[static_cast<size_t>(x)] += (0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2]) / 7.0;
            }
        }
        double back = 0.0;
        for (size_t x = 0; x < 8; ++x) back += (e[x] + e[e.size() - 1 - x]) / 16.0;
        for (double& v : e) v -= back;
        return e;
    };
    struct Seen {
        int wide = 0;
        double peak = 0.0, light = 0.0;
    };
    auto seen = [&](const Image& img, double over) {
        Seen s;
        for (const double v : across(img)) {
            s.wide += v > over;
            s.peak = std::max(s.peak, v);
            s.light += v;
        }
        return s;
    };

    // Standing -- or moving, the shutter shut: the same picture, to the bit.
    const Image still = render(input(square(Vec3(0.0f)), nullptr), 0.5f);
    CHECK(render(input(square(fast), nullptr), 0.0f).pixels == still.pixels);
    const double over = 0.1 * seen(still, 0.0).peak;
    const Seen sharp = seen(still, over);
    SceneInput passing = input(square(Vec3(0.0f)), nullptr);
    passing.cameraMotion = true;
    passing.cameraBefore = passing.cameraAfter = cam;
    passing.cameraBefore.position.x -= 1.0f;  // 1 m a frame: 25 cm while the shutter is open
    passing.cameraAfter.position.x += 1.0f;
    const Seen moving = seen(render(input(square(fast), nullptr), 0.5f), over);
    const Seen camera = seen(render(passing, 0.5f), over);
    std::printf("  the square %d columns wide, at most %.3f, %.2f in all; flying %d, %.3f, %.2f; the camera passing "
                "%d, %.3f, %.2f\n",
                sharp.wide, sharp.peak, sharp.light, moving.wide, moving.peak, moving.light, camera.wide, camera.peak,
                camera.light);
    CHECK(sharp.wide >= 8 && sharp.wide <= 13);
    for (const Seen& b : {moving, camera}) {
        CHECK(b.wide >= sharp.wide + 8);
        CHECK(b.peak < 0.8 * sharp.peak);
        CHECK(std::fabs(b.light / sharp.light - 1.0) < 0.15);
    }
    // A chip of grit: placed, it flies as its point does.
    const Image chipStill = render(input(nullptr, chip(Vec3(0.0f))), 0.5f);
    const double chipOver = 0.1 * seen(chipStill, 0.0).peak;
    const Seen chipSharp = seen(chipStill, chipOver), chipFlying = seen(render(input(nullptr, chip(fast)), 0.5f), chipOver);
    std::printf("  the chip %d columns wide, at most %.3f, %.2f in all; flying %d, %.3f, %.2f\n", chipSharp.wide,
                chipSharp.peak, chipSharp.light, chipFlying.wide, chipFlying.peak, chipFlying.light);
    CHECK(chipSharp.wide >= 4);
    CHECK(chipFlying.wide >= chipSharp.wide + 8);
    CHECK(chipFlying.peak < 0.8 * chipSharp.peak);
    CHECK(std::fabs(chipFlying.light / chipSharp.light - 1.0) < 0.15);
}

TEST(render_unshown_gives_back_the_light_a_picture_shows) {
    // Every grey, to the level; the colours of a photograph -- all but the
    // most saturated -- to the level too, in each view.
    std::mt19937 rng(7);
    for (const Settings::View view : {Settings::View::AgXPunchy, Settings::View::AgX, Settings::View::Aces}) {
        // But white in Punchy: its curve shows no more than 254.5 of 255.
        int greys = 0;
        for (int k = 0; k < 256; ++k) {
            const float d = static_cast<float>(k) / 255.0f;
            const Vec3 back = shown(unshown(Vec3(d), view), view);
            const long slack = k == 255 && view == Settings::View::AgXPunchy ? 1 : 0;
            bool same = true;
            for (int c = 0; c < 3; ++c) same = same && std::lround(back[c] * 255.0f) >= k - slack && std::lround(back[c] * 255.0f) <= k;
            greys += same;
        }
        int colours = 0, near = 0;
        for (int i = 0; i < 4000; ++i) {
            // As photographs have them: a grey, and a colour less than half its way off it.
            const float grey = 0.5f + 0.5f * centred(rng);
            const Vec3 d = glm::clamp(Vec3(grey) + centred3(rng) * (0.45f * std::min(grey, 1.0f - grey)), 0.0f, 1.0f);
            const Vec3 q = glm::round(d * 255.0f) / 255.0f;
            const Vec3 back = shown(unshown(q, view), view);
            float off = 0.0f;
            for (int c = 0; c < 3; ++c) off = std::max(off, std::fabs(std::round(back[c] * 255.0f) - q[c] * 255.0f));
            colours += off < 0.5f;
            near += off < 1.5f;
        }
        std::printf("  view %d: %d of 256 greys back to the level; colours: %d of 4000 to the level, %d within one\n",
                    static_cast<int>(view), greys, colours, near);
        CHECK_EQ(greys, 256);
        CHECK(colours > 4000 * 98 / 100);
        CHECK(near > 4000 * 999 / 1000);
    }
    // White: the least light that shows as white -- not infinitely much.
    CHECK(unshown(Vec3(1.0f), Settings::View::AgXPunchy).x < 100.0f);
}

namespace {

/// A shot over a plate: a grey box on the ground -- the ground the plate was
/// filmed on, a catcher, as the floor over a plate is -- in the sun from the
/// left; a holdout before a second box; a pane of glass. The plate a smooth
/// picture as shown, 8 bits a channel.
struct PlateShot {
    SceneInput in;
    std::shared_ptr<Plate> plate;
    Vec3 boxTop{-0.9f, 0.8f, 0.0f};    // the first box's top
    Vec3 shadow{-0.1f, 0.0f, 0.1f};    // the ground in its shadow
    Vec3 holdout{1.1f, 0.35f, 1.3f};   // the holdout, the second box behind it
    Vec3 pane{-0.9f, 0.3f, 2.5f};      // the glass, the ground behind it
};

PlateShot plateShot(int w, int h) {
    PlateShot shot;
    sim::Camera cam = sim::Camera::lookingAt(Vec3(0.0f, 1.6f, 5.0f), Vec3(0.0f, 0.4f, 0.0f));
    cam.width = w;
    cam.height = h;
    shot.in.camera = cam;
    // The sun from the left (-x), half way up: shadows fall towards +x.
    shot.in.look.lightAzimuth = 180.0f;
    shot.in.look.lightElevation = 45.0f;
    shot.in.look.lightIntensity = 2.0f;
    shot.in.look.skyIntensity = 0.5f;
    shot.in.look.grid = false;
    sim::Solid box, behind, holdout;
    box.body.shape = behind.body.shape = holdout.body.shape = sim::Shape::Box;
    box.body.center = Vec3(-0.9f, 0.4f, 0.0f);
    box.body.size = Vec3(0.8f);
    behind.body.center = Vec3(1.1f, 0.4f, -0.6f);
    behind.body.size = Vec3(0.8f);
    holdout.body.center = Vec3(1.1f, 0.35f, 1.2f);
    holdout.body.size = Vec3(0.5f, 0.7f, 0.2f);
    holdout.matte = sim::Matte::Holdout;
    shot.in.solids = {box, behind, holdout};
    // A pane of glass between the camera and the ground on the left.
    auto glass = std::make_shared<Geometry>();
    glass->addPoints(4);
    auto P = glass->positionsForWrite();
    P[0] = Vec3(-1.3f, 0.0f, 2.5f);
    P[1] = Vec3(-0.5f, 0.0f, 2.5f);
    P[2] = Vec3(-0.5f, 0.6f, 2.5f);
    P[3] = Vec3(-1.3f, 0.6f, 2.5f);
    const uint32_t quad[4] = {0, 1, 2, 3};
    glass->addPrimitive(quad, true);
    glass->primitives().create("glass", AttrType::Int).write<int32_t>()[0] = 1;
    shot.in.geometry = glass;
    auto plate = std::make_shared<Plate>();
    plate->camera = cam;
    io::Picture& p = plate->picture;
    p.width = w;
    p.height = h;
    p.rgba.resize(4 * static_cast<size_t>(w) * static_cast<size_t>(h));
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            float* q = &p.rgba[4 * (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x))];
            q[0] = std::round(255.0f * (0.3f + 0.35f * static_cast<float>(x) / static_cast<float>(w - 1))) / 255.0f;
            q[1] = std::round(255.0f * (0.32f + 0.3f * static_cast<float>(y) / static_cast<float>(h - 1))) / 255.0f;
            q[2] = std::round(255.0f * 0.42f) / 255.0f;
            q[3] = 1.0f;
        }
    }
    shot.plate = plate;
    shot.in.plate = plate;
    return shot;
}

/// The pixel `p` is seen in, through `c`'s lens, a picture w x h.
std::pair<int, int> pixelOf(const sim::Camera& c, int w, int h, const Vec3& p) {
    const Vec3 d = p - c.position;
    const float z = dot(d, c.forward()), tanY = std::tan(c.fovY() * 3.14159265f / 360.0f);
    const float tanX = tanY * static_cast<float>(w) / static_cast<float>(h);
    const float u = dot(d, c.right()) / (z * tanX), v = dot(d, c.up()) / (z * tanY);
    return {std::clamp(static_cast<int>(0.5f * (u + 1.0f) * static_cast<float>(w)), 0, w - 1),
            std::clamp(static_cast<int>(0.5f * (1.0f - v) * static_cast<float>(h)), 0, h - 1)};
}

/// Over a plate, what a render made looks as it should: `name` the renderer's.
void checkOverPlate(const PlateShot& shot, const Rendered& r, const char* name) {
    const int w = shot.in.camera.width, h = shot.in.camera.height;
    CHECK_EQ(r.alpha.width, w);
    CHECK_EQ(r.catcher.width, w);
    CHECK_EQ(r.plate.width, w);
    if (r.alpha.width != w || r.catcher.width != w || r.plate.width != w) return;
    const std::vector<uint8_t> shown = displayRgb(r);
    auto byte = [&](int x, int y, int c) { return static_cast<int>(shown[3 * (static_cast<size_t>(y) * w + x) + c]); };
    auto platebyte = [&](int x, int y, int c) {
        return static_cast<int>(std::lround(shot.plate->picture.rgba[4 * (static_cast<size_t>(y) * w + x) + c] * 255.0f));
    };
    auto alphaAt = [&](const Vec3& p) {
        const auto [x, y] = pixelOf(shot.in.camera, w, h, p);
        return r.alpha.pixels[static_cast<size_t>(y) * w + x];
    };
    auto brightness = [&](const Vec3& p, bool plate) {
        const auto [x, y] = pixelOf(shot.in.camera, w, h, p);
        int sum = 0;
        for (int c = 0; c < 3; ++c) sum += plate ? platebyte(x, y, c) : byte(x, y, c);
        return sum;
    };
    // Above the horizon nothing covers the plate: it comes out as it went in.
    int rows = 0, same = 0;
    for (int y = 0; y < h / 10; ++y) {
        for (int x = 0; x < w; ++x, ++rows) {
            bool equal = true;
            for (int c = 0; c < 3; ++c) equal = equal && std::abs(byte(x, y, c) - platebyte(x, y, c)) <= 1;
            same += equal;
        }
    }
    const auto [sx, sy] = pixelOf(shot.in.camera, w, h, shot.shadow);
    const float shade = r.catcher.pixels[3 * (static_cast<size_t>(sy) * w + sx) + 1];
    const auto [hx, hy] = pixelOf(shot.in.camera, w, h, shot.holdout);
    int hidden = 0;
    for (int c = 0; c < 3; ++c) hidden = std::max(hidden, std::abs(byte(hx, hy, c) - platebyte(hx, hy, c)));
    std::printf("  %s: %d of %d pixels above the horizon the plate's; the box covers %.2f of its pixel, the holdout "
                "%.2f; its shadow x %.2f, %d against %d; behind the holdout %d levels off the plate; through the "
                "glass (it covers %.2f) %d against %d\n",
                name, same, rows, alphaAt(shot.boxTop), alphaAt(shot.holdout), shade, brightness(shot.shadow, false),
                brightness(shot.shadow, true), hidden, alphaAt(shot.pane), brightness(shot.pane, false),
                brightness(shot.pane, true));
    CHECK(same >= rows * 99 / 100);
    CHECK(alphaAt(shot.boxTop) > 0.95f);
    CHECK(alphaAt(shot.holdout) < 0.05f);
    CHECK(hidden <= 2);
    CHECK(shade < 0.8f);
    CHECK(brightness(shot.shadow, false) < brightness(shot.shadow, true) * 85 / 100);
    // Through the glass, the plate -- a little of the light reflected away.
    CHECK(std::abs(brightness(shot.pane, false) - brightness(shot.pane, true)) < brightness(shot.pane, true) / 4);
}

}  // namespace

TEST(render_the_path_tracer_draws_the_cg_over_a_plate) {
    const int w = 80, h = 48;
    PlateShot shot = plateShot(w, h);
    Settings s;
    s.width = w;
    s.height = h;
    s.samples = 64;
    s.denoise = false;
    PathTracer t;
    t.setSettings(s);
    SceneBuilder builder;
    t.setScene(builder.build(shot.in));
    while (!t.done()) t.pass();
    const Rendered r = renderedOf(t, false);
    checkOverPlate(shot, r, "the path tracer");
    // Into an EXR: the CG alone, its alpha, what the plate is multiplied by.
    const fs::path dir = fs::temp_directory_path() / "pg_test_plate";
    fs::create_directories(dir);
    std::string error;
    CHECK(savePicture(r, (dir / "over.exr").string(), "", error));
    io::ExrImage exr;
    CHECK(io::readExr((dir / "over.exr").string(), exr, error));
    std::set<std::string> names;
    const io::ExrChannel* a = nullptr;
    for (const auto& c : exr.channels) {
        names.insert(c.name);
        if (c.name == "A") a = &c;
    }
    for (const char* want : {"R", "G", "B", "A", "catcher.R", "catcher.G", "catcher.B", "Z"}) CHECK(names.count(want));
    if (a) {
        const auto [bx, by] = pixelOf(shot.in.camera, w, h, shot.boxTop);
        CHECK(a->values[0] < 0.01f);
        CHECK(a->values[static_cast<size_t>(by) * w + bx] > 0.95f);
    }
    fs::remove_all(dir);
    // Without the plate, holdouts and catchers are drawn as themselves: the
    // holdout, nothing of the CG over the plate, is there in its light.
    shot.in.plate = nullptr;
    t.setScene(builder.build(shot.in));
    while (!t.done()) t.pass();
    const Rendered plain = renderedOf(t, false);
    CHECK(plain.alpha.pixels.empty() && plain.catcher.pixels.empty() && plain.plate.pixels.empty());
    const auto [hx, hy] = pixelOf(shot.in.camera, w, h, shot.holdout);
    const size_t held = 3 * (static_cast<size_t>(hy) * w + hx);
    auto lightAt = [&](const Image& image) {
        return std::max(image.pixels[held], std::max(image.pixels[held + 1], image.pixels[held + 2]));
    };
    std::printf("  the holdout's pixel: %.3f over the plate, %.3f without it\n", lightAt(r.beauty), lightAt(plain.beauty));
    CHECK(lightAt(r.beauty) < 0.01f);
    CHECK(lightAt(plain.beauty) > 0.05f);
}

TEST(render_cycles_draws_the_cg_over_a_plate) {
    if (!cyclesAvailable()) return;
    const int w = 80, h = 48;
    const PlateShot shot = plateShot(w, h);
    Settings s;
    s.width = w;
    s.height = h;
    s.samples = 64;
    s.denoise = false;
    s.sky = Settings::Sky::Look;
    s.detail = 0.0f;
    SceneBuilder builder;
    CyclesRender render;
    render.start(builder.build(shot.in), s);
    render.wait();
    CHECK(render.error().empty());
    checkOverPlate(shot, render.rendered(), "Cycles");
}
