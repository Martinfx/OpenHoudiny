// Concrete Fracture: a closed mesh broken as concrete breaks -- chunks of
// every size, corners chipped off, the faces of the cracks rough:
//
//   seeds    Count points inside the mesh -- or those linked into Points --
//            denser where a noise says (Uneven) and round Impact (Focus,
//            within Reach): big chunks and small ones, the smallest where
//            it was struck
//   cells    each point's Voronoi cell (voronoiCell, as Voronoi Fracture)
//   rough    the cut faces cut into triangles Detail across and moved by
//            smooth 3D noise, Rough in and out, its bumps Rough Scale apart.
//            The same noise on both sides of a crack and the same triangles,
//            so the pieces still fit; less and less towards the outside,
//            which stays as it was -- so nothing pokes out of the object
//   spalls   Chips of the cells' corners cut off by a plane Chip Size deep,
//            through the rough faces: a flat spall, a small piece of its own
//            glued on by that face
//
// Where each point was before the cut faces were made rough stays in the
// point attribute proxy: the RBD Solver simulates the pieces as the proxy
// has them -- convex, face to face, glued where they touch -- and draws them
// as they are. The pieces carry piece (on primitives and points), the cut
// faces are in Inside Group, the spalls' primitives have chip 1.
#include "pg/core/Parallel.h"
#include "pg/nodes/Nodes.h"
#include "pg/nodes/Rebuild.h"
#include "pg/nodes/Rough.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numeric>
#include <unordered_map>

namespace pg {
namespace {

constexpr const char* kSpall = "__spall";  // the spalls' flat faces, while the node works

uint64_t splitmix(uint64_t& state) {
    uint64_t z = (state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

float unit(uint64_t& state) { return static_cast<float>(splitmix(state) >> 40) / static_cast<float>(1ull << 24); }

// --- where the pieces are: seeds ------------------------------------------------------

void boundsOf(const Geometry& g, Vec3& lo, Vec3& hi) {
    lo = Vec3(1e30f, 1e30f, 1e30f);
    hi = Vec3(-1e30f, -1e30f, -1e30f);
    for (const Vec3& p : g.positions()) {
        for (int a = 0; a < 3; ++a) {
            lo[a] = std::min(lo[a], p[a]);
            hi[a] = std::max(hi[a], p[a]);
        }
    }
}

/// `count` points inside `mesh`, the same for the same `seed`: drawn where
/// the density is -- a noise, Uneven strong, times a bump round `impact`,
/// Focus high and Reach wide.
std::vector<Vec3> seedsInside(const Geometry& mesh, int count, float uneven, const Vec3& impact, float focus,
                              float reach, uint64_t seed) {
    std::vector<Vec3> out;
    if (mesh.pointCount() == 0 || count <= 0) return out;
    Vec3 lo, hi;
    boundsOf(mesh, lo, hi);
    const Vec3 size = hi - lo;
    const float extent = std::max({size.x, size.y, size.z, 1e-3f});
    const float spread = 4.0f * std::clamp(uneven, 0.0f, 1.0f);
    const float boost = 15.0f * std::clamp(focus, 0.0f, 1.0f);
    const float r2 = 2.0f * std::max(reach, 1e-3f) * std::max(reach, 1e-3f);
    const uint32_t noiseSeed = static_cast<uint32_t>(seed * 2654435761ull);
    const Vec3 offset(0.37f, 0.61f, 0.19f);  // off the lattice, where the noise is not 0
    auto density = [&](const Vec3& p) {
        const float n = spread > 0.0f ? gradientNoise(p * (3.0f / extent) + offset, noiseSeed) : 0.0f;
        const Vec3 d = p - impact;
        return std::exp(spread * n) * (1.0f + boost * std::exp(-dot(d, d) / r2));
    };
    const float most = std::exp(spread) * (1.0f + boost);
    uint64_t state = seed * 0x2545F4914F6CDD1Dull + 1;
    for (long tries = static_cast<long>(count) * 4000; tries > 0 && static_cast<int>(out.size()) < count; --tries) {
        const Vec3 p(lo.x + size.x * unit(state), lo.y + size.y * unit(state), lo.z + size.z * unit(state));
        if (unit(state) * most > density(p)) continue;
        if (insideMesh(mesh, p)) out.push_back(p);
    }
    return out;
}

// --- spalls: corners chipped off --------------------------------------------------------

struct Piece {
    std::shared_ptr<Geometry> geo;
    bool chip = false;
};

/// A corner of a cell -- a point where three or more faces meet -- and the
/// way it points out: its faces' directions together.
struct Corner {
    uint32_t point;
    Vec3 out;
};

std::vector<Corner> cornersOf(const Geometry& cell) {
    const size_t n = cell.pointCount();
    std::vector<Vec3> facing(n);
    std::vector<int> faces(n, 0);
    for (size_t prim = 0; prim < cell.primitiveCount(); ++prim) {
        const auto c = cell.primitivePoints(prim);
        if (c.size() < 3 || !cell.primitiveClosed(prim)) continue;
        const Vec3 d = normalize(polygonNormal(cell, c));
        for (const uint32_t pt : c) {
            facing[pt] += d;
            ++faces[pt];
        }
    }
    std::vector<Corner> out;
    for (size_t pt = 0; pt < n; ++pt) {
        if (faces[pt] >= 3 && length(facing[pt]) >= 0.5f) out.push_back({static_cast<uint32_t>(pt), normalize(facing[pt])});
    }
    return out;
}

/// A piece and the spalls chipped off its corners -- `chips` of `corners`,
/// found on the plain cell: each cut off by a plane across the way it
/// points, tilted at random, `chipSize` deep or a little less, as long as
/// what comes off is small and what stays keeps the middle. Cut where the
/// piece is -- through its rough faces, which are the same on the other
/// side of the crack whatever is cut from them here. The piece first, then
/// its spalls.
std::vector<Piece> chipped(std::shared_ptr<Geometry> piece, const std::vector<Corner>& corners, float chips, float chipSize,
                           uint64_t seed) {
    std::vector<Piece> out;
    Vec3 lo, hi;
    boundsOf(*piece, lo, hi);
    const float extent = std::max({hi.x - lo.x, hi.y - lo.y, hi.z - lo.z});
    if (chips <= 0.0f || chipSize <= 0.0f || corners.empty() || extent < 4.0f * chipSize) {
        out.push_back({piece, false});
        return out;
    }
    Vec3 middle;
    const auto P = piece->positions();
    for (const Vec3& p : P) middle += p;
    middle = middle * (1.0f / static_cast<float>(std::max<size_t>(P.size(), 1)));
    uint64_t state = seed;
    struct Cut {
        Vec3 at, n;
        float depth;
    };
    std::vector<Cut> cuts;
    for (const Corner& corner : corners) {
        const bool chosen = unit(state) < chips;
        const Vec3 tilt(2.0f * unit(state) - 1.0f, 2.0f * unit(state) - 1.0f, 2.0f * unit(state) - 1.0f);
        const float depth = chipSize * (0.55f + 0.45f * unit(state));
        if (!chosen || corner.point >= P.size()) continue;
        const Vec3 n = normalize(corner.out + tilt * 0.45f);
        cuts.push_back({P[corner.point], n, depth});
    }
    std::shared_ptr<Geometry> left = std::move(piece);
    for (const Cut& cut : cuts) {
        // Still a corner of what is left: another spall may have taken it.
        bool there = false;
        for (const Vec3& p : left->positions()) there = there || length(p - cut.at) < 1e-5f * extent;
        if (!there) continue;
        const Vec3 at = cut.at - cut.n * cut.depth;
        if (dot(middle - at, cut.n) > -0.5f * cut.depth) continue;  // it would take the middle
        std::shared_ptr<Geometry> chip = clipGeometry(*left, at, cut.n, true, kSpall);
        if (chip->primitiveCount() < 4) continue;
        Vec3 clo, chi;
        boundsOf(*chip, clo, chi);
        if (length(chi - clo) > 3.0f * chipSize) continue;  // not a spall: a slice
        std::shared_ptr<Geometry> rest = clipGeometry(*left, at, -cut.n, true, kSpall);
        if (rest->primitiveCount() < 4) continue;
        out.push_back({chip, true});
        left = std::move(rest);
    }
    out.insert(out.begin(), Piece{left, false});
    return out;
}

class ConcreteFractureNode : public Node {
public:
    explicit ConcreteFractureNode(std::string name) : Node("concretefracture", std::move(name)) {
        setInputCount(2);
        params_.setInt("count", 60);
        params_.setInt("seed", 1);
        params_.setFloat("uneven", 0.5f);
        params_.setVec3("impact", Vec3(0.0f, 1.0f, 0.0f));
        params_.setFloat("focus", 0.0f);
        params_.setFloat("reach", 1.0f);
        params_.setFloat("chips", 0.2f);
        params_.setFloat("chipsize", 0.06f);
        params_.setFloat("rough", 0.02f);
        params_.setFloat("roughscale", 0.3f);
        params_.setFloat("detail", 0.03f);
        params_.setString("attribute", "piece");
        params_.setString("insidegroup", "inside");
    }

    GeometryPtr cookNode(const CookContext& ctx, std::span<const GeometryPtr> in) override {
        if (in.empty() || !in[0] || in[0]->primitiveCount() == 0) return std::make_shared<Geometry>();
        const GeometryPtr mesh = in[0];
        const uint64_t seed = static_cast<uint64_t>(std::max(params_.evalInt("seed", ctx, 1), 0));
        std::vector<Vec3> seeds;
        if (in.size() > 1 && in[1] && in[1]->pointCount() > 0) {
            const auto P = in[1]->positions();
            seeds.assign(P.begin(), P.end());
        } else {
            seeds = seedsInside(*mesh, std::clamp(params_.evalInt("count", ctx, 60), 0, 10000),
                                params_.evalFloat("uneven", ctx, 0.5f), params_.evalVec3("impact", ctx, Vec3(0.0f, 1.0f, 0.0f)),
                                params_.evalFloat("focus", ctx, 0.0f), params_.evalFloat("reach", ctx, 1.0f), seed);
        }
        const std::string attribute = params_.getString("attribute", "piece");
        const std::string inside = params_.getString("insidegroup", "inside");
        const float chips = std::clamp(params_.evalFloat("chips", ctx, 0.2f), 0.0f, 1.0f);
        const float chipSize = std::max(params_.evalFloat("chipsize", ctx, 0.06f), 0.0f);
        RoughCut rough;
        rough.amount = std::max(params_.evalFloat("rough", ctx, 0.02f), 0.0f);
        rough.scale = std::max(params_.evalFloat("roughscale", ctx, 0.3f), 1e-3f);
        rough.detail = std::max(params_.evalFloat("detail", ctx, 0.03f), 1e-3f);
        rough.seed = static_cast<uint32_t>(seed * 0x9E3779B1ull + 7);
        const std::string group = inside.empty() ? std::string("inside") : inside;

        // The cells -- each by itself, in parallel.
        std::vector<std::shared_ptr<Geometry>> cells(seeds.size());
        const VoronoiCells cutter(mesh, seeds);
        parallelFor(seeds.size(), 1, [&](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i) {
                if (ctx.interrupted()) return;
                std::shared_ptr<Geometry> cell = cutter.cell(i, group);
                if (cell && cell->primitiveCount() > 0) cells[i] = std::move(cell);
            }
        });
        if (ctx.interrupted()) return nullptr;
        // Their cut faces rough -- how far from the outside counted over all
        // of them, so both sides of a crack agree -- and then their corners
        // chipped off.
        const RoughAnchors anchors(cells, group, kSpall, 2.0f * rough.amount);
        std::vector<std::vector<Piece>> byCell(cells.size());
        parallelFor(cells.size(), 1, [&](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i) {
                if (ctx.interrupted() || !cells[i]) continue;
                const std::vector<Corner> corners = cornersOf(*cells[i]);
                byCell[i] = chipped(roughenCuts(*cells[i], group, kSpall, rough, anchors), corners, chips, chipSize,
                                    seed * 0x94D049BB133111EBull + (i + 1) * 0x9E3779B97F4A7C15ull);
            }
        });
        if (ctx.interrupted()) return nullptr;
        std::vector<Piece> pieces;
        for (auto& cell : byCell) {
            for (Piece& p : cell) pieces.push_back(std::move(p));
        }
        auto out = std::make_shared<Geometry>();
        int32_t number = 0;
        for (size_t i = 0; i < pieces.size(); ++i) {
            Geometry& g = *pieces[i].geo;
            if (g.primitiveCount() == 0) continue;
            if (!attribute.empty()) {
                auto pp = g.primitives().create(attribute, AttrType::Int).write<int32_t>();
                std::fill(pp.begin(), pp.end(), number);
                auto pt = g.points().create(attribute, AttrType::Int).write<int32_t>();
                std::fill(pt.begin(), pt.end(), number);
            }
            auto chip = g.primitives().create("chip", AttrType::Int).write<int32_t>();
            std::fill(chip.begin(), chip.end(), pieces[i].chip ? 1 : 0);
            // The spalls' faces are broken inside too.
            Group& cut = g.createGroup(group, AttrClass::Primitive);
            if (const Group* flat = g.findGroup(kSpall)) {
                for (size_t prim = 0; prim < g.primitiveCount(); ++prim) {
                    if (flat->contains(prim)) cut.set(prim, true);
                }
            }
            g.eraseGroup(kSpall);
            ++number;
            out->append(g);
        }
        if (!out->findGroup(group)) out->createGroup(group, AttrClass::Primitive);
        // For a renderer (s@material): the faces of the cracks broken
        // concrete; the rest what they were, concrete where they were not
        // anything.
        const Group* cut = out->findGroup(group);
        std::vector<uint8_t> broken(out->primitiveCount()), plain(out->primitiveCount());
        for (size_t p = 0; p < broken.size(); ++p) {
            broken[p] = cut->contains(p);
            plain[p] = !broken[p] && primitiveString(*out, "material", p).empty();
        }
        setPrimitiveString(*out, "material", "concrete", plain);
        setPrimitiveString(*out, "material", "broken_concrete", broken);
        return out;
    }
};

}  // namespace

void registerConcreteNodes() {
    NodeRegistry::instance().add("concretefracture", [](const std::string& n) { return std::make_unique<ConcreteFractureNode>(n); });
}

}  // namespace pg
