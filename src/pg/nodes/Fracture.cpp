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
#include "pg/nodes/Rebuild.h"

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
    return VoronoiCells(mesh, seeds).cell(i, inside);
}

VoronoiCells::VoronoiCells(GeometryPtr mesh, std::vector<Vec3> seeds) : mesh_(std::move(mesh)), seeds_(std::move(seeds)) {
    if (!mesh_) mesh_ = std::make_shared<Geometry>();
    const Geometry& g = *mesh_;
    const auto P = g.positions();
    // The parts: the primitives that share points, each with its box.
    std::vector<uint32_t> up(P.size());
    std::iota(up.begin(), up.end(), 0u);
    auto root = [&](uint32_t a) {
        while (up[a] != a) a = up[a] = up[up[a]];
        return a;
    };
    for (size_t p = 0; p < g.primitiveCount(); ++p) {
        const auto c = g.primitivePoints(p);
        for (size_t k = 1; k < c.size(); ++k) {
            const uint32_t a = root(c[0]), b = root(c[k]);
            if (a != b) up[std::max(a, b)] = std::min(a, b);
        }
    }
    std::vector<int32_t> partOf(P.size(), -1);
    Vec3 lo(1e30f, 1e30f, 1e30f), hi(-1e30f, -1e30f, -1e30f);
    for (size_t p = 0; p < g.primitiveCount(); ++p) {
        const auto c = g.primitivePoints(p);
        if (c.empty()) continue;
        const uint32_t r = root(c[0]);
        if (partOf[r] < 0) {
            partOf[r] = static_cast<int32_t>(parts_.size());
            parts_.push_back({{}, Vec3(1e30f, 1e30f, 1e30f), Vec3(-1e30f, -1e30f, -1e30f)});
        }
        Part& part = parts_[static_cast<size_t>(partOf[r])];
        part.prims.push_back(static_cast<uint32_t>(p));
        for (const uint32_t q : c) {
            for (int a = 0; a < 3; ++a) {
                part.lo[a] = std::min(part.lo[a], P[q][a]);
                part.hi[a] = std::max(part.hi[a], P[q][a]);
                lo[a] = std::min(lo[a], P[q][a]);
                hi[a] = std::max(hi[a], P[q][a]);
            }
        }
    }
    if (parts_.empty()) lo = hi = Vec3();
    // Far beyond what rounding does to a distance of this scene -- a
    // plane no nearer than that to a piece does not cut it.
    float scale = length(hi - lo);
    for (int a = 0; a < 3; ++a) scale = std::max({scale, std::fabs(lo[a]), std::fabs(hi[a])});
    margin_ = 1e-4f * scale + 1e-6f;
    // The box round the mesh, a little bigger: a cell before the mesh is in it.
    auto box = std::make_shared<Geometry>();
    box->addPoints(8);
    const auto B = box->positionsForWrite();
    const Vec3 blo = lo - Vec3(10.0f * margin_), bhi = hi + Vec3(10.0f * margin_);
    for (uint32_t k = 0; k < 8; ++k) B[k] = Vec3(k & 1 ? bhi.x : blo.x, k & 2 ? bhi.y : blo.y, k & 4 ? bhi.z : blo.z);
    const uint32_t faces[6][4] = {{0, 2, 3, 1}, {4, 5, 7, 6}, {0, 1, 5, 4}, {2, 6, 7, 3}, {0, 4, 6, 2}, {1, 3, 7, 5}};
    for (const auto& f : faces) box->addPrimitive(std::span<const uint32_t>(f, 4));
    box_ = box;
    // The seeds in a grid of about one a cell.
    if (seeds_.empty()) return;
    Vec3 slo = seeds_[0], shi = seeds_[0];
    for (const Vec3& q : seeds_) {
        for (int a = 0; a < 3; ++a) {
            slo[a] = std::min(slo[a], q[a]);
            shi[a] = std::max(shi[a], q[a]);
        }
    }
    const Vec3 extent = shi - slo;
    reachAll_ = length(extent) * 1.001f + margin_;
    float volume = 1.0f;
    const float least = std::max(1e-3f * length(extent), 1e-6f);
    for (int a = 0; a < 3; ++a) volume *= std::max(extent[a], least);
    gridCell_ = std::max(std::cbrt(volume / static_cast<float>(seeds_.size())), least);
    gridLo_ = slo;
    size_t cells = 1;
    for (int a = 0; a < 3; ++a) {
        dims_[a] = std::clamp(static_cast<int>(extent[a] / gridCell_) + 1, 1, 512);
        cells *= static_cast<size_t>(dims_[a]);
    }
    auto cellOf = [&](const Vec3& q) {
        size_t at = 0;
        for (int a = 2; a >= 0; --a) {
            const int k = std::clamp(static_cast<int>((q[a] - gridLo_[a]) / gridCell_), 0, dims_[a] - 1);
            at = at * static_cast<size_t>(dims_[a]) + static_cast<size_t>(k);
        }
        return at;
    };
    gridStart_.assign(cells + 1, 0);
    for (const Vec3& q : seeds_) ++gridStart_[cellOf(q) + 1];
    for (size_t c = 0; c < cells; ++c) gridStart_[c + 1] += gridStart_[c];
    gridSeeds_.resize(seeds_.size());
    std::vector<uint32_t> fill(gridStart_.begin(), gridStart_.end() - 1);
    for (size_t j = 0; j < seeds_.size(); ++j) gridSeeds_[fill[cellOf(seeds_[j])]++] = static_cast<uint32_t>(j);
}

void VoronoiCells::shell(size_t i, float from, float to, std::vector<std::pair<float, uint32_t>>& out) const {
    const Vec3 s = seeds_[i];
    int k0[3], k1[3];
    for (int a = 0; a < 3; ++a) {
        k0[a] = std::clamp(static_cast<int>(std::floor((s[a] - to - gridLo_[a]) / gridCell_)), 0, dims_[a] - 1);
        k1[a] = std::clamp(static_cast<int>(std::floor((s[a] + to - gridLo_[a]) / gridCell_)), 0, dims_[a] - 1);
    }
    for (int z = k0[2]; z <= k1[2]; ++z) {
        for (int y = k0[1]; y <= k1[1]; ++y) {
            for (int x = k0[0]; x <= k1[0]; ++x) {
                const size_t c = (static_cast<size_t>(z) * static_cast<size_t>(dims_[1]) + static_cast<size_t>(y)) *
                                     static_cast<size_t>(dims_[0]) + static_cast<size_t>(x);
                for (uint32_t e = gridStart_[c]; e < gridStart_[c + 1]; ++e) {
                    const uint32_t j = gridSeeds_[e];
                    if (j == i) continue;
                    // The distance as a sort of them all would have it.
                    const float d = length(seeds_[j] - s);
                    if (d > from && d <= to) out.push_back({d, j});
                }
            }
        }
    }
    std::sort(out.begin(), out.end());
}

std::shared_ptr<Geometry> VoronoiCells::cut(size_t i, std::shared_ptr<Geometry> piece, const std::string& group) const {
    const Vec3 s = seeds_[i];
    auto radius = [&](const Geometry& g) {
        float r = 0.0f;
        for (const Vec3& p : g.positions()) r = std::max(r, length(p - s));
        return r;
    };
    float reach = radius(*piece);
    // The seeds nearest first, a shell of them at a time -- each twice as
    // far out as the one before; one further than twice the piece's reach
    // from seed i cuts nothing -- nor any after it.
    std::vector<std::pair<float, uint32_t>> near;
    size_t at = 0;
    float done = -1.0f;
    for (;;) {
        if (at == near.size()) {
            const float need = 2.0f * reach + margin_;
            if (done >= need || done >= reachAll_) break;
            const float to = done < 0.0f ? 2.0f * gridCell_ : 2.0f * done;
            near.clear();
            at = 0;
            shell(i, done, to, near);
            done = to;
            continue;
        }
        const auto [distance, j] = near[at++];
        if (distance > 2.0f * reach + margin_) break;
        const Vec3 dir = s - seeds_[j];
        if (length(dir) < 1e-7f) continue;  // two seeds on one place: the first keeps it
        const Vec3 origin = (s + seeds_[j]) * 0.5f;
        const Vec3 n = normalize(dir);
        // All of it on this side: nothing to cut.
        float least = 1e30f;
        for (const Vec3& p : piece->positions()) least = std::min(least, dot(p - origin, n));
        if (least >= 0.0f) continue;
        piece = clipGeometry(*piece, origin, n, true, group);
        if (piece->primitiveCount() == 0) break;
        reach = radius(*piece);
    }
    return piece;
}

std::shared_ptr<Geometry> VoronoiCells::cell(size_t i, const std::string& inside) const {
    if (i >= seeds_.size()) return std::make_shared<Geometry>();
    // One part: all of it is cut.
    if (parts_.size() <= 1) return cut(i, std::make_shared<Geometry>(*mesh_), inside);
    // The cell in the box round the mesh -- a handful of faces: which parts
    // of the mesh it reaches.
    const std::shared_ptr<Geometry> hull = cut(i, std::make_shared<Geometry>(*box_), "");
    if (hull->primitiveCount() == 0) return hull;
    Vec3 lo(1e30f, 1e30f, 1e30f), hi(-1e30f, -1e30f, -1e30f);
    for (const Vec3& p : hull->positions()) {
        for (int a = 0; a < 3; ++a) {
            lo[a] = std::min(lo[a], p[a] - margin_);
            hi[a] = std::max(hi[a], p[a] + margin_);
        }
    }
    std::vector<uint32_t> prims;
    size_t reached = 0;
    for (const Part& part : parts_) {
        bool meets = true;
        for (int a = 0; a < 3; ++a) meets = meets && part.lo[a] <= hi[a] && part.hi[a] >= lo[a];
        if (!meets) continue;
        ++reached;
        prims.insert(prims.end(), part.prims.begin(), part.prims.end());
    }
    std::shared_ptr<Geometry> piece;
    if (reached == parts_.size()) {
        piece = std::make_shared<Geometry>(*mesh_);
    } else {
        // Those parts alone -- their points and primitives in the order they
        // had, so that what is cut of them is cut as it was of the whole.
        std::sort(prims.begin(), prims.end());
        const Geometry& g = *mesh_;
        std::vector<uint32_t> index(g.pointCount(), ~0u);
        for (const uint32_t p : prims) {
            for (const uint32_t q : g.primitivePoints(p)) index[q] = 0;
        }
        Blends points, vertices;
        for (size_t q = 0; q < index.size(); ++q) {
            if (index[q] == ~0u) continue;
            index[q] = static_cast<uint32_t>(points.size());
            points.one(static_cast<uint32_t>(q));
        }
        std::vector<std::vector<uint32_t>> faces;
        std::vector<uint8_t> closed;
        for (const uint32_t p : prims) {
            const auto c = g.primitivePoints(p);
            std::vector<uint32_t> face(c.size());
            for (size_t k = 0; k < c.size(); ++k) face[k] = index[c[k]];
            faces.push_back(std::move(face));
            closed.push_back(g.primitiveClosed(p) ? 1 : 0);
            const uint32_t v0 = static_cast<uint32_t>(g.primitiveVertexStart(p));
            for (size_t k = 0; k < c.size(); ++k) vertices.one(v0 + static_cast<uint32_t>(k));
        }
        piece = rebuild(g, points, faces, closed, vertices, prims);
    }
    return cut(i, piece, inside);
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
        const VoronoiCells cutter(mesh, seeds);
        parallelFor(seeds.size(), 1, [&](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i) {
                if (ctx.interrupted()) return;
                cells[i] = cutter.cell(i, inside);
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
