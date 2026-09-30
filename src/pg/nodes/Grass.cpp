// Grass: clumps of grass (pg/core/Grass.h) over a surface -- a meadow -- as
// instances (pg/core/Instances.h): a few clumps, Variants of them, held
// once, and a point for each clump in the meadow that stands for one of
// them.
//
//   surface  clumps scattered over its polygons, Density to a square metre,
//            by Scatter's rules: fewer where the density attribute is low,
//            none on faces steeper than Max Slope. Only points in (no
//            polygons): a clump on each.
//   output   the points -- instance, orient (turned about +y as it
//            happens; with Along Normal, +y to N as well), pscale (Size
//            Variation more or less, times their own), tint (Variation:
//            one clump a little lighter, darker or yellower than the
//            next) -- and the clumps; with Instances off, the clumps made
//            copies of, geometry that every node can change. Nothing in:
//            one clump at Center.
//
#include "pg/nodes/Nodes.h"

#include "pg/core/Geometry.h"
#include "pg/core/Grass.h"
#include "pg/core/Instances.h"
#include "pg/core/Parallel.h"

#include <algorithm>
#include <cmath>

namespace pg {
namespace {

constexpr float kPi = 3.14159265358979f;

uint64_t mixSeed(uint64_t seed, uint64_t id) {
    uint64_t z = seed * 0x9E3779B97F4A7C15ull ^ (id + 1) * 0xD1B54A32D192ED03ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

float unitOf(uint64_t bits) { return static_cast<float>(bits >> 40) / static_cast<float>(1u << 24); }

/// Whether `geo` has a polygon to scatter over.
bool hasSurface(const Geometry& geo) {
    for (size_t prim = 0; prim < geo.primitiveCount(); ++prim) {
        if (geo.primitiveClosed(prim) && geo.primitiveVertexCount(prim) >= 3) return true;
    }
    return false;
}

class GrassNode : public Node {
public:
    explicit GrassNode(std::string name) : Node("grass", std::move(name)) {
        setInputCount(1);
        const GrassSettings d;
        params_.setFloat("density", 50.0f);
        params_.setInt("seed", 1);
        params_.setVec3("center", Vec3());
        params_.setFloat("sizevariation", 0.3f);
        params_.setBool("alongnormal", false);
        params_.setString("densityattribute", "");
        params_.setFloat("maxslope", 45.0f);
        params_.setInt("blades", d.blades);
        params_.setFloat("height", d.height);
        params_.setFloat("heightvariation", d.heightVariation);
        params_.setFloat("width", d.width);
        params_.setFloat("bend", d.bend);
        params_.setFloat("lean", d.lean);
        params_.setFloat("spread", d.spread);
        params_.setInt("segments", d.segments);
        params_.setVec3("rootcolor", d.rootColor);
        params_.setVec3("tipcolor", d.tipColor);
        params_.setFloat("dry", d.dry);
        params_.setVec3("drycolor", d.dryColor);
        params_.setFloat("variation", d.variation);
        params_.setInt("variants", 8);
        params_.setBool("instances", true);
    }

    GeometryPtr cookNode(const CookContext& ctx, std::span<const GeometryPtr> in) override {
        GrassSettings s;
        const GrassSettings d;
        s.blades = std::clamp(params_.evalInt("blades", ctx, d.blades), 0, 1000);
        s.height = std::max(params_.evalFloat("height", ctx, d.height), 0.0f);
        s.heightVariation = std::clamp(params_.evalFloat("heightvariation", ctx, d.heightVariation), 0.0f, 1.0f);
        s.width = std::max(params_.evalFloat("width", ctx, d.width), 0.0f);
        s.bend = std::clamp(params_.evalFloat("bend", ctx, d.bend), 0.0f, 1.0f);
        s.lean = std::clamp(params_.evalFloat("lean", ctx, d.lean), 0.0f, 90.0f);
        s.spread = std::max(params_.evalFloat("spread", ctx, d.spread), 0.0f);
        s.segments = std::clamp(params_.evalInt("segments", ctx, d.segments), 1, 16);
        s.rootColor = params_.evalVec3("rootcolor", ctx, d.rootColor);
        s.tipColor = params_.evalVec3("tipcolor", ctx, d.tipColor);
        s.dry = std::clamp(params_.evalFloat("dry", ctx, d.dry), 0.0f, 1.0f);
        s.dryColor = params_.evalVec3("drycolor", ctx, d.dryColor);
        s.variation = std::clamp(params_.evalFloat("variation", ctx, d.variation), 0.0f, 1.0f);
        const uint64_t seed = static_cast<uint64_t>(std::max(params_.evalInt("seed", ctx, 1), 0));
        const int variants = std::clamp(params_.evalInt("variants", ctx, 8), 1, 64);

        const Geometry* src = !in.empty() && in[0] && in[0]->pointCount() > 0 ? in[0].get() : nullptr;
        if (!src) {
            // One clump, where it is asked for.
            auto clump = std::make_shared<Geometry>(growGrassClump(s, mixSeed(seed, 0)));
            const Vec3 center = params_.evalVec3("center", ctx, Vec3());
            if (center != Vec3()) {
                for (Vec3& p : clump->positionsForWrite()) p = p + center;
            }
            return clump;
        }

        // Where the clumps stand.
        std::shared_ptr<Geometry> out;
        if (hasSurface(*src)) {
            ScatterRules r;
            r.density = std::max(params_.evalFloat("density", ctx, 50.0f), 0.0f);
            r.seed = static_cast<uint32_t>(seed);
            r.densityAttribute = params_.getString("densityattribute", "");
            r.maxSlope = params_.evalFloat("maxslope", ctx, 45.0f);
            out = scatterPoints(*src, r);
        } else {
            // The points as they are: no primitives, nothing they stood for.
            out = std::make_shared<Geometry>(*src);
            if (out->primitiveCount() > 0) {
                const std::vector<uint8_t> none(out->primitiveCount(), 0);
                out->deletePrimitives(none, false);
            }
            out->clearPrototypes();
            out->points().erase("instance");
        }
        if (ctx.interrupted()) return nullptr;
        const size_t n = out->pointCount();

        // Each clump its own: which it is, how it is turned, how big, what shade.
        const float sizeVariation = std::clamp(params_.evalFloat("sizevariation", ctx, 0.3f), 0.0f, 1.0f);
        const bool alongNormal = params_.evalBool("alongnormal", ctx, false);
        const AttributeArray* idAttr = out->points().find("id");
        if (idAttr && idAttr->type() != AttrType::Int) idAttr = nullptr;
        const AttributeArray* nAttr = alongNormal ? out->points().find("N") : nullptr;
        if (nAttr && nAttr->type() != AttrType::Vec3) nAttr = nullptr;
        AttributeArray* psAttr = out->points().find("pscale");
        const bool hadScale = psAttr && psAttr->type() == AttrType::Float;
        if (psAttr && !hadScale) out->points().erase("pscale");
        out->points().erase("orient");
        out->points().erase("tint");
        auto instance = out->points().create("instance", AttrType::Int).write<int32_t>();
        auto orient = out->points().create("orient", AttrType::Vec4).write<Vec4>();
        auto pscale = out->points().create("pscale", AttrType::Float).write<float>();
        auto tint = out->points().create("tint", AttrType::Vec3).write<Vec3>();
        const auto ids = idAttr ? idAttr->read<int32_t>() : std::span<const int32_t>();
        const auto N = nAttr ? nAttr->read<Vec3>() : std::span<const Vec3>();
        parallelFor(n, 4096, [&](size_t b, size_t e) {
            for (size_t i = b; i < e; ++i) {
                const uint64_t own = mixSeed(seed ^ 0x5bd1e995ull, idAttr ? static_cast<uint64_t>(static_cast<uint32_t>(ids[i])) : i);
                instance[i] = static_cast<int32_t>(own % static_cast<uint64_t>(variants));
                const float yaw = 2.0f * kPi * unitOf(mixSeed(own, 1));
                Vec4 q(0.0f, std::sin(0.5f * yaw), 0.0f, std::cos(0.5f * yaw));
                if (nAttr && dot(N[i], N[i]) > 1e-12f) q = quatMultiply(quatUpTo(normalize(N[i])), q);
                orient[i] = q;
                const float size = 1.0f + sizeVariation * (2.0f * unitOf(mixSeed(own, 2)) - 1.0f);
                pscale[i] = (hadScale ? std::max(pscale[i], 0.0f) : 1.0f) * size;
                const float shade = 1.0f + 0.5f * s.variation * (2.0f * unitOf(mixSeed(own, 3)) - 1.0f);
                const float warm = s.variation * unitOf(mixSeed(own, 4));
                tint[i] = Vec3(shade * (1.0f + 0.25f * warm), shade * (1.0f + 0.1f * warm), shade * (1.0f - 0.4f * warm));
            }
        });

        // The clumps, each of its own seed.
        std::vector<std::shared_ptr<Geometry>> clumps(static_cast<size_t>(variants));
        parallelFor(clumps.size(), 1, [&](size_t b, size_t e) {
            for (size_t v = b; v < e; ++v) clumps[v] = std::make_shared<Geometry>(growGrassClump(s, mixSeed(seed, 1000 + v)));
        });
        for (auto& clump : clumps) out->addPrototype(std::move(clump));
        if (!params_.evalBool("instances", ctx, true)) return unpackInstances(*out);
        return out;
    }
};

}  // namespace

void registerGrassNodes() {
    NodeRegistry::instance().add("grass", [](const std::string& n) { return std::make_unique<GrassNode>(n); });
}

}  // namespace pg
