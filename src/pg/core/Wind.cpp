#include "pg/core/Wind.h"

#include "pg/core/Instances.h"

#include <glm/gtx/rotate_vector.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <vector>

namespace pg {
namespace {

constexpr float kPi = 3.14159265358979f;

uint64_t mix(uint64_t a, uint64_t b) {
    uint64_t z = a * 0x9E3779B97F4A7C15ull ^ (b + 0x632BE59BD9B4E019ull) * 0xD6E8FEB86659FD93ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
float unitOf(uint64_t bits) { return static_cast<float>(bits >> 40) / static_cast<float>(1u << 24); }

Vec3 levelOf(const Vec3& v) {
    const Vec3 l(v.x, 0.0f, v.z);
    return length(l) > 1e-6f ? normalize(l) : Vec3(1.0f, 0.0f, 0.0f);
}

/// A geometry's plants: which each point is of (-1 none: no flex), and
/// each one's foot.
struct Plants {
    std::vector<int32_t> of;
    std::vector<Vec3> foot;
    std::vector<uint64_t> key;  // a number of each plant's own: its blade or tree, else its foot
};

Plants plantsOf(const Geometry& geo, std::span<const float> flex) {
    Plants out;
    const size_t points = geo.pointCount();
    out.of.assign(points, -1);
    if (flex.empty()) return out;
    const auto P = geo.positions();
    auto intAttr = [&](const char* name) {
        const AttributeArray* a = geo.primitives().find(name);
        return a && a->type() == AttrType::Int ? a->read<int32_t>() : std::span<const int32_t>();
    };
    const auto blades = intAttr("blade"), trees = intAttr("tree");
    const auto& ids = !blades.empty() ? blades : trees;
    std::map<int32_t, int32_t> plantOfId;
    for (size_t prim = 0; prim < geo.primitiveCount(); ++prim) {
        // Not of a plant: what has no flex along it -- the ground merged
        // with the trees, its flex filled with 0.
        float most = 0.0f;
        for (const uint32_t p : geo.primitivePoints(prim)) {
            if (p < points) most = std::max(most, flex[p]);
        }
        if (!(most > 0.0f)) continue;
        const int32_t id = ids.empty() ? 0 : ids[prim];
        auto [it, added] = plantOfId.emplace(id, static_cast<int32_t>(out.foot.size()));
        if (added) {
            out.foot.push_back(Vec3(0.0f));
            out.key.push_back(static_cast<uint64_t>(static_cast<uint32_t>(id)));
        }
        for (const uint32_t p : geo.primitivePoints(prim)) {
            if (p < points && out.of[p] < 0) out.of[p] = it->second;
        }
    }
    // Loose points -- a skeleton's leaves -- of the one plant, if one.
    if (ids.empty()) {
        if (out.foot.empty()) {
            out.foot.push_back(Vec3(0.0f));
            out.key.push_back(0);
        }
        for (size_t p = 0; p < points; ++p) {
            if (out.of[p] < 0 && flex[p] > 0.0f) out.of[p] = 0;
        }
    }
    // Each foot: the point of the least flex.
    std::vector<float> least(out.foot.size(), 1e30f);
    for (size_t p = 0; p < points; ++p) {
        const int32_t k = out.of[p];
        if (k >= 0 && flex[p] < least[static_cast<size_t>(k)]) {
            least[static_cast<size_t>(k)] = flex[p];
            out.foot[static_cast<size_t>(k)] = P[p];
        }
    }
    return out;
}

/// The flex each point is bowed by: its own -- but a leaf's points all
/// by its stalk's, so that a leaf turns whole, not bent across itself.
std::vector<float> bowingFlex(const Geometry& geo, std::span<const float> flex) {
    std::vector<float> out(flex.begin(), flex.end());
    const AttributeArray* levelAttr = geo.primitives().find("level");
    if (!levelAttr || levelAttr->type() != AttrType::Int) return out;
    const auto level = levelAttr->read<int32_t>();
    for (size_t prim = 0; prim < geo.primitiveCount(); ++prim) {
        if (level[prim] >= 0) continue;
        const auto pts = geo.primitivePoints(prim);
        if (pts.empty() || pts[0] >= out.size()) continue;
        const float stalk = flex[pts[0]];
        for (const uint32_t q : pts) {
            if (q < out.size()) out[q] = stalk;
        }
    }
    return out;
}

/// `p` of flex `f` turned about `foot` by the bend `bend` (level: the way,
/// its length the radians at flex 1) -- stiff at the foot, f^2 of it --
/// and the way it faces, `n`, turned with it.
Vec3 bowed(const Vec3& p, const Vec3& foot, float f, const Vec3& bend, Vec3* n = nullptr) {
    const float angle = length(bend) * f * f;
    if (angle < 1e-7f) return p;
    const Vec3 axis = normalize(cross(Vec3(0.0f, 1.0f, 0.0f), bend));
    if (n) *n = glm::rotate(*n, angle, axis);
    return foot + glm::rotate(p - foot, angle, axis);
}

/// Where the points of `geo` are, bent at `time` -- and, where `normals`
/// is not empty, the way they face, turned with them.
std::vector<Vec3> blown(const Geometry& geo, const Plants& plants, std::span<const float> flex, const WindSettings& s,
                        float time, std::span<const Vec3> from, std::vector<Vec3>& normals) {
    std::vector<Vec3> out(from.begin(), from.end());
    const bool turnN = !normals.empty();
    std::vector<Vec3> bend(plants.foot.size());
    for (size_t k = 0; k < bend.size(); ++k) bend[k] = windBend(s, plants.foot[k], time, plants.key[k] ^ s.seed);
    // Blades toss at their tips: as much again as flutter says, each in its own time.
    const bool blades = geo.primitives().find("blade") != nullptr;
    const float spin = 2.0f * kPi * s.flutterSpeed * time;
    for (size_t p = 0; p < out.size(); ++p) {
        const int32_t k = plants.of[p];
        if (k < 0) continue;
        Vec3 b = bend[static_cast<size_t>(k)];
        if (blades && s.flutter > 0.0f) {
            const float phase = 2.0f * kPi * unitOf(mix(plants.key[static_cast<size_t>(k)], s.seed + 7));
            const float strong = std::min(length(b) / std::max(s.strength, 1e-4f), 2.0f);
            b = b + levelOf(b) * (s.flutter * 0.5f * strong * std::sin(spin + phase) * std::max(flex[p] - 0.4f, 0.0f));
        }
        out[p] = bowed(from[p], plants.foot[static_cast<size_t>(k)], flex[p], b, turnN ? &normals[p] : nullptr);
    }
    // Leaves flap about their stalks, across them.
    const AttributeArray* levelAttr = geo.primitives().find("level");
    if (levelAttr && levelAttr->type() == AttrType::Int && s.flutter > 0.0f) {
        const auto level = levelAttr->read<int32_t>();
        for (size_t prim = 0; prim < geo.primitiveCount(); ++prim) {
            if (level[prim] >= 0) continue;
            const auto pts = geo.primitivePoints(prim);
            if (pts.size() < 3) continue;
            const Vec3 base = out[pts[0]];
            Vec3 tip = base;
            for (const uint32_t q : pts) {
                if (length(out[q] - base) > length(tip - base)) tip = out[q];
            }
            const Vec3 along = tip - base;
            const Vec3 axis = cross(along, Vec3(0.0f, 1.0f, 0.0f));
            if (length(axis) < 1e-6f) continue;
            const int32_t k = plants.of[pts[0]];
            const float strong =
                k >= 0 ? std::min(length(bend[static_cast<size_t>(k)]) / std::max(s.strength, 1e-4f), 2.0f) : 1.0f;
            const float phase = 2.0f * kPi * unitOf(mix(prim, s.seed + 11));
            const float angle = s.flutter * strong * std::sin(spin * (0.8f + 0.4f * unitOf(mix(prim, s.seed + 13))) + phase);
            const Vec3 u = normalize(axis);
            for (const uint32_t q : pts) {
                out[q] = base + glm::rotate(out[q] - base, angle, u);
                if (turnN) normals[q] = glm::rotate(normals[q], angle, u);
            }
        }
    }
    return out;
}

}  // namespace

Vec3 windBend(const WindSettings& s, const Vec3& at, float time, uint64_t plant) {
    const Vec3 dir = levelOf(s.direction);
    const Vec3 across(-dir.z, 0.0f, dir.x);
    // Gusts: waves along the wind, a little crooked across it.
    const float k = 2.0f * kPi / std::max(s.gustSize, 1e-3f);
    const float along = dot(at, dir), side = dot(at, across);
    const float wave = 0.5f + 0.5f * std::sin(k * (along - s.gustSpeed * time) + 0.7f * std::sin(0.37f * k * side));
    const float gust = std::clamp(s.gust, 0.0f, 1.0f);
    const float strength = s.strength * ((1.0f - gust) + gust * 2.0f * wave * wave);
    // Each plant wavering of its own: along the wind and across it.
    const float a = 2.0f * kPi * unitOf(mix(plant, 1)), b = 2.0f * kPi * unitOf(mix(plant, 2));
    const float rate = 0.9f + 0.4f * unitOf(mix(plant, 3));
    const float wobble = s.turbulence * std::sin(rate * 2.3f * time + a);
    const float sideways = 0.5f * s.turbulence * std::sin(rate * 1.7f * time + b);
    return dir * (strength * (1.0f + wobble)) + across * (s.strength * sideways);
}

void blowPlants(Geometry& geo, const WindSettings& s, float time) {
    const AttributeArray* flexAttr = geo.points().find("flex");
    if (!flexAttr || flexAttr->type() != AttrType::Float || geo.pointCount() == 0) return;
    // Its own copy: the flex read as P is written.
    const std::vector<float> flex(flexAttr->read<float>().begin(), flexAttr->read<float>().end());
    const Plants plants = plantsOf(geo, flex);
    const std::vector<float> bowing = bowingFlex(geo, flex);
    const std::vector<Vec3> rest(geo.positions().begin(), geo.positions().end());
    const AttributeArray* nAttr = geo.points().find("N");
    std::vector<Vec3> normals;
    if (nAttr && nAttr->type() == AttrType::Vec3) normals.assign(nAttr->read<Vec3>().begin(), nAttr->read<Vec3>().end());
    const std::vector<Vec3> now = blown(geo, plants, bowing, s, time, rest, normals);
    // How fast: a moment on.
    constexpr float dt = 1.0f / 240.0f;
    std::vector<Vec3> none;
    const std::vector<Vec3> next = blown(geo, plants, bowing, s, time + dt, rest, none);
    auto P = geo.positionsForWrite();
    std::copy(now.begin(), now.end(), P.begin());
    if (!normals.empty()) {
        auto N = geo.points().create("N", AttrType::Vec3).write<Vec3>();
        std::copy(normals.begin(), normals.end(), N.begin());
    }
    auto v = geo.points().create("v", AttrType::Vec3).write<Vec3>();
    for (size_t p = 0; p < now.size(); ++p) v[p] = (next[p] - now[p]) / dt;
}

Geometry bentPlant(const Geometry& plant, const Vec3& bend) {
    Geometry out = plant;
    const AttributeArray* flexAttr = plant.points().find("flex");
    if (!flexAttr || flexAttr->type() != AttrType::Float) return out;
    const auto flex = flexAttr->read<float>();
    const Plants plants = plantsOf(plant, flex);
    const std::vector<float> bowing = bowingFlex(plant, flex);
    const Vec3 b(bend.x, 0.0f, bend.z);
    const AttributeArray* nAttr = plant.points().find("N");
    std::vector<Vec3> normals;
    if (nAttr && nAttr->type() == AttrType::Vec3) normals.assign(nAttr->read<Vec3>().begin(), nAttr->read<Vec3>().end());
    auto P = out.positionsForWrite();
    for (size_t p = 0; p < P.size(); ++p) {
        const int32_t k = plants.of[p];
        if (k >= 0) P[p] = bowed(P[p], plants.foot[static_cast<size_t>(k)], bowing[p], b, normals.empty() ? nullptr : &normals[p]);
    }
    if (!normals.empty()) {
        auto N = out.points().create("N", AttrType::Vec3).write<Vec3>();
        std::copy(normals.begin(), normals.end(), N.begin());
    }
    return out;
}

void bowPlants(Geometry& geo, const std::function<Vec3(const Vec3& foot, uint64_t plant)>& bendOf) {
    const AttributeArray* flexAttr = geo.points().find("flex");
    if (!flexAttr || flexAttr->type() != AttrType::Float || geo.pointCount() == 0) return;
    const std::vector<float> flex(flexAttr->read<float>().begin(), flexAttr->read<float>().end());
    const Plants plants = plantsOf(geo, flex);
    const std::vector<float> bowing = bowingFlex(geo, flex);
    std::vector<Vec3> bend(plants.foot.size());
    for (size_t k = 0; k < bend.size(); ++k) bend[k] = bendOf(plants.foot[k], plants.key[k]);
    const AttributeArray* nAttr = geo.points().find("N");
    std::vector<Vec3> normals;
    if (nAttr && nAttr->type() == AttrType::Vec3) normals.assign(nAttr->read<Vec3>().begin(), nAttr->read<Vec3>().end());
    auto P = geo.positionsForWrite();
    for (size_t p = 0; p < P.size(); ++p) {
        const int32_t k = plants.of[p];
        if (k < 0) continue;
        P[p] = bowed(P[p], plants.foot[static_cast<size_t>(k)], bowing[p], bend[static_cast<size_t>(k)],
                     normals.empty() ? nullptr : &normals[p]);
    }
    if (!normals.empty()) {
        auto N = geo.points().create("N", AttrType::Vec3).write<Vec3>();
        std::copy(normals.begin(), normals.end(), N.begin());
    }
}

std::shared_ptr<const Geometry> BentShapes::shape(const std::shared_ptr<const Geometry>& plant, int way, int ways, int step,
                                                  int steps, float most) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (made_.size() > 4096) made_.clear();
    const auto key = std::make_tuple(plant.get(), way, ways, step, steps, most);
    auto it = made_.find(key);
    if (it == made_.end()) {
        const float angle = 2.0f * kPi * static_cast<float>(way) / static_cast<float>(ways);
        const Vec3 bend = Vec3(std::cos(angle), 0.0f, std::sin(angle)) * (most * static_cast<float>(step) / static_cast<float>(steps));
        it = made_.emplace(key, Entry{plant, std::make_shared<const Geometry>(bentPlant(*plant, bend))}).first;
    }
    return it->second.bent;
}

void bowInstances(Geometry& geo, std::span<const Vec3> bends, int ways, int steps, float most, BentShapes& shapes) {
    if (geo.prototypeCount() == 0) return;
    const AttributeArray* instAttr = geo.points().find("instance");
    if (!instAttr || instAttr->type() != AttrType::Int) return;
    const auto protos = geo.prototypes();  // a copy: shapes are added as it goes
    std::vector<uint8_t> bends_(protos.size(), 0);
    bool any = false;
    for (size_t k = 0; k < protos.size(); ++k) {
        const AttributeArray* flex = protos[k] ? protos[k]->points().find("flex") : nullptr;
        bends_[k] = flex && flex->type() == AttrType::Float ? 1 : 0;
        any = any || bends_[k];
    }
    if (!any) return;
    ways = std::max(ways, 1);
    steps = std::max(steps, 1);
    most = std::max(most, 1e-6f);
    const std::vector<int32_t> instance(instAttr->read<int32_t>().begin(), instAttr->read<int32_t>().end());
    const AttributeArray* nAttr = geo.points().find("N");
    const auto N = nAttr && nAttr->type() == AttrType::Vec3 ? nAttr->read<Vec3>() : std::span<const Vec3>();
    const AttributeArray* oAttr = geo.points().find("orient");
    const bool hadOrient = oAttr && oAttr->type() == AttrType::Vec4;
    std::vector<Vec4> orient(geo.pointCount());
    for (size_t p = 0; p < orient.size(); ++p) {
        orient[p] = hadOrient ? oAttr->read<Vec4>()[p]
                              : !N.empty() && dot(N[p], N[p]) > 1e-12f ? quatUpTo(normalize(N[p])) : Vec4(0.0f, 0.0f, 0.0f, 1.0f);
    }
    const float stepSize = most / static_cast<float>(steps);
    const float wayStep = 2.0f * kPi / static_cast<float>(ways);
    std::map<std::tuple<size_t, int, int>, int32_t> made;  // prototype, way, step -> its index
    std::vector<int32_t> standsFor = instance;
    for (size_t p = 0; p < standsFor.size() && p < bends.size(); ++p) {
        const int32_t k = instance[p];
        if (k < 0 || static_cast<size_t>(k) >= protos.size() || !bends_[static_cast<size_t>(k)]) continue;
        // In the plant's own turn.
        const Vec4 q = orient[p];
        Vec3 local = quatRotate(Vec4(-q.x, -q.y, -q.z, q.w), bends[p]);
        local.y = 0.0f;
        const int step = std::min(static_cast<int>(std::floor(length(local) / stepSize + 0.5f)), steps);
        Vec3 shaped(0.0f);
        if (step > 0) {
            int way = static_cast<int>(std::floor(std::atan2(local.z, local.x) / wayStep + 0.5f));
            way = ((way % ways) + ways) % ways;
            const float angle = static_cast<float>(way) * wayStep;
            shaped = Vec3(std::cos(angle), 0.0f, std::sin(angle)) * (static_cast<float>(step) * stepSize);
            const auto key = std::make_tuple(static_cast<size_t>(k), way, step);
            auto it = made.find(key);
            if (it == made.end()) {
                const auto bent = shapes.shape(protos[static_cast<size_t>(k)], way, ways, step, steps, most);
                it = made.emplace(key, static_cast<int32_t>(geo.addPrototype(bent))).first;
            }
            standsFor[p] = it->second;
        }
        // The rest of the way: the plant tilted from its foot -- about half
        // as far as bowing moves its top.
        const Vec3 rest = local - shaped;
        const float tilt = 0.5f * length(rest);
        if (tilt > 1e-6f) {
            const Vec3 axis = normalize(cross(Vec3(0.0f, 1.0f, 0.0f), rest));
            const float h = std::sin(0.5f * tilt);
            orient[p] = quatMultiply(q, Vec4(axis.x * h, axis.y * h, axis.z * h, std::cos(0.5f * tilt)));
        }
    }
    auto outInstance = geo.points().create("instance", AttrType::Int).write<int32_t>();
    std::copy(standsFor.begin(), standsFor.end(), outInstance.begin());
    auto outOrient = geo.points().create("orient", AttrType::Vec4).write<Vec4>();
    std::copy(orient.begin(), orient.end(), outOrient.begin());
}

}  // namespace pg
