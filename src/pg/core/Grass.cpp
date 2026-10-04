#include "pg/core/Grass.h"

#include <glm/common.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace pg {
namespace {

constexpr float kPi = 3.14159265358979f;

uint64_t splitmix(uint64_t& state) {
    uint64_t z = (state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

/// Numbers of their own for each blade.
struct Random {
    uint64_t state;

    Random(uint64_t seed, uint64_t blade)
        : state(seed * 0x9E3779B97F4A7C15ull ^ (blade + 0x632BE59BD9B4E019ull) * 0xD6E8FEB86659FD93ull) {
        splitmix(state);
    }
    float unit() { return static_cast<float>(splitmix(state) >> 40) / static_cast<float>(1u << 24); }  // [0, 1)
    float centred() { return 2.0f * unit() - 1.0f; }                                                  // [-1, 1)
};

}  // namespace

Geometry growGrassClump(const GrassSettings& s, uint64_t seed) {
    Geometry geo;
    const int blades = std::clamp(s.blades, 0, 10000);
    const int segments = std::clamp(s.segments, 1, 16);
    const size_t perBlade = 2 * static_cast<size_t>(segments) + 1;
    if (blades == 0 || s.height <= 0.0f) return geo;
    const size_t count = static_cast<size_t>(blades) * perBlade;
    geo.addPoints(count);
    auto P = geo.positionsForWrite();
    auto Cd = geo.points().create("Cd", AttrType::Vec3).write<Vec3>();
    auto flex = geo.points().create("flex", AttrType::Float).write<float>();
    std::vector<uint32_t> corners, sizes;
    std::vector<Vec3> uvs;  // each corner's: across the blade, up it
    std::vector<int32_t> blade;
    corners.reserve(static_cast<size_t>(blades) * (4 * static_cast<size_t>(segments) - 1));
    uvs.reserve(corners.capacity());
    const float perSegment = 1.0f / static_cast<float>(segments);

    // The roots a little in the ground: on a slope the clump stands up
    // straight, its uphill side no deeper than its downhill side is high.
    const float spread = std::max(s.spread, 0.0f);
    const float sink = std::min(0.6f * spread, 0.2f * s.height);
    for (int b = 0; b < blades; ++b) {
        Random rng(seed, static_cast<uint64_t>(b));
        // Where it grows from, and which way it leans: out from the middle.
        const float r = spread * std::sqrt(rng.unit()), around = 2.0f * kPi * rng.unit();
        const Vec3 root(r * std::cos(around), -sink, r * std::sin(around));
        const float facing = around + 0.6f * rng.centred();
        const Vec3 out(std::cos(facing), 0.0f, std::sin(facing));
        const Vec3 side(-out.z, 0.0f, out.x);
        const float lean = std::clamp(s.lean, 0.0f, 90.0f) * kPi / 180.0f * (0.3f + 0.7f * rng.unit());
        const float bend = std::clamp(s.bend, 0.0f, 1.0f) * (0.6f + 0.8f * rng.unit());
        const float length = (s.height + sink) * std::max(0.1f, 1.0f + std::clamp(s.heightVariation, 0.0f, 1.0f) * rng.centred());
        const float twist = 0.6f * rng.centred();
        // Its colours: a shade of its own; now and then dry.
        const float shade = 1.0f + 0.5f * std::clamp(s.variation, 0.0f, 1.0f) * rng.centred();
        const bool dry = rng.unit() < s.dry;
        const Vec3 rootColor = (dry ? s.dryColor * 0.7f : s.rootColor) * shade;
        const Vec3 tipColor = (dry ? s.dryColor : s.tipColor) * shade;

        // Up the blade, piece by piece: each leaning out further than the
        // one before -- the more, the higher up.
        const size_t base = static_cast<size_t>(b) * perBlade;
        Vec3 at = root;
        for (int k = 0; k <= segments; ++k) {
            const float t = static_cast<float>(k) / static_cast<float>(segments);
            const float angle = lean + bend * (0.5f * kPi - lean) * std::pow(t, 1.5f);
            const Vec3 along = out * std::sin(angle) + Vec3(0.0f, std::cos(angle), 0.0f);
            const Vec3 colour = glm::mix(rootColor, tipColor, std::pow(t, 0.8f));
            if (k == segments) {
                P[base + 2 * k] = at;
                Cd[base + 2 * k] = colour;
                flex[base + 2 * k] = 1.0f;
            } else {
                // Across it: square to the way it bows, turned about the blade a little more on the way up.
                const Vec3 facingOut = cross(along, side);
                const Vec3 across = side * std::cos(twist * t) + facingOut * std::sin(twist * t);
                // As wide most of the way up, narrowing to a point over the last half.
                const float half = 0.5f * std::max(s.width, 0.0f) * std::sqrt(std::min(1.0f, 2.0f * (1.0f - t)));
                P[base + 2 * k] = at - across * half;
                P[base + 2 * k + 1] = at + across * half;
                Cd[base + 2 * k] = Cd[base + 2 * k + 1] = colour;
                flex[base + 2 * k] = flex[base + 2 * k + 1] = t;
            }
            if (k < segments) {
                // To the next row, along the way halfway between.
                const float tm = (static_cast<float>(k) + 0.5f) / static_cast<float>(segments);
                const float am = lean + bend * (0.5f * kPi - lean) * std::pow(tm, 1.5f);
                at = at + (out * std::sin(am) + Vec3(0.0f, std::cos(am), 0.0f)) * (length / static_cast<float>(segments));
            }
        }
        const uint32_t p0 = static_cast<uint32_t>(base);
        for (int k = 0; k + 1 < segments; ++k) {
            const uint32_t a = p0 + 2 * static_cast<uint32_t>(k);
            corners.insert(corners.end(), {a, a + 1, a + 3, a + 2});
            const float v0 = static_cast<float>(k) * perSegment, v1 = v0 + perSegment;
            uvs.insert(uvs.end(), {Vec3(0.0f, v0, 0.0f), Vec3(1.0f, v0, 0.0f), Vec3(1.0f, v1, 0.0f), Vec3(0.0f, v1, 0.0f)});
            sizes.push_back(4);
            blade.push_back(b);
        }
        const uint32_t last = p0 + 2 * static_cast<uint32_t>(segments - 1);
        corners.insert(corners.end(), {last, last + 1, last + 2});
        const float v0 = 1.0f - perSegment;
        uvs.insert(uvs.end(), {Vec3(0.0f, v0, 0.0f), Vec3(1.0f, v0, 0.0f), Vec3(0.5f, 1.0f, 0.0f)});
        sizes.push_back(3);
        blade.push_back(b);
    }
    const uint8_t closed = 1;
    geo.addPrimitives(corners, sizes, std::span<const uint8_t>(&closed, 1));
    auto outUv = geo.vertices().create("uv", AttrType::Vec3).write<Vec3>();
    std::copy(uvs.begin(), uvs.end(), outUv.begin());
    auto outBlade = geo.primitives().create("blade", AttrType::Int).write<int32_t>();
    std::copy(blade.begin(), blade.end(), outBlade.begin());
    // For a renderer that follows light (render/Scene.h): the blades thin,
    // letting through a share of the light that falls on them.
    auto translucency = geo.primitives().create("translucency", AttrType::Float).write<float>();
    std::fill(translucency.begin(), translucency.end(), 0.35f);
    setPrimitiveString(geo, "material", "grass");
    return geo;
}

}  // namespace pg
