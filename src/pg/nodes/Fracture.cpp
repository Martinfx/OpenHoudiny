// Voronoi Fracture: a closed mesh cut into the cells of points -- each cell
// the part of it nearer its point than any other -- as the pieces of
// something broken. A cell is the mesh clipped by the planes half way to
// the other points (clipGeometry, nearest first), each cut capped; so a
// piece is closed as the mesh was, and the pieces together are the mesh.
//
//   points given (the second input), or Count of them, random -- the same
//   for the same Seed -- inside the mesh
//
// The pieces carry `piece` -- their number, from 0 -- on their primitives
// and points, and their cut faces are in the group Inside: what a rigid
// body solver takes apart, and what a shader paints as the broken inside.
#include "pg/nodes/Nodes.h"

#include "pg/core/Parallel.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace pg {
namespace {

uint64_t splitmix(uint64_t& state) {
    uint64_t z = (state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

float unit(uint64_t& state) { return static_cast<float>(splitmix(state) >> 40) / static_cast<float>(1ull << 24); }

}  // namespace

bool insideMesh(const Geometry& geo, const Vec3& p) {
    const Vec3 dir = normalize(Vec3(1.0f, 0.1234567f, 0.0765432f));
    const auto P = geo.positions();
    int crossings = 0;
    for (size_t prim = 0; prim < geo.primitiveCount(); ++prim) {
        if (!geo.primitiveClosed(prim)) continue;
        const auto c = geo.primitivePoints(prim);
        for (size_t i = 1; i + 1 < c.size(); ++i) {
            // Moller-Trumbore, the fan's triangle (0, i, i + 1).
            const Vec3 a = P[c[0]], b = P[c[i]], d = P[c[i + 1]];
            const Vec3 e1 = b - a, e2 = d - a;
            const Vec3 h = cross(dir, e2);
            const float det = dot(e1, h);
            if (std::fabs(det) < 1e-12f) continue;
            const float inv = 1.0f / det;
            const Vec3 s = p - a;
            const float u = dot(s, h) * inv;
            if (u < 0.0f || u > 1.0f) continue;
            const Vec3 q = cross(s, e1);
            const float v = dot(dir, q) * inv;
            if (v < 0.0f || u + v > 1.0f) continue;
            if (dot(e2, q) * inv > 0.0f) ++crossings;
        }
    }
    return (crossings & 1) != 0;
}

std::shared_ptr<Geometry> voronoiCell(const GeometryPtr& mesh, const std::vector<Vec3>& seeds, size_t i,
                                      const std::string& inside) {
    std::vector<size_t> others(seeds.size());
    std::iota(others.begin(), others.end(), size_t{0});
    others.erase(others.begin() + static_cast<long>(i));
    const Vec3 s = seeds[i];
    std::stable_sort(others.begin(), others.end(), [&](size_t a, size_t b) {
        return length(seeds[a] - s) < length(seeds[b] - s);
    });
    std::shared_ptr<Geometry> piece = std::make_shared<Geometry>(*mesh);
    for (const size_t j : others) {
        const Vec3 dir = s - seeds[j];
        if (length(dir) < 1e-7f) continue;  // two seeds on one place: the first keeps it
        const Vec3 origin = (s + seeds[j]) * 0.5f;
        const Vec3 n = normalize(dir);
        // All of it on this side: nothing to cut.
        float least = 1e30f;
        for (const Vec3& p : piece->positions()) least = std::min(least, dot(p - origin, n));
        if (least >= 0.0f) continue;
        piece = clipGeometry(*piece, origin, n, true, inside);
        if (piece->primitiveCount() == 0) break;
    }
    return piece;
}

namespace {

class VoronoiFractureNode : public Node {
public:
    explicit VoronoiFractureNode(std::string name) : Node("voronoifracture", std::move(name)) {
        setInputCount(2);
        params_.setInt("count", 20);
        params_.setInt("seed", 1);
        params_.setString("attribute", "piece");
        params_.setString("insidegroup", "inside");
    }

    GeometryPtr cookNode(const CookContext& ctx, std::span<const GeometryPtr> in) override {
        if (in.empty() || !in[0]) return std::make_shared<Geometry>();
        const GeometryPtr mesh = in[0];
        std::vector<Vec3> seeds;
        if (in.size() > 1 && in[1] && in[1]->pointCount() > 0) {
            const auto P = in[1]->positions();
            seeds.assign(P.begin(), P.end());
        } else {
            seeds = randomInside(*mesh, std::clamp(params_.evalInt("count", ctx, 20), 0, 10000),
                                 static_cast<uint64_t>(params_.evalInt("seed", ctx, 1)));
        }
        const std::string attribute = params_.getString("attribute", "piece");
        const std::string inside = params_.getString("insidegroup", "inside");
        // Each cell by itself -- in parallel, put together in order.
        std::vector<std::shared_ptr<Geometry>> cells(seeds.size());
        parallelFor(seeds.size(), 1, [&](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i) {
                if (ctx.interrupted()) return;
                cells[i] = voronoiCell(mesh, seeds, i, inside);
            }
        });
        if (ctx.interrupted()) return nullptr;
        auto out = std::make_shared<Geometry>();
        int32_t number = 0;
        for (auto& c : cells) {
            if (!c || c->primitiveCount() == 0) continue;
            if (!attribute.empty()) {
                auto pp = c->primitives().create(attribute, AttrType::Int).write<int32_t>();
                std::fill(pp.begin(), pp.end(), number);
                auto pt = c->points().create(attribute, AttrType::Int).write<int32_t>();
                std::fill(pt.begin(), pt.end(), number);
            }
            ++number;
            out->append(*c);
        }
        // A group only some pieces have reaches the rest empty: made whole.
        if (!inside.empty() && !out->findGroup(inside)) out->createGroup(inside, AttrClass::Primitive);
        return out;
    }

private:
    /// `count` points inside `mesh`, the same for the same `seed`: drawn in
    /// the box round it, those outside drawn again.
    static std::vector<Vec3> randomInside(const Geometry& mesh, int count, uint64_t seed) {
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
        uint64_t state = seed * 0x2545F4914F6CDD1Dull + 1;
        const int tries = count * 200;
        for (int t = 0; t < tries && static_cast<int>(out.size()) < count; ++t) {
            const Vec3 p(lo.x + (hi.x - lo.x) * unit(state), lo.y + (hi.y - lo.y) * unit(state),
                         lo.z + (hi.z - lo.z) * unit(state));
            if (insideMesh(mesh, p)) out.push_back(p);
        }
        return out;
    }
};

}  // namespace

void registerFractureNodes() {
    NodeRegistry::instance().add("voronoifracture", [](const std::string& n) { return std::make_unique<VoronoiFractureNode>(n); });
}

}  // namespace pg
