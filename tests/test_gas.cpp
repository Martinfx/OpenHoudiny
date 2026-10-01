//
// The gas in the path tracer (render/Gas.h): the NanoVDB grid reads what the
// frame holds, faded as the viewport fades it; delta tracking scatters light
// as often as the smoke stops it and adds up the light the flames give off;
// ratio tracking lets through what the smoke lets through; the flames glow
// as the viewport's; the look's colour comes out of thick smoke; a render
// with gas shades, casts a shadow, glows, and is the same however it is run;
// and the grids Cycles reads (Gas::dense) are the gas, which Cycles renders
// as the path tracer does.
//
#include "pg/core/Parallel.h"
#include "pg/render/Cycles.h"
#include "pg/render/Gas.h"
#include "pg/render/PathTracer.h"
#include "pg/render/Random.h"
#include "pg/sim/Frame.h"
#include "pg/sim/SparseGrid.h"

#include "test_framework.h"

#include <cmath>
#include <functional>
#include <random>

using namespace pg;
using namespace pg::render;

namespace {

/// A frame of gas over a box of n x n x n cells of `voxel` standing on the
/// floor, each cell's smoke, temperature and flame from `fields`; `sparse`:
/// only the tiles of 8 x 8 x 8 cells that hold any, as a sparse solver
/// keeps them.
sim::Frame frameOf(int n, float voxel, const std::function<Vec3(int, int, int)>& fields, bool sparse) {
    sim::Frame f;
    f.domain.cells[0] = f.domain.cells[1] = f.domain.cells[2] = n;
    f.domain.voxel = voxel;
    auto put = [&](std::vector<uint16_t>& out, int i, int j, int k) {
        const Vec3 v = fields(i, j, k);
        out.push_back(sim::toHalf(v.x));
        out.push_back(sim::toHalf(v.y));
        out.push_back(sim::toHalf(v.z));
    };
    if (!sparse) {
        for (int k = 0; k < n; ++k)
            for (int j = 0; j < n; ++j)
                for (int i = 0; i < n; ++i) put(f.fields, i, j, k);
        return f;
    }
    const int t = n / sim::Tiles::kSide;
    for (int c = 0; c < t; ++c) {
        for (int b = 0; b < t; ++b) {
            for (int a = 0; a < t; ++a) {
                std::vector<uint16_t> tile;
                bool any = false;
                for (int z = 0; z < 8; ++z)
                    for (int y = 0; y < 8; ++y)
                        for (int x = 0; x < 8; ++x) {
                            put(tile, 8 * a + x, 8 * b + y, 8 * c + z);
                            any = any || fields(8 * a + x, 8 * b + y, 8 * c + z) != Vec3(0.0f, 0.0f, 0.0f);
                        }
                if (!any) continue;
                f.gasTiles.push_back(static_cast<uint32_t>(a + t * (b + t * c)));
                f.fields.insert(f.fields.end(), tile.begin(), tile.end());
            }
        }
    }
    return f;
}

/// A blob of smoke round the box's middle, hot and burning low in it.
Vec3 blob(int i, int j, int k, int n) {
    const float x = (static_cast<float>(i) + 0.5f) / static_cast<float>(n) - 0.5f;
    const float y = (static_cast<float>(j) + 0.5f) / static_cast<float>(n) - 0.45f;
    const float z = (static_cast<float>(k) + 0.5f) / static_cast<float>(n) - 0.5f;
    const float r2 = (x * x + y * y + z * z) / (0.3f * 0.3f);
    if (r2 >= 1.0f) return {};
    const float smoke = (1.0f - r2) * (1.2f + 0.6f * std::sin(17.0f * x) * std::cos(11.0f * z));
    const float flame = y < -0.05f ? 0.8f * (1.0f - r2) : 0.0f;
    return {smoke, 3.0f * (1.0f - r2), flame};
}

/// The fields at p as the viewport's texture reads them: each cell's,
/// faded over the last cells before the open sides and the top, and
/// between the cells' middles trilinearly; 0 outside.
Vec3 expected(const sim::Frame& f, const Vec3& p) {
    const int n = f.domain.cells[0];
    const float v = f.domain.voxel;
    const Vec3 o = f.domain.origin();
    auto cell = [&](int i, int j, int k) -> Vec3 {
        if (i < 0 || j < 0 || k < 0 || i >= n || j >= n || k >= n) return {};
        const float fi = i + 0.5f, fj = j + 0.5f, fk = k + 0.5f;
        const float side = std::min(std::min(fi, n - fi), std::min(fk, n - fk)) / 6.0f, top = (n - fj) / 10.0f;
        const float t = std::clamp(std::min(side, top), 0.0f, 1.0f);
        const float fade = t * t * (3.0f - 2.0f * t);
        return {std::max(f.at(0, i, j, k), 0.0f) * fade, f.at(1, i, j, k), std::max(f.at(2, i, j, k), 0.0f) * fade};
    };
    const float x = (p.x - o.x) / v - 0.5f, y = (p.y - o.y) / v - 0.5f, z = (p.z - o.z) / v - 0.5f;
    const int i = static_cast<int>(std::floor(x)), j = static_cast<int>(std::floor(y)), k = static_cast<int>(std::floor(z));
    const float u = x - i, w = y - j, s = z - k;
    Vec3 out;
    for (int dz = 0; dz < 2; ++dz)
        for (int dy = 0; dy < 2; ++dy)
            for (int dx = 0; dx < 2; ++dx)
                out = out + cell(i + dx, j + dy, k + dz) * ((dx ? u : 1 - u) * (dy ? w : 1 - w) * (dz ? s : 1 - s));
    return out;
}

/// The share of light the smoke lets through along a ray, summed finely.
double throughFinely(const Gas& gas, const GasLook& look, const Vec3& o, const Vec3& d, float t0, float t1) {
    const int steps = 4000;
    const double h = (t1 - t0) / steps;
    double tau = 0.0;
    for (int i = 0; i < steps; ++i) tau += Gas::extinction(gas.at(o + d * static_cast<float>(t0 + (i + 0.5) * h)), look) * h;
    return std::exp(-tau);
}

/// The viewport's glow of a flame at temperature `heat` (Volume.cpp: glowAt).
Vec3 viewportGlow(float heat, const GasLook& look) {
    const float x = std::clamp((heat - look.flameStart) / look.flameRange, 0.0f, 1.0f);
    const float kelvin = 1000.0f + 2000.0f * x, t = kelvin / 100.0f;
    Vec3 c(1.0f, 0.39008157876f * std::log(t) - 0.63184144378f, t <= 19.0f ? 0.0f : 0.54320678911f * std::log(t - 10.0f) - 1.19625408914f);
    for (int a = 0; a < 3; ++a) c[a] = std::pow(std::clamp(c[a], 0.0f, 1.0f), 2.2f);
    const float k = kelvin / 3000.0f, e = std::clamp(x / 0.1f, 0.0f, 1.0f);
    return c * (k * k * k * k * e * e * (3.0f - 2.0f * e));
}

}  // namespace

TEST(gas_grid_reads_the_frame_as_the_viewport_does) {
    if (!gasAvailable()) return;
    const int n = 32;
    auto fields = [&](int i, int j, int k) { return blob(i, j, k, n); };
    const sim::Frame dense = frameOf(n, 0.05f, fields, false), sparse = frameOf(n, 0.05f, fields, true);
    CHECK(sparse.gasTiles.size() > 0 && sparse.gasTiles.size() < 64u);
    const auto a = Gas::build(dense), b = Gas::build(sparse);
    CHECK(a && b);
    CHECK(a->cells() == b->cells());
    CHECK(a->cells() > 1000u);
    CHECK(std::fabs(a->bounds().lo.x + 0.8f) < 1e-6f && std::fabs(a->bounds().hi.y - 1.6f) < 1e-6f);
    std::mt19937 rng(5);
    float worst = 0.0f;
    for (int s = 0; s < 20000; ++s) {
        const Vec3 p(static_cast<float>(rng() % 2000) / 1000.0f - 1.0f, static_cast<float>(rng() % 2000) / 1100.0f - 0.05f,
                     static_cast<float>(rng() % 2000) / 1000.0f - 1.0f);
        const Vec3 want = expected(dense, p), gotA = a->at(p), gotB = b->at(p);
        for (int c = 0; c < 3; ++c) worst = std::max(worst, std::max(std::fabs(gotA[c] - want[c]), std::fabs(gotB[c] - want[c])));
    }
    std::printf("  worst difference %.3g\n", worst);
    CHECK(worst < 1e-5f);
    // No gas, no grid.
    sim::Frame empty = frameOf(16, 0.1f, [](int, int, int) { return Vec3(); }, false);
    CHECK(!Gas::build(empty));
    CHECK(!Gas::build(sim::Frame()));
}

TEST(gas_tracking_lets_through_what_the_smoke_does) {
    if (!gasAvailable()) return;
    const int n = 32;
    const sim::Frame frame = frameOf(n, 0.05f, [&](int i, int j, int k) { return blob(i, j, k, n); }, true);
    const auto gas = Gas::build(frame);
    CHECK(gas);
    GasLook look;
    look.density = 3.0f;
    const Vec3 rays[][2] = {{{-1.0f, 0.72f, 0.0f}, {1.0f, 0.0f, 0.0f}},
                            {{0.05f, 0.0f, -1.0f}, {0.0f, 0.3f, 1.0f}},
                            {{-0.9f, 1.3f, -0.7f}, {0.6f, -0.45f, 0.5f}},
                            {{0.2f, 2.0f, 0.1f}, {-0.1f, -1.0f, 0.05f}}};
    int r = 0;
    for (const auto& ray : rays) {
        const Vec3 o = ray[0], d = normalize(ray[1]);
        const double want = throughFinely(*gas, look, o, d, 0.0f, 4.0f);
        const int trials = 20000;
        int missed = 0;
        double ratio = 0.0, ratio2 = 0.0;
        for (int s = 0; s < trials; ++s) {
            Rng rng(static_cast<uint32_t>(r), static_cast<uint32_t>(s), 3);
            float t = 0.0f;
            Vec3 glow;
            missed += gas->track(o, d, 0.0f, 4.0f, look, rng, t, glow) ? 0 : 1;
            const double tr = gas->transmittance(o, d, 0.0f, 4.0f, look, rng);
            ratio += tr;
            ratio2 += tr * tr;
        }
        const double delta = static_cast<double>(missed) / trials, mean = ratio / trials;
        const double sdDelta = std::sqrt(want * (1.0 - want) / trials);
        const double sdRatio = std::sqrt(std::max(ratio2 / trials - mean * mean, 0.0) / trials);
        float seen = 0.0f, depth = 0.0f;
        gas->seen(o, d, 4.0f, look, seen, depth);
        std::printf("  ray %d: through %.4f; delta tracking %.4f, ratio tracking %.4f, marched %.4f\n", r, want, delta,
                    mean, seen);
        CHECK(want > 0.02 && want < 0.98);
        CHECK(std::fabs(delta - want) < 4.0 * sdDelta + 0.003);
        CHECK(std::fabs(mean - want) < 4.0 * sdRatio + 0.003);
        CHECK(std::fabs(seen - want) < 0.02);
        CHECK(std::isfinite(depth) && depth > 0.0f && depth < 4.0f);
        ++r;
    }
    // A ray past the gas meets nothing, and is let through whole.
    Rng rng(9, 9, 9);
    float t = 0.0f;
    Vec3 glow;
    CHECK(!gas->track(Vec3(-1.0f, 3.0f, 0.0f), Vec3(1.0f, 0.0f, 0.0f), 0.0f, 10.0f, look, rng, t, glow));
    CHECK_EQ(gas->transmittance(Vec3(-1.0f, 3.0f, 0.0f), Vec3(1.0f, 0.0f, 0.0f), 0.0f, 10.0f, look, rng), 1.0f);
}

TEST(gas_flames_give_off_light_along_the_way) {
    if (!gasAvailable()) return;
    // Fire all through the box, and smoke or none: along a ray in the
    // middle, out of the faded rim, the light given off adds up to
    // emission * (1 - e^(-sigma L)) / sigma -- emission * L without smoke.
    GasLook look;
    for (const float smoke : {0.0f, 0.04f}) {
        const sim::Frame frame = frameOf(48, 0.05f, [&](int, int, int) { return Vec3(smoke, 3.0f, 0.5f); }, false);
        const auto gas = Gas::build(frame);
        CHECK(gas);
        const Vec3 o(-0.8f, 0.9f, 0.0f), d(1.0f, 0.0f, 0.0f);
        const float length = 1.6f;
        const Vec3 f(sim::fromHalf(sim::toHalf(smoke)), sim::fromHalf(sim::toHalf(3.0f)), sim::fromHalf(sim::toHalf(0.5f)));
        const Vec3 e = Gas::emission(f, look);
        const float sigma = Gas::extinction(f, look);
        const double want = sigma > 0.0f ? e.x * (1.0 - std::exp(-sigma * length)) / sigma : e.x * length;
        const int trials = 4000;
        double sum = 0.0;
        for (int s = 0; s < trials; ++s) {
            Rng rng(1, static_cast<uint32_t>(s), 0);
            float t = 0.0f;
            Vec3 glow;
            gas->track(o, d, 0.0f, length, look, rng, t, glow);
            sum += glow.x;
        }
        const double mean = sum / trials;
        std::printf("  smoke %.2f: light given off %.4f, expected %.4f\n", smoke, mean, want);
        CHECK(e.x > 0.0f && e.x > e.z);
        CHECK(std::fabs(mean - want) < 0.03 * want);
    }
}

TEST(gas_flames_glow_as_the_viewports) {
    GasLook look;
    for (const float heat : {0.2f, 0.35f, 0.5f, 1.0f, 2.0f, 3.3f, 4.3f, 9.0f}) {
        const Vec3 got = Gas::emission(Vec3(0.0f, heat, 1.0f), look);
        const Vec3 want = viewportGlow(heat, look) * (look.flame * (1.0f - std::exp(-4.0f)));
        for (int c = 0; c < 3; ++c) CHECK(std::fabs(got[c] - want[c]) <= 0.01f * look.flame * 0.02f + 0.01f * want[c]);
    }
    // Neither glows without flame, nor cold.
    CHECK(Gas::emission(Vec3(1.0f, 4.0f, 0.0f), look) == Vec3(0.0f, 0.0f, 0.0f));
    CHECK(Gas::emission(Vec3(1.0f, 0.1f, 1.0f), look) == Vec3(0.0f, 0.0f, 0.0f));
    // Smoke stops light, less where it burns.
    CHECK_NEAR(Gas::extinction(Vec3(0.5f, 0.0f, 0.0f), look), 0.5f * look.density, 1e-5);
    CHECK(Gas::extinction(Vec3(0.5f, 0.0f, 1.0f), look) < Gas::extinction(Vec3(0.5f, 0.0f, 0.0f), look));
}

TEST(gas_look_colour_comes_out_of_thick_smoke) {
    CHECK_NEAR(GasLook::albedoOf(Vec3(0.0f)).x, 0.0f, 1e-4);
    CHECK_NEAR(GasLook::albedoOf(Vec3(1.0f)).x, 1.0f, 1e-4);
    CHECK_NEAR(GasLook::albedoOf(Vec3(0.75f)).x, 0.984f, 0.002);
    float before = -1.0f;
    for (int i = 0; i <= 20; ++i) {
        const float a = GasLook::albedoOf(Vec3(i / 20.0f)).x;
        CHECK(a > before);
        before = a;
    }
    sim::Look look;
    look.smokeColor = Vec3(0.2f, 0.5f, 0.9f);
    const GasLook g = GasLook::of(look);
    CHECK(g.color == look.smokeColor);
    CHECK(g.albedo.x < g.albedo.y && g.albedo.y < g.albedo.z);
}

namespace {

/// A slab of thick smoke above the floor -- the sun straight above -- and a
/// block of fire beside it, seen from the side; or the same without them.
std::shared_ptr<const Scene> gasScene(bool withGas, int width, int height) {
    const int n = 32;
    auto frame = std::make_shared<sim::Frame>(frameOf(
        n, 0.0625f,
        [&](int i, int j, int k) {
            if (i >= 8 && i < 18 && j >= 12 && j < 18 && k >= 8 && k < 24) return Vec3(1.0f, 0.0f, 0.0f);
            if (i >= 20 && i < 25 && j >= 2 && j < 8 && k >= 13 && k < 18) return Vec3(0.05f, 3.5f, 1.0f);
            return Vec3();
        },
        true));
    sim::Look look;
    look.lightElevation = 89.0f;
    look.groundColor = Vec3(0.5f, 0.5f, 0.5f);
    sim::Camera cam = sim::Camera::lookingAt(Vec3(0.0f, 0.9f, 3.2f), Vec3(0.0f, 0.2f, 0.0f));
    cam.width = width;
    cam.height = height;
    SceneInput in;
    in.look = look;
    in.camera = cam;
    if (withGas) in.gas = frame;
    SceneBuilder builder;
    return builder.build(in);
}

Image rendered(std::shared_ptr<const Scene> scene, int width, int height, int samples) {
    Settings s;
    s.width = width;
    s.height = height;
    s.samples = samples;
    s.denoise = false;
    PathTracer t;
    t.setSettings(s);
    t.setScene(std::move(scene));
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

}  // namespace

TEST(render_gas_shades_casts_a_shadow_and_glows) {
    if (!gasAvailable()) return;
    const int w = 64, h = 40;
    const auto withGas = gasScene(true, w, h);
    CHECK(withGas->gas);
    CHECK(!gasScene(false, w, h)->gas);
    const Image a = rendered(withGas, w, h, 32), b = rendered(gasScene(false, w, h), w, h, 32);
    const sim::Camera& cam = withGas->camera;
    // The floor under the slab: in its shadow.
    const Vec3 under(-0.19f, 0.0f, 0.0f), fire(0.4f, 0.31f, -0.03f);
    const double shadeA = meanAt(a, 1, cam, under), shadeB = meanAt(b, 1, cam, under);
    // The fire: its light, red more than blue.
    const double fireR = meanAt(a, 0, cam, fire), fireB = meanAt(a, 2, cam, fire), floorR = meanAt(b, 0, cam, fire);
    std::printf("  floor under the smoke %.3f (%.3f without); fire %.3f red, %.3f blue (floor %.3f)\n", shadeA, shadeB,
                fireR, fireB, floorR);
    CHECK(shadeA < 0.5 * shadeB);
    CHECK(fireR > 1.5 * floorR && fireR > 1.5 * fireB);
}

TEST(render_gas_is_the_same_however_it_is_run) {
    if (!gasAvailable()) return;
    const int w = 48, h = 32;
    const unsigned saved = TaskPool::instance().threadCount();
    TaskPool::instance().setThreadCount(1);
    const Image one = rendered(gasScene(true, w, h), w, h, 3);
    TaskPool::instance().setThreadCount(4);
    const Image four = rendered(gasScene(true, w, h), w, h, 3);
    TaskPool::instance().setThreadCount(saved);
    CHECK(one.pixels == four.pixels);
    // The builder keeps a frame's gas: rendered again, not made again.
    auto frame = std::make_shared<sim::Frame>(frameOf(16, 0.1f, [](int i, int, int) { return Vec3(i > 4 ? 0.5f : 0.0f, 0.0f, 0.0f); }, false));
    SceneInput in;
    in.gas = frame;
    SceneBuilder builder;
    const auto first = builder.build(in), again = builder.build(in);
    CHECK(first->gas && first->gas == again->gas);
}

TEST(gas_dense_grids_are_the_gas_at_the_cells_middles) {
    if (!gasAvailable()) return;
    const int n = 32;
    const sim::Frame f = frameOf(n, 0.05f, [&](int i, int j, int k) { return blob(i, j, k, n); }, true);
    const auto gas = Gas::build(f);
    CHECK(gas != nullptr);
    if (!gas) return;
    const GasLook look;
    const Gas::Dense d = gas->dense(look, size_t(1) << 30);
    const size_t cells = static_cast<size_t>(d.size[0]) * static_cast<size_t>(d.size[1]) * static_cast<size_t>(d.size[2]);
    CHECK(cells > 0 && d.extinction.size() == cells);
    CHECK_EQ(d.emission.size(), cells);  // the blob burns low in it
    // The box: the domain's, but for the empty tiles round the blob -- a
    // cell of reading past those that hold it.
    for (int a = 0; a < 3; ++a) {
        CHECK(d.box.lo[a] >= -0.8f - 1e-4f && d.box.hi[a] <= 0.8f + (a == 1 ? 0.8f : 0.0f) + 1e-4f);
        CHECK_NEAR((d.box.hi[a] - d.box.lo[a]) / d.size[a], 0.05f, 1e-5f);
    }
    // Each cell: the light the gas stops and gives off at its middle.
    std::mt19937 rng(3);
    for (int t = 0; t < 2000; ++t) {
        const int x = static_cast<int>(rng() % static_cast<unsigned>(d.size[0]));
        const int y = static_cast<int>(rng() % static_cast<unsigned>(d.size[1]));
        const int z = static_cast<int>(rng() % static_cast<unsigned>(d.size[2]));
        const Vec3 p = d.box.lo + Vec3(x + 0.5f, y + 0.5f, z + 0.5f) * 0.05f;
        const Vec3 fields = gas->at(p);
        const size_t i = static_cast<size_t>(x) + static_cast<size_t>(d.size[0]) * (static_cast<size_t>(y) + static_cast<size_t>(d.size[1]) * static_cast<size_t>(z));
        CHECK_NEAR(d.extinction[i], Gas::extinction(fields, look), 1e-4f * (1.0f + d.extinction[i]));
        const Vec3 e = Gas::emission(fields, look);
        CHECK_NEAR(d.emission[i].x, e.x, 1e-4f * (1.0f + e.x));
        CHECK_NEAR(d.emission[i].z, e.z, 1e-4f * (1.0f + e.z));
    }
    // Too many cells: blocks of them, as few as fit -- the same box.
    const Gas::Dense coarse = gas->dense(look, cells / 6);
    const size_t fewer = static_cast<size_t>(coarse.size[0]) * static_cast<size_t>(coarse.size[1]) * static_cast<size_t>(coarse.size[2]);
    CHECK(fewer <= cells / 6 && fewer >= cells / 27);
    for (int a = 0; a < 3; ++a) {
        CHECK_NEAR(coarse.box.lo[a], d.box.lo[a], 1e-5f);
        CHECK(coarse.box.hi[a] >= d.box.hi[a] - 1e-5f);
    }
    // Smoke without fire: nothing given off, no grid of it.
    const auto smoke = Gas::build(frameOf(16, 0.1f, [](int i, int, int) { return Vec3(i > 4 ? 0.5f : 0.0f, 0.0f, 0.0f); }, false));
    CHECK(smoke && smoke->dense(look, size_t(1) << 30).emission.empty());
}

TEST(render_cycles_renders_the_gas_as_the_path_tracer_does) {
    if (!gasAvailable() || !cyclesAvailable()) return;
    const int w = 64, h = 40;
    Settings s;
    s.width = w;
    s.height = h;
    s.samples = 64;
    s.denoise = false;
    s.sky = Settings::Sky::Look;
    s.detail = 0.0f;
    auto cycles = [&](std::shared_ptr<const Scene> scene) {
        CyclesRender r;
        r.start(std::move(scene), s);
        r.wait();
        return r.beauty();
    };
    const auto withGas = gasScene(true, w, h);
    const Image a = cycles(withGas), b = cycles(gasScene(false, w, h)), ours = rendered(withGas, w, h, 64);
    CHECK_EQ(a.width, w);
    const sim::Camera& cam = withGas->camera;
    const Vec3 under(-0.19f, 0.0f, 0.0f), fire(0.4f, 0.31f, -0.03f), slab(-0.19f, 0.95f, 0.5f);
    const double shadeA = meanAt(a, 1, cam, under), shadeB = meanAt(b, 1, cam, under);
    const double fireR = meanAt(a, 0, cam, fire), fireB = meanAt(a, 2, cam, fire), floorR = meanAt(b, 0, cam, fire);
    const double smokeA = meanAt(a, 1, cam, slab), smokeOurs = meanAt(ours, 1, cam, slab);
    const double fireOurs = meanAt(ours, 0, cam, fire);
    std::printf("  %s: floor under the smoke %.3f (%.3f without); fire %.3f red (ours %.3f), %.3f blue (floor %.3f); "
                "the smoke %.3f (ours %.3f)\n",
                cyclesVersion().c_str(), shadeA, shadeB, fireR, fireOurs, fireB, floorR, smokeA, smokeOurs);
    // Its shadow, its fire; as bright as ours.
    CHECK(shadeA < 0.5 * shadeB);
    CHECK(fireR > 1.5 * floorR && fireR > 1.5 * fireB);
    CHECK(std::fabs(fireR / fireOurs - 1.0) < 0.3);
    CHECK(std::fabs(smokeA / smokeOurs - 1.0) < 0.3);
}
