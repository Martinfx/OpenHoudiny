#include "pg/core/Wind.h"

#include "pg/core/Instances.h"
#include "pg/core/Parallel.h"
#include "pg/core/Spatial.h"

#include <glm/gtx/rotate_vector.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>
#include <utility>
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
    // A plant: the primitives of one id one after another, as Tree and
    // Grass make them -- two Trees merged number their trees from 0 alike,
    // a run of another id between them.
    int32_t current = -1, lastId = 0;
    std::map<int32_t, uint64_t> seen;  // how many plants of each id so far
    for (size_t prim = 0; prim < geo.primitiveCount(); ++prim) {
        // Not of a plant: what has no flex along it -- the ground merged
        // with the trees, its flex filled with 0.
        float most = 0.0f;
        for (const uint32_t p : geo.primitivePoints(prim)) {
            if (p < points) most = std::max(most, flex[p]);
        }
        if (!(most > 0.0f)) continue;
        const int32_t id = ids.empty() ? 0 : ids[prim];
        if (current < 0 || id != lastId) {
            current = static_cast<int32_t>(out.foot.size());
            lastId = id;
            out.foot.push_back(Vec3(0.0f));
            // Its number: its id -- another plant of the same id one of its own.
            const uint64_t own = static_cast<uint64_t>(static_cast<uint32_t>(id));
            const uint64_t before = seen[id]++;
            out.key.push_back(before == 0 ? own : mix(own, before));
        }
        for (const uint32_t p : geo.primitivePoints(prim)) {
            if (p < points && out.of[p] < 0) out.of[p] = current;
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

/// Leaves flap about their stalks, across them: each leaf of `geo`
/// (primitive level -1) where `out` has it, as strongly as `strongOf` says
/// of its first point -- and, where `normals` is not empty, the way it
/// faces turned with it.
void flapLeaves(const Geometry& geo, const WindSettings& s, float time, const std::function<float(uint32_t)>& strongOf,
                std::vector<Vec3>& out, std::vector<Vec3>& normals) {
    const AttributeArray* levelAttr = geo.primitives().find("level");
    if (!levelAttr || levelAttr->type() != AttrType::Int || !(s.flutter > 0.0f)) return;
    const bool turnN = !normals.empty();
    const float spin = 2.0f * kPi * s.flutterSpeed * time;
    const auto level = levelAttr->read<int32_t>();
    // Each leaf its own points: apart on any number of threads.
    parallelFor(geo.primitiveCount(), 1024, [&](size_t b, size_t e) {
        for (size_t prim = b; prim < e; ++prim) {
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
            const float strong = strongOf(pts[0]);
            const float phase = 2.0f * kPi * unitOf(mix(prim, s.seed + 11));
            const float angle = s.flutter * strong * std::sin(spin * (0.8f + 0.4f * unitOf(mix(prim, s.seed + 13))) + phase);
            const Vec3 u = normalize(axis);
            for (const uint32_t q : pts) {
                out[q] = base + glm::rotate(out[q] - base, angle, u);
                if (turnN) normals[q] = glm::rotate(normals[q], angle, u);
            }
        }
    });
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
    parallelFor(out.size(), 4096, [&](size_t first, size_t end) {
        for (size_t p = first; p < end; ++p) {
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
    });
    // Leaves flap about their stalks, across them.
    flapLeaves(
        geo, s, time,
        [&](uint32_t p) {
            const int32_t k = plants.of[p];
            return k >= 0 ? std::min(length(bend[static_cast<size_t>(k)]) / std::max(s.strength, 1e-4f), 2.0f) : 1.0f;
        },
        out, normals);
    return out;
}

}  // namespace

namespace {

/// How a plant wavers of its own: two phases and a pace, of its number.
struct Waver {
    float a = 0.0f, b = 0.0f, rate = 1.0f;
};

Waver waverOf(uint64_t plant) {
    return {2.0f * kPi * unitOf(mix(plant, 1)), 2.0f * kPi * unitOf(mix(plant, 2)), 0.9f + 0.4f * unitOf(mix(plant, 3))};
}

Vec3 bendOf(const WindSettings& s, const Vec3& at, float time, const Waver& w) {
    const Vec3 dir = levelOf(s.direction);
    const Vec3 across(-dir.z, 0.0f, dir.x);
    // Gusts: waves along the wind, a little crooked across it.
    const float k = 2.0f * kPi / std::max(s.gustSize, 1e-3f);
    const float along = dot(at, dir), side = dot(at, across);
    const float wave = 0.5f + 0.5f * std::sin(k * (along - s.gustSpeed * time) + 0.7f * std::sin(0.37f * k * side));
    const float gust = std::clamp(s.gust, 0.0f, 1.0f);
    const float strength = s.strength * ((1.0f - gust) + gust * 2.0f * wave * wave);
    // Each plant wavering of its own: along the wind and across it.
    const float wobble = s.turbulence * std::sin(w.rate * 2.3f * time + w.a);
    const float sideways = 0.5f * s.turbulence * std::sin(w.rate * 1.7f * time + w.b);
    return dir * (strength * (1.0f + wobble)) + across * (s.strength * sideways);
}

}  // namespace

Vec3 windBend(const WindSettings& s, const Vec3& at, float time, uint64_t plant) {
    return bendOf(s, at, time, waverOf(plant));
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


// ---- Plants as springs --------------------------------------------------

namespace {

/// The rotation by `v`, its axis times radians (Rodrigues).
Mat3 turnBy(const Vec3& v) {
    const float angle = length(v);
    if (!(angle > 1e-9f)) return Mat3(1.0f);
    const Vec3 k = v / angle;
    const float c = std::cos(angle), s = std::sin(angle), t = 1.0f - c;
    return Mat3(t * k.x * k.x + c, t * k.x * k.y + s * k.z, t * k.x * k.z - s * k.y,  // the columns
                t * k.x * k.y - s * k.z, t * k.y * k.y + c, t * k.y * k.z + s * k.x,
                t * k.x * k.z + s * k.y, t * k.y * k.z - s * k.x, t * k.z * k.z + c);
}

/// How a stem bends along itself: s^2 of its bend at `s` of its length.
float profile(float s) { return s * s; }

/// The Hz a stem `length` metres long sways at.
float swayHz(const SwaySettings& d, float length) {
    const float f = std::max(d.frequency, 0.01f) * std::pow(10.0f / std::max(length, 0.01f), 0.6f);
    return std::min(f, 12.0f);
}

/// The steps' damping, a share of critical, below 1.
float dampingOf(const SwaySettings& d) { return std::clamp(d.damping, 0.0f, 0.95f); }

}  // namespace

void SwayingPlants::clear() {
    stems_.clear();
    plantStart_.clear();
    parentOf_.clear();
    stemOf_.clear();
    along_.clear();
    flexOf_.clear();
    steps_ = -1;
    state_ = State();
    kept_.clear();
    now_.clear();
}

float SwayingPlants::stemFrequency(size_t stem, const SwaySettings& d) const {
    return stem < stems_.size() ? swayHz(d, stems_[stem].length) : 0.0f;
}

void SwayingPlants::build(const Geometry& geo) {
    clear();
    const AttributeArray* flexAttr = geo.points().find("flex");
    if (!flexAttr || flexAttr->type() != AttrType::Float || geo.pointCount() == 0) return;
    const std::vector<float> flex(flexAttr->read<float>().begin(), flexAttr->read<float>().end());
    const Plants plants = plantsOf(geo, flex);
    if (plants.foot.empty()) return;
    const std::vector<float> bowing = bowingFlex(geo, flex);
    const size_t points = geo.pointCount();
    const auto P = geo.positions();
    auto primInts = [&](const char* name) {
        const AttributeArray* a = geo.primitives().find(name);
        return a && a->type() == AttrType::Int ? a->read<int32_t>() : std::span<const int32_t>();
    };
    const auto blades = primInts("blade"), trees = primInts("tree"), stemIds = primInts("stem"),
               parents = primInts("parent"), levels = primInts("level");
    // A tree's stems, each with the one it grows from: Tree's mesh and
    // skeleton. Else each blade, else each plant, one stem.
    const bool hierarchy = blades.empty() && !trees.empty() && !stemIds.empty() && !parents.empty();

    stemOf_.assign(points, -1);
    along_.assign(points, 0.0f);
    flexOf_ = bowing;
    std::vector<uint8_t> wood(points, 0);  // of a stem's own wood, not a leaf on it
    std::vector<Stem> stems;
    std::vector<int32_t> plantOf, idOf, parentIdOf, levelAt;
    if (hierarchy) {
        std::map<std::pair<int32_t, int32_t>, int32_t> index;  // (plant, stem) -> its stem
        std::pair<int32_t, int32_t> lastKey(INT32_MIN, INT32_MIN);
        int32_t last = -1;
        auto stemFor = [&](size_t prim, bool make) -> int32_t {
            const auto pts = geo.primitivePoints(prim);
            const std::pair<int32_t, int32_t> key(plants.of[pts[0]], stemIds[prim]);
            if (key == lastKey) return last;
            auto it = index.find(key);
            if (it == index.end()) {
                if (!make) return -1;
                it = index.emplace(key, static_cast<int32_t>(stems.size())).first;
                stems.emplace_back();
                plantOf.push_back(key.first);
                idOf.push_back(key.second);
                parentIdOf.push_back(parents[prim]);
                levelAt.push_back(levels.empty() ? 0 : levels[prim]);
            }
            lastKey = key;
            last = it->second;
            return last;
        };
        // The wood first -- tubes and polylines -- then the leaves on it.
        for (int pass = 0; pass < 2; ++pass) {
            for (size_t prim = 0; prim < geo.primitiveCount(); ++prim) {
                const bool leaf = !levels.empty() && levels[prim] < 0;
                if (leaf != (pass == 1)) continue;
                const auto pts = geo.primitivePoints(prim);
                if (pts.empty() || pts[0] >= points || plants.of[pts[0]] < 0) continue;  // a root, the ground
                const int32_t st = stemFor(prim, !leaf);
                if (st < 0) continue;
                for (const uint32_t q : pts) {
                    if (q >= points || stemOf_[q] >= 0) continue;
                    stemOf_[q] = st;
                    wood[q] = leaf ? 0 : 1;
                }
            }
        }
    } else {
        // A stem a plant: its points, all its wood.
        stems.resize(plants.foot.size());
        for (size_t k = 0; k < plants.foot.size(); ++k) {
            plantOf.push_back(static_cast<int32_t>(k));
            idOf.push_back(0);
            parentIdOf.push_back(-1);
            levelAt.push_back(0);
        }
        for (size_t p = 0; p < points; ++p) {
            stemOf_[p] = plants.of[p];
            wood[p] = plants.of[p] >= 0 ? 1 : 0;
        }
    }
    if (stems.empty()) {
        clear();
        return;
    }

    // Each stem's wood: its least and most flex, its base and tip there.
    const size_t n = stems.size();
    std::vector<float> lo(n, 1e30f), hi(n, -1e30f);
    for (size_t p = 0; p < points; ++p) {
        const int32_t st = stemOf_[p];
        if (st < 0 || !wood[p]) continue;
        lo[static_cast<size_t>(st)] = std::min(lo[static_cast<size_t>(st)], bowing[p]);
        hi[static_cast<size_t>(st)] = std::max(hi[static_cast<size_t>(st)], bowing[p]);
    }
    std::vector<Vec3> baseSum(n, Vec3(0.0f)), tipSum(n, Vec3(0.0f));
    std::vector<float> baseCount(n, 0.0f), tipCount(n, 0.0f), top(n, -1e30f);
    for (size_t p = 0; p < points; ++p) {
        const int32_t st = stemOf_[p];
        if (st < 0 || !wood[p]) continue;
        const size_t i = static_cast<size_t>(st);
        const float eps = 1e-6f * std::max(1.0f, std::fabs(hi[i]));
        if (bowing[p] <= lo[i] + eps) baseSum[i] += P[p], baseCount[i] += 1.0f;
        if (bowing[p] >= hi[i] - eps) tipSum[i] += P[p], tipCount[i] += 1.0f;
        top[i] = std::max(top[i], P[p].y);
    }
    const Vec3 up(0.0f, 1.0f, 0.0f);
    for (size_t i = 0; i < n; ++i) {
        Stem& st = stems[i];
        if (!(baseCount[i] > 0.0f)) continue;  // no wood: its leaves stay
        st.base = baseSum[i] / baseCount[i];
        const Vec3 tip = tipSum[i] / std::max(tipCount[i], 1.0f);
        if (hierarchy || !blades.empty()) {
            const Vec3 way = tip - st.base;
            st.length = std::max(length(way), 0.01f);
            st.dir = length(way) > 1e-6f ? way / length(way) : up;
        } else {
            // A whole plant stands up, as tall as it reaches.
            st.dir = up;
            st.length = std::max(top[i] - st.base.y, 0.01f);
        }
        const float a = std::max(lo[i], 0.0f), b = std::max(hi[i], a);
        // In a steady wind as far as the bow turns it from its base to its tip.
        st.share = b * b - a * a;
        st.branch = hierarchy && levelAt[i] >= 1;
        st.blade = !blades.empty();
        const int32_t k = plantOf[i];
        const uint64_t plantKey = k >= 0 ? plants.key[static_cast<size_t>(k)] : 0;
        st.key = !hierarchy || parentIdOf[i] < 0 ? plantKey : mix(plantKey, static_cast<uint64_t>(idOf[i]) + 1);
    }
    // Who grows from whom, and where on it.
    if (hierarchy) {
        std::map<std::pair<int32_t, int32_t>, int32_t> index;
        for (size_t i = 0; i < n; ++i) index.emplace(std::make_pair(plantOf[i], idOf[i]), static_cast<int32_t>(i));
        for (size_t i = 0; i < n; ++i) {
            if (parentIdOf[i] < 0) continue;
            const auto it = index.find(std::make_pair(plantOf[i], parentIdOf[i]));
            if (it == index.end() || it->second == static_cast<int32_t>(i)) continue;
            const size_t p = static_cast<size_t>(it->second);
            stems[i].parent = it->second;
            stems[i].attach = hi[p] > lo[p] ? std::clamp((lo[i] - lo[p]) / (hi[p] - lo[p]), 0.0f, 1.0f) : 0.0f;
        }
        // A skeleton's leaves -- points of no primitive -- on the nearest stem.
        std::vector<Vec3> woodAt;
        std::vector<int32_t> woodStem;
        std::vector<uint8_t> used(points, 0);
        for (const uint32_t q : geo.vertexPoints()) {
            if (q < points) used[q] = 1;
        }
        bool loose = false;
        for (size_t p = 0; p < points && !loose; ++p) loose = !used[p] && flex[p] > 0.0f;
        if (loose) {
            for (size_t p = 0; p < points; ++p) {
                if (wood[p] && stemOf_[p] >= 0) woodAt.push_back(P[p]), woodStem.push_back(stemOf_[p]);
            }
            PointTree tree(woodAt);
            for (size_t p = 0; p < points; ++p) {
                if (used[p] || !(flex[p] > 0.0f) || stemOf_[p] >= 0) continue;
                const int32_t q = tree.nearest(P[p]);
                if (q >= 0) stemOf_[p] = woodStem[static_cast<size_t>(q)];
            }
        }
    }
    for (size_t p = 0; p < points; ++p) {
        const int32_t st = stemOf_[p];
        if (st < 0) continue;
        const size_t i = static_cast<size_t>(st);
        if (!(baseCount[i] > 0.0f)) {
            stemOf_[p] = -1;
            continue;
        }
        along_[p] = hi[i] > lo[i] ? std::clamp((bowing[p] - lo[i]) / (hi[i] - lo[i]), 0.0f, 1.0f) : 0.0f;
    }
    finish(stems, plantOf);
}

void SwayingPlants::buildInstances(const Geometry& geo) {
    clear();
    if (geo.prototypeCount() == 0) return;
    const AttributeArray* instAttr = geo.points().find("instance");
    if (!instAttr || instAttr->type() != AttrType::Int) return;
    const auto protos = geo.prototypes();
    // Each plant's height above its foot, the point of the least flex.
    std::vector<float> height(protos.size(), 0.0f);
    for (size_t k = 0; k < protos.size(); ++k) {
        const AttributeArray* f = protos[k] ? protos[k]->points().find("flex") : nullptr;
        if (!f || f->type() != AttrType::Float || protos[k]->pointCount() == 0) continue;
        const auto flex = f->read<float>();
        const auto P = protos[k]->positions();
        size_t foot = 0;
        float top = P[0].y;
        for (size_t p = 0; p < P.size(); ++p) {
            if (flex[p] < flex[foot]) foot = p;
            top = std::max(top, P[p].y);
        }
        height[k] = std::max(top - P[foot].y, 0.05f);
    }
    const auto instance = instAttr->read<int32_t>();
    const auto P = geo.positions();
    const AttributeArray* sAttr = geo.points().find("pscale");
    const auto pscale = sAttr && sAttr->type() == AttrType::Float ? sAttr->read<float>() : std::span<const float>();
    const AttributeArray* idAttr = geo.points().find("id");
    const auto ids = idAttr && idAttr->type() == AttrType::Int ? idAttr->read<int32_t>() : std::span<const int32_t>();
    stemOf_.assign(geo.pointCount(), -1);
    along_.assign(geo.pointCount(), 1.0f);
    std::vector<Stem> stems;
    std::vector<int32_t> plantOf;
    for (size_t p = 0; p < geo.pointCount(); ++p) {
        const int32_t k = instance[p];
        if (k < 0 || static_cast<size_t>(k) >= protos.size() || !(height[static_cast<size_t>(k)] > 0.0f)) continue;
        Stem st;
        st.base = P[p];
        st.dir = Vec3(0.0f, 1.0f, 0.0f);
        st.length = height[static_cast<size_t>(k)] * std::max(pscale.empty() ? 1.0f : pscale[p], 1e-3f);
        // Its own number, as Plant Wind's for the same point.
        const uint64_t own = ids.empty() ? static_cast<uint64_t>(p) : static_cast<uint64_t>(static_cast<uint32_t>(ids[p]));
        st.key = own * 0x9E3779B97F4A7C15ull;
        stemOf_[p] = static_cast<int32_t>(stems.size());
        plantOf.push_back(static_cast<int32_t>(stems.size()));
        stems.push_back(st);
    }
    finish(stems, plantOf);
}

void SwayingPlants::finish(std::vector<Stem>& stems, std::vector<int32_t>& plantOf) {
    // Plant by plant, each its stems parents first: by how deep they are,
    // then as they were found.
    const size_t n = stems.size();
    std::vector<int32_t> depth(n, -1);
    for (size_t i = 0; i < n; ++i) {
        int32_t d = 0;
        for (int32_t at = stems[i].parent; at >= 0 && d <= static_cast<int32_t>(n); at = stems[static_cast<size_t>(at)].parent) {
            ++d;
        }
        if (d > static_cast<int32_t>(n)) stems[i].parent = -1, d = 0;  // round in a ring: none
        depth[i] = d;
    }
    std::vector<uint32_t> order(n);
    std::iota(order.begin(), order.end(), 0u);
    std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
        return std::make_tuple(plantOf[a], depth[a], a) < std::make_tuple(plantOf[b], depth[b], b);
    });
    std::vector<int32_t> newIndex(n, -1);
    for (size_t j = 0; j < n; ++j) newIndex[order[j]] = static_cast<int32_t>(j);
    stems_.resize(n);
    parentOf_.assign(n, -1);
    plantStart_.clear();
    for (size_t j = 0; j < n; ++j) {
        Stem st = stems[order[j]];
        if (st.parent >= 0) st.parent = newIndex[static_cast<size_t>(st.parent)];
        // A parent of another plant is none.
        if (st.parent >= 0 && plantOf[order[static_cast<size_t>(st.parent)]] != plantOf[order[j]]) st.parent = -1;
        stems_[j] = st;
        parentOf_[j] = st.parent;
        if (j == 0 || plantOf[order[j]] != plantOf[order[j - 1]]) plantStart_.push_back(static_cast<uint32_t>(j));
    }
    plantStart_.push_back(static_cast<uint32_t>(n));
    for (int32_t& st : stemOf_) {
        if (st >= 0) st = newIndex[static_cast<size_t>(st)];
    }
    spin_.assign(n, Vec3(0.0f));
    swing_.assign(n, Vec3(0.0f));
    shove_.assign(n, Vec3(0.0f));
    steps_ = -1;
}

void SwayingPlants::reset(const WindSettings& w, const SwaySettings& d) {
    const size_t n = stems_.size();
    const float zeta = dampingOf(d);
    springs_.resize(n);
    for (size_t i = 0; i < n; ++i) {
        Spring& sp = springs_[i];
        sp.omega = 2.0f * kPi * swayHz(d, stems_[i].length);
        const float a = zeta * sp.omega, wd = sp.omega * std::sqrt(1.0f - zeta * zeta);
        sp.fade = std::exp(-a * dt_);
        sp.cos = std::cos(wd * dt_);
        sp.sin = std::sin(wd * dt_);
    }
    seed_ = ~w.seed;  // the wavers made at the first step
    // Still, bent as the wind is at the start: each stem where it holds it.
    state_.bend.assign(n, Vec3(0.0f));
    state_.rate.assign(n, Vec3(0.0f));
    for (size_t i = 0; i < n; ++i) {
        const Stem& st = stems_[i];
        const float share = st.branch ? st.share * std::max(d.branches, 0.0f) : st.share;
        const Vec3 bend = cross(st.dir, windBend(w, st.base, d.start, st.key ^ w.seed)) * share;
        state_.bend[i] = bend - st.dir * dot(bend, st.dir);
    }
    settings_ = d;
    steps_ = 0;
    kept_.clear();
    keepEvery_ = 120;
    kept_[0] = state_;
}

void SwayingPlants::step(State& state, float t0, float h, const WindSettings& w, const SwaySettings& d) const {
    const float zeta = dampingOf(d);
    const float branches = std::max(d.branches, 0.0f);
    const float middle = t0 + 0.5f * h;
    const bool whole = h == dt_;
    const size_t plants = plantStart_.empty() ? 0 : plantStart_.size() - 1;
    parallelFor(plants, 16, [&](size_t b, size_t e) {
        for (size_t k = b; k < e; ++k) {
            for (size_t i = plantStart_[k]; i < plantStart_[k + 1]; ++i) {
                const Stem& st = stems_[i];
                const Spring& sp = springs_[i];
                const float omega = sp.omega;
                const float share = st.branch ? st.share * branches : st.share;
                // Where the wind would hold it now.
                const Vec3 target = cross(st.dir, bendOf(w, st.base, middle, Waver{sp.a, sp.b, sp.rate})) * share;
                // How what carries it swings it: its parent turning faster or
                // slower under it -- its own bend at the stem's place on it --
                // and its base shoved as the parent swings.
                Vec3 spin(0.0f), shove(0.0f);
                if (st.parent >= 0) {
                    const size_t p = static_cast<size_t>(st.parent);
                    spin = spin_[p] + swing_[p] * profile(st.attach);
                    shove = shove_[p] + cross(spin, st.base - stems_[p].base);
                }
                spin_[i] = spin;
                shove_[i] = shove;
                // Left behind by both: as a rod pivoted at its base, the
                // shove turning it 3/2 of it over its length.
                const Vec3 push = -(spin - st.dir * dot(spin, st.dir)) - cross(st.dir, shove) * (1.5f / std::max(st.length, 0.01f));
                const Vec3 held = target + push / (omega * omega);
                // The spring over the step, exactly: toward where it is held,
                // swinging past and back, dying away.
                const float a = zeta * omega, wd = omega * std::sqrt(1.0f - zeta * zeta);
                const float fade = whole ? sp.fade : std::exp(-a * h), c = whole ? sp.cos : std::cos(wd * h);
                const float s = (whole ? sp.sin : std::sin(wd * h)) / wd;
                const Vec3 y = state.bend[i] - held, v = state.rate[i];
                Vec3 bend = held + (y * c + (v + y * a) * s) * fade;
                Vec3 rate = (v * c - (y * (omega * omega) + v * a) * s) * fade;
                bend -= st.dir * dot(bend, st.dir);  // no twist
                rate -= st.dir * dot(rate, st.dir);
                swing_[i] = (rate - v) / h;
                state.bend[i] = bend;
                state.rate[i] = rate;
            }
        }
    });
}

bool SwayingPlants::reach(const WindAt& windAt, const SwaySettings& d, float time, const std::atomic<bool>* interrupt) {
    if (stems_.empty()) return true;
    if (steps_ < 0 || !(settings_ == d)) reset(windAt(d.start), d);
    const double since = static_cast<double>(time) - static_cast<double>(d.start);
    const int64_t target = since <= 0.0 ? 0 : static_cast<int64_t>(std::floor(since / static_cast<double>(dt_) + 1e-6));
    if (target < steps_) {
        // Back: from the nearest state kept at or before it.
        auto it = kept_.upper_bound(target);
        --it;
        steps_ = it->first;
        state_ = it->second;
    }
    while (steps_ < target) {
        if (interrupt && (steps_ & 63) == 0 && interrupt->load(std::memory_order_relaxed)) return false;
        const double t0 = static_cast<double>(d.start) + static_cast<double>(steps_) * static_cast<double>(dt_);
        const WindSettings w = windAt(static_cast<float>(t0 + 0.5 * dt_));
        waver(w.seed);
        step(state_, static_cast<float>(t0), dt_, w, d);
        ++steps_;
        if (steps_ % keepEvery_ == 0) {
            kept_[steps_] = state_;
            if (kept_.size() > 48) {
                // Kept half as often from here, the rest let go.
                keepEvery_ *= 2;
                for (auto it = kept_.begin(); it != kept_.end();) {
                    it = it->first % keepEvery_ != 0 ? kept_.erase(it) : std::next(it);
                }
            }
        }
    }
    return true;
}

void SwayingPlants::waver(uint64_t seed) {
    if (seed == seed_) return;
    seed_ = seed;
    for (size_t i = 0; i < stems_.size(); ++i) {
        const Waver w = waverOf(stems_[i].key ^ seed);
        springs_[i].a = w.a;
        springs_[i].b = w.b;
        springs_[i].rate = w.rate;
    }
}

void SwayingPlants::at(const WindAt& windAt, const SwaySettings& d, float time, std::vector<Vec3>& out) {
    const double t0 = static_cast<double>(d.start) + static_cast<double>(steps_) * static_cast<double>(dt_);
    const double h = static_cast<double>(time) - t0;
    if (!(h > 1e-7)) {
        out = state_.bend;
        return;
    }
    const WindSettings w = windAt(static_cast<float>(t0 + 0.5 * h));
    waver(w.seed);
    State s = state_;
    step(s, static_cast<float>(t0), static_cast<float>(h), w, d);
    out = std::move(s.bend);
}

void SwayingPlants::deform(const Geometry& geo, std::span<const Vec3> bend, const WindSettings& w, float time,
                           std::vector<Vec3>& P, std::vector<Vec3>* N) const {
    const size_t n = stems_.size();
    // What carries each stem: its parent's turn where it grows from it, on
    // what carries the parent.
    std::vector<Mat3> carry(n, Mat3(1.0f));
    std::vector<Vec3> shift(n, Vec3(0.0f));
    for (size_t i = 0; i < n; ++i) {
        const int32_t p = stems_[i].parent;
        if (p < 0) continue;
        const size_t q = static_cast<size_t>(p);
        const Mat3 turn = turnBy(bend[q] * profile(stems_[i].attach));
        carry[i] = carry[q] * turn;
        shift[i] = carry[q] * (stems_[q].base - turn * stems_[q].base) + shift[q];
    }
    // The wind at each stem now: how hard the leaves flap, how far the tips
    // of blades toss.
    std::vector<Vec3> wind(n);
    for (size_t i = 0; i < n; ++i) wind[i] = windBend(w, stems_[i].base, time, stems_[i].key ^ w.seed);
    const float strength = std::max(w.strength, 1e-4f);
    const float spin = 2.0f * kPi * w.flutterSpeed * time;
    const auto rest = geo.positions();
    const AttributeArray* nAttr = geo.points().find("N");
    const auto normals = N && nAttr && nAttr->type() == AttrType::Vec3 ? nAttr->read<Vec3>() : std::span<const Vec3>();
    P.assign(rest.begin(), rest.end());
    if (N) N->assign(normals.begin(), normals.end());
    parallelFor(rest.size(), 4096, [&](size_t b, size_t e) {
        for (size_t p = b; p < e; ++p) {
            const int32_t at = stemOf_[p];
            if (at < 0) continue;
            const Stem& st = stems_[static_cast<size_t>(at)];
            Vec3 turn = bend[static_cast<size_t>(at)] * profile(along_[p]);
            if (st.blade && w.flutter > 0.0f) {
                // The tip of a blade tosses, as the bow has it.
                const Vec3 b0 = wind[static_cast<size_t>(at)];
                const float phase = 2.0f * kPi * unitOf(mix(st.key, w.seed + 7));
                const float strong = std::min(length(b0) / strength, 2.0f);
                const float f = flexOf_[p];
                const float toss = w.flutter * 0.5f * strong * std::sin(spin + phase) * std::max(f - 0.4f, 0.0f);
                turn += cross(st.dir, levelOf(b0)) * (toss * f * f);
            }
            const Mat3 R = turnBy(turn);
            const Mat3& C = carry[static_cast<size_t>(at)];
            P[p] = C * (st.base + R * (rest[p] - st.base)) + shift[static_cast<size_t>(at)];
            if (N && p < N->size()) (*N)[p] = C * (R * (*N)[p]);
        }
    });
    std::vector<Vec3> none;
    flapLeaves(
        geo, w, time,
        [&](uint32_t p) {
            const int32_t at = stemOf_[p];
            return at >= 0 ? std::min(length(wind[static_cast<size_t>(at)]) / strength, 2.0f) : 1.0f;
        },
        P, N ? *N : none);
}

bool SwayingPlants::blow(Geometry& geo, const WindAt& windAt, const SwaySettings& d, float time,
                         const std::atomic<bool>* interrupt) {
    if (stems_.empty() || stemOf_.size() != geo.pointCount()) return true;
    if (!reach(windAt, d, time, interrupt)) return false;
    // Now, and a moment on: how fast each point goes.
    constexpr float dt = 1.0f / 240.0f;
    std::vector<Vec3> bendNow, bendNext;
    at(windAt, d, time, bendNow);
    at(windAt, d, time + dt, bendNext);
    std::vector<Vec3> now, normals, next;
    const AttributeArray* nAttr = geo.points().find("N");
    const bool turnN = nAttr && nAttr->type() == AttrType::Vec3;
    deform(geo, bendNow, windAt(time), time, now, turnN ? &normals : nullptr);
    deform(geo, bendNext, windAt(time + dt), time + dt, next, nullptr);
    now_ = std::move(bendNow);
    auto P = geo.positionsForWrite();
    std::copy(now.begin(), now.end(), P.begin());
    if (turnN) {
        auto N = geo.points().create("N", AttrType::Vec3).write<Vec3>();
        std::copy(normals.begin(), normals.end(), N.begin());
    }
    auto v = geo.points().create("v", AttrType::Vec3).write<Vec3>();
    for (size_t p = 0; p < now.size(); ++p) v[p] = (next[p] - now[p]) / dt;
    return true;
}

bool SwayingPlants::bends(const WindAt& windAt, const SwaySettings& d, float time, std::vector<Vec3>& out,
                          const std::atomic<bool>* interrupt) {
    out.assign(stemOf_.size(), Vec3(0.0f));
    if (stems_.empty()) return true;
    if (!reach(windAt, d, time, interrupt)) return false;
    at(windAt, d, time, now_);
    const Vec3 up(0.0f, 1.0f, 0.0f);
    for (size_t p = 0; p < out.size(); ++p) {
        const int32_t at = stemOf_[p];
        // The bend about the level axis -- turning the top the way it says.
        if (at >= 0) out[p] = cross(now_[static_cast<size_t>(at)], up);
    }
    return true;
}

}  // namespace pg
