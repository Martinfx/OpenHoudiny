#include "pg/sim/Rain.h"

#include "pg/core/Parallel.h"
#include "pg/sim/Liquid.h"
#include "pg/sim/Shared.h"
#include "pg/sim/State.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace pg::sim {

using detail::fix;

namespace {

constexpr float kHuge = std::numeric_limits<float>::max();
constexpr float kGravity = 9.81f;
/// How fast a drop takes on the air's velocity: a raindrop's own time, its
/// speed over g, is some 0.7 s.
constexpr float kDropGrip = 1.4f;
/// Ripples: how fast they spread, m/s, and fade, 1/s; how deep a drop pushes
/// the water, m, and over how many cells.
constexpr float kRippleSpeed = 0.35f;
constexpr float kRippleFade = 3.0f;
constexpr float kRippleDepth = 0.003f;
constexpr float kRippleWidth = 2.0f;
/// The ripples' grid: at most this many cells along its longer side.
constexpr int kRippleCells = 256;

float unit(uint32_t h) { return static_cast<float>(h & 0xFFFFFFu) / 16777216.0f; }

/// Three numbers in [0, 1) for number `n` and channel `c`.
Vec3 random3(uint64_t n, int c, uint32_t seed) {
    const int lo = static_cast<int>(n & 0x7FFFFFFFu), hi = static_cast<int>(n >> 31);
    return {unit(detail::hash(lo, hi, 3 * c, seed)), unit(detail::hash(lo, hi, 3 * c + 1, seed)),
            unit(detail::hash(lo, hi, 3 * c + 2, seed))};
}

/// Two unit vectors square to `n` and to each other.
void across(const Vec3& n, Vec3& u, Vec3& v) {
    const Vec3 helper = std::fabs(n.y) < 0.9f ? Vec3(0.0f, 1.0f, 0.0f) : Vec3(1.0f, 0.0f, 0.0f);
    u = normalize(cross(n, helper));
    v = cross(n, u);
}

}  // namespace

RainScene RainScene::sanitized() const {
    const RainSettings d;
    RainScene s = *this;
    RainSettings& r = s.rain;
    r.center = fix(r.center, -kHuge, kHuge, d.center);
    r.size = fix(r.size, 0.01f, 100.0f, d.size);
    r.rate = fix(r.rate, 0.0f, 20000.0f, d.rate);
    r.speed = fix(r.speed, 0.1f, 50.0f, d.speed);
    r.splash = fix(r.splash, 0.0f, 20.0f, d.splash);
    r.ripples = fix(r.ripples, 0.0f, 20.0f, d.ripples);
    r.fill = fix(r.fill, 0.0f, 1000.0f, d.fill);
    r.start = fix(r.start, 0.0f, kHuge, 0.0f);
    r.end = fix(r.end, 0.0f, kHuge, 0.0f);
    r.timeStep = fix(r.timeStep, 1e-4f, 1.0f, d.timeStep);
    detail::sanitize(s.forces);
    detail::sanitize(s.colliders);
    return s;
}

RainSolver::RainSolver(const RainScene& scene) : scene_(scene.sanitized()) {
    for (const Collider& c : scene_.colliders) solids_.push_back(c.instance());
    noise_.assign(scene_.forces.size(), {});
}

void RainSolver::setScene(const RainScene& scene) {
    scene_ = scene.sanitized();
    solids_.clear();
    for (const Collider& c : scene_.colliders) solids_.push_back(c.instance());
    noise_.resize(scene_.forces.size());
}

void RainSolver::saveState(StateWriter& out) const {
    out.pod(static_cast<int32_t>(frame_));
    out.pod(time_);
    out.pod(made_);
    out.pod(splashed_);
    out.pod(static_cast<int32_t>(lastSolid_));
    out.pod(static_cast<int32_t>(lastWater_));
    out.list(drops_);
    out.list(droplets_);
    out.pod(ripples_.origin);
    out.pod(ripples_.cell);
    out.pod(static_cast<int32_t>(ripples_.nx));
    out.pod(static_cast<int32_t>(ripples_.nz));
    out.list(ripples_.height);
    out.list(previous_);
}

bool RainSolver::loadState(StateReader& in) {
    int32_t frame = 0, lastSolid = 0, lastWater = 0, nx = 0, nz = 0;
    float time = 0.0f;
    uint64_t made = 0;
    uint32_t splashed = 0;
    std::vector<RainParticle> drops, droplets;
    Ripples ripples;
    std::vector<float> previous;
    if (!in.pod(frame) || !in.pod(time) || !in.pod(made) || !in.pod(splashed) || !in.pod(lastSolid) ||
        !in.pod(lastWater) || !in.list(drops) || !in.list(droplets) || !in.pod(ripples.origin) ||
        !in.pod(ripples.cell) || !in.pod(nx) || !in.pod(nz) || !in.list(ripples.height) || !in.list(previous)) {
        return false;
    }
    if (nx < 0 || nz < 0 || ripples.height.size() != static_cast<size_t>(nx) * static_cast<size_t>(nz) ||
        (!previous.empty() && previous.size() != ripples.height.size())) {
        return in.fail();
    }
    ripples.nx = nx;
    ripples.nz = nz;
    frame_ = frame;
    time_ = time;
    made_ = made;
    splashed_ = splashed;
    lastSolid_ = lastSolid;
    lastWater_ = lastWater;
    drops_ = std::move(drops);
    droplets_ = std::move(droplets);
    ripples_ = std::move(ripples);
    previous_ = std::move(previous);
    return true;
}

Vec3 RainSolver::air(const Vec3& p) const {
    Vec3 wind;
    for (size_t f = 0; f < scene_.forces.size(); ++f) {
        const Force& force = scene_.forces[f];
        if (force.kind == ForceKind::Wind) {
            const uint32_t seed = force.seed * 7919u + scene_.rain.seed * 31u;
            wind += detail::windAt(force, time_, seed, p);
        }
    }
    return wind;
}

bool RainSolver::solidAt(const Vec3& p, Vec3& normal) const {
    if (p.y <= 0.0f) {
        normal = Vec3(0.0f, 1.0f, 0.0f);
        return true;
    }
    for (const ShapeInstance& s : solids_) {
        if (s.contains(p)) {
            normal = s.normal(p);
            return true;
        }
    }
    return false;
}

void RainSolver::spawn(float dt, const LiquidSolver* water) {
    const RainSettings& r = scene_.rain;
    if (!r.activeAt(time_)) return;
    const double area = static_cast<double>(r.size.x) * static_cast<double>(r.size.z);
    // Rain from the start has been falling before it: the air below the
    // cloud is full of drops already -- as many as fall in the time a drop
    // takes to reach the floor. Rain that starts later starts at the cloud.
    const double fall = r.start <= 0.0f ? std::max(r.center.y, 0.0f) / r.speed : 0.0;
    const double already = static_cast<double>(r.rate) * area * fall;
    // The drops due by the end of this step; a share of a drop waits for
    // the next one.
    const double total = static_cast<double>(r.rate) * area * (time_ - r.start + dt) + already;
    const uint64_t due = total > 0.0 ? static_cast<uint64_t>(total) : 0;
    const Vec3 lo = r.center - r.size * 0.5f;
    for (; made_ < due; ++made_) {
        const Vec3 u = random3(made_, 0, r.seed);
        RainParticle d;
        d.id = static_cast<uint32_t>(made_);
        d.position = lo + Vec3(u.x * r.size.x, u.y * r.size.y, u.z * r.size.z);
        d.velocity = air(d.position) + Vec3(0.0f, -r.speed, 0.0f);
        if (static_cast<double>(made_) < already) {
            // One of those: some of the way down, drifted with the wind.
            const float top = d.position.y;
            d.position.y *= 1.0f - random3(made_, 1, r.seed).x;
            const float t = (top - d.position.y) / r.speed;
            d.position.x += d.velocity.x * t;
            d.position.z += d.velocity.z * t;
            d.age = t;
            // One that would have landed by now is gone.
            Vec3 n;
            if (solidAt(d.position, n) || (water && water->distanceToSurface(d.position) < 0.0f)) continue;
        }
        drops_.push_back(d);
    }
}

void RainSolver::fitRipples(const LiquidSolver* water) {
    if (!water || scene_.rain.ripples <= 0.0f) {
        ripples_ = Ripples();
        previous_.clear();
        return;
    }
    const Domain& d = water->domain();
    const Vec3 size = d.size();
    const float cell = std::max(0.5f * d.voxel, std::max(size.x, size.z) / static_cast<float>(kRippleCells));
    const int nx = std::max(2, static_cast<int>(std::ceil(size.x / cell)));
    const int nz = std::max(2, static_cast<int>(std::ceil(size.z / cell)));
    if (ripples_.nx == nx && ripples_.nz == nz && ripples_.cell == cell) return;
    ripples_.origin = d.origin();
    ripples_.cell = cell;
    ripples_.nx = nx;
    ripples_.nz = nz;
    ripples_.height.assign(static_cast<size_t>(nx) * static_cast<size_t>(nz), 0.0f);
    previous_ = ripples_.height;
}

void RainSolver::ring(const Vec3& at, float strength) {
    if (ripples_.empty()) return;
    const float cx = (at.x - ripples_.origin.x) / ripples_.cell, cz = (at.z - ripples_.origin.z) / ripples_.cell;
    const int reach = static_cast<int>(std::ceil(3.0f * kRippleWidth));
    const int i0 = std::max(static_cast<int>(cx) - reach, 0), i1 = std::min(static_cast<int>(cx) + reach, ripples_.nx - 1);
    const int k0 = std::max(static_cast<int>(cz) - reach, 0), k1 = std::min(static_cast<int>(cz) + reach, ripples_.nz - 1);
    for (int k = k0; k <= k1; ++k) {
        for (int i = i0; i <= i1; ++i) {
            const float dx = static_cast<float>(i) + 0.5f - cx, dz = static_cast<float>(k) + 0.5f - cz;
            // A crater with a rim round it, as much water up as down:
            // (1 - q) e^-q, q = (r / width)^2, sums to nothing.
            const float q = (dx * dx + dz * dz) / (kRippleWidth * kRippleWidth);
            ripples_.height[static_cast<size_t>(i) + static_cast<size_t>(ripples_.nx) * static_cast<size_t>(k)] -=
                strength * kRippleDepth * (1.0f - q) * std::exp(-q);
        }
    }
}

void RainSolver::waves(float dt) {
    if (ripples_.empty()) return;
    // The wave equation, h'' = c^2 laplace(h) - fade h', in as many steps as
    // keep it stable (c dt / cell below 1/2). The Laplacian takes the
    // diagonal neighbours too: with the four alone, rings come out square.
    const int nx = ripples_.nx, nz = ripples_.nz;
    // The rings take and give back as much water, but not to the last bit:
    // what is left over is taken off, or the level would creep.
    double sum = 0.0;
    for (const float h : ripples_.height) sum += h;
    const float mean = static_cast<float>(sum / static_cast<double>(ripples_.height.size()));
    for (size_t c = 0; c < ripples_.height.size(); ++c) {
        ripples_.height[c] -= mean;
        previous_[c] -= mean;
    }
    const int steps = std::max(1, static_cast<int>(std::ceil(2.0f * kRippleSpeed * dt / ripples_.cell)));
    const float h = dt / static_cast<float>(steps);
    const float k = (kRippleSpeed * h / ripples_.cell) * (kRippleSpeed * h / ripples_.cell);
    const float keep = std::exp(-kRippleFade * h);
    std::vector<float> next(ripples_.height.size());
    for (int s = 0; s < steps; ++s) {
        const float* now = ripples_.height.data();
        const float* before = previous_.data();
        pg::parallelFor(static_cast<size_t>(nz), 16, [&](size_t begin, size_t end) {
            for (size_t z = begin; z < end; ++z) {
                const int kz = static_cast<int>(z);
                // Walls that reflect: a missing neighbour is the nearest cell inside.
                auto at = [&](int x, int y) {
                    x = std::clamp(x, 0, nx - 1);
                    y = std::clamp(y, 0, nz - 1);
                    return now[static_cast<size_t>(x) + static_cast<size_t>(nx) * static_cast<size_t>(y)];
                };
                for (int i = 0; i < nx; ++i) {
                    const size_t c = static_cast<size_t>(i) + static_cast<size_t>(nx) * z;
                    const float edges = at(i - 1, kz) + at(i + 1, kz) + at(i, kz - 1) + at(i, kz + 1);
                    const float corners = at(i - 1, kz - 1) + at(i + 1, kz - 1) + at(i - 1, kz + 1) + at(i + 1, kz + 1);
                    const float laplace = (4.0f * edges + corners - 20.0f * now[c]) / 6.0f;
                    next[c] = now[c] + keep * (now[c] - before[c]) + k * laplace;
                }
            }
        });
        previous_.swap(ripples_.height);
        ripples_.height.swap(next);
    }
}

void RainSolver::move(float dt, const LiquidSolver* water) {
    const RainSettings& r = scene_.rain;
    // What a drop landed on: 0 nothing yet, 1 a solid or the floor, 2 water.
    std::vector<uint8_t> landed(drops_.size(), 0);
    std::vector<Vec3> normal(drops_.size());
    const float grip = 1.0f - std::exp(-kDropGrip * dt);
    auto push = [&](const Vec3& p, Vec3 v, float fall) {
        // Towards the air's velocity and the drop's own fall; turbulence
        // shakes it, drag slows it, a vortex swirls it.
        const Vec3 target = air(p) + Vec3(0.0f, -fall, 0.0f);
        v = v + (target - v) * grip;
        for (size_t f = 0; f < scene_.forces.size(); ++f) {
            const Force& force = scene_.forces[f];
            const uint32_t seed = force.seed * 7919u + r.seed * 31u;
            switch (force.kind) {
                case ForceKind::Turbulence: {
                    const float s = 1.0f / force.scale, t = time_ * force.speed;
                    const Vec3 shake(detail::noise3(p.x * s + t, p.y * s, p.z * s, seed) * 2.0f - 1.0f,
                                     detail::noise3(p.x * s, p.y * s + t, p.z * s, seed + 1u) * 2.0f - 1.0f,
                                     detail::noise3(p.x * s, p.y * s, p.z * s + t, seed + 2u) * 2.0f - 1.0f);
                    v = v + shake * (force.strength * dt);
                    break;
                }
                case ForceKind::Drag: v = v * std::exp(-force.strength * dt); break;
                case ForceKind::Attractor: {
                    const Vec3 to = force.center - p;
                    const float d = length(to);
                    if (d > 1e-6f && d < force.radius) {
                        const float falloff = (1.0f - d / force.radius) * (1.0f - d / force.radius);
                        v = v + to * (force.strength * falloff * dt / d);
                    }
                    break;
                }
                case ForceKind::Vortex: {
                    const Vec3 axis = normalize(force.direction);
                    const Vec3 rel = p - force.center;
                    const float along = dot(rel, axis);
                    if (force.height > 0.0f && std::fabs(along) >= 0.5f * force.height) break;
                    const Vec3 out = rel - axis * along;
                    const float d = length(out);
                    if (d >= force.radius || d < 1e-6f) break;
                    const float x = d / force.radius;
                    const Vec3 outward = out * (1.0f / d);
                    const Vec3 swirl = cross(axis, outward) * (force.speed * 4.0f * x * (1.0f - x)) +
                                       axis * force.lift - outward * (force.suction * x);
                    const float pull = (1.0f - std::exp(-force.strength * dt)) * (1.0f - detail::smoothstep(0.75f, 1.0f, x));
                    v = v + (swirl - v) * pull;
                    break;
                }
                case ForceKind::Wind: break;  // in air()
            }
        }
        return v;
    };

    pg::parallelFor(drops_.size(), 1024, [&](size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i) {
            RainParticle& d = drops_[i];
            d.velocity = push(d.position, d.velocity, r.speed);
            Vec3 next = d.position + d.velocity * dt;
            Vec3 n;
            // A step of a fast drop is longer than shallow water is deep: one
            // that would end under the floor is asked where it reaches it.
            const Vec3 reached(next.x, std::max(next.y, 1e-3f), next.z);
            if (water && water->distanceToSurface(reached) < 0.0f) {
                landed[i] = 2;
                next = reached;
            } else if (solidAt(next, n)) {
                landed[i] = 1;
                normal[i] = n;
            }
            d.position = next;
            if (next.y <= 0.0f) d.position.y = 0.0f;
            d.age += dt;
        }
    });

    // Where they landed, in the order of the drops: spray off solids,
    // rings on the water.
    lastSolid_ = lastWater_ = 0;
    intoWater_.clear();
    std::vector<RainParticle> kept;
    kept.reserve(drops_.size());
    const uint32_t seed = r.seed * 2654435761u + 17u;
    for (size_t i = 0; i < drops_.size(); ++i) {
        const RainParticle& d = drops_[i];
        if (!landed[i]) {
            kept.push_back(d);
            continue;
        }
        const uint64_t id = (static_cast<uint64_t>(frame_) << 32) ^ (static_cast<uint64_t>(i) * 2654435761ull);
        if (landed[i] == 2) {
            ++lastWater_;
            if (r.fill > 0.0f) intoWater_.push_back(d);
            ring(d.position, r.ripples);
            if (r.ripples > 0.0f && random3(id, 0, seed).x < 0.5f) {
                RainParticle jump;
                jump.position = d.position + Vec3(0.0f, 0.004f, 0.0f);
                jump.velocity = Vec3(0.0f, 0.8f + 0.7f * random3(id, 1, seed).y, 0.0f);
                jump.life = 0.18f;
                jump.id = splashed_++;
                droplets_.push_back(jump);
            }
            continue;
        }
        ++lastSolid_;
        const Vec3 n = normal[i];
        Vec3 u, v;
        across(n, u, v);
        const int whole = static_cast<int>(r.splash);
        const int count = whole + (random3(id, 2, seed).x < r.splash - static_cast<float>(whole) ? 1 : 0);
        for (int c = 0; c < count; ++c) {
            const Vec3 q = random3(id, 3 + c, seed);
            const float turn = 6.2831853f * q.x;
            RainParticle drop;
            drop.position = d.position + n * 0.002f;
            // A crown: up a few centimetres and out, gone in a blink.
            drop.velocity = n * (0.6f + 0.9f * q.y) + (u * std::cos(turn) + v * std::sin(turn)) * (0.2f + 0.6f * q.z) +
                            Vec3(d.velocity.x, 0.0f, d.velocity.z) * 0.2f;
            drop.life = 0.12f + 0.18f * random3(id, 13 + c, seed).x;
            drop.id = splashed_++;
            droplets_.push_back(drop);
        }
    }
    drops_.swap(kept);

    // The droplets: thrown, falling, gone when their time is up or they
    // land again.
    std::vector<uint8_t> gone(droplets_.size(), 0);
    pg::parallelFor(droplets_.size(), 1024, [&](size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i) {
            RainParticle& d = droplets_[i];
            d.velocity = d.velocity + Vec3(0.0f, -kGravity * dt, 0.0f);
            d.position = d.position + d.velocity * dt;
            d.age += dt;
            Vec3 n;
            if (d.age >= d.life || d.position.y < 0.0f || (d.velocity.y < 0.0f && solidAt(d.position, n)) ||
                (water && d.velocity.y < 0.0f && water->distanceToSurface(d.position) < 0.0f)) {
                gone[i] = 1;
            }
        }
    });
    size_t k = 0;
    for (size_t i = 0; i < droplets_.size(); ++i) {
        if (!gone[i]) droplets_[k++] = droplets_[i];
    }
    droplets_.resize(k);
}

size_t RainSolver::evaporate(const std::function<float(const Vec3&)>& rate, float dt, uint32_t seed) {
    if (drops_.empty() || !rate || !(dt > 0.0f)) return 0;
    const size_t before = drops_.size();
    std::erase_if(drops_, [&](const RainParticle& d) {
        const float r = rate(d.position);
        return r > 0.0f && detail::boiledAway(d.id, seed, r * dt);
    });
    return before - drops_.size();
}

float RainSolver::dropVolume() const {
    const RainSettings& r = scene_.rain;
    // Fill mm a second over a square metre, shared by the drops falling on it.
    return r.rate > 0.0f ? 0.001f * r.fill / r.rate : 0.0f;
}

void RainSolver::step(const LiquidSolver* water) {
    const float dt = scene_.rain.timeStep;
    fitRipples(water);
    spawn(dt, water);
    move(dt, water);
    waves(dt);
    time_ += dt;
    ++frame_;
}

}  // namespace pg::sim
