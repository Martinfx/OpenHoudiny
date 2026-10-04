#include "pg/render/Particles.h"

#include "pg/core/Chips.h"
#include "pg/core/Geometry.h"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <mutex>
#include <numeric>
#include <tuple>

namespace pg::render {
namespace {

constexpr float kPi = 3.14159265358979f;

/// How thick a drop is drawn, metres across: 2.5 mm, as the viewport has it;
/// a droplet of a splash half that, its streak 0.6 as long.
constexpr float kDrop = 0.0025f;
/// Drops nearer the eye than this are left out: just in front of the lens,
/// one would be out of focus -- a blur, faint -- not a sharp streak.
constexpr float kNearest = 0.5f;

/// The `k`-th number of `seed`, 0 to 1 -- as the viewport's chips have theirs.
float random(uint32_t seed, uint32_t k) { return chipRandom(seed, k); }

/// The mesh of a chip's faces, each its own corners -- flat, its edges
/// sharp -- white, for its placements' tints to colour: broken concrete,
/// laid on as on a chip some 5 cm across, or glass.
std::shared_ptr<const Mesh> chipOf(const std::vector<ChipFace>& faces, bool glass, uint32_t seed, RayEngine engine) {
    auto geo = std::make_shared<Geometry>();
    size_t corners = 0;
    for (const ChipFace& f : faces) corners += f.size();
    geo->addPoints(corners);
    {
        auto P = geo->positionsForWrite();
        auto cd = geo->points().create("Cd", AttrType::Vec3).write<Vec3>();
        auto rest = geo->points().create("rest", AttrType::Vec3).write<Vec3>();
        // Each shape a patch of the photographs of its own.
        const Vec3 patch(10.0f * random(seed, 50), 10.0f * random(seed, 51), 10.0f * random(seed, 52));
        size_t i = 0;
        for (const ChipFace& f : faces) {
            for (const Vec3& p : f) {
                P[i] = p;
                cd[i] = Vec3(1.0f);
                rest[i] = p * 0.025f + patch;
                ++i;
            }
        }
    }
    uint32_t first = 0;
    for (const ChipFace& f : faces) {
        std::vector<uint32_t> ids(f.size());
        std::iota(ids.begin(), ids.end(), first);
        geo->addPrimitive(ids, true);
        first += static_cast<uint32_t>(f.size());
    }
    if (glass) {
        auto g = geo->primitives().create("glass", AttrType::Int).write<int32_t>();
        std::fill(g.begin(), g.end(), 1);
    } else {
        setPrimitiveString(*geo, "material", std::string(materialName(MaterialPreset::BrokenConcrete)));
    }
    return meshOf(*geo, false, engine);
}

}  // namespace

std::shared_ptr<const Mesh> chipMesh(size_t shape, bool glass, RayEngine engine) {
    static std::mutex mutex;
    // Made once and kept as long as the program runs: never let go, so that
    // no Embree scene of theirs outlives what releases it at exit.
    static auto* made = new std::map<std::tuple<int, bool, size_t>, std::shared_ptr<const Mesh>>();
    const size_t which = shape % (glass ? kSliverShapes : kChipShapes);
    const auto key = std::make_tuple(static_cast<int>(engine), glass, which);
    std::lock_guard<std::mutex> lock(mutex);
    if (auto it = made->find(key); it != made->end()) return it->second;
    const uint32_t seed = static_cast<uint32_t>(which) * 7919u + (glass ? 104729u : 15485863u);
    std::shared_ptr<const Mesh> mesh = chipOf(chipFaces(which, glass), glass, seed, engine);
    (*made)[key] = mesh;
    return mesh;
}

size_t placeChips(const Geometry& geo, Scene& scene) {
    const AttributeArray* scales = geo.points().find("pscale");
    if (!scales || scales->type() != AttrType::Float) return 0;
    const size_t n = geo.pointCount();
    // The points no primitive has: the loose ones.
    std::vector<uint8_t> used(n, 0);
    for (size_t p = 0; p < geo.primitiveCount(); ++p) {
        for (const uint32_t pt : geo.primitivePoints(p)) {
            if (pt < n) used[pt] = 1;
        }
    }
    auto of = [&](const char* name, AttrType type) {
        const AttributeArray* a = geo.points().find(name);
        return a && a->type() == type && a->size() == n ? a : nullptr;
    };
    const AttributeArray* colors = of("Cd", AttrType::Vec3);
    const AttributeArray* orients = of("orient", AttrType::Vec4);
    const AttributeArray* ids = of("id", AttrType::Int);
    const AttributeArray* glassy = of("glass", AttrType::Int);
    const AttributeArray* velocities = of("v", AttrType::Vec3);
    const auto P = geo.positions();
    const auto size = scales->read<float>();
    // Each shape's mesh among the scene's, once it is wanted.
    std::array<int64_t, kChipShapes> stones;
    std::array<int64_t, kSliverShapes> slivers;
    stones.fill(-1);
    slivers.fill(-1);
    size_t count = 0;
    for (size_t i = 0; i < n; ++i) {
        if (used[i] || !(size[i] > 0.0f)) continue;
        const uint32_t seed = ids ? static_cast<uint32_t>(ids->read<int32_t>()[i]) : static_cast<uint32_t>(i);
        const bool glass = glassy && glassy->read<int32_t>()[i] != 0;
        const size_t shape = chipShapeOf(seed, glass);
        int64_t& mesh = glass ? slivers[shape] : stones[shape];
        if (mesh < 0) {
            mesh = static_cast<int64_t>(scene.meshes.size());
            scene.meshes.push_back(chipMesh(shape, glass, scene.engine));
        }
        Placed p;
        p.mesh = static_cast<uint32_t>(mesh);
        p.at = P[i];
        p.scale = size[i];
        if (velocities) p.velocity = velocities->read<Vec3>()[i];
        // Turned as it tumbles; without its orient, a turn of its own.
        const Quat q = quatOf(orients ? orients->read<Vec4>()[i] : chipTurn(seed));
        const float len = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
        p.axes = len > 1e-12f ? glm::mat3_cast(q * (1.0f / len)) : Mat3(1.0f);
        // Its colour -- stones are not all of one: lighter and darker, some greyer.
        p.tint = chipTint(colors ? colors->read<Vec3>()[i] : Vec3(0.5f), seed, glass);
        scene.placed.push_back(p);
        ++count;
    }
    return count;
}

std::shared_ptr<const Mesh> rainMesh(const sim::RainFrame& rain, const sim::Look& look, RayEngine engine,
                                     const Vec3* eye) {
    const size_t drops = rain.dropCount(), droplets = rain.dropletCount();
    if (drops + droplets == 0) return nullptr;
    const float streak = std::max(look.rainStreak, 0.0f) * std::max(rain.timeStep, 0.0f);
    std::vector<Vec3> corners, normals;
    corners.reserve(24 * (drops + droplets));
    normals.reserve(24 * (drops + droplets));
    auto add = [&](const std::vector<float>& from, size_t count, float radius, float share) {
        for (size_t i = 0; i < count; ++i) {
            const float* q = from.data() + 6 * i;
            const Vec3 head(q[0], q[1], q[2]), v(q[3], q[4], q[5]);
            if (eye && length(head - *eye) < kNearest) continue;
            const float speed = length(v);
            // From the head back to the tail, as far as it fell in the share
            // of a frame -- a drop barely moving a little ball.
            const Vec3 back = speed > 1e-6f ? v * (-1.0f / speed) : Vec3(0.0f, 1.0f, 0.0f);
            const float len = std::max(speed * streak * share, 2.0f * radius);
            const Vec3 tail = head + back * len, tip = head - back * radius;
            const Vec3 side = normalize(cross(back, std::fabs(back.y) < 0.9f ? Vec3(0.0f, 1.0f, 0.0f) : Vec3(1.0f, 0.0f, 0.0f)));
            const Vec3 other = cross(back, side);
            Vec3 ring[4], out[4];
            for (int k = 0; k < 4; ++k) {
                const float a = 0.5f * kPi * static_cast<float>(k);
                out[k] = side * std::cos(a) + other * std::sin(a);
                ring[k] = head + out[k] * radius;
            }
            for (int k = 0; k < 4; ++k) {
                const int j = (k + 1) % 4;
                const Vec3 mid = normalize(out[k] + out[j]);
                // Back to the tail, thinning to nothing; and round the head.
                corners.insert(corners.end(), {ring[k], ring[j], tail, ring[j], ring[k], tip});
                normals.insert(normals.end(), {out[k], out[j], mid, out[j], out[k], mid});
            }
        }
    };
    add(rain.drops, drops, 0.5f * kDrop, 1.0f);
    add(rain.droplets, droplets, 0.25f * kDrop, 0.6f);
    if (corners.empty()) return nullptr;
    Material m;
    m.kind = Material::Kind::Rain;
    m.roughness = 0.0f;
    m.ior = 1.33f;
    m.opacity = std::clamp(look.rainOpacity, 0.0f, 1.0f);
    return meshOfTriangles(corners, normals, m, look.rainColor, engine);
}

}  // namespace pg::render
