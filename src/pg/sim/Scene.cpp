#include "pg/sim/Scene.h"

#include "pg/sim/Shared.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace pg::sim {
namespace {

constexpr float kTau = 6.28318530717958647692f;
constexpr float kHuge = std::numeric_limits<float>::max();

}  // namespace

namespace detail {

float fix(float v, float lo, float hi, float fallback) { return std::isfinite(v) ? std::clamp(v, lo, hi) : fallback; }

Vec3 fix(const Vec3& v, float lo, float hi, const Vec3& fallback) {
    return {fix(v.x, lo, hi, fallback.x), fix(v.y, lo, hi, fallback.y), fix(v.z, lo, hi, fallback.z)};
}

Vec3 fixDirection(const Vec3& v, const Vec3& fallback) {
    const Vec3 d = fix(v, -kHuge, kHuge, fallback);
    return length(d) > 1e-6f ? d : fallback;
}

void sanitize(std::vector<Force>& forces) {
    const Force df;
    for (Force& f : forces) {
        // A negative attractor pushes away, a vortex with a negative speed
        // spins the other way; the rest only makes sense forwards.
        const bool vortex = f.kind == ForceKind::Vortex;
        f.strength = fix(f.strength, f.kind == ForceKind::Attractor ? -kHuge : 0.0f, kHuge, df.strength);
        f.scale = fix(f.scale, 0.005f, kHuge, df.scale);
        f.speed = fix(f.speed, vortex ? -kHuge : 0.0f, kHuge, df.speed);
        f.direction = fixDirection(f.direction, vortex ? Vec3(0, 1, 0) : df.direction);
        f.gusts = fix(f.gusts, 0.0f, 1.0f, 0.0f);
        f.center = fix(f.center, -kHuge, kHuge, df.center);
        f.radius = fix(f.radius, 0.005f, kHuge, df.radius);
        f.height = fix(f.height, 0.0f, kHuge, 0.0f);
        f.lift = fix(f.lift, -kHuge, kHuge, 0.0f);
        f.suction = fix(f.suction, -kHuge, kHuge, 0.0f);
    }
}

void sanitize(std::vector<Collider>& colliders) {
    const Collider dc;
    for (Collider& c : colliders) {
        c.center = fix(c.center, -kHuge, kHuge, dc.center);
        c.rotation = fix(c.rotation, -kHuge, kHuge, Vec3());
        c.size = fix(c.size, 0.005f, kHuge, dc.size);
    }
}

}  // namespace detail

using detail::fix;

Vec3 Emitter::centerAt(float t) const {
    const float w = kTau / motionPeriod;
    switch (motion) {
        case Motion::Static: break;
        case Motion::Circle: return center + Vec3(motionSize * std::cos(w * t), 0.0f, motionSize * std::sin(w * t));
        case Motion::Sway: return center + Vec3(motionSize * std::sin(w * t), 0.0f, 0.0f);
    }
    return center;
}

Vec3 Emitter::motionVelocityAt(float t) const {
    const float w = kTau / motionPeriod;
    switch (motion) {
        case Motion::Static: break;
        case Motion::Circle: return Vec3(-motionSize * w * std::sin(w * t), 0.0f, motionSize * w * std::cos(w * t));
        case Motion::Sway: return Vec3(motionSize * w * std::cos(w * t), 0.0f, 0.0f);
    }
    return Vec3();
}

Domain Domain::ofBox(const Vec3& size, int resolution) {
    Domain d;
    const float longest = std::max({size.x, size.y, size.z});
    d.voxel = longest / static_cast<float>(resolution);
    for (int a = 0; a < 3; ++a) {
        const int n = static_cast<int>(std::ceil(size[a] / d.voxel - 1e-3f));
        d.cells[a] = std::max(8, (n + 7) / 8 * 8);
    }
    return d;
}

Domain SolverSettings::domain() const { return Domain::ofBox(size, resolution); }

Scene Scene::sanitized() const {
    const SolverSettings ds;
    Scene s = *this;
    SolverSettings& v = s.solver;
    v.size = fix(v.size, 0.1f, 1000.0f, ds.size);
    v.resolution = std::clamp(v.resolution, 16, 1024);
    v.cutoff = fix(v.cutoff, 0.0f, 1.0f, ds.cutoff);
    v.timeStep = fix(v.timeStep, 1e-4f, 1.0f, ds.timeStep);
    v.substeps = std::clamp(v.substeps, 1, 16);
    v.pressureCycles = std::clamp(v.pressureCycles, 1, 16);
    v.buoyancy = fix(v.buoyancy, -kHuge, kHuge, ds.buoyancy);  // below 0: heat sinks, which is fine
    v.weight = fix(v.weight, -kHuge, kHuge, ds.weight);
    v.vorticity = fix(v.vorticity, 0.0f, kHuge, ds.vorticity);
    v.burnRate = fix(v.burnRate, 0.0f, kHuge, ds.burnRate);
    v.heatRelease = fix(v.heatRelease, 0.0f, kHuge, ds.heatRelease);
    v.sootRelease = fix(v.sootRelease, 0.0f, kHuge, ds.sootRelease);
    v.expansion = fix(v.expansion, 0.0f, kHuge, ds.expansion);
    v.flameLife = fix(v.flameLife, 0.0f, kHuge, ds.flameLife);
    v.cooling = fix(v.cooling, 0.0f, kHuge, ds.cooling);
    v.smokeDecay = fix(v.smokeDecay, 0.0f, kHuge, ds.smokeDecay);
    v.quench = fix(v.quench, 0.0f, 100.0f, ds.quench);
    v.steam = fix(v.steam, 0.0f, 100.0f, ds.steam);
    v.steamLift = fix(v.steamLift, 0.0f, 100.0f, ds.steamLift);
    v.steamFade = fix(v.steamFade, 0.0f, 100.0f, ds.steamFade);
    v.evaporate = fix(v.evaporate, 0.0f, 1000.0f, ds.evaporate);

    const Emitter de;
    for (Emitter& e : s.emitters) {
        e.center = fix(e.center, -kHuge, kHuge, de.center);
        e.rotation = fix(e.rotation, -kHuge, kHuge, Vec3());
        e.size = fix(e.size, 0.005f, kHuge, de.size);
        e.fuel = fix(e.fuel, 0.0f, kHuge, 0.0f);
        e.smoke = fix(e.smoke, 0.0f, kHuge, 0.0f);
        e.heat = fix(e.heat, 0.0f, kHuge, 0.0f);
        e.velocity = fix(e.velocity, -kHuge, kHuge, Vec3());
        e.expansion = fix(e.expansion, 0.0f, kHuge, 0.0f);
        e.flicker = fix(e.flicker, 0.0f, 2.0f, 0.0f);
        e.flickerSize = fix(e.flickerSize, 0.005f, kHuge, de.flickerSize);
        e.start = fix(e.start, 0.0f, kHuge, 0.0f);
        e.end = fix(e.end, 0.0f, kHuge, 0.0f);
        e.motionSize = fix(e.motionSize, 0.0f, kHuge, 0.0f);
        e.motionPeriod = fix(e.motionPeriod, 0.05f, kHuge, de.motionPeriod);
    }
    detail::sanitize(s.forces);
    detail::sanitize(s.colliders);
    return s;
}

Scene Scene::fire() {
    Scene s;
    SolverSettings& v = s.solver;
    v.sparse = true;  // as the network's Pyro Solver has it
    v.buoyancy = 0.9f;
    v.weight = 0.05f;
    v.vorticity = 0.9f;
    v.burnRate = 10.0f;
    v.heatRelease = 2.5f;
    v.sootRelease = 0.5f;
    v.expansion = 0.8f;
    v.flameLife = 0.1f;
    v.cooling = 1.5f;
    v.smokeDecay = 0.15f;

    Emitter e;
    e.center = Vec3(0.0f, 0.12f, 0.0f);
    e.size = Vec3(0.2f);
    e.fuel = 14.0f;
    e.heat = 1.0f;
    e.velocity = Vec3(0.0f, 0.4f, 0.0f);
    e.flicker = 0.7f;
    s.emitters.push_back(e);

    Force t;
    t.kind = ForceKind::Turbulence;
    t.strength = 3.5f;
    t.scale = 0.05f;
    t.mask = Mask::Heat;
    s.forces.push_back(t);
    return s;
}

Scene Scene::smoke() {
    Scene s;
    SolverSettings& v = s.solver;
    v.sparse = true;  // as the network's Pyro Solver has it
    v.buoyancy = 1.0f;
    v.weight = 0.08f;
    v.vorticity = 0.9f;
    v.cooling = 0.6f;
    v.smokeDecay = 0.03f;

    Emitter e;
    e.center = Vec3(0.0f, 0.12f, 0.0f);
    e.size = Vec3(0.2f);
    e.smoke = 5.0f;
    e.heat = 3.0f;
    e.velocity = Vec3(0.0f, 0.55f, 0.0f);
    e.flicker = 0.15f;
    s.emitters.push_back(e);

    Force t;
    t.kind = ForceKind::Turbulence;
    t.strength = 3.5f;
    t.scale = 0.05f;
    t.mask = Mask::Heat;
    s.forces.push_back(t);
    return s;
}

}  // namespace pg::sim
