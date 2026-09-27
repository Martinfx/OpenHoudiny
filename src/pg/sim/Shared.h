#pragma once
//
// What the solvers share (Pyro.h, Liquid.h): loops over the cells of a grid
// in parallel, noise that is the same on every machine, the forces of a
// scene acting on the velocity of a MAC grid, and the checks that keep the
// numbers of a scene in range.
//
// Internal to the solvers: namespace detail.
//
#include "pg/core/Parallel.h"
#include "pg/sim/Grid.h"
#include "pg/sim/Scene.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace pg::sim::detail {

// --- numbers in range (Scene.cpp) --------------------------------------------------

/// `v` in [lo, hi]; `fallback` when it is not a number at all.
float fix(float v, float lo, float hi, float fallback);
Vec3 fix(const Vec3& v, float lo, float hi, const Vec3& fallback);
/// A direction: finite and not zero, else the fallback.
Vec3 fixDirection(const Vec3& v, const Vec3& fallback);
/// Every force and every collider with its numbers in range, as Scene::sanitized().
void sanitize(std::vector<Force>& forces);
void sanitize(std::vector<Collider>& colliders);

// --- loops and noise -----------------------------------------------------------------

/// f(i, j, k) for every cell of a box of cells [x0, x1) x [y0, y1) x [z0, z1),
/// in parallel. The rows are split into chunks of some 8k cells -- enough work
/// that handing a chunk to a thread costs little. Every cell is visited by
/// exactly one chunk and computed the same whichever it is, so the split
/// changes how the work spreads over threads, never a result.
template <class F>
void forEachIn(int x0, int x1, int y0, int y1, int z0, int z1, const F& f) {
    if (x0 >= x1 || y0 >= y1 || z0 >= z1) return;
    const size_t ny = static_cast<size_t>(y1 - y0);
    const size_t rows = ny * static_cast<size_t>(z1 - z0);
    const size_t grain = std::max<size_t>(1, 8192 / static_cast<size_t>(x1 - x0));
    pg::parallelFor(rows, grain, [&](size_t begin, size_t end) {
        for (size_t r = begin; r < end; ++r) {
            const int j = y0 + static_cast<int>(r % ny);
            const int k = z0 + static_cast<int>(r / ny);
            for (int i = x0; i < x1; ++i) f(i, j, k);
        }
    });
}

template <class F>
void forEachCell(const Grid& g, const F& f) {
    forEachIn(0, g.nx(), 0, g.ny(), 0, g.nz(), f);
}

inline uint32_t hash(int x, int y, int z, uint32_t seed) {
    uint32_t h = seed * 0x9E3779B9u + 0x7F4A7C15u;
    h ^= static_cast<uint32_t>(x) * 0x85EBCA6Bu;
    h = (h << 13) | (h >> 19);
    h ^= static_cast<uint32_t>(y) * 0xC2B2AE35u;
    h = (h << 13) | (h >> 19);
    h ^= static_cast<uint32_t>(z) * 0x27D4EB2Fu;
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    h *= 0x846CA68Bu;
    h ^= h >> 16;
    return h;
}

/// A number in [0, 1] for each point of the integer lattice.
inline float lattice(int x, int y, int z, uint32_t seed) {
    return static_cast<float>(hash(x, y, z, seed) & 0xFFFFFFu) / static_cast<float>(0xFFFFFFu);
}

inline float smoothstep(float edge0, float edge1, float x) {
    const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

/// Smooth value noise in [0, 1]: what makes sources flicker.
inline float noise3(float x, float y, float z, uint32_t seed) {
    const float fx = std::floor(x), fy = std::floor(y), fz = std::floor(z);
    const int ix = static_cast<int>(fx), iy = static_cast<int>(fy), iz = static_cast<int>(fz);
    auto fade = [](float t) { return t * t * (3.0f - 2.0f * t); };
    const float tx = fade(x - fx), ty = fade(y - fy), tz = fade(z - fz);
    auto l = [&](int a, int b, int c) { return lattice(ix + a, iy + b, iz + c, seed); };
    auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
    const float y0 = lerp(lerp(l(0, 0, 0), l(1, 0, 0), tx), lerp(l(0, 1, 0), l(1, 1, 0), tx), ty);
    const float y1 = lerp(lerp(l(0, 0, 1), l(1, 0, 1), tx), lerp(l(0, 1, 1), l(1, 1, 1), tx), ty);
    return lerp(y0, y1, tz);
}

/// Smooth noise over time alone, in [0, 1]: gusts.
inline float noise1(float t, uint32_t seed) {
    const float f = std::floor(t);
    const int i = static_cast<int>(f);
    const float u = smoothstep(0.0f, 1.0f, t - f);
    const float a = lattice(i, 0, 0, seed), b = lattice(i + 1, 0, 0, seed);
    return a + (b - a) * u;
}

/// Position of sample (0, 0, 0) of velocity component `axis` along axis `a`,
/// in cell units: the faces sit on the cell borders along their own axis.
inline float faceOffset(int axis, int a) { return axis == a ? 0.0f : 0.5f; }

// --- forces --------------------------------------------------------------------------

/// How fast a wind of `force` blows at time t at p: its speed, varied by
/// its gusts -- fronts that sweep through with the wind, so that what is
/// downwind feels a gust a little later.
inline Vec3 windAt(const Force& force, float t, uint32_t seed, const Vec3& p) {
    const Vec3 d = normalize(force.direction);
    float gust = 1.0f;
    if (force.gusts > 0.0f) {
        const float late = dot(p, d) / std::max(force.speed, 0.5f);
        gust += force.gusts * (2.0f * noise1((t - late) * 0.8f, seed) - 1.0f);
    }
    return d * (force.speed * gust);
}

/// Adds `force` over dt at time t to the velocity `vel` (the three
/// components, each on its faces) of a MAC grid over `domain`. `seed` makes
/// the force's noise; `knots` keeps a turbulence force's lattice from step to
/// step. `mask(a, i, j, k)`: how much the force acts at face (i, j, k) of
/// component a, 0 to 1.
template <class Mask>
void addForce(const Force& force, uint32_t seed, const Domain& domain, float time, float dt, Grid* vel,
              std::array<Grid, 3>& knots, const Mask& mask) {
    const Vec3 o = domain.origin();
    const float h = domain.voxel;
    const int nx = domain.cells[0], ny = domain.cells[1], nz = domain.cells[2];
    // World position of face (i, j, k) of component a.
    auto facePosition = [&](int a, int i, int j, int k) {
        const float x = static_cast<float>(i) + faceOffset(a, 0), y = static_cast<float>(j) + faceOffset(a, 1),
                    z = static_cast<float>(k) + faceOffset(a, 2);
        return Vec3(o.x + x * h, o.y + y * h, o.z + z * h);
    };

    switch (force.kind) {
        case ForceKind::Turbulence: {
            // A random force on a coarse lattice, a knot every `scale`, that
            // changes smoothly `speed` times a second and is interpolated to
            // the faces. Random, so it pushes together here and apart there;
            // the projection that follows keeps only its swirling part.
            const float cellsPerKnot = std::max(force.scale / h, 2.0f);
            const int kx = static_cast<int>(std::ceil(static_cast<float>(nx) / cellsPerKnot)) + 2;
            const int ky = static_cast<int>(std::ceil(static_cast<float>(ny) / cellsPerKnot)) + 2;
            const int kz = static_cast<int>(std::ceil(static_cast<float>(nz) / cellsPerKnot)) + 2;
            const float clock = time * force.speed;
            const int slice = static_cast<int>(std::floor(clock));
            const float blend = smoothstep(0.0f, 1.0f, clock - static_cast<float>(slice));
            for (int a = 0; a < 3; ++a) {
                Grid& g = knots[static_cast<size_t>(a)];
                if (g.nx() != kx || g.ny() != ky || g.nz() != kz) g = Grid(kx, ky, kz);
                const uint32_t s = seed + 104729u * static_cast<uint32_t>(a + 1);
                for (int k = 0; k < kz; ++k) {
                    for (int j = 0; j < ky; ++j) {
                        for (int i = 0; i < kx; ++i) {
                            const float now = lattice(i, j, k, s + static_cast<uint32_t>(slice) * 31u);
                            const float next = lattice(i, j, k, s + static_cast<uint32_t>(slice + 1) * 31u);
                            g.at(i, j, k) = 2.0f * (now + (next - now) * blend) - 1.0f;
                        }
                    }
                }
            }
            for (int a = 0; a < 3; ++a) {
                const float ox = faceOffset(a, 0), oy = faceOffset(a, 1), oz = faceOffset(a, 2);
                const Grid& g = knots[static_cast<size_t>(a)];
                forEachCell(vel[a], [&](int i, int j, int k) {
                    const float m = mask(a, i, j, k);
                    if (m <= 0.0f) return;
                    const float push = g.sample((static_cast<float>(i) + ox) / cellsPerKnot + 0.5f,
                                                (static_cast<float>(j) + oy) / cellsPerKnot + 0.5f,
                                                (static_cast<float>(k) + oz) / cellsPerKnot + 0.5f);
                    vel[a].at(i, j, k) += dt * force.strength * m * push;
                });
            }
            break;
        }
        case ForceKind::Wind: {
            // Pulled towards the wind's velocity, `strength` per second; gusts
            // sweep through with it.
            const float pull = 1.0f - std::exp(-force.strength * dt);
            for (int a = 0; a < 3; ++a) {
                forEachCell(vel[a], [&](int i, int j, int k) {
                    const float m = mask(a, i, j, k);
                    const float wind = windAt(force, time, seed, facePosition(a, i, j, k))[a];
                    float& v = vel[a].at(i, j, k);
                    v += pull * m * (wind - v);
                });
            }
            break;
        }
        case ForceKind::Vortex: {
            // Inside a cylinder round the axis -- `radius` wide, `height`
            // long -- pulled towards going round the axis at `speed` (fastest
            // halfway out), along it at `lift` and in towards it at `suction`,
            // `strength` per second; eased off at the edges. A pull, not a
            // push: however long it acts, nothing gets faster than those
            // speeds -- except by the pressure it builds up.
            const Vec3 axis = normalize(force.direction);
            const float pull = 1.0f - std::exp(-force.strength * dt);
            const float half = 0.5f * force.height;
            for (int a = 0; a < 3; ++a) {
                forEachCell(vel[a], [&](int i, int j, int k) {
                    const Vec3 r = facePosition(a, i, j, k) - force.center;
                    const float along = dot(r, axis);
                    if (half > 0.0f && std::fabs(along) >= half) return;
                    const Vec3 out = r - axis * along;
                    const float d = length(out);
                    if (d >= force.radius) return;
                    const float x = d / force.radius;
                    float w = 1.0f - smoothstep(0.75f, 1.0f, x);
                    if (half > 0.0f) w *= 1.0f - smoothstep(0.75f, 1.0f, std::fabs(along) / half);
                    const Vec3 outward = d > 1e-6f ? out * (1.0f / d) : Vec3();
                    const Vec3 target = cross(axis, outward) * (force.speed * 4.0f * x * (1.0f - x)) +
                                        axis * force.lift - outward * (force.suction * x);
                    float& v = vel[a].at(i, j, k);
                    v += pull * w * mask(a, i, j, k) * (target[a] - v);
                });
            }
            break;
        }
        case ForceKind::Attractor: {
            for (int a = 0; a < 3; ++a) {
                forEachCell(vel[a], [&](int i, int j, int k) {
                    const Vec3 r = force.center - facePosition(a, i, j, k);
                    const float d = length(r);
                    if (d < 1e-6f || d >= force.radius) return;
                    const float falloff = (1.0f - d / force.radius) * (1.0f - d / force.radius);
                    vel[a].at(i, j, k) += dt * force.strength * falloff * mask(a, i, j, k) * r[a] / d;
                });
            }
            break;
        }
        case ForceKind::Drag: {
            const float keep = std::exp(-force.strength * dt);
            for (int a = 0; a < 3; ++a) {
                forEachCell(vel[a], [&](int i, int j, int k) {
                    const float m = mask(a, i, j, k);
                    vel[a].at(i, j, k) *= 1.0f - (1.0f - keep) * m;
                });
            }
            break;
        }
    }
}

}  // namespace pg::sim::detail
