// Plant Wind: trees and grass in the wind (pg/core/Wind.h). Plant Trample:
// grass and shrubs flattened where something treads, straightening again.
//
//   geometry  plants as geometry -- point flex, as the Tree and Grass
//             nodes give it -- bent where they are, and v, how fast each
//             point goes (what motion blur blurs it by); points that stand
//             for plants (instances): each standing for its plant bent
//             ahead into the nearest of a few shapes -- Directions ways
//             round it, Steps far -- tilted the rest of the way.
//   treads    (Plant Trample) points where something trod: pscale times
//             Radius how wide, time when -- the plants round each bowed
//             away from it, Flatten at its middle, none at its edge,
//             straightening over Recovery seconds after.
//
#include "pg/nodes/Nodes.h"

#include "pg/core/Geometry.h"
#include "pg/core/Instances.h"
#include "pg/core/Wind.h"

#include <algorithm>
#include <cmath>
#include <vector>

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
        // The plants that points stand for: bent ahead into a few shapes.
        if (in.prototypeCount() > 0) {
            const AttributeArray* idAttr = in.points().find("id");
            const auto ids = idAttr && idAttr->type() == AttrType::Int ? idAttr->read<int32_t>() : std::span<const int32_t>();
            const auto P = in.positions();
            std::vector<Vec3> bends(in.pointCount());
            for (size_t p = 0; p < bends.size(); ++p) {
                const uint64_t own = ids.empty() ? static_cast<uint64_t>(p) : static_cast<uint64_t>(static_cast<uint32_t>(ids[p]));
                bends[p] = windBend(s, P[p], time, own * 0x9E3779B97F4A7C15ull ^ s.seed);
            }
            // The most a plant bows: the gusts at their strongest, wavering.
            const float most = s.strength * (1.0f + s.gust) * (1.0f + s.turbulence);
            bowInstances(*out, bends, directions, steps, most, shapes_);
        }
        return out;
    }

private:
    BentShapes shapes_;
};

/// A plant's foot by a foot of what treads: how far it is flattened, and
/// which way -- away from it.
struct Tread {
    Vec3 at;
    float radius = 0.4f;
    float time = -1e30f;  // when it trod there
};

class PlantTrampleNode : public Node {
public:
    explicit PlantTrampleNode(std::string name) : Node("planttrample", std::move(name)) {
        setInputCount(2);
        params_.setFloat("radius", 0.4f);
        params_.setFloat("flatten", 70.0f);
        params_.setFloat("recovery", 4.0f);
        params_.setInt("directions", 8);
        params_.setInt("steps", 4);
    }

    bool isTimeDependentSelf() const override { return true; }

    GeometryPtr cookNode(const CookContext& ctx, std::span<const GeometryPtr> inputs) override {
        if (inputs.empty() || !inputs[0]) return std::make_shared<Geometry>();
        auto out = std::make_shared<Geometry>(*inputs[0]);
        if (inputs.size() < 2 || !inputs[1] || inputs[1]->pointCount() == 0) return out;
        const float radius = std::max(params_.evalFloat("radius", ctx, 0.4f), 1e-3f);
        const float flatten = std::clamp(params_.evalFloat("flatten", ctx, 70.0f), 0.0f, 89.0f) * kPi / 180.0f;
        const float recovery = std::max(params_.evalFloat("recovery", ctx, 4.0f), 0.0f);
        const int directions = std::clamp(params_.evalInt("directions", ctx, 8), 1, 32);
        const int steps = std::clamp(params_.evalInt("steps", ctx, 4), 1, 16);
        const float now = static_cast<float>(ctx.time);

        // Where it treads: each point, as wide as its pscale says (times
        // Radius), since its time -- none yet where that is still to come.
        const Geometry& feet = *inputs[1];
        const AttributeArray* ps = feet.points().find("pscale");
        const auto pscale = ps && ps->type() == AttrType::Float ? ps->read<float>() : std::span<const float>();
        const AttributeArray* tm = feet.points().find("time");
        const auto times = tm && tm->type() == AttrType::Float ? tm->read<float>() : std::span<const float>();
        std::vector<Tread> treads;
        for (size_t i = 0; i < feet.pointCount(); ++i) {
            Tread t;
            t.at = feet.positions()[i];
            t.radius = radius * (pscale.empty() ? 1.0f : std::max(pscale[i], 0.0f));
            t.time = times.empty() ? -1e30f : times[i];
            if (t.time <= now) treads.push_back(t);
        }
        // How far a plant at `foot` is flattened, and which way: the most of
        // the treads round it -- each strongest where it trod, none past its
        // radius -- straightening as Recovery says since.
        auto bendAt = [&](const Vec3& foot) {
            Vec3 best(0.0f);
            for (const Tread& t : treads) {
                const Vec3 off(foot.x - t.at.x, 0.0f, foot.z - t.at.z);
                const float d = length(off);
                if (d >= t.radius) continue;
                const float near = 1.0f - d / t.radius;
                float amount = flatten * near * near * (3.0f - 2.0f * near);
                if (recovery > 0.0f && t.time > -1e29f) amount *= std::exp(-(now - t.time) / recovery);
                if (amount > length(best)) best = (d > 1e-5f ? off / d : Vec3(1.0f, 0.0f, 0.0f)) * amount;
            }
            return best;
        };
        if (hasFlex(*inputs[0])) bowPlants(*out, [&](const Vec3& foot, uint64_t) { return bendAt(foot); });
        if (inputs[0]->prototypeCount() > 0) {
            const auto P = inputs[0]->positions();
            std::vector<Vec3> bends(inputs[0]->pointCount());
            for (size_t p = 0; p < bends.size(); ++p) bends[p] = bendAt(P[p]);
            bowInstances(*out, bends, directions, steps, flatten, shapes_);
        }
        return out;
    }

private:
    BentShapes shapes_;
};

}  // namespace

void registerWindNodes() {
    NodeRegistry::instance().add("plantwind", [](const std::string& n) { return std::make_unique<PlantWindNode>(n); });
    NodeRegistry::instance().add("planttrample", [](const std::string& n) { return std::make_unique<PlantTrampleNode>(n); });
}

}  // namespace pg
