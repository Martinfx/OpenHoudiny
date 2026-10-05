#include "pg/render/Plate.h"

#include "pg/core/Parallel.h"

#include <algorithm>
#include <cmath>

namespace pg::render {
namespace {

constexpr float kPi = 3.14159265358979f;

}  // namespace

std::shared_ptr<const Plate> loadPlate(const std::string& file, const sim::Camera& camera, std::string& error) {
    auto plate = std::make_shared<Plate>();
    if (!io::readPicture(file, plate->picture, error)) return nullptr;
    plate->camera = camera.sanitized();
    plate->file = file;
    return plate;
}

Image plateLight(const Plate& plate, Settings::View view, float exposure, const OcioView* ocio) {
    const io::Picture& p = plate.picture;
    Image out;
    if (p.empty()) return out;
    out.width = p.width;
    out.height = p.height;
    out.channels = 3;
    const size_t n = static_cast<size_t>(p.width) * static_cast<size_t>(p.height);
    out.pixels.resize(3 * n);
    const float k = 1.0f / std::max(exposure, 1e-6f);
    parallelFor(n, 4096, [&](size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i) {
            const float* q = &p.rgba[4 * i];
            Vec3 light;
            if (p.linear) {
                // Light as it is; what a half float cannot hold, not at all.
                for (int c = 0; c < 3; ++c) light[c] = std::isfinite(q[c]) ? std::clamp(q[c], 0.0f, 65504.0f) : 0.0f;
            } else {
                light = unshown(Vec3(q[0], q[1], q[2]), view, ocio) * k;
            }
            out.pixels[3 * i] = light.x;
            out.pixels[3 * i + 1] = light.y;
            out.pixels[3 * i + 2] = light.z;
        }
    });
    return out;
}

Image plateSeen(const Plate& plate, const Image& light, const sim::Camera& camera, int width, int height) {
    Image out;
    out.width = std::max(width, 0);
    out.height = std::max(height, 0);
    out.channels = 3;
    out.pixels.assign(3 * static_cast<size_t>(out.width) * static_cast<size_t>(out.height), 0.0f);
    const int pw = light.width, ph = light.height;
    if (pw <= 0 || ph <= 0 || light.channels != 3 || out.pixels.empty()) return out;
    // The camera that filmed it, and the one the picture is seen by: their
    // axes and how wide they see, as the renderers make their rays.
    const sim::Camera& f = plate.camera;
    const Vec3 ff = f.forward(), fr = f.right(), fu = f.up();
    const float fy = std::tan(f.fovY() * kPi / 360.0f), fx = fy * f.aspect();
    const sim::Camera c = camera.sanitized();
    const Vec3 cf = c.forward(), cr = c.right(), cu = c.up();
    const float cy = std::tan(c.fovY() * kPi / 360.0f), cx = cy * static_cast<float>(out.width) / static_cast<float>(out.height);
    auto texel = [&](int x, int y) {
        return &light.pixels[3 * (static_cast<size_t>(std::clamp(y, 0, ph - 1)) * static_cast<size_t>(pw) +
                                  static_cast<size_t>(std::clamp(x, 0, pw - 1)))];
    };
    parallelFor(static_cast<size_t>(out.height), 16, [&](size_t begin, size_t end) {
        for (size_t row = begin; row < end; ++row) {
            const float v = (1.0f - 2.0f * (static_cast<float>(row) + 0.5f) / static_cast<float>(out.height)) * cy;
            for (int x = 0; x < out.width; ++x) {
                const float u = (2.0f * (static_cast<float>(x) + 0.5f) / static_cast<float>(out.width) - 1.0f) * cx;
                const Vec3 d = cf + cr * u + cu * v;
                const float z = dot(d, ff);
                if (z <= 1e-6f) continue;
                const float s = dot(d, fr) / (z * fx), t = dot(d, fu) / (z * fy);
                if (std::fabs(s) > 1.0f || std::fabs(t) > 1.0f) continue;
                // Where in the plate, its pixels' middles at whole numbers;
                // one of them, where it falls on one -- the same camera and
                // size -- else between the four round it.
                float px = 0.5f * (s + 1.0f) * static_cast<float>(pw) - 0.5f;
                float py = 0.5f * (1.0f - t) * static_cast<float>(ph) - 0.5f;
                if (std::fabs(px - std::round(px)) < 1e-3f) px = std::round(px);
                if (std::fabs(py - std::round(py)) < 1e-3f) py = std::round(py);
                const int x0 = static_cast<int>(std::floor(px)), y0 = static_cast<int>(std::floor(py));
                const float ax = px - static_cast<float>(x0), ay = py - static_cast<float>(y0);
                const float *a = texel(x0, y0), *b = texel(x0 + 1, y0), *e = texel(x0, y0 + 1), *g = texel(x0 + 1, y0 + 1);
                float* o = &out.pixels[3 * (row * static_cast<size_t>(out.width) + static_cast<size_t>(x))];
                for (int k = 0; k < 3; ++k) {
                    const float top = a[k] + (b[k] - a[k]) * ax, bottom = e[k] + (g[k] - e[k]) * ax;
                    o[k] = top + (bottom - top) * ay;
                }
            }
        }
    });
    return out;
}

Image overPlate(const Image& cg, const Image& alpha, const Image& catcher, const Image& plate) {
    Image out = cg;
    const size_t n = static_cast<size_t>(cg.width) * static_cast<size_t>(cg.height);
    if (cg.channels != 3 || cg.pixels.size() < 3 * n || plate.width != cg.width || plate.height != cg.height ||
        plate.pixels.size() < 3 * n) {
        return out;
    }
    const bool covers = alpha.width == cg.width && alpha.height == cg.height && alpha.pixels.size() >= n;
    const bool relit = catcher.width == cg.width && catcher.height == cg.height && catcher.channels == 3 &&
                       catcher.pixels.size() >= 3 * n;
    parallelFor(n, 16384, [&](size_t begin, size_t end) {
        for (size_t p = begin; p < end; ++p) {
            const float behind = 1.0f - (covers ? std::clamp(alpha.pixels[p * static_cast<size_t>(alpha.channels)], 0.0f, 1.0f) : 0.0f);
            for (size_t c = 0; c < 3; ++c) {
                const float k = relit ? std::max(catcher.pixels[3 * p + c], 0.0f) : 1.0f;
                out.pixels[3 * p + c] += plate.pixels[3 * p + c] * k * behind;
            }
        }
    });
    return out;
}

}  // namespace pg::render
