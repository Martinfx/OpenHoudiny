#include "pg/render/Particles.h"

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
float random(uint32_t seed, uint32_t k) {
    uint32_t h = seed + k * 0x9e3779b9u;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return static_cast<float>(h >> 8) / 16777216.0f;
}

/// A way, any way as likely as any other.
Vec3 anyWay(float u, float v) {
    const float z = 2.0f * u - 1.0f, r = std::sqrt(std::max(0.0f, 1.0f - z * z)), phi = 2.0f * kPi * v;
    return {r * std::cos(phi), r * std::sin(phi), z};
}

/// A face of a solid: its corners round the way it faces, counter-clockwise
/// seen from outside.
using Polygon = std::vector<Vec3>;

/// The faces of a box `h` from its middle each way.
std::vector<Polygon> boxFaces(const Vec3& h) {
    auto corner = [&](int i) { return Vec3(i & 1 ? h.x : -h.x, i & 2 ? h.y : -h.y, i & 4 ? h.z : -h.z); };
    const int faces[6][4] = {{0, 4, 6, 2}, {1, 3, 7, 5}, {0, 1, 5, 4}, {2, 6, 7, 3}, {0, 2, 3, 1}, {4, 5, 7, 6}};
    std::vector<Polygon> out;
    for (const auto& f : faces) out.push_back({corner(f[0]), corner(f[1]), corner(f[2]), corner(f[3])});
    return out;
}

/// `p` without the corners that are where the one before them is.
void dropRepeats(Polygon& p) {
    Polygon kept;
    for (const Vec3& c : p) {
        if (kept.empty() || length(c - kept.back()) > 1e-6f) kept.push_back(c);
    }
    while (kept.size() > 1 && length(kept.front() - kept.back()) <= 1e-6f) kept.pop_back();
    p = std::move(kept);
}

/// What of a convex solid's `faces` is on the near side of the plane where
/// dot(n, p) = d: the faces it cuts cut, the cut a face of its own.
void clip(std::vector<Polygon>& faces, const Vec3& n, float d) {
    std::vector<Polygon> out;
    std::vector<Vec3> cut;
    for (const Polygon& f : faces) {
        Polygon kept;
        for (size_t i = 0; i < f.size(); ++i) {
            const Vec3& a = f[i];
            const Vec3& b = f[(i + 1) % f.size()];
            const float da = dot(n, a) - d, db = dot(n, b) - d;
            if (da <= 0.0f) kept.push_back(a);
            if ((da <= 0.0f) != (db <= 0.0f)) {
                const Vec3 x = a + (b - a) * (da / (da - db));
                kept.push_back(x);
                cut.push_back(x);
            }
        }
        dropRepeats(kept);
        if (kept.size() >= 3) out.push_back(std::move(kept));
    }
    // The cut's corners, each once, round the plane's normal.
    Polygon corners;
    for (const Vec3& p : cut) {
        if (std::none_of(corners.begin(), corners.end(), [&](const Vec3& q) { return length(p - q) <= 1e-5f; })) {
            corners.push_back(p);
        }
    }
    if (corners.size() >= 3) {
        Vec3 mid(0.0f);
        for (const Vec3& p : corners) mid += p;
        mid /= static_cast<float>(corners.size());
        const Vec3 u = normalize(cross(n, std::fabs(n.x) < 0.9f ? Vec3(1.0f, 0.0f, 0.0f) : Vec3(0.0f, 1.0f, 0.0f)));
        const Vec3 v = cross(n, u);
        std::sort(corners.begin(), corners.end(), [&](const Vec3& a, const Vec3& b) {
            return std::atan2(dot(a - mid, v), dot(a - mid, u)) < std::atan2(dot(b - mid, v), dot(b - mid, u));
        });
        out.push_back(std::move(corners));
    }
    faces = std::move(out);
}

/// A chip of stone: a box flattened and stretched, six to nine of its
/// corners and edges broken off -- each plane cutting the chip as it is
/// by then, somewhere between a third and a tenth of the way in.
std::vector<Polygon> stone(uint32_t seed) {
    std::vector<Polygon> faces = boxFaces(Vec3(1.0f, 0.55f + 0.3f * random(seed, 1), 0.3f + 0.25f * random(seed, 2)));
    const int breaks = 6 + static_cast<int>(random(seed, 3) * 4.0f);
    for (int k = 0; k < breaks; ++k) {
        const uint32_t i = static_cast<uint32_t>(k);
        const Vec3 n = anyWay(random(seed, 10 + 2 * i), random(seed, 11 + 2 * i));
        float reach = 0.0f;
        for (const Polygon& f : faces) {
            for (const Vec3& p : f) reach = std::max(reach, dot(n, p));
        }
        clip(faces, n, reach * (0.6f + 0.3f * random(seed, 40 + i)));
    }
    return faces;
}

/// A sliver of glass: flat, of three to five sides, as thick as a pane
/// is to a hand-sized shard.
std::vector<Polygon> sliver(uint32_t seed) {
    const int sides = 3 + static_cast<int>(random(seed, 1) * 3.0f);
    const float half = 0.025f + 0.02f * random(seed, 2);
    Polygon ring;
    for (int k = 0; k < sides; ++k) {
        const uint32_t i = static_cast<uint32_t>(k);
        const float a = 2.0f * kPi * (static_cast<float>(k) + 0.6f * (random(seed, 10 + i) - 0.5f)) / static_cast<float>(sides);
        const float r = 0.55f + 0.45f * random(seed, 20 + i);
        ring.push_back(Vec3(r * std::cos(a), 0.0f, r * std::sin(a)));
    }
    const Vec3 up(0.0f, half, 0.0f);
    // Round the ring as the angle grows is clockwise seen from above.
    Polygon top, bottom;
    for (size_t k = ring.size(); k-- > 0;) top.push_back(ring[k] + up);
    for (const Vec3& q : ring) bottom.push_back(q - up);
    std::vector<Polygon> faces = {top, bottom};
    for (size_t k = 0; k < ring.size(); ++k) {
        const Vec3& a = ring[k];
        const Vec3& b = ring[(k + 1) % ring.size()];
        faces.push_back({a - up, a + up, b + up, b - up});
    }
    return faces;
}

/// `faces` moved to have their corners' middle at the origin, sized to
/// have the farthest of them a unit from it.
void centred(std::vector<Polygon>& faces) {
    Vec3 mid(0.0f);
    size_t n = 0;
    for (const Polygon& f : faces) {
        for (const Vec3& p : f) {
            mid += p;
            ++n;
        }
    }
    if (n == 0) return;
    mid /= static_cast<float>(n);
    float far = 0.0f;
    for (const Polygon& f : faces) {
        for (const Vec3& p : f) far = std::max(far, length(p - mid));
    }
    const float k = far > 0.0f ? 1.0f / far : 1.0f;
    for (Polygon& f : faces) {
        for (Vec3& p : f) p = (p - mid) * k;
    }
}

/// The mesh of a chip's faces, each its own corners -- flat, its edges
/// sharp -- white, for its placements' tints to colour: broken concrete,
/// laid on as on a chip some 5 cm across, or glass.
std::shared_ptr<const Mesh> chipOf(const std::vector<Polygon>& faces, bool glass, uint32_t seed, RayEngine engine) {
    auto geo = std::make_shared<Geometry>();
    size_t corners = 0;
    for (const Polygon& f : faces) corners += f.size();
    geo->addPoints(corners);
    {
        auto P = geo->positionsForWrite();
        auto cd = geo->points().create("Cd", AttrType::Vec3).write<Vec3>();
        auto rest = geo->points().create("rest", AttrType::Vec3).write<Vec3>();
        // Each shape a patch of the photographs of its own.
        const Vec3 patch(10.0f * random(seed, 50), 10.0f * random(seed, 51), 10.0f * random(seed, 52));
        size_t i = 0;
        for (const Polygon& f : faces) {
            for (const Vec3& p : f) {
                P[i] = p;
                cd[i] = Vec3(1.0f);
                rest[i] = p * 0.025f + patch;
                ++i;
            }
        }
    }
    uint32_t first = 0;
    for (const Polygon& f : faces) {
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
    std::vector<Polygon> faces = glass ? sliver(seed) : stone(seed);
    centred(faces);
    std::shared_ptr<const Mesh> mesh = chipOf(faces, glass, seed, engine);
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
        const size_t shapes = glass ? kSliverShapes : kChipShapes;
        const size_t shape = std::min(static_cast<size_t>(random(seed, 0) * static_cast<float>(shapes)), shapes - 1);
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
        Quat q;
        if (orients) {
            q = quatOf(orients->read<Vec4>()[i]);
        } else {
            const float u1 = random(seed, 60), u2 = 2.0f * kPi * random(seed, 61), u3 = 2.0f * kPi * random(seed, 62);
            const float a = std::sqrt(1.0f - u1), b = std::sqrt(u1);
            q = Quat::wxyz(b * std::cos(u3), a * std::sin(u2), a * std::cos(u2), b * std::sin(u3));
        }
        const float len = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
        p.axes = len > 1e-12f ? glm::mat3_cast(q * (1.0f / len)) : Mat3(1.0f);
        // Its colour -- stones are not all of one: lighter and darker, some greyer.
        Vec3 c = colors ? colors->read<Vec3>()[i] : Vec3(0.5f);
        if (!glass) {
            const float grey = dot(c, Vec3(0.3f, 0.5f, 0.2f));
            c = (c + (Vec3(grey) - c) * (0.4f * random(seed, 40))) * (0.7f + 0.55f * random(seed, 41));
        }
        p.tint = c;
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
