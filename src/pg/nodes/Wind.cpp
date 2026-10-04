// Plant Wind: trees and grass in the wind (pg/core/Wind.h).
//
//   geometry  plants as geometry -- point flex, as the Tree and Grass
//             nodes give it -- bent where they are, and v, how fast each
//             point goes (what motion blur blurs it by); points that stand
//             for plants (instances): each standing for its plant bent
//             ahead into the nearest of a few shapes -- Directions ways
//             round it, Steps far -- tilted the rest of the way.
//
#include "pg/nodes/Nodes.h"

#include "pg/core/Geometry.h"
#include "pg/core/Instances.h"
#include "pg/core/Wind.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <tuple>

namespace pg {
namespace {

constexpr float kPi = 3.14159265358979f;

bool hasFlex(const Geometry& geo) {
    const AttributeArray* flex = geo.points().find("flex");
    return flex && flex->type() == AttrType::Float;
}

class PlantWindNode : public Node {
public:
    explicit PlantWindNode(std::string name) : Node("plantwind", std::move(name)) {
        setInputCount(1);
        const WindSettings d;
        params_.setFloat("direction", 0.0f);
        params_.setFloat("strength", d.strength * 180.0f / kPi);
        params_.setFloat("gust", d.gust);
        params_.setFloat("gustspeed", d.gustSpeed);
        params_.setFloat("gustsize", d.gustSize);
        params_.setFloat("turbulence", d.turbulence);
        params_.setFloat("flutter", d.flutter * 180.0f / kPi);
        params_.setFloat("flutterspeed", d.flutterSpeed);
        params_.setInt("seed", 1);
        params_.setInt("directions", 8);
        params_.setInt("steps", 4);
    }

    bool isTimeDependentSelf() const override { return true; }

    GeometryPtr cookNode(const CookContext& ctx, std::span<const GeometryPtr> inputs) override {
        if (inputs.empty() || !inputs[0]) return std::make_shared<Geometry>();
        WindSettings s;
        const float heading = params_.evalFloat("direction", ctx, 0.0f) * kPi / 180.0f;
        s.direction = Vec3(std::cos(heading), 0.0f, -std::sin(heading));
        s.strength = std::max(params_.evalFloat("strength", ctx, 14.0f), 0.0f) * kPi / 180.0f;
        s.gust = std::clamp(params_.evalFloat("gust", ctx, s.gust), 0.0f, 1.0f);
        s.gustSpeed = params_.evalFloat("gustspeed", ctx, s.gustSpeed);
        s.gustSize = std::max(params_.evalFloat("gustsize", ctx, s.gustSize), 0.01f);
        s.turbulence = std::clamp(params_.evalFloat("turbulence", ctx, s.turbulence), 0.0f, 2.0f);
        s.flutter = std::max(params_.evalFloat("flutter", ctx, 20.0f), 0.0f) * kPi / 180.0f;
        s.flutterSpeed = std::max(params_.evalFloat("flutterspeed", ctx, s.flutterSpeed), 0.0f);
        s.seed = static_cast<uint64_t>(params_.evalInt("seed", ctx, 1));
        const int directions = std::clamp(params_.evalInt("directions", ctx, 8), 1, 32);
        const int steps = std::clamp(params_.evalInt("steps", ctx, 4), 1, 16);
        const float time = static_cast<float>(ctx.time);

        const Geometry& in = *inputs[0];
        auto out = std::make_shared<Geometry>(in);
        // The plants that are geometry: bent where they are.
        if (hasFlex(in)) blowPlants(*out, s, time);
        if (in.prototypeCount() == 0) return out;

        // The plants that points stand for.
        const AttributeArray* instAttr = in.points().find("instance");
        if (!instAttr || instAttr->type() != AttrType::Int) return out;
        const auto& protos = in.prototypes();
        std::vector<uint8_t> bends(protos.size(), 0);
        bool any = false;
        for (size_t k = 0; k < protos.size(); ++k) any |= (bends[k] = protos[k] && hasFlex(*protos[k]) ? 1 : 0) != 0;
        if (!any) return out;
        const auto instance = instAttr->read<int32_t>();
        const AttributeArray* idAttr = in.points().find("id");
        const auto ids = idAttr && idAttr->type() == AttrType::Int ? idAttr->read<int32_t>() : std::span<const int32_t>();
        const AttributeArray* nAttr = in.points().find("N");
        const auto N = nAttr && nAttr->type() == AttrType::Vec3 ? nAttr->read<Vec3>() : std::span<const Vec3>();
        const AttributeArray* oAttr = in.points().find("orient");
        const bool hadOrient = oAttr && oAttr->type() == AttrType::Vec4;
        std::vector<Vec4> orient(in.pointCount());
        for (size_t p = 0; p < orient.size(); ++p) {
            orient[p] = hadOrient ? oAttr->read<Vec4>()[p]
                                  : !N.empty() && dot(N[p], N[p]) > 1e-12f ? quatUpTo(normalize(N[p])) : Vec4(0.0f, 0.0f, 0.0f, 1.0f);
        }
        // The most a plant bows: the gusts at their strongest, wavering.
        const float most = s.strength * (1.0f + s.gust) * (1.0f + s.turbulence) + 1e-6f;
        const float stepSize = most / static_cast<float>(steps);
        const auto P = in.positions();
        std::lock_guard<std::mutex> lock(mutex_);
        if (cache_.size() > 4096) cache_.clear();
        std::map<std::tuple<size_t, int, int>, int32_t> made;  // prototype, way, step -> its index in `out`
        std::vector<int32_t> standsFor(instance.begin(), instance.end());
        for (size_t p = 0; p < standsFor.size(); ++p) {
            const int32_t k = instance[p];
            if (k < 0 || static_cast<size_t>(k) >= protos.size() || !bends[static_cast<size_t>(k)]) continue;
            const uint64_t own = ids.empty() ? static_cast<uint64_t>(p) : static_cast<uint64_t>(static_cast<uint32_t>(ids[p]));
            const Vec3 bend = windBend(s, P[p], time, own * 0x9E3779B97F4A7C15ull ^ s.seed);
            // In the plant's own frame.
            const Vec4 q = orient[p];
            Vec3 local = quatRotate(Vec4(-q.x, -q.y, -q.z, q.w), bend);
            local.y = 0.0f;
            const float amount = length(local);
            const int step = std::min(static_cast<int>(std::floor(amount / stepSize + 0.5f)), steps);
            Vec3 shaped(0.0f);
            if (step > 0) {
                const float wayStep = 2.0f * kPi / static_cast<float>(directions);
                int way = static_cast<int>(std::floor(std::atan2(local.z, local.x) / wayStep + 0.5f));
                way = ((way % directions) + directions) % directions;
                const float angle = static_cast<float>(way) * wayStep;
                shaped = Vec3(std::cos(angle), 0.0f, std::sin(angle)) * (static_cast<float>(step) * stepSize);
                const auto key = std::make_tuple(static_cast<size_t>(k), way, step);
                auto it = made.find(key);
                if (it == made.end()) {
                    const auto cacheKey = std::make_tuple(protos[static_cast<size_t>(k)].get(), way, step, directions, steps,
                                                          stepSize);
                    auto c = cache_.find(cacheKey);
                    if (c == cache_.end()) {
                        Entry e;
                        e.of = protos[static_cast<size_t>(k)];
                        e.bent = std::make_shared<const Geometry>(bentPlant(*e.of, shaped));
                        c = cache_.emplace(cacheKey, std::move(e)).first;
                    }
                    it = made.emplace(key, static_cast<int32_t>(out->addPrototype(c->second.bent))).first;
                }
                standsFor[p] = it->second;
            }
            // The rest of the way: the plant tilted from its foot -- about half
            // as far as bowing moves its top.
            const Vec3 rest = local - shaped;
            const float tilt = 0.5f * length(rest);
            if (tilt > 1e-6f) {
                const Vec3 axis = normalize(cross(Vec3(0.0f, 1.0f, 0.0f), rest));
                const Vec4 r(axis.x * std::sin(0.5f * tilt), axis.y * std::sin(0.5f * tilt), axis.z * std::sin(0.5f * tilt),
                             std::cos(0.5f * tilt));
                orient[p] = quatMultiply(q, r);
            }
        }
        auto outInstance = out->points().create("instance", AttrType::Int).write<int32_t>();
        std::copy(standsFor.begin(), standsFor.end(), outInstance.begin());
        auto outOrient = out->points().create("orient", AttrType::Vec4).write<Vec4>();
        std::copy(orient.begin(), orient.end(), outOrient.begin());
        return out;
    }

private:
    struct Entry {
        std::shared_ptr<const Geometry> of;    // held: its pointer names it
        std::shared_ptr<const Geometry> bent;  // the same each frame: the viewport keeps it on the GPU
    };
    std::mutex mutex_;
    std::map<std::tuple<const Geometry*, int, int, int, int, float>, Entry> cache_;
};

}  // namespace

void registerWindNodes() {
    NodeRegistry::instance().add("plantwind", [](const std::string& n) { return std::make_unique<PlantWindNode>(n); });
}

}  // namespace pg
