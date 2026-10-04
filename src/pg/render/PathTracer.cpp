#include "pg/render/PathTracer.h"

#include "pg/core/Parallel.h"
#include "pg/render/Denoise.h"
#include "pg/render/Plate.h"
#include "pg/render/Random.h"

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/matrix.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>

namespace pg::render {
namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kInfinity = std::numeric_limits<float>::infinity();
constexpr int kTile = 16;

float luminance(const Vec3& c) { return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z; }
float largest(const Vec3& c) { return std::max(c.x, std::max(c.y, c.z)); }
Vec3 clamp01(const Vec3& c) { return glm::clamp(c, 0.0f, 1.0f); }

/// Axes round a unit normal (Duff et al., Building an Orthonormal Basis,
/// Revisited): the columns of a matrix, its transpose the way back.
struct Frame {
    Mat3 axes;

    explicit Frame(const Vec3& n) {
        const float sign = std::copysign(1.0f, n.z);
        const float a = -1.0f / (sign + n.z);
        const float c = n.x * n.y * a;
        axes = Mat3(Vec3(1.0f + sign * n.x * n.x * a, sign * c, -sign * n.x), Vec3(c, sign + n.y * n.y * a, -n.y), n);
    }
    Vec3 toLocal(const Vec3& v) const { return v * axes; }
    Vec3 toWorld(const Vec3& v) const { return axes * v; }
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

/// How likely the smoke scatters light going one way into a way at an angle
/// to it of cosine `c`: Henyey and Greenstein's.
float henyeyGreenstein(float c, float g) {
    const float d = 1.0f + g * g - 2.0f * g * c;
    return (1.0f - g * g) / (4.0f * kPi * d * std::sqrt(d));
}

/// ... as the viewport mixes it: mostly forwards, a little back.
float phase(float c) { return 0.7f * henyeyGreenstein(c, 0.55f) + 0.3f * henyeyGreenstein(c, -0.25f); }

/// A way for light going along `dir` to be scattered into, as likely as
/// phase() has it.
Vec3 samplePhase(const Vec3& dir, float u0, float u1, float u2) {
    const float g = u0 < 0.7f ? 0.55f : -0.25f;
    const float k = (1.0f - g * g) / (1.0f - g + 2.0f * g * u1);
    const float c = std::clamp((1.0f + g * g - k * k) / (2.0f * g), -1.0f, 1.0f);
    const float s = std::sqrt(std::max(0.0f, 1.0f - c * c)), phi = 2.0f * kPi * u2;
    return Frame(dir).toWorld(Vec3(s * std::cos(phi), s * std::sin(phi), c));
}

struct Sample {
    Vec3 color, albedo{1.0f, 1.0f, 1.0f}, normal;
    float depth = kInfinity;
    /// Over a plate: whether the CG covers it -- not where the camera ray
    /// went on to the plate, or met a holdout or a catcher; and on a
    /// catcher, the light there with the CG and without it (catcherLight).
    bool covered = true, caught = false;
    Vec3 with, without;
};

using Textures = std::unordered_map<const Material*, std::shared_ptr<const TexturePicture>>;

/// The plate as a camera ray that went through glass or water sees it: its
/// light (plateLight) where the camera that filmed it looked that way.
struct PlateSight {
    const Image* light = nullptr;
    Vec3 forward{0.0f, 0.0f, -1.0f}, right{1.0f, 0.0f, 0.0f}, up{0.0f, 1.0f, 0.0f};
    float tanX = 1.0f, tanY = 1.0f;

    /// False outside its frame.
    bool along(const Vec3& dir, Vec3& out) const {
        if (!light || light->width <= 0 || light->height <= 0 || light->channels != 3) return false;
        const float z = dot(dir, forward);
        if (z <= 1e-6f) return false;
        const float u = dot(dir, right) / (z * tanX), v = dot(dir, up) / (z * tanY);
        if (std::fabs(u) > 1.0f || std::fabs(v) > 1.0f) return false;
        const int x = std::clamp(static_cast<int>(0.5f * (u + 1.0f) * static_cast<float>(light->width)), 0, light->width - 1);
        const int y = std::clamp(static_cast<int>(0.5f * (1.0f - v) * static_cast<float>(light->height)), 0, light->height - 1);
        const float* p = &light->pixels[3 * (static_cast<size_t>(y) * static_cast<size_t>(light->width) + static_cast<size_t>(x))];
        out = Vec3(p[0], p[1], p[2]);
        return true;
    }
};

Sample trace(const Scene& scene, const Settings& s, const Textures& textures, const PlateSight* plate, Vec3 origin,
             Vec3 dir, float up, Rng& rng, bool camera = true, float firstPdf = 0.0f);

/// On a catcher -- `hit`, met along `dir` -- the light a white matt surface
/// there gets, with the CG and without it: with, the sun past everything,
/// the smoke too, and what comes along a direction of the sky -- the sky,
/// or what the CG sends: its light, the fire's; without, the sun past the
/// real things alone (Scene::realBlocks), the sky between them. Along a
/// direction where a real thing is first, neither: the plate has what it
/// sends. The same rays for both -- where the CG changes nothing, the two
/// are equal.
void catcherLight(const Scene& scene, const Settings& s, const Textures& textures, const Hit& hit, const Vec3& dir,
                  Rng& rng, Vec3& with, Vec3& without) {
    with = without = Vec3(0.0f);
    Vec3 face = hit.face, n = hit.normal;
    if (dot(face, dir) > 0.0f) {
        face = face * -1.0f;
        n = n * -1.0f;
    }
    if (dot(n, dir) >= 0.0f) n = face;
    const Vec3& p = hit.position;
    const Vec3 from = p + face * (1e-4f * (1.0f + std::max(std::fabs(p.x), std::max(std::fabs(p.y), std::fabs(p.z)))));
    if (largest(scene.sunLight) > 0.0f) {
        // Taken one after the other: a call's arguments are in no set order.
        const float u1 = rng.next();
        const float u2 = rng.next();
        const Vec3 wl = sampleCone(scene.sunDirection, scene.sunCosine, u1, u2);
        const float c = dot(n, wl);
        if (c > 0.0f && dot(face, wl) > 0.0f) {
            // A matt surface's: the sun weighed against finding it along the sky's direction.
            const float pdfSun = 1.0f / (2.0f * kPi * std::max(1.0f - scene.sunCosine, 1e-9f)), pdfSky = c / kPi;
            const Vec3 light = scene.sunRadiance() * (pdfSky * pdfSun / (pdfSun * pdfSun + pdfSky * pdfSky));
            if (!scene.realBlocks(from, wl, kInfinity)) without = without + light;
            Vec3 through = scene.transmittance(from, wl, kInfinity);
            if (scene.gas && largest(through) > 0.0f) {
                through = through * scene.gas->transmittance(from, wl, 0.0f, kInfinity, scene.gasLook, rng);
            }
            with = with + light * through;
        }
    }
    const float u1 = rng.next();
    const float u2 = rng.next();
    const Vec3 wi = Frame(n).toWorld(cosineHemisphere(u1, u2));
    const float c = dot(n, wi);
    if (c <= 0.0f || dot(face, wi) <= 0.0f) return;
    if (scene.realBlocks(from, wi, kInfinity)) {
        // Behind a real thing: what the CG in front of it sends, if any.
        Hit h;
        if (!scene.intersect(from, wi, kInfinity, rng.next(), h) || scene.matteOf(h) != sim::Matte::None) return;
    } else {
        without = without + scene.sky(wi);
        if (largest(scene.sunLight) > 0.0f && dot(wi, scene.sunDirection) >= scene.sunCosine) {
            // The sun, found along it: weighed as trace weighs it.
            const float pdfSun = 1.0f / (2.0f * kPi * std::max(1.0f - scene.sunCosine, 1e-9f)), pdfSky = c / kPi;
            without = without + scene.sunRadiance() * (pdfSky * pdfSky / (pdfSky * pdfSky + pdfSun * pdfSun));
        }
    }
    with = with + trace(scene, s, textures, nullptr, from, wi, 0.5f, rng, false, c / kPi).color;
}

/// The light that comes along a camera ray, and what the ray first meets;
/// `textures` what is laid on the materials. Over a plate, what the plate
/// shows -- the sky behind, the real things -- the camera ray leaves to it
/// (Sample::covered); through glass and water it sees `plate` itself. Not a
/// camera ray: a path from a catcher on, the first of its directions as
/// likely as `firstPdf` says.
Sample trace(const Scene& scene, const Settings& s, const Textures& textures, const PlateSight* plate, Vec3 origin,
             Vec3 dir, float up, Rng& rng, bool camera, float firstPdf) {
    Sample out;
    Vec3 light, beta(1.0f, 1.0f, 1.0f);
    const Vec3 sun = scene.sunRadiance();
    const bool sunOn = largest(scene.sunLight) > 0.0f;
    const float pdfSun = 1.0f / (2.0f * kPi * std::max(1.0f - scene.sunCosine, 1e-9f));
    const float clarity = std::max(scene.look.waterClarity, 1e-3f);
    const Vec3 waterGlow = scene.look.waterColor * (scene.skyLight * 1.5f + scene.sunLight * (0.35f * std::max(scene.sunDirection.y, 0.0f)));
    const Gas* gas = scene.gas.get();
    const GasLook& gasLook = scene.gasLook;
    Vec3 gasAlbedo = clamp01(gasLook.albedo);  // where it last scattered: the smoke's, the steam's
    bool specular = camera, inWater = false;
    float lastPdf = camera ? 0.0f : firstPdf;
    int bounces = 0, turns = 0, streaks = 0;
    // Over a plate: a camera ray that so far has only gone through glass and water.
    const bool over = camera && scene.plate != nullptr;
    bool seenThrough = over;
    for (;;) {
        Hit hit;
        const bool met = scene.intersect(origin, dir, kInfinity, rng.next(), hit);
        // A streak of rain: its drop there as much of the time as its
        // opacity says -- met from the front; passed through the rest of the
        // time, and from behind (the light that went in comes out bent once,
        // as Cycles has it).
        const bool past = met && hit.material->kind == Material::Kind::Rain &&
                          (dot(hit.face, dir) >= 0.0f || rng.next() >= hit.material->opacity);
        if (met && !textures.empty()) {
            if (const auto it = textures.find(hit.material); it != textures.end()) {
                hit.color = it->second->shade(hit.color, hit.tint, hit.rest, hit.restFace);
            }
        }
        if (camera && turns == 0 && met && !past) {
            // What the camera ray meets, for the denoiser to go by -- the gas
            // before it comes in as a whole pixel sees it (PathTracer::pass).
            out.depth = hit.t;
            out.normal = hit.normal;
            out.albedo = hit.material->kind == Material::Kind::Surface ? clamp01(hit.color) : Vec3(1.0f, 1.0f, 1.0f);
        }
        // Past the first bounce, no one path adds more than `clamp`: no fireflies.
        auto add = [&](const Vec3& c) {
            Vec3 v = c;
            const float most = largest(v);
            if (bounces > 0 && most > s.clamp) v = v * (s.clamp / most);
            light = light + v;
        };
        // The gas on the way to what the ray meets: the light the flames
        // give off along it, and where the smoke scatters it, if it does.
        float tGas = 0.0f;
        bool scattered = false;
        if (gas) {
            Vec3 emitted, kept;
            scattered = gas->track(origin, dir, 0.0f, met ? hit.t : kInfinity, gasLook, rng, tGas, emitted, kept);
            if (largest(emitted) > 0.0f) add(beta * emitted);
            if (scattered) gasAlbedo = clamp01(kept);
        }
        if (inWater) {
            // Through water: the more of it, the more of its colour.
            const float through = std::exp(-(scattered ? tGas : met ? hit.t : 1e3f) / clarity);
            add(beta * waterGlow * (1.0f - through));
            beta = beta * through;
        }
        if (scattered) {
            // Scattered by the smoke: the sun, directly, if nothing hides it;
            // then on, as the smoke scatters light.
            const Vec3 at = origin + dir * tGas;
            ++turns;
            seenThrough = false;
            if (sunOn) {
                const float u1 = rng.next();
                const float u2 = rng.next();
                const Vec3 wl = sampleCone(scene.sunDirection, scene.sunCosine, u1, u2);
                const float p = phase(dot(wl, dir));
                Vec3 through = scene.transmittance(at, wl, kInfinity);
                if (largest(through) > 0.0f) through = through * gas->transmittance(at, wl, 0.0f, kInfinity, gasLook, rng);
                if (largest(through) > 0.0f) {
                    const float w = pdfSun * pdfSun / (pdfSun * pdfSun + p * p);
                    add(beta * gasAlbedo * through * sun * (p * w / pdfSun));
                }
            }
            if (bounces >= s.bounces) break;
            const float u0 = rng.next();
            const float u1 = rng.next();
            const float u2 = rng.next();
            const Vec3 wi = samplePhase(dir, u0, u1, u2);
            lastPdf = phase(dot(wi, dir));
            beta = beta * gasAlbedo;
            origin = at;
            dir = wi;
            specular = false;
            ++bounces;
            if (bounces >= 3) {
                const float keep = std::min(0.95f, largest(beta));
                if (rng.next() >= keep) break;
                beta = beta * (1.0f / keep);
            }
            continue;
        }
        if (!met) {
            if (over) {
                // The plate behind it all: straight from the camera, left
                // to it; through glass and water, as the plate shows it there.
                if (turns == 0) {
                    out.covered = false;
                    break;
                }
                Vec3 seen;
                if (seenThrough && plate && plate->along(dir, seen)) {
                    add(beta * seen);
                    break;
                }
            }
            if (turns == 0 && camera) {
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
        const float eps = 1e-4f * (1.0f + std::max(std::fabs(hit.position.x), std::max(std::fabs(hit.position.y), std::fabs(hit.position.z))));
        if (past) {
            if (++streaks > 1024) break;
            origin = hit.position + dir * eps;
            continue;
        }
        if (over) {
            // A real thing the plate shows: met by the camera ray, left to the
            // plate -- a catcher relit by the CG; through glass and water, as
            // the plate shows it there. Met later on, drawn as itself.
            const sim::Matte matte = scene.matteOf(hit);
            if (matte != sim::Matte::None && turns == 0) {
                out.covered = false;
                if (matte == sim::Matte::Catcher) {
                    out.caught = true;
                    catcherLight(scene, s, textures, hit, dir, rng, out.with, out.without);
                }
                break;
            }
            Vec3 seen;
            if (matte != sim::Matte::None && seenThrough && plate && plate->along(dir, seen)) {
                add(beta * seen);
                break;
            }
        }
        ++turns;

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
                dir = normalize(glm::reflect(dir, n));
                origin = hit.position + face * eps;
                seenThrough = false;
            } else {
                const float k = 1.0f / eta;
                const float c2 = 1.0f - k * k * (1.0f - cosi * cosi);
                dir = normalize(dir * k + n * (k * cosi - std::sqrt(std::max(c2, 0.0f))));
                origin = hit.position - face * eps;
                if ((m.kind == Material::Kind::Glass || m.kind == Material::Kind::Rain) && entering) {
                    beta = beta * (Vec3(1.0f, 1.0f, 1.0f) * 0.65f + clamp01(hit.color) * 0.35f);
                }
                if (m.kind == Material::Kind::Water) inWater = entering;
            }
            specular = true;
            lastPdf = 0.0f;
            continue;
        }

        // An opaque surface, seen from the side the ray came from.
        seenThrough = false;
        Vec3 face = hit.face, n = hit.normal;
        if (dot(face, dir) > 0.0f) {
            face = face * -1.0f;
            n = n * -1.0f;
        }
        if (dot(n, dir) >= 0.0f) n = face;
        // Wet where the rain falls: darker, and smoother -- a film of water on it.
        const float wet = scene.wetAt(hit.position, n);
        Material wetted;
        if (wet > 0.0f) {
            wetted.roughness = m.roughness + (0.06f - m.roughness) * wet;
            wetted.metallic = m.metallic;
            wetted.translucency = m.translucency;
        }
        const Surface surface(wet > 0.0f ? hit.color * (1.0f - 0.5f * wet) : hit.color, wet > 0.0f ? wetted : m,
                              dir * -1.0f, n);

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
                const Vec3 from = hit.position + face * (side > 0.0f ? eps : -eps);
                Vec3 through = scene.transmittance(from, wl, kInfinity);
                if (gas && largest(through) > 0.0f) through = through * gas->transmittance(from, wl, 0.0f, kInfinity, gasLook, rng);
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
    findTextures();
    restart();
}

void PathTracer::findTextures() {
    textures_.clear();
    if (!scene_ || !settings_.textures) return;
    for (const auto& mesh : scene_->meshes) {
        if (!mesh) continue;
        for (const Material& m : mesh->materials) {
            const TextureSet set = textureOf(m, settings_);
            if (!set.valid()) continue;
            if (auto picture = texturePicture(set)) textures_[&m] = std::move(picture);
        }
    }
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
    const bool textures = s.textures != settings_.textures || s.textureFolder != settings_.textureFolder;
    settings_ = s;
    if (textures) findTextures();
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
    gasSeen_.clear();
    // Over a plate: what the CG covers, the catchers' light; the plate's
    // light, for what is seen of it through glass and water.
    const bool over = scene_ && scene_->plate;
    cover_.assign(over ? n : 0, 0.0f);
    caught_.assign(over ? n : 0, 0.0f);
    with_.assign(over ? 3 * n : 0, 0.0f);
    without_.assign(over ? 3 * n : 0, 0.0f);
    plateLight_ = over ? render::plateLight(*scene_->plate, settings_.view, scene_->look.exposure) : Image();
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
    const bool over = scene.plate != nullptr;
    passCover_.resize(over ? n : 0);
    passCaught_.resize(over ? n : 0);
    passWith_.resize(over ? 3 * n : 0);
    passWithout_.resize(over ? 3 * n : 0);
    // The plate as the camera that filmed it saw it, for what is seen of it
    // through glass and water.
    PlateSight sight;
    if (over) {
        const sim::Camera& f = scene.plate->camera;
        sight.light = &plateLight_;
        sight.forward = normalize(f.forward());
        sight.right = normalize(f.right());
        sight.up = normalize(f.up());
        sight.tanY = std::tan(0.5f * f.fovY() * kPi / 180.0f);
        sight.tanX = sight.tanY * f.aspect();
    }

    // The camera: its lens a full frame's, 24 mm high; the picture as wide as asked.
    const sim::Camera& cam = scene.camera;
    const Vec3 eye = cam.position, forward = normalize(cam.forward()), right = normalize(cam.right()), upAxis = normalize(cam.up());
    const float tanY = std::tan(0.5f * cam.fovY() * kPi / 180.0f);
    const float tanX = tanY * static_cast<float>(w) / static_cast<float>(h);
    const float aperture = s.fstop > 0.0f ? 0.5f * cam.focal * 0.001f / s.fstop : 0.0f;
    const float focus = focus_;

    const int tilesX = (w + kTile - 1) / kTile, tilesY = (h + kTile - 1) / kTile;
    std::atomic<bool> stopped{false};
    // With the first pass, what each pixel sees of the gas -- through its
    // middle, up to the surface there -- for the denoiser to go by: a
    // sample either is scattered by the smoke or goes through, and what
    // each finds first would be as noisy as the light.
    std::vector<float> seen;
    if (scene.gas && sample == 0) {
        seen.assign(5 * n, 1.0f);
        parallelFor(static_cast<size_t>(h), 4, [&](size_t y0, size_t y1) {
            for (size_t y = y0; y < y1; ++y) {
                for (size_t x = 0; x < static_cast<size_t>(w); ++x) {
                    const size_t p = y * static_cast<size_t>(w) + x;
                    const float px = (static_cast<float>(x) + 0.5f) / static_cast<float>(w) * 2.0f - 1.0f;
                    const float py = 1.0f - (static_cast<float>(y) + 0.5f) / static_cast<float>(h) * 2.0f;
                    const Vec3 dir = normalize(forward + right * (px * tanX) + upAxis * (py * tanY));
                    Hit hit;
                    const float tMax = scene.intersect(eye, dir, kInfinity, 0.5f, hit) ? hit.t : kInfinity;
                    scene.gas->seen(eye, dir, tMax, scene.gasLook, seen[5 * p], seen[5 * p + 1]);
                    seen[5 * p + 2] = -dir.x;
                    seen[5 * p + 3] = -dir.y;
                    seen[5 * p + 4] = -dir.z;
                }
            }
        });
        if (stop && stop->load()) return false;
    }
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
                    const Sample got = trace(scene, s, textures_, over ? &sight : nullptr, origin, dir, 0.5f * (py + 1.0f), rng);
                    Vec3 c = got.color;
                    if (!(std::isfinite(c.x) && std::isfinite(c.y) && std::isfinite(c.z))) c = Vec3();
                    if (over) {
                        passCover_[p] = got.covered ? 1.0f : 0.0f;
                        passCaught_[p] = got.caught ? 1.0f : 0.0f;
                        const bool finite = std::isfinite(largest(got.with)) && std::isfinite(largest(got.without));
                        for (int k = 0; k < 3; ++k) {
                            passWith_[3 * p + static_cast<size_t>(k)] = got.caught && finite ? got.with[k] : 0.0f;
                            passWithout_[3 * p + static_cast<size_t>(k)] = got.caught && finite ? got.without[k] : 0.0f;
                        }
                    }
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
    if (over != (cover_.size() == n)) return false;  // ... with a plate or without
    if (!seen.empty()) gasSeen_ = std::move(seen);
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
            if (over) {
                cover_[p] += passCover_[p];
                caught_[p] += passCaught_[p];
                for (size_t c = 0; c < 3; ++c) {
                    with_[3 * p + c] += passWith_[3 * p + c];
                    without_[3 * p + c] += passWithout_[3 * p + c];
                }
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

Image PathTracer::alpha() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (cover_.empty()) return {};
    return average(cover_, 1);
}

Image PathTracer::catcher(bool denoise) const {
    Image raw = catcherOf();
    if (!denoise || raw.pixels.empty() || defaultDenoiser() != Denoiser::Oidn) return raw;
    Image out;
    std::string error;
    return oidnDenoise(raw, albedo(), normal(), out, error) ? out : raw;
}

Image PathTracer::catcherOf() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (cover_.empty()) return {};
    Image out;
    out.width = settings_.width;
    out.height = settings_.height;
    out.channels = 3;
    const size_t n = cover_.size();
    out.pixels.assign(3 * n, 1.0f);
    // Of the samples the plate shows, those over the plate itself or a
    // holdout as it is; those on a catcher by the light there with the CG
    // over the light without it -- the sums of each, not their samples'
    // ratios, which would be noisy.
    const float all = static_cast<float>(samples_);
    for (size_t p = 0; p < n; ++p) {
        const float shown = all - cover_[p], caught = caught_[p];
        if (shown <= 0.0f || caught <= 0.0f) continue;
        for (size_t c = 0; c < 3; ++c) {
            const float without = without_[3 * p + c];
            const float ratio = without > 0.0f ? with_[3 * p + c] / without : 1.0f;
            out.pixels[3 * p + c] = (shown - caught + caught * ratio) / shown;
        }
    }
    return out;
}

Image PathTracer::albedo() const {
    std::lock_guard<std::mutex> lock(mutex_);
    Image out = average(albedo_, 3);
    // The gas in front, as much as it hides: the colour it looks.
    if (gasSeen_.size() == 5 * (out.pixels.size() / 3) && scene_) {
        const Vec3 gas = clamp01(scene_->gasLook.color);
        for (size_t p = 0; p < out.pixels.size() / 3; ++p) {
            const float through = gasSeen_[5 * p];
            for (int c = 0; c < 3; ++c) {
                float& a = out.pixels[3 * p + static_cast<size_t>(c)];
                a = a * through + gas[c] * (1.0f - through);
            }
        }
    }
    return out;
}

Image PathTracer::normal() const {
    std::lock_guard<std::mutex> lock(mutex_);
    Image out = average(normal_, 3);
    const bool gas = gasSeen_.size() == 5 * (out.pixels.size() / 3);
    for (size_t p = 0; p + 2 < out.pixels.size(); p += 3) {
        Vec3 v(out.pixels[p], out.pixels[p + 1], out.pixels[p + 2]);
        const float l = length(v);
        if (l > 1e-6f) v = v * (1.0f / l);
        if (gas) {
            // The gas in front, as much as it hides: turned to the eye.
            const float* g = &gasSeen_[5 * (p / 3)];
            v = v * g[0] + Vec3(g[2], g[3], g[4]) * (1.0f - g[0]);
            const float k = length(v);
            if (k > 1e-6f) v = v * (1.0f / k);
        }
        out.pixels[p] = v.x;
        out.pixels[p + 1] = v.y;
        out.pixels[p + 2] = v.z;
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
    const bool gas = gasSeen_.size() == 5 * n;
    // Seen by most of its samples, else the sky's. The gas in front comes
    // in as much as it hides what is behind it -- gradually, or the
    // denoiser would stop at where it hid half; over the sky, all of it.
    for (size_t p = 0; p < n; ++p) {
        const float hits = depth_[2 * p + 1];
        float d = hits > 0.5f * static_cast<float>(samples_) && hits > 0.0f ? depth_[2 * p] / hits : kInfinity;
        if (gas) {
            const float through = gasSeen_[5 * p], z = gasSeen_[5 * p + 1];
            if (std::isfinite(z)) {
                if (std::isfinite(d)) d = d * through + z * (1.0f - through);
                else if (through < 0.98f) d = z;
            }
        }
        out.pixels[p] = d;
    }
    return out;
}

Image PathTracer::denoised() const {
    const Image color = beauty(), a = albedo(), nrm = normal();
    // Open Image Denoise, where the build has it; else, or when it fails, our own filter.
    if (defaultDenoiser() == Denoiser::Oidn) {
        Image out;
        std::string error;
        if (oidnDenoise(color, a, nrm, out, error)) return out;
        static std::once_flag said;
        std::call_once(said, [&] { std::fprintf(stderr, "%s: our own filter takes the noise out\n", error.c_str()); });
    }
    const Image z = depth();
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
    return toDisplay(image, scene_ ? scene_->look.exposure : 1.0f, settings_.view);
}

namespace {

/// AgX's logarithm: from 12.5 stops below middle grey to 4 above.
constexpr float kAgxLow = -12.47393f, kAgxHigh = 4.026069f;

/// AgX's curve (Sobotka's polynomial fit), from the logarithm put into 0 to
/// 1 to the picture's 0 to 1; rising all the way.
float agxCurve(float x) {
    const float x2 = x * x, x4 = x2 * x2;
    return 15.5f * x4 * x2 - 40.14f * x4 * x + 31.96f * x4 - 6.868f * x2 * x + 0.4298f * x2 + 0.1191f * x - 0.00232f;
}

/// AgX's primaries, in and out (shown's), as matrices: their columns.
const Mat3 kAgxInset(0.842479062253094f, 0.0423282422610123f, 0.0423756549057051f,  //
                     0.0784335999999992f, 0.878468636469772f, 0.0784336f,           //
                     0.0792237451477643f, 0.0791661274605434f, 0.879142973793104f);
const Mat3 kAgxOutset(1.19687900512017f, -0.0528968517574562f, -0.0529716355144438f,  //
                      -0.0980208811401368f, 1.15190312990417f, -0.0980434501171241f,  //
                      -0.0990297440797205f, -0.0989611768448433f, 1.15107367264116f);

}  // namespace

Vec3 shown(const Vec3& linear, Settings::View view) {
    if (view == Settings::View::Aces) {
        return {std::pow(aces(std::max(linear.x, 0.0f)), 1.0f / 2.2f), std::pow(aces(std::max(linear.y, 0.0f)), 1.0f / 2.2f),
                std::pow(aces(std::max(linear.z, 0.0f)), 1.0f / 2.2f)};
    }
    // AgX: the colour moved in from the primaries a little -- so that a
    // bright saturated one goes towards white rather than to a hue of its
    // own -- its logarithm from 12.5 stops below middle grey to 4 above,
    // the curve, and the primaries out again.
    const Vec3 c(std::max(linear.x, 0.0f), std::max(linear.y, 0.0f), std::max(linear.z, 0.0f));
    Vec3 in(0.842479062253094f * c.x + 0.0784335999999992f * c.y + 0.0792237451477643f * c.z,
            0.0423282422610123f * c.x + 0.878468636469772f * c.y + 0.0791661274605434f * c.z,
            0.0423756549057051f * c.x + 0.0784336f * c.y + 0.879142973793104f * c.z);
    auto curve = [&](float v) {
        return agxCurve((std::clamp(std::log2(std::max(v, 1e-10f)), kAgxLow, kAgxHigh) - kAgxLow) / (kAgxHigh - kAgxLow));
    };
    in = Vec3(curve(in.x), curve(in.y), curve(in.z));
    if (view == Settings::View::AgXPunchy) {
        // Blender's look Punchy: more contrast -- a power of 1.35 -- and
        // 1.4 times the saturation.
        in = Vec3(std::pow(std::max(in.x, 0.0f), 1.35f), std::pow(std::max(in.y, 0.0f), 1.35f),
                  std::pow(std::max(in.z, 0.0f), 1.35f));
        const float luma = 0.2126f * in.x + 0.7152f * in.y + 0.0722f * in.z;
        in = Vec3(luma, luma, luma) + (in - Vec3(luma, luma, luma)) * 1.4f;
    }
    const Vec3 out(1.19687900512017f * in.x - 0.0980208811401368f * in.y - 0.0990297440797205f * in.z,
                   -0.0528968517574562f * in.x + 1.15190312990417f * in.y - 0.0989611768448433f * in.z,
                   -0.0529716355144438f * in.x - 0.0980434501171241f * in.y + 1.15107367264116f * in.z);
    return {std::clamp(out.x, 0.0f, 1.0f), std::clamp(out.y, 0.0f, 1.0f), std::clamp(out.z, 0.0f, 1.0f)};
}

Vec3 unshown(const Vec3& display, Settings::View view) {
    const Vec3 d = glm::clamp(display, 0.0f, 1.0f);
    if (view == Settings::View::Aces) {
        // Narkowicz's fit of x is y: x the root of a quadratic.
        Vec3 out;
        for (int c = 0; c < 3; ++c) {
            const double y = std::pow(static_cast<double>(d[c]), 2.2);
            const double a = 2.51 - 2.43 * y, b = 0.03 - 0.59 * y, k = -0.14 * y;
            out[c] = static_cast<float>((-b + std::sqrt(std::max(b * b - 4.0 * a * k, 0.0))) / (2.0 * a));
        }
        return out;
    }
    // AgX backwards: the primaries in again; Punchy's saturation undone --
    // about the luma it keeps -- and its power; the curve undone by halving
    // the way, as it rises all the way; the logarithm, and the primaries.
    static const Mat3 outsetBack = glm::inverse(kAgxOutset), insetBack = glm::inverse(kAgxInset);
    Vec3 y = outsetBack * d;
    if (view == Settings::View::AgXPunchy) {
        const float luma = 0.2126f * y.x + 0.7152f * y.y + 0.0722f * y.z;
        y = Vec3(luma) + (y - Vec3(luma)) * (1.0f / 1.4f);
        y = Vec3(std::pow(std::max(y.x, 0.0f), 1.0f / 1.35f), std::pow(std::max(y.y, 0.0f), 1.0f / 1.35f),
                 std::pow(std::max(y.z, 0.0f), 1.0f / 1.35f));
    }
    static const float lowest = agxCurve(0.0f), highest = agxCurve(1.0f);
    Vec3 in;
    for (int c = 0; c < 3; ++c) {
        const float target = std::clamp(y[c], lowest, highest);
        float lo = 0.0f, hi = 1.0f;
        for (int i = 0; i < 32; ++i) {
            const float mid = 0.5f * (lo + hi);
            (agxCurve(mid) < target ? lo : hi) = mid;
        }
        in[c] = std::exp2(0.5f * (lo + hi) * (kAgxHigh - kAgxLow) + kAgxLow);
    }
    // A colour no light shows -- too saturated for AgX -- the nearest that is.
    return glm::max(insetBack * in, Vec3(0.0f));
}

std::vector<uint8_t> toDisplay(const Image& image, float exposure, Settings::View view) {
    const size_t n = static_cast<size_t>(image.width) * static_cast<size_t>(image.height);
    std::vector<uint8_t> out(4 * n, 255);
    if (image.pixels.size() < n * static_cast<size_t>(image.channels)) return out;
    parallelFor(n, 16384, [&](size_t begin, size_t end) {
        for (size_t p = begin; p < end; ++p) {
            float v[3];
            for (int c = 0; c < 3; ++c) {
                v[c] = image.pixels[p * static_cast<size_t>(image.channels) + static_cast<size_t>(std::min(c, image.channels - 1))];
            }
            const Vec3 s = shown(Vec3(v[0], v[1], v[2]) * exposure, view);
            out[4 * p] = static_cast<uint8_t>(std::lround(std::clamp(s.x, 0.0f, 1.0f) * 255.0f));
            out[4 * p + 1] = static_cast<uint8_t>(std::lround(std::clamp(s.y, 0.0f, 1.0f) * 255.0f));
            out[4 * p + 2] = static_cast<uint8_t>(std::lround(std::clamp(s.z, 0.0f, 1.0f) * 255.0f));
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
