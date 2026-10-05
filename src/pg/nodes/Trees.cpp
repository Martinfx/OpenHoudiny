// Tree: a tree grown as a plant grows (pg/core/Tree.h) -- or, with points
// in, one on each of them: a forest, each tree its own.
//
//   output  mesh: the trunk and the branches as tubes of bark, the leaves as
//           polygons -- point Cd and flex (how far along the wood from the
//           tree's foot, a share of its height: what wind bends it by);
//           primitive level (-1 a leaf), stem, parent, tree; the primitive groups
//           bark and leaves. skeleton: each stem an open polyline, point
//           pscale its radius, flex, primitive level, stem, parent, tree;
//           each leaf a loose point, N the way it faces, pscale its size,
//           orient -- in the point group leaves -- for Copy to Points to put
//           a leaf of one's own on: modelled lying flat, facing +y, its
//           stalk at the origin, pointing along +z.
//           instances: Variants trees grown once, and a point for each
//           tree that stands for one of them (pg/core/Instances.h) --
//           instance, orient (turned about +y as it happens), pscale, tint
//           (a shade of its own) -- a forest of thousands.
//   points  each tree stands on one, pscale times as big (and Size
//           Variation more or less); its id, else its number, makes it its
//           own. Without them one tree stands at Center.
//
#include "pg/nodes/Nodes.h"

#include "pg/core/Geometry.h"
#include "pg/core/Instances.h"
#include "pg/core/Parallel.h"
#include "pg/core/Tree.h"

#include <algorithm>
#include <cmath>

namespace pg {
namespace {

uint64_t mixSeed(uint64_t seed, uint64_t id) {
    uint64_t z = seed * 0x9E3779B97F4A7C15ull ^ (id + 1) * 0xD1B54A32D192ED03ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

float unitOf(uint64_t bits) { return static_cast<float>(bits >> 40) / static_cast<float>(1u << 24); }

/// The primitive groups bark and leaves of a tree's mesh.
void groupMesh(Geometry& geo) {
    const AttributeArray* level = geo.primitives().find("level");
    if (!level) return;
    const auto lv = level->read<int32_t>();
    uint8_t* bark = geo.createGroup("bark", AttrClass::Primitive).writableMask();
    uint8_t* leaves = geo.createGroup("leaves", AttrClass::Primitive).writableMask();
    for (size_t i = 0; i < lv.size(); ++i) {
        bark[i] = lv[i] >= 0 ? 1 : 0;
        leaves[i] = lv[i] < 0 ? 1 : 0;
    }
}

class TreeNode : public Node {
public:
    explicit TreeNode(std::string name) : Node("tree", std::move(name)) {
        setInputCount(1);
        const TreeSettings d;
        params_.setInt("shape", static_cast<int>(d.shape));
        params_.setFloat("height", d.height);
        params_.setFloat("radius", d.radius);
        params_.setInt("seed", 1);
        params_.setVec3("center", Vec3());
        params_.setFloat("sizevariation", 0.2f);
        params_.setFloat("tip", d.tip);
        params_.setFloat("flare", d.flare);
        params_.setFloat("lean", d.lean);
        params_.setFloat("crown", d.crown);
        params_.setInt("forks", d.forks);
        params_.setFloat("forkheight", d.forkHeight);
        params_.setFloat("forkangle", d.forkAngle);
        params_.setInt("levels", d.levels);
        params_.setFloat("gravity", d.gravity);
        params_.setFloat("up", d.up);
        params_.setFloat("wobble", d.wobble);
        params_.setFloat("prune", d.prune);
        params_.setFloat("prunewidth", d.pruneWidth);
        params_.setFloat("prunepeak", d.prunePeak);
        params_.setFloat("prunepowerlow", d.prunePowerLow);
        params_.setFloat("prunepowerhigh", d.prunePowerHigh);
        params_.setInt("roots", d.roots);
        params_.setFloat("rootlength", d.rootLength);
        params_.setFloat("thickness", d.thickness);
        for (size_t l = 0; l < 3; ++l) {
            const std::string n = std::to_string(l + 1);
            params_.setInt("branches" + n, d.branches[l]);
            params_.setFloat("angle" + n, d.angle[l]);
            params_.setFloat("length" + n, d.length[l]);
        }
        params_.setInt("leaves", d.leaves);
        params_.setFloat("leafsize", d.leafSize);
        params_.setInt("leafshape", static_cast<int>(d.leaf));
        params_.setVec3("barkcolor", d.barkColor);
        params_.setVec3("leafcolor", d.leafColor);
        params_.setFloat("variation", d.variation);
        params_.setInt("sides", d.sides);
        params_.setFloat("segment", d.segment);
        params_.setInt("output", 0);  // 0 mesh, 1 skeleton, 2 instances
        params_.setInt("variants", 8);
    }

    GeometryPtr cookNode(const CookContext& ctx, std::span<const GeometryPtr> in) override {
        TreeSettings s;
        const TreeSettings d;
        s.shape = static_cast<TreeSettings::Shape>(std::clamp(params_.evalInt("shape", ctx, 1), 0, 6));
        s.height = std::max(params_.evalFloat("height", ctx, d.height), 0.01f);
        s.radius = std::max(params_.evalFloat("radius", ctx, d.radius), 1e-4f);
        s.tip = std::clamp(params_.evalFloat("tip", ctx, d.tip), 0.0f, 1.0f);
        s.flare = std::max(params_.evalFloat("flare", ctx, d.flare), 0.0f);
        s.lean = std::clamp(params_.evalFloat("lean", ctx, d.lean), 0.0f, 1.0f);
        s.crown = std::clamp(params_.evalFloat("crown", ctx, d.crown), 0.0f, 0.95f);
        s.forks = std::clamp(params_.evalInt("forks", ctx, d.forks), 1, 5);
        s.forkHeight = std::clamp(params_.evalFloat("forkheight", ctx, d.forkHeight), 0.0f, 1.0f);
        s.forkAngle = std::clamp(params_.evalFloat("forkangle", ctx, d.forkAngle), 0.0f, 90.0f);
        s.levels = std::clamp(params_.evalInt("levels", ctx, d.levels), 0, 3);
        s.gravity = std::max(params_.evalFloat("gravity", ctx, d.gravity), 0.0f);
        s.up = std::max(params_.evalFloat("up", ctx, d.up), 0.0f);
        s.wobble = std::max(params_.evalFloat("wobble", ctx, d.wobble), 0.0f);
        s.prune = std::clamp(params_.evalFloat("prune", ctx, d.prune), 0.0f, 1.0f);
        s.pruneWidth = std::max(params_.evalFloat("prunewidth", ctx, d.pruneWidth), 0.0f);
        s.prunePeak = std::clamp(params_.evalFloat("prunepeak", ctx, d.prunePeak), 0.0f, 1.0f);
        s.prunePowerLow = std::max(params_.evalFloat("prunepowerlow", ctx, d.prunePowerLow), 0.0f);
        s.prunePowerHigh = std::max(params_.evalFloat("prunepowerhigh", ctx, d.prunePowerHigh), 0.0f);
        s.roots = std::clamp(params_.evalInt("roots", ctx, d.roots), 0, 16);
        s.rootLength = std::max(params_.evalFloat("rootlength", ctx, d.rootLength), 0.0f);
        s.thickness = std::clamp(params_.evalFloat("thickness", ctx, d.thickness), 0.05f, 0.95f);
        for (size_t l = 0; l < 3; ++l) {
            const std::string n = std::to_string(l + 1);
            s.branches[l] = std::clamp(params_.evalInt("branches" + n, ctx, d.branches[l]), 0, 200);
            s.angle[l] = params_.evalFloat("angle" + n, ctx, d.angle[l]);
            s.length[l] = std::max(params_.evalFloat("length" + n, ctx, d.length[l]), 0.0f);
        }
        s.leaves = std::clamp(params_.evalInt("leaves", ctx, d.leaves), 0, 500);
        s.leafSize = std::max(params_.evalFloat("leafsize", ctx, d.leafSize), 1e-4f);
        s.leaf = static_cast<TreeSettings::Leaf>(std::clamp(params_.evalInt("leafshape", ctx, 0), 0, 2));
        s.barkColor = params_.evalVec3("barkcolor", ctx, d.barkColor);
        s.leafColor = params_.evalVec3("leafcolor", ctx, d.leafColor);
        s.variation = std::clamp(params_.evalFloat("variation", ctx, d.variation), 0.0f, 1.0f);
        s.sides = std::clamp(params_.evalInt("sides", ctx, d.sides), 3, 64);
        s.segment = std::max(params_.evalFloat("segment", ctx, d.segment), 0.01f);
        const uint64_t seed = static_cast<uint64_t>(std::max(params_.evalInt("seed", ctx, 1), 0));
        const int output = std::clamp(params_.evalInt("output", ctx, 0), 0, 2);
        const bool skeleton = output == 1;

        // Where the trees stand.
        std::vector<Place> places;
        const Geometry* points = !in.empty() && in[0] && in[0]->pointCount() > 0 ? in[0].get() : nullptr;
        if (!points) {
            places.push_back({params_.evalVec3("center", ctx, Vec3()), 1.0f, mixSeed(seed, 0)});
        } else {
            const auto P = points->positions();
            const AttributeArray* pscale = points->points().find("pscale");
            const AttributeArray* id = points->points().find("id");
            if (pscale && pscale->type() != AttrType::Float) pscale = nullptr;
            if (id && id->type() != AttrType::Int) id = nullptr;
            const float sizeVariation = std::clamp(params_.evalFloat("sizevariation", ctx, 0.2f), 0.0f, 1.0f);
            places.resize(P.size());
            for (size_t i = 0; i < P.size(); ++i) {
                Place& place = places[i];
                place.at = P[i];
                place.seed = mixSeed(seed, id ? static_cast<uint64_t>(static_cast<uint32_t>(id->read<int32_t>()[i])) : i);
                // One more or less as big as the next, as its seed says.
                const float u = static_cast<float>(mixSeed(place.seed, 7) >> 40) / static_cast<float>(1u << 24);
                place.scale = (pscale ? std::max(pscale->read<float>()[i], 0.0f) : 1.0f) *
                              (1.0f + sizeVariation * (2.0f * u - 1.0f));
            }
        }

        if (output == 2) return instances(ctx, s, seed, places, !points);

        // Each grown on its own, then one after the other.
        std::vector<Geometry> parts(places.size());
        parallelFor(places.size(), 1, [&](size_t b, size_t e) {
            for (size_t i = b; i < e && !ctx.interrupted(); ++i) {
                if (places[i].scale <= 0.0f) continue;
                const Tree tree = growTree(s, places[i].at, places[i].scale, places[i].seed);
                if (skeleton) {
                    skeletonTree(tree, static_cast<int>(i), parts[i]);
                } else {
                    meshTree(tree, s, static_cast<int>(i), parts[i]);
                }
            }
        });
        if (ctx.interrupted()) return nullptr;
        auto geo = std::make_shared<Geometry>();
        if (parts.size() == 1) {
            *geo = std::move(parts[0]);
        } else {
            for (const Geometry& part : parts) geo->append(part);
        }

        if (skeleton) {
            // The leaves: the points no primitive has.
            std::vector<uint8_t> used(geo->pointCount(), 0);
            for (const uint32_t p : geo->vertexPoints()) used[p] = 1;
            Group& leaves = geo->createGroup("leaves", AttrClass::Point);
            uint8_t* mask = leaves.writableMask();
            for (size_t p = 0; p < used.size(); ++p) mask[p] = used[p] ? 0 : 1;
        } else {
            groupMesh(*geo);
        }
        return geo;
    }

private:
    struct Place {
        Vec3 at;
        float scale = 1.0f;
        uint64_t seed = 0;
    };

    /// Variants trees -- the ones the first points would grow -- and a
    /// point for each place that stands for one of them, turned about +y
    /// as it happens, a shade of its own; `alone`: the one tree as it
    /// grows at Center.
    GeometryPtr instances(const CookContext& ctx, const TreeSettings& s, uint64_t seed, const std::vector<Place>& places,
                          bool alone) {
        const size_t variants = static_cast<size_t>(std::clamp(params_.evalInt("variants", ctx, 8), 1, 64));
        std::vector<std::shared_ptr<Geometry>> kinds(variants);
        parallelFor(variants, 1, [&](size_t b, size_t e) {
            for (size_t v = b; v < e && !ctx.interrupted(); ++v) {
                kinds[v] = std::make_shared<Geometry>();
                meshTree(growTree(s, Vec3(), 1.0f, mixSeed(seed, v)), s, static_cast<int>(v), *kinds[v]);
                groupMesh(*kinds[v]);
            }
        });
        if (ctx.interrupted()) return nullptr;
        auto geo = std::make_shared<Geometry>();
        geo->addPoints(places.size());
        auto P = geo->positionsForWrite();
        auto instance = geo->points().create("instance", AttrType::Int).write<int32_t>();
        auto orient = geo->points().create("orient", AttrType::Vec4).write<Vec4>();
        auto pscale = geo->points().create("pscale", AttrType::Float).write<float>();
        auto tint = geo->points().create("tint", AttrType::Vec3).write<Vec3>();
        for (size_t i = 0; i < places.size(); ++i) {
            const Place& place = places[i];
            P[i] = place.at;
            pscale[i] = std::max(place.scale, 0.0f);
            if (alone) {
                instance[i] = 0;
                orient[i] = Vec4(0.0f, 0.0f, 0.0f, 1.0f);
                tint[i] = Vec3(1.0f, 1.0f, 1.0f);
                continue;
            }
            instance[i] = static_cast<int32_t>(mixSeed(place.seed, 11) % variants);
            const float yaw = 6.28318531f * unitOf(mixSeed(place.seed, 12));
            orient[i] = Vec4(0.0f, std::sin(0.5f * yaw), 0.0f, std::cos(0.5f * yaw));
            const float shade = 1.0f + 0.4f * s.variation * (2.0f * unitOf(mixSeed(place.seed, 13)) - 1.0f);
            const float warm = s.variation * unitOf(mixSeed(place.seed, 14));
            tint[i] = Vec3(shade * (1.0f + 0.2f * warm), shade * (1.0f + 0.08f * warm), shade * (1.0f - 0.3f * warm));
        }
        for (auto& kind : kinds) geo->addPrototype(std::move(kind));
        return geo;
    }
};

}  // namespace

void registerTreeNodes() {
    NodeRegistry::instance().add("tree", [](const std::string& n) { return std::make_unique<TreeNode>(n); });
}

}  // namespace pg
