#include "pg/render/PathTracer.h"

#include "pg/core/Parallel.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

namespace pg::render {
namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kInfinity = std::numeric_limits<float>::infinity();
constexpr int kTile = 16;

uint32_t hash32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

/// The numbers of one sample of one pixel: the same whatever else runs.
struct Rng {
    uint32_t state;

    Rng(uint32_t pixel, uint32_t sample, uint32_t seed)
        : state(hash32(pixel * 0x9E3779B9u ^ hash32(sample * 0x85EBCA6Bu + seed * 0xC2B2AE35u + 0x27d4eb2fu))) {}
    float next() {
        state = state * 747796405u + 2891336453u;
        uint32_t w = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
        w = (w >> 22u) ^ w;
        return static_cast<float>(w >> 8) * (1.0f / 16777216.0f);
    }
};

float luminance(const Vec3& c) { return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z; }
float largest(const Vec3& c) { return std::max(c.x, std::max(c.y, c.z)); }
Vec3 clamp01(const Vec3& c) {
    return {std::clamp(c.x, 0.0f, 1.0f), std::clamp(c.y, 0.0f, 1.0f), std::clamp(c.z, 0.0f, 1.0f)};
}

/// Axes round a unit normal (Duff et al., Building an Orthonormal Basis, Revisited).
struct Frame {
    Vec3 t, b, n;

    explicit Frame(const Vec3& normal) : n(normal) {
        const float sign = std::copysign(1.0f, n.z);
        const float a = -1.0f / (sign + n.z);
        const float c = n.x * n.y * a;
        t = Vec3(1.0f + sign * n.x * n.x * a, sign * c, -sign * n.x);
        b = Vec3(c, sign + n.y * n.y * a, -n.y);
    }
    Vec3 toLocal(const Vec3& v) const { return {dot(v, t), dot(v, b), dot(v, n)}; }
    Vec3 toWorld(const Vec3& v) const { return t * v.x + b * v.y + n * v.z; }
};

Vec3 cosineHemisphere(float u1, float u2) {
    const float r = std::sqrt(u1), phi = 2.0f * kPi * u2;
    return {r * std::cos(phi), r * std::sin(phi), std::sqrt(std::max(0.0f, 1.0f - u1))};
}

/// A direction within `cosMax` of the unit `axis`, all as likely.
Vec3 sampleCone(const Vec3& axis, float cosMax, float u1, float u2) {
    const float c = 1.0f - u1 * (1.0f - cosMax);
    const float s = std::sqrt(std::max(0.0f, 1.0f - c * c)), phi = 2.0f * kPi * u2;
    return Frame(axis).toWorld(Vec3(s * std::cos(phi), s * std::sin(phi), c));
}

float ggxD(float nh, float a2) {
    const float d = nh * nh * (a2 - 1.0f) + 1.0f;
    return a2 / (kPi * d * d);
}
float smithG1(float nv, float a2) { return 2.0f * nv / (nv + std::sqrt(a2 + (1.0f - a2) * nv * nv)); }
Vec3 schlick(const Vec3& f0, float c) {
    const float m = std::pow(std::clamp(1.0f - c, 0.0f, 1.0f), 5.0f);
    return f0 + (Vec3(1.0f, 1.0f, 1.0f) - f0) * m;
}

/// How much light a smooth boundary reflects, `cosi` the cosine on the
/// side it comes from, `eta` the index beyond over the index before.
float fresnelDielectric(float cosi, float eta) {
    const float sint2 = std::max(0.0f, 1.0f - cosi * cosi) / (eta * eta);
    if (sint2 >= 1.0f) return 1.0f;
    const float cost = std::sqrt(1.0f - sint2);
    const float rs = (cosi - eta * cost) / (cosi + eta * cost);
    const float rp = (eta * cosi - cost) / (eta * cosi + cost);
    return 0.5f * (rs * rs + rp * rp);
}

Vec3 reflect(const Vec3& d, const Vec3& n) { return d - n * (2.0f * dot(d, n)); }

/// An opaque surface as it scatters light: diffusely, off a GGX sheen, and
/// -- a thin one -- through to its other side. Directions leave it.
struct Surface {
    Frame frame;
    Vec3 wo;  // towards the eye, local
    Vec3 albedo, f0, fo;
    float a2 = 0.25f, kd = 1.0f, kt = 0.0f;
    float pD = 1.0f, pT = 0.0f, pS = 0.0f;

    Surface(const Vec3& color, const Material& m, const Vec3& toEye, const Vec3& normal) : frame(normal) {
        wo = frame.toLocal(toEye);
        wo.z = std::max(wo.z, 1e-4f);
        albedo = clamp01(color);
        const float r = std::clamp(m.roughness, 0.03f, 1.0f);
        a2 = std::max(r * r * r * r, 1e-7f);
        const float metal = std::clamp(m.metallic, 0.0f, 1.0f);
        f0 = Vec3(0.04f, 0.04f, 0.04f) * (1.0f - metal) + albedo * metal;
        fo = schlick(f0, wo.z);
        kd = (1.0f - metal) * (1.0f - std::clamp(m.translucency, 0.0f, 1.0f));
        kt = (1.0f - metal) * std::clamp(m.translucency, 0.0f, 1.0f);
        const float shine = luminance(fo);
        const float wS = std::max(shine, 0.08f) * (1.0f - metal) + metal;
        const float wD = kd * std::max(luminance(albedo), 0.01f) * (1.0f - shine);
        const float wT = kt * std::max(luminance(albedo), 0.01f) * (1.0f - shine);
        const float sum = wS + wD + wT;
        pS = wS / sum;
        pD = wD / sum;
        pT = wT / sum;
    }

    /// What it sends towards the eye of the light from along `wiWorld`,
    /// and how likely sample() is to pick that way.
    Vec3 eval(const Vec3& wiWorld, float& pdf) const {
        const Vec3 wi = frame.toLocal(wiWorld);
        pdf = 0.0f;
        if (wi.z > 0.0f) {
            const Vec3 diffuse = albedo * (Vec3(1.0f, 1.0f, 1.0f) - fo) * (kd / kPi);
            const Vec3 h = normalize(wo + wi);
            const float d = ggxD(h.z, a2);
            const float oh = std::max(dot(wo, h), 1e-6f);
            const Vec3 spec = schlick(f0, oh) * (d * smithG1(wo.z, a2) * smithG1(wi.z, a2) / (4.0f * wo.z * wi.z));
            pdf = pD * wi.z / kPi + pS * d * h.z / (4.0f * oh);
            return diffuse + spec;
        }
        if (wi.z < 0.0f && kt > 0.0f) {
            pdf = pT * -wi.z / kPi;
            return albedo * (Vec3(1.0f, 1.0f, 1.0f) - fo) * (kt / kPi);
        }
        return {};
    }

    bool sample(Rng& rng, Vec3& wiWorld, Vec3& f, float& pdf) const {
        const float u = rng.next(), u1 = rng.next(), u2 = rng.next();
        Vec3 wi;
        if (u < pD) {
            wi = cosineHemisphere(u1, u2);
        } else if (u < pD + pT) {
            wi = cosineHemisphere(u1, u2);
            wi.z = -wi.z;
        } else {
            // A microfacet as likely as GGX has it, the eye's way mirrored in it.
            const float tan2 = a2 * u1 / std::max(1.0f - u1, 1e-7f);
            const float c = 1.0f / std::sqrt(1.0f + tan2), s = std::sqrt(std::max(0.0f, 1.0f - c * c));
            const float phi = 2.0f * kPi * u2;
            const Vec3 h(s * std::cos(phi), s * std::sin(phi), c);
            wi = h * (2.0f * dot(wo, h)) - wo;
            if (wi.z <= 0.0f) return false;
        }
        wiWorld = frame.toWorld(wi);
        f = eval(wiWorld, pdf);
        return pdf > 1e-12f;
    }
};

struct Sample {
    Vec3 color, albedo{1.0f, 1.0f, 1.0f}, normal;
    float depth = kInfinity;
};

/// The light that comes along a camera ray, and what the ray first meets.
Sample trace(const Scene& scene, const Settings& s, Vec3 origin, Vec3 dir, float up, Rng& rng) {
    Sample out;
    Vec3 light, beta(1.0f, 1.0f, 1.0f);
    const Vec3 sun = scene.sunRadiance();
    const bool sunOn = largest(scene.sunLight) > 0.0f;
    const float pdfSun = 1.0f / (2.0f * kPi * std::max(1.0f - scene.sunCosine, 1e-9f));
    const float clarity = std::max(scene.look.waterClarity, 1e-3f);
    const Vec3 waterGlow = scene.look.waterColor * (scene.skyLight * 1.5f + scene.sunLight * (0.35f * std::max(scene.sunDirection.y, 0.0f)));
    bool specular = true, inWater = false;
    float lastPdf = 0.0f;
    int bounces = 0, turns = 0;
    for (;;) {
        Hit hit;
        const bool met = scene.intersect(origin, dir, kInfinity, rng.next(), hit);
        // Past the first bounce, no one path adds more than `clamp`: no fireflies.
        auto add = [&](const Vec3& c) {
            Vec3 v = c;
            const float most = largest(v);
            if (bounces > 0 && most > s.clamp) v = v * (s.clamp / most);
            light = light + v;
        };
        if (inWater) {
            // Through water: the more of it, the more of its colour.
            const float through = std::exp(-(met ? hit.t : 1e3f) / clarity);
            add(beta * waterGlow * (1.0f - through));
            beta = beta * through;
        }
        if (!met) {
            if (turns == 0) {
                light = light + scene.background(dir, up);
            } else {
                add(beta * scene.sky(dir));
            }
            if (sunOn && dot(dir, scene.sunDirection) >= scene.sunCosine) {
                // The sun, found by the bounce: weighed against finding it directly.
                const float w = specular ? 1.0f : lastPdf * lastPdf / (lastPdf * lastPdf + pdfSun * pdfSun);
                add(beta * sun * w);
            }
            break;
        }
        const Material& m = *hit.material;
        if (turns == 0) {
            out.depth = hit.t;
            out.normal = hit.normal;
            out.albedo = m.kind == Material::Kind::Surface ? clamp01(hit.color) : Vec3(1.0f, 1.0f, 1.0f);
        }
        ++turns;
        const float eps = 1e-4f * (1.0f + std::max(std::fabs(hit.position.x), std::max(std::fabs(hit.position.y), std::fabs(hit.position.z))));

        if (m.kind != Material::Kind::Surface) {
            // Glass and water: reflected, else bent into it or out of it.
            if (turns > s.bounces + 12) break;
            const bool entering = dot(hit.face, dir) < 0.0f;
            const Vec3 face = entering ? hit.face : hit.face * -1.0f;
            Vec3 n = entering ? hit.normal : hit.normal * -1.0f;
            if (dot(n, dir) >= 0.0f) n = face;
            const float eta = entering ? m.ior : 1.0f / m.ior;
            const float cosi = std::clamp(-dot(dir, n), 0.0f, 1.0f);
            const float reflected = fresnelDielectric(cosi, eta);
            if (rng.next() < reflected) {
                dir = normalize(reflect(dir, n));
                origin = hit.position + face * eps;
            } else {
                const float k = 1.0f / eta;
                const float c2 = 1.0f - k * k * (1.0f - cosi * cosi);
                dir = normalize(dir * k + n * (k * cosi - std::sqrt(std::max(c2, 0.0f))));
                origin = hit.position - face * eps;
                if (m.kind == Material::Kind::Glass && entering) {
                    beta = beta * (Vec3(1.0f, 1.0f, 1.0f) * 0.65f + clamp01(hit.color) * 0.35f);
                }
                if (m.kind == Material::Kind::Water) inWater = entering;
            }
            specular = true;
            lastPdf = 0.0f;
            continue;
        }

        // An opaque surface, seen from the side the ray came from.
        Vec3 face = hit.face, n = hit.normal;
        if (dot(face, dir) > 0.0f) {
            face = face * -1.0f;
            n = n * -1.0f;
        }
        if (dot(n, dir) >= 0.0f) n = face;
        const Surface surface(hit.color, m, dir * -1.0f, n);

        // The sun, directly: a point of its disc, if nothing is in the way.
        if (sunOn) {
            // Taken one after the other: a call's arguments are in no set order.
            const float u1 = rng.next();
            const float u2 = rng.next();
            const Vec3 wl = sampleCone(scene.sunDirection, scene.sunCosine, u1, u2);
            float pdf = 0.0f;
            const Vec3 f = surface.eval(wl, pdf);
            const float side = dot(face, wl);
            if (largest(f) > 0.0f && side != 0.0f && (dot(n, wl) > 0.0f) == (side > 0.0f)) {
                const Vec3 through = scene.transmittance(hit.position + face * (side > 0.0f ? eps : -eps), wl, kInfinity);
                if (largest(through) > 0.0f) {
                    const float w = pdfSun * pdfSun / (pdfSun * pdfSun + pdf * pdf);
                    add(beta * f * through * sun * (std::fabs(dot(n, wl)) * w / pdfSun));
                }
            }
        }
        if (bounces >= s.bounces) break;

        // The next bounce.
        Vec3 wi, f;
        float pdf = 0.0f;
        if (!surface.sample(rng, wi, f, pdf)) break;
        const float side = dot(face, wi);
        if (side == 0.0f || (dot(n, wi) > 0.0f) != (side > 0.0f)) break;
        beta = beta * f * (std::fabs(dot(n, wi)) / pdf);
        origin = hit.position + face * (side > 0.0f ? eps : -eps);
        dir = wi;
        specular = false;
        lastPdf = pdf;
        ++bounces;
        if (bounces >= 3) {
            // Russian roulette: a dim path ends, as likely as it is dim.
            const float keep = std::min(0.95f, largest(beta));
            if (rng.next() >= keep) break;
            beta = beta * (1.0f / keep);
        }
    }
    out.color = light;
    return out;
}

float aces(float x) { return std::clamp(x * (2.51f * x + 0.03f) / (x * (2.43f * x + 0.59f) + 0.14f), 0.0f, 1.0f); }

}  // namespace

void PathTracer::setScene(std::shared_ptr<const Scene> scene) {
    scene_ = std::move(scene);
    restart();
}

void PathTracer::setSettings(const Settings& settings) {
    Settings s = settings;
    s.width = std::clamp(s.width, 1, 16384);
    s.height = std::clamp(s.height, 1, 16384);
    s.samples = std::max(s.samples, 1);
    s.bounces = std::clamp(s.bounces, 0, 64);
    // How many samples make it done, and whether to denoise, do not change what a sample is.
    Settings a = s, b = settings_;
    a.samples = b.samples = 0;
    a.denoise = b.denoise = false;
    const bool same = a == b;
    settings_ = s;
    if (!same) restart();
}

void PathTracer::restart() {
    std::lock_guard<std::mutex> lock(mutex_);
    samples_ = 0;
    seconds_ = 0.0;
    paths_ = 0;
    const size_t n = static_cast<size_t>(settings_.width) * static_cast<size_t>(settings_.height);
    sum_.assign(3 * n, 0.0f);
    square_.assign(n, 0.0f);
    albedo_.assign(3 * n, 0.0f);
    normal_.assign(3 * n, 0.0f);
    depth_.assign(2 * n, 0.0f);
    // How far the lens focuses: as asked, else what the middle of the picture sees.
    focus_ = settings_.focus > 0.0f ? settings_.focus : 10.0f;
    if (scene_ && settings_.focus <= 0.0f) {
        const sim::Camera& c = scene_->camera;
        Hit hit;
        if (scene_->intersect(c.position, normalize(c.forward()), kInfinity, 0.0f, hit)) focus_ = std::max(hit.t, 0.05f);
    }
}

int PathTracer::samples() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return samples_;
}

double PathTracer::seconds() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return seconds_;
}

uint64_t PathTracer::paths() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return paths_;
}

bool PathTracer::pass(const std::atomic<bool>* stop) {
    if (!scene_) return false;
    const auto start = std::chrono::steady_clock::now();
    const Scene& scene = *scene_;
    const Settings s = settings_;
    const int w = s.width, h = s.height;
    const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h);
    int sample = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        sample = samples_;
    }
    passColor_.resize(3 * n);
    passAlbedo_.resize(3 * n);
    passNormal_.resize(3 * n);
    passDepth_.resize(n);

    // The camera: its lens a full frame's, 24 mm high; the picture as wide as asked.
    const sim::Camera& cam = scene.camera;
    const Vec3 eye = cam.position, forward = normalize(cam.forward()), right = normalize(cam.right()), upAxis = normalize(cam.up());
    const float tanY = std::tan(0.5f * cam.fovY() * kPi / 180.0f);
    const float tanX = tanY * static_cast<float>(w) / static_cast<float>(h);
    const float aperture = s.fstop > 0.0f ? 0.5f * cam.focal * 0.001f / s.fstop : 0.0f;
    const float focus = focus_;

    const int tilesX = (w + kTile - 1) / kTile, tilesY = (h + kTile - 1) / kTile;
    std::atomic<bool> stopped{false};
    parallelFor(static_cast<size_t>(tilesX) * static_cast<size_t>(tilesY), 1, [&](size_t begin, size_t end) {
        for (size_t tile = begin; tile < end; ++tile) {
            if (stop && stop->load(std::memory_order_relaxed)) {
                stopped = true;
                return;
            }
            const int tx = static_cast<int>(tile % static_cast<size_t>(tilesX)) * kTile;
            const int ty = static_cast<int>(tile / static_cast<size_t>(tilesX)) * kTile;
            for (int y = ty; y < std::min(ty + kTile, h); ++y) {
                for (int x = tx; x < std::min(tx + kTile, w); ++x) {
                    const size_t p = static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x);
                    Rng rng(static_cast<uint32_t>(p), static_cast<uint32_t>(sample), s.seed);
                    // Anywhere in the pixel: edges come out smooth.
                    const float px = (static_cast<float>(x) + rng.next()) / static_cast<float>(w) * 2.0f - 1.0f;
                    const float py = 1.0f - (static_cast<float>(y) + rng.next()) / static_cast<float>(h) * 2.0f;
                    Vec3 dir = normalize(forward + right * (px * tanX) + upAxis * (py * tanY));
                    Vec3 origin = eye;
                    if (aperture > 0.0f) {
                        // Through a point of the lens, to where the pixel is sharp.
                        const Vec3 sharp = eye + dir * (focus / std::max(dot(dir, forward), 1e-4f));
                        const float r = aperture * std::sqrt(rng.next()), phi = 2.0f * kPi * rng.next();
                        origin = eye + right * (r * std::cos(phi)) + upAxis * (r * std::sin(phi));
                        dir = normalize(sharp - origin);
                    }
                    const Sample got = trace(scene, s, origin, dir, 0.5f * (py + 1.0f), rng);
                    Vec3 c = got.color;
                    if (!(std::isfinite(c.x) && std::isfinite(c.y) && std::isfinite(c.z))) c = Vec3();
                    passColor_[3 * p] = c.x;
                    passColor_[3 * p + 1] = c.y;
                    passColor_[3 * p + 2] = c.z;
                    passAlbedo_[3 * p] = got.albedo.x;
                    passAlbedo_[3 * p + 1] = got.albedo.y;
                    passAlbedo_[3 * p + 2] = got.albedo.z;
                    passNormal_[3 * p] = got.normal.x;
                    passNormal_[3 * p + 1] = got.normal.y;
                    passNormal_[3 * p + 2] = got.normal.z;
                    passDepth_[p] = got.depth;
                }
            }
        }
    });
    if (stopped || (stop && stop->load())) return false;

    std::lock_guard<std::mutex> lock(mutex_);
    if (sum_.size() != 3 * n) return false;  // started again meanwhile, at another size
    parallelFor(n, 16384, [&](size_t begin, size_t end) {
        for (size_t p = begin; p < end; ++p) {
            for (size_t c = 0; c < 3; ++c) {
                sum_[3 * p + c] += passColor_[3 * p + c];
                albedo_[3 * p + c] += passAlbedo_[3 * p + c];
                normal_[3 * p + c] += passNormal_[3 * p + c];
            }
            const float l = luminance(Vec3(passColor_[3 * p], passColor_[3 * p + 1], passColor_[3 * p + 2]));
            square_[p] += l * l;
            if (std::isfinite(passDepth_[p])) {
                depth_[2 * p] += passDepth_[p];
                depth_[2 * p + 1] += 1.0f;
            }
        }
    });
    ++samples_;
    paths_ += n;
    seconds_ += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    return true;
}

Image PathTracer::average(const std::vector<float>& sum, int channels) const {
    Image out;
    out.width = settings_.width;
    out.height = settings_.height;
    out.channels = channels;
    out.pixels.assign(sum.size(), 0.0f);
    if (samples_ == 0) return out;
    const float k = 1.0f / static_cast<float>(samples_);
    for (size_t i = 0; i < sum.size(); ++i) out.pixels[i] = sum[i] * k;
    return out;
}

Image PathTracer::beauty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return average(sum_, 3);
}

Image PathTracer::albedo() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return average(albedo_, 3);
}

Image PathTracer::normal() const {
    std::lock_guard<std::mutex> lock(mutex_);
    Image out = average(normal_, 3);
    for (size_t p = 0; p + 2 < out.pixels.size(); p += 3) {
        const Vec3 v(out.pixels[p], out.pixels[p + 1], out.pixels[p + 2]);
        const float l = length(v);
        if (l > 1e-6f) {
            out.pixels[p] = v.x / l;
            out.pixels[p + 1] = v.y / l;
            out.pixels[p + 2] = v.z / l;
        }
    }
    return out;
}

Image PathTracer::depth() const {
    std::lock_guard<std::mutex> lock(mutex_);
    Image out;
    out.width = settings_.width;
    out.height = settings_.height;
    out.channels = 1;
    const size_t n = depth_.size() / 2;
    out.pixels.resize(n);
    // Seen by most of its samples, else the sky's.
    for (size_t p = 0; p < n; ++p) {
        const float hits = depth_[2 * p + 1];
        out.pixels[p] = hits > 0.5f * static_cast<float>(samples_) && hits > 0.0f ? depth_[2 * p] / hits : kInfinity;
    }
    return out;
}

Image PathTracer::denoised() const {
    const Image color = beauty(), a = albedo(), nrm = normal(), z = depth();
    std::vector<float> variance;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const size_t n = square_.size();
        variance.assign(n, 0.0f);
        if (samples_ > 1) {
            const float k = 1.0f / static_cast<float>(samples_);
            for (size_t p = 0; p < n; ++p) {
                const float mean = luminance(Vec3(sum_[3 * p], sum_[3 * p + 1], sum_[3 * p + 2])) * k;
                // Of the mean: the samples' spread over their number.
                variance[p] = std::max(square_[p] * k - mean * mean, 0.0f) * k;
            }
        } else {
            for (size_t p = 0; p < n; ++p) variance[p] = 1.0f;
        }
    }
    return denoise(color, a, nrm, z, variance);
}

std::vector<uint8_t> PathTracer::display(bool withDenoise) const {
    const Image image = withDenoise ? denoised() : beauty();
    return toDisplay(image, scene_ ? scene_->look.exposure : 1.0f);
}

std::vector<uint8_t> toDisplay(const Image& image, float exposure) {
    const size_t n = static_cast<size_t>(image.width) * static_cast<size_t>(image.height);
    std::vector<uint8_t> out(4 * n, 255);
    if (image.pixels.size() < n * static_cast<size_t>(image.channels)) return out;
    parallelFor(n, 16384, [&](size_t begin, size_t end) {
        for (size_t p = begin; p < end; ++p) {
            for (int c = 0; c < 3; ++c) {
                const float v = image.pixels[p * static_cast<size_t>(image.channels) + static_cast<size_t>(std::min(c, image.channels - 1))];
                const float shown = std::pow(aces(std::max(v * exposure, 0.0f)), 1.0f / 2.2f);
                out[4 * p + static_cast<size_t>(c)] = static_cast<uint8_t>(std::lround(std::clamp(shown, 0.0f, 1.0f) * 255.0f));
            }
        }
    });
    return out;
}

Image denoise(const Image& beauty, const Image& albedo, const Image& normal, const Image& depth,
              const std::vector<float>& variance) {
    const int w = beauty.width, h = beauty.height;
    const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h);
    Image out = beauty;
    if (n == 0 || albedo.pixels.size() < 3 * n || normal.pixels.size() < 3 * n || depth.pixels.size() < n ||
        variance.size() < n) {
        return out;
    }
    // The light each surface gets: its colour divided out, so the filter
    // keeps the detail of the colours and smooths only the noise.
    std::vector<Vec3> light(n), colour(n), nrm(n), next(n);
    std::vector<float> spread(n);
    std::vector<uint8_t> sky(n);
    for (size_t p = 0; p < n; ++p) {
        const Vec3 c(beauty.pixels[3 * p], beauty.pixels[3 * p + 1], beauty.pixels[3 * p + 2]);
        const Vec3 a(std::max(albedo.pixels[3 * p], 0.03f), std::max(albedo.pixels[3 * p + 1], 0.03f),
                     std::max(albedo.pixels[3 * p + 2], 0.03f));
        sky[p] = std::isfinite(depth.pixels[p]) ? 0 : 1;
        colour[p] = a;
        light[p] = sky[p] ? c : Vec3(c.x / a.x, c.y / a.y, c.z / a.z);
        nrm[p] = Vec3(normal.pixels[3 * p], normal.pixels[3 * p + 1], normal.pixels[3 * p + 2]);
    }
    // How far a pixel may differ: its noise -- as the pixels round it have
    // it, a few samples' own spread being too noisy to go by.
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const size_t p = static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x);
            float v = 0.0f;
            int count = 0;
            for (int qy = std::max(y - 2, 0); qy <= std::min(y + 2, h - 1); ++qy) {
                for (int qx = std::max(x - 2, 0); qx <= std::min(x + 2, w - 1); ++qx) {
                    const size_t q = static_cast<size_t>(qy) * static_cast<size_t>(w) + static_cast<size_t>(qx);
                    if (sky[q]) continue;
                    v += variance[q];
                    ++count;
                }
            }
            spread[p] = std::sqrt(count > 0 ? v / static_cast<float>(count) : variance[p]) / std::max(luminance(colour[p]), 0.03f);
        }
    }
    const float kernel[3] = {3.0f / 8.0f, 1.0f / 4.0f, 1.0f / 16.0f};
    for (int pass = 0; pass < 5; ++pass) {
        const int step = 1 << pass;
        // The noise left after each pass is less: so is how far a pixel may differ.
        const float sigma = 4.0f / std::sqrt(static_cast<float>(1 << pass));
        parallelFor(static_cast<size_t>(h), 4, [&](size_t y0, size_t y1) {
            for (size_t yy = y0; yy < y1; ++yy) {
                const int y = static_cast<int>(yy);
                for (int x = 0; x < w; ++x) {
                    const size_t p = static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x);
                    if (sky[p]) {
                        next[p] = light[p];
                        continue;
                    }
                    const float zp = depth.pixels[p];
                    const float lp = luminance(light[p]);
                    Vec3 sum;
                    float weights = 0.0f;
                    for (int dy = -2; dy <= 2; ++dy) {
                        const int qy = y + dy * step;
                        if (qy < 0 || qy >= h) continue;
                        for (int dx = -2; dx <= 2; ++dx) {
                            const int qx = x + dx * step;
                            if (qx < 0 || qx >= w) continue;
                            const size_t q = static_cast<size_t>(qy) * static_cast<size_t>(w) + static_cast<size_t>(qx);
                            if (sky[q]) continue;
                            float k = kernel[std::abs(dx)] * kernel[std::abs(dy)];
                            if (q != p) {
                                const float facing = std::max(dot(nrm[p], nrm[q]), 0.0f);
                                const float wn = std::pow(facing, 32.0f);
                                const float far = std::sqrt(static_cast<float>(dx * dx + dy * dy)) * static_cast<float>(step);
                                const float wz = std::exp(-std::fabs(zp - depth.pixels[q]) / (0.02f * zp * far + 1e-4f));
                                const Vec3 da = colour[p] - colour[q];
                                const float wa = std::exp(-dot(da, da) / 0.004f);
                                const float wc = std::exp(-std::fabs(lp - luminance(light[q])) / (sigma * spread[p] + 1e-5f));
                                k *= wn * wz * wa * wc;
                            }
                            sum = sum + light[q] * k;
                            weights += k;
                        }
                    }
                    next[p] = weights > 0.0f ? sum * (1.0f / weights) : light[p];
                }
            }
        });
        std::swap(light, next);
    }
    for (size_t p = 0; p < n; ++p) {
        const Vec3 c = sky[p] ? light[p] : light[p] * colour[p];
        out.pixels[3 * p] = c.x;
        out.pixels[3 * p + 1] = c.y;
        out.pixels[3 * p + 2] = c.z;
    }
    return out;
}

}  // namespace pg::render
