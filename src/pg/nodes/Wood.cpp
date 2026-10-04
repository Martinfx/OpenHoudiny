// Wood Fracture: a closed mesh -- a board, a beam, a post -- broken as wood
// breaks: along its fibres, into long splints and slats, the ends torn into
// splinters.
//
//   grain    the way the fibres run: Grain, or -- left at zero -- the
//            longest side of the box round the mesh (fitBox)
//   seeds    Count points inside the mesh -- or those linked into Points --
//            denser round Impact as Focus says, within Reach
//   cells    each seed's Voronoi cell, measured with the grain Stretch times
//            shorter than the other ways (grainCells): pieces that much
//            longer along the fibres than across them
//   splinter the cut faces across the fibres torn into spikes along them --
//            bundles of fibres Splinter Size across broken at lengths up to
//            Splinter; the faces along them grooved as the fibres run, Rough
//            deep. The same on both sides of a crack: the pieces still fit
//
// Where each point was before the faces were torn stays in the point
// attribute proxy: the RBD Solver simulates the pieces as the proxy has them
// and draws them as they are. The pieces carry piece (on primitives and
// points) and the way the fibres run in grain (on the points: a piece the
// RBD Solver breaks while it runs splits along them too); the cut faces are
// in Inside Group; the faces are wood (s@material) where they are nothing
// else.
#include "pg/nodes/Nodes.h"

#include "pg/core/Parallel.h"
#include "pg/nodes/Rough.h"

#include <algorithm>
#include <cmath>

namespace pg {
namespace {

uint64_t splitmix(uint64_t& state) {
    uint64_t z = (state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

float unit(uint64_t& state) { return static_cast<float>(splitmix(state) >> 40) / static_cast<float>(1ull << 24); }

/// `count` points inside `mesh`, the same for the same `seed`: drawn in the
/// box round it, those outside it drawn again, and those far from `impact`
/// -- measured with the grain `stretch` times shorter -- as much less often
/// as Focus says.
std::vector<Vec3> seedsInside(const Geometry& mesh, int count, const Vec3& impact, float focus, float reach,
                              const Vec3& grain, float stretch, uint64_t seed) {
    std::vector<Vec3> out;
    const auto P = mesh.positions();
    if (P.empty() || count <= 0) return out;
    Vec3 lo = P[0], hi = P[0];
    for (const Vec3& p : P) {
        for (int a = 0; a < 3; ++a) {
            lo[a] = std::min(lo[a], p[a]);
            hi[a] = std::max(hi[a], p[a]);
        }
    }
    reach = std::max(reach, 1e-3f);
    auto weight = [&](const Vec3& p) {
        if (focus <= 0.0f) return 1.0f;
        const Vec3 d = p - impact;
        const float along = dot(d, grain);
        const Vec3 measured = d - grain * (along * (1.0f - 1.0f / stretch));
        const float r = length(measured) / reach;
        return (1.0f + focus * std::exp(-r * r)) / (1.0f + focus);
    };
    uint64_t state = seed * 0x2545F4914F6CDD1Dull + 1;
    for (long tries = static_cast<long>(count) * 4000; tries > 0 && static_cast<int>(out.size()) < count; --tries) {
        const Vec3 p(lo.x + (hi.x - lo.x) * unit(state), lo.y + (hi.y - lo.y) * unit(state),
                     lo.z + (hi.z - lo.z) * unit(state));
        const float keep = unit(state);
        if (keep > weight(p) || !insideMesh(mesh, p)) continue;
        out.push_back(p);
    }
    return out;
}

class WoodFractureNode : public Node {
public:
    explicit WoodFractureNode(std::string name) : Node("woodfracture", std::move(name)) {
        setInputCount(2);
        params_.setInt("count", 14);
        params_.setInt("seed", 1);
        params_.setVec3("grain", Vec3(0.0f, 0.0f, 0.0f));
        params_.setFloat("stretch", 6.0f);
        params_.setVec3("impact", Vec3(0.0f, 1.0f, 0.0f));
        params_.setFloat("focus", 0.0f);
        params_.setFloat("reach", 0.5f);
        params_.setFloat("splinter", 0.05f);
        params_.setFloat("splintersize", 0.012f);
        params_.setFloat("rough", 0.003f);
        params_.setFloat("roughscale", 0.04f);
        params_.setFloat("detail", 0.008f);
        params_.setString("attribute", "piece");
        params_.setString("insidegroup", "inside");
    }

    GeometryPtr cookNode(const CookContext& ctx, std::span<const GeometryPtr> in) override {
        if (in.empty() || !in[0] || in[0]->primitiveCount() == 0) return std::make_shared<Geometry>();
        const GeometryPtr mesh = in[0];
        const uint64_t seed = static_cast<uint64_t>(std::max(params_.evalInt("seed", ctx, 1), 0));
        // The way the fibres run: given, or the longest side of the box.
        Vec3 grain = params_.evalVec3("grain", ctx, Vec3());
        if (!(dot(grain, grain) > 1e-12f)) {
            const OrientedBox box = fitBox(*mesh, proxyPositions(*mesh));
            int longest = 0;
            for (int a = 1; a < 3; ++a) {
                if (box.size(a) > box.size(longest)) longest = a;
            }
            grain = box.axis[static_cast<size_t>(longest)];
        }
        grain = normalize(grain);
        const float stretch = std::clamp(params_.evalFloat("stretch", ctx, 6.0f), 1.0f, 50.0f);
        std::vector<Vec3> seeds;
        if (in.size() > 1 && in[1] && in[1]->pointCount() > 0) {
            const auto P = in[1]->positions();
            seeds.assign(P.begin(), P.end());
        } else {
            seeds = seedsInside(*mesh, std::clamp(params_.evalInt("count", ctx, 14), 0, 10000),
                                params_.evalVec3("impact", ctx, Vec3(0.0f, 1.0f, 0.0f)),
                                std::max(params_.evalFloat("focus", ctx, 0.0f), 0.0f), params_.evalFloat("reach", ctx, 0.5f),
                                grain, stretch, seed);
        }
        const std::string attribute = params_.getString("attribute", "piece");
        const std::string inside = params_.getString("insidegroup", "inside");
        const std::string group = inside.empty() ? std::string("inside") : inside;
        RoughCut rough;
        rough.amount = std::max(params_.evalFloat("rough", ctx, 0.003f), 0.0f);
        rough.scale = std::max(params_.evalFloat("roughscale", ctx, 0.04f), 1e-3f);
        rough.detail = std::max(params_.evalFloat("detail", ctx, 0.008f), 1e-3f);
        rough.seed = static_cast<uint32_t>(seed * 0x9E3779B1ull + 11);
        rough.grain = grain;
        rough.splinter = std::max(params_.evalFloat("splinter", ctx, 0.05f), 0.0f);
        rough.splinterSize = std::max(params_.evalFloat("splintersize", ctx, 0.012f), 1e-3f);

        // The cells, long along the fibres; their faces torn -- how far from
        // the outside counted over all of them, so both sides of a crack
        // agree.
        std::vector<std::shared_ptr<Geometry>> cells = grainCells(mesh, seeds, grain, stretch, group);
        if (ctx.interrupted()) return nullptr;
        const RoughAnchors anchors(cells, group, "", 2.0f * rough.amount);
        std::vector<std::shared_ptr<Geometry>> pieces(cells.size());
        parallelForEach(cells.size(), [&](size_t i) { pieces[i] = roughenCuts(*cells[i], group, "", rough, anchors); });
        if (ctx.interrupted()) return nullptr;
        auto out = std::make_shared<Geometry>();
        int32_t number = 0;
        for (const std::shared_ptr<Geometry>& piece : pieces) {
            Geometry& g = *piece;
            if (g.primitiveCount() == 0) continue;
            if (!attribute.empty()) {
                auto pp = g.primitives().create(attribute, AttrType::Int).write<int32_t>();
                std::fill(pp.begin(), pp.end(), number);
                auto pt = g.points().create(attribute, AttrType::Int).write<int32_t>();
                std::fill(pt.begin(), pt.end(), number);
            }
            auto fibres = g.points().create("grain", AttrType::Vec3).write<Vec3>();
            std::fill(fibres.begin(), fibres.end(), grain);
            ++number;
            out->append(g);
        }
        if (!out->findGroup(group)) out->createGroup(group, AttrClass::Primitive);
        // For a renderer (s@material): wood, where the faces are nothing else.
        std::vector<uint8_t> plain(out->primitiveCount());
        for (size_t p = 0; p < plain.size(); ++p) plain[p] = primitiveString(*out, "material", p).empty();
        setPrimitiveString(*out, "material", "wood", plain);
        return out;
    }

private:
    /// f(i) for each i below n, in parallel.
    template <typename F>
    static void parallelForEach(size_t n, F&& f) {
        parallelFor(n, 1, [&](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i) f(i);
        });
    }
};

}  // namespace

void registerWoodNodes() {
    NodeRegistry::instance().add("woodfracture", [](const std::string& n) { return std::make_unique<WoodFractureNode>(n); });
}

}  // namespace pg
