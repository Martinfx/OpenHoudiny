// Tree: a tree grown as a plant grows (pg/core/Tree.h) -- or, with points
// in, one on each of them: a forest, each tree its own.
//
//   output  mesh: the trunk and the branches as tubes of bark, the leaves as
//           polygons -- point Cd and flex (how far along the wood from the
//           tree's foot, a share of its height: what wind bends it by);
//           primitive level (-1 a leaf), stem, tree; the primitive groups
//           bark and leaves. skeleton: each stem an open polyline, point
//           pscale its radius, flex, primitive level, stem, parent, tree;
//           each leaf a loose point, N the way it faces, pscale its size,
//           orient -- in the point group leaves -- for Copy to Points to put
//           a leaf of one's own on: modelled lying flat, facing +y, its
//           stalk at the origin, pointing along +z.
//   points  each tree stands on one, pscale times as big (and Size
//           Variation more or less); its id, else its number, makes it its
//           own. Without them one tree stands at Center.
//
#include "pg/nodes/Nodes.h"

#include "pg/core/Geometry.h"
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
        params_.setInt("output", 0);  // 0 mesh, 1 skeleton
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
        const bool skeleton = params_.evalInt("output", ctx, 0) == 1;

        // Where the trees stand.
        struct Place {
            Vec3 at;
            float scale = 1.0f;
            uint64_t seed = 0;
        };
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
        } else if (const AttributeArray* level = geo->primitives().find("level")) {
            const auto lv = level->read<int32_t>();
            uint8_t* bark = geo->createGroup("bark", AttrClass::Primitive).writableMask();
            uint8_t* leaves = geo->createGroup("leaves", AttrClass::Primitive).writableMask();
            for (size_t i = 0; i < lv.size(); ++i) {
                bark[i] = lv[i] >= 0 ? 1 : 0;
                leaves[i] = lv[i] < 0 ? 1 : 0;
            }
        }
        return geo;
    }
};

}  // namespace

void registerTreeNodes() {
    NodeRegistry::instance().add("tree", [](const std::string& n) { return std::make_unique<TreeNode>(n); });
}

}  // namespace pg
