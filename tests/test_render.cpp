//
// The path tracer (render/): its hierarchies meet what trying every triangle
// meets; instances as the copies they stand for; a render the same however
// it is run; the sun lighting a floor as the viewport lights it; the
// denoiser nearer the converged picture than the noise; the Output's
// settings; the files; the materials vegetation brings.
//
#include "pg/core/Grass.h"
#include "pg/core/Instances.h"
#include "pg/core/Tree.h"
#include "pg/io/Exr.h"
#include "pg/render/PathTracer.h"
#include "pg/render/Save.h"
#include "pg/sim/Network.h"

#include "test_framework.h"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <random>
#include <set>

using namespace pg;
using namespace pg::render;
namespace fs = std::filesystem;

namespace {

/// `n` triangles strewn in a box 2 m wide, each some `size` across.
std::shared_ptr<Geometry> strewn(int n, uint32_t seed, float size = 0.3f) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> at(-1.0f, 1.0f), off(-0.5f * size, 0.5f * size);
    auto geo = std::make_shared<Geometry>();
    geo->addPoints(3 * static_cast<size_t>(n));
    auto P = geo->positionsForWrite();
    for (int t = 0; t < n; ++t) {
        const Vec3 c(at(rng), at(rng) + 1.2f, at(rng));
        for (int k = 0; k < 3; ++k) P[3 * t + k] = c + Vec3(off(rng), off(rng), off(rng));
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

SceneInput inputOf(GeometryPtr geo, const sim::Look& look, const sim::Camera& cam) {
    SceneInput in;
    in.geometry = std::move(geo);
    in.look = look;
    in.camera = cam;
    return in;
}

/// A small picture of triangles over the floor: many small ones, or a few
/// large -- broad surfaces in each other's shadows and light.
std::shared_ptr<const Scene> smallScene(int width, int height, bool broad = false) {
    sim::Camera cam = sim::Camera::lookingAt(Vec3(1.6f, 1.9f, 2.0f), Vec3(0.0f, 1.0f, 0.0f));
    cam.width = width;
    cam.height = height;
    SceneBuilder builder;
    return builder.build(inputOf(broad ? strewn(12, 3, 1.6f) : strewn(300, 3), sim::Look(), cam));
}

}  // namespace

TEST(render_bvh_meets_what_every_triangle_meets) {
    const auto geo = strewn(400, 11);
    SceneBuilder builder;
    const auto scene = builder.build(inputOf(geo, noFloor(), sim::Camera()));
    std::mt19937 rng(5);
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    int hits = 0;
    for (int r = 0; r < 3000; ++r) {
        const Vec3 o(3.0f * u(rng), 1.2f + 3.0f * u(rng), 3.0f * u(rng));
        const Vec3 target(0.8f * u(rng), 1.2f + 0.8f * u(rng), 0.8f * u(rng));
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
    CHECK(hits > 1000);
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
    SceneBuilder builder;
    const auto a = builder.build(inputOf(geo, noFloor(), sim::Camera()));
    const auto b = builder.build(inputOf(copies, noFloor(), sim::Camera()));
    CHECK_EQ(a->meshes.size(), 1u);  // the prototype once
    CHECK_EQ(a->placed.size(), static_cast<size_t>(n));
    std::mt19937 rng(9);
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    int hits = 0;
    for (int r = 0; r < 2000; ++r) {
        // At one of them, from anywhere round.
        const Vec3 at = P[static_cast<size_t>(r % n)] + Vec3(0.6f * u(rng), 1.0f + 0.6f * u(rng), 0.6f * u(rng));
        const Vec3 o = at + normalize(Vec3(u(rng), 0.3f + std::fabs(u(rng)), u(rng))) * 8.0f;
        const Vec3 d = normalize(at - o);
        Hit ha, hb;
        const bool ma = a->intersect(o, d, 1e30f, 0.5f, ha), mb = b->intersect(o, d, 1e30f, 0.5f, hb);
        CHECK_EQ(ma, mb);
        if (ma && mb) {
            ++hits;
            CHECK_NEAR(ha.t, hb.t, 1e-3f);
            CHECK(length(ha.face - hb.face) < 1e-3f || length(ha.face + hb.face) < 1e-3f);
        }
    }
    std::printf("  %d of 2000 met\n", hits);
    CHECK(hits > 100);
}

TEST(render_is_the_same_however_it_is_run) {
    Settings s;
    s.width = 64;
    s.height = 40;
    s.samples = 3;
    s.denoise = false;
    PathTracer a, b;
    a.setSettings(s);
    b.setSettings(s);
    a.setScene(smallScene(64, 40));
    b.setScene(smallScene(64, 40));
    for (int i = 0; i < 3; ++i) {
        CHECK(a.pass());
        CHECK(b.pass());
    }
    CHECK(a.done());
    const Image ia = a.beauty(), ib = b.beauty();
    CHECK(ia.pixels == ib.pixels);
    // Another seed: another noise.
    s.seed = 7;
    b.setSettings(s);
    for (int i = 0; i < 3; ++i) b.pass();
    CHECK(!(b.beauty().pixels == ia.pixels));
    // Stopped on the way: no sample added.
    std::atomic<bool> stop{true};
    PathTracer c;
    c.setSettings(s);
    c.setScene(smallScene(64, 40));
    CHECK(!c.pass(&stop));
    CHECK_EQ(c.samples(), 0);
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
    CHECK(clean < 0.8 * raw);
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
    const sim::Compiled c = net.compile();
    CHECK_EQ(c.render.samples, 300);
    CHECK_EQ(c.render.bounces, 7);
    CHECK(!c.render.denoise);
    CHECK_NEAR(c.render.fstop, 2.8f, 1e-5f);
    CHECK_NEAR(c.render.focus, 4.5f, 1e-5f);
    CHECK_NEAR(c.render.sunAngle, 2.0f, 1e-5f);
    // Without them set: the defaults, saved and read back the same.
    sim::Network plain;
    plain.add("output");
    CHECK(plain.compile().render == Settings());
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
