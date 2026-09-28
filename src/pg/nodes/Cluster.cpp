// RBD Cluster: the pieces of a fracture grouped into chunks, the glue
// between the pieces of one chunk Strength times as strong as between
// chunks -- so that a knock breaks a thing into chunks first, and a chunk
// breaks up only where a hard knock lands on it again: secondary fracture,
// as Houdini's RBD Cluster sets it up.
//
//   centres  each piece's middle: of its volume, as its proxy has it when
//            it has one (Concrete Fracture's)
//   seeds    Count of them: the first at random, each next far from those
//            chosen (k-means++), then moved to the middle of the pieces
//            nearest to it a few times, weighted by volume (Lloyd) -- chunks
//            of about one size, round rather than long
//   chunks   each piece in the chunk of its nearest seed: the primitive and
//            point attribute cluster, 1 and up in the order of the pieces
//            (0: in none), and the primitive attribute clusterglue, Strength
//
// The RBD Solver multiplies the glue between two pieces of one cluster by
// the smaller of their clusterglue.
#include "pg/nodes/Nodes.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>

namespace pg {
namespace {

uint64_t splitmix(uint64_t& state) {
    uint64_t z = (state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

double unit(uint64_t& state) { return static_cast<double>(splitmix(state) >> 11) / static_cast<double>(1ull << 53); }

class RbdClusterNode : public Node {
public:
    explicit RbdClusterNode(std::string name) : Node("rbdcluster", std::move(name)) {
        setInputCount(1);
        params_.setInt("count", 8);
        params_.setInt("seed", 1);
        params_.setFloat("strength", 5.0f);
        params_.setString("attribute", "piece");
    }

    GeometryPtr cookNode(const CookContext& ctx, std::span<const GeometryPtr> in) override {
        auto geo = editableCopy(in.empty() ? nullptr : in[0]);
        const AttributeArray* pieceAttr = geo->primitives().find(params_.getString("attribute", "piece"));
        if (!pieceAttr || pieceAttr->type() != AttrType::Int || geo->primitiveCount() == 0) return geo;
        const auto piece = pieceAttr->read<int32_t>();

        // The pieces, in the order of their numbers.
        std::map<int32_t, size_t> slot;
        for (const int32_t p : piece) slot.emplace(p, 0);
        size_t n = 0;
        for (auto& [p, s] : slot) s = n++;

        // Where each piece is, as the solver will have it: its proxy, where
        // it carries one.
        const auto P = geo->positions();
        std::vector<Vec3> at(P.begin(), P.end());
        if (const AttributeArray* proxy = geo->points().find("proxy"); proxy && proxy->type() == AttrType::Vec3) {
            const auto Q = proxy->read<Vec3>();
            for (size_t i = 0; i < at.size(); ++i) {
                if (!(Q[i] == Vec3() && length(at[i]) > 1e-3f)) at[i] = Q[i];
            }
        }
        // The middle of each piece's volume (the divergence theorem over the
        // fans of its faces); the middle of its points when it holds none.
        std::vector<double> volume(n, 0.0), count(n, 0.0);
        std::vector<std::array<double, 3>> moment(n, {0.0, 0.0, 0.0}), points(n, {0.0, 0.0, 0.0});
        for (size_t prim = 0; prim < geo->primitiveCount(); ++prim) {
            const size_t k = slot[piece[prim]];
            const auto c = geo->primitivePoints(prim);
            for (const uint32_t pt : c) {
                for (int a = 0; a < 3; ++a) points[k][static_cast<size_t>(a)] += at[pt][a];
                count[k] += 1.0;
            }
            if (!geo->primitiveClosed(prim)) continue;
            for (size_t i = 1; i + 1 < c.size(); ++i) {
                const Vec3 &a = at[c[0]], &b = at[c[i]], &d = at[c[i + 1]];
                const double v = static_cast<double>(dot(a, cross(b, d))) / 6.0;
                volume[k] += v;
                for (int ax = 0; ax < 3; ++ax) moment[k][static_cast<size_t>(ax)] += v * (a[ax] + b[ax] + d[ax]) / 4.0;
            }
        }
        std::vector<std::array<double, 3>> centre(n);
        std::vector<double> weight(n);
        double largest = 0.0;
        for (size_t k = 0; k < n; ++k) largest = std::max(largest, std::fabs(volume[k]));
        for (size_t k = 0; k < n; ++k) {
            const bool solid = std::fabs(volume[k]) > 1e-9 * std::max(largest, 1e-30);
            for (size_t a = 0; a < 3; ++a) {
                centre[k][a] = solid ? moment[k][a] / volume[k] : points[k][a] / std::max(count[k], 1.0);
            }
            weight[k] = solid ? std::fabs(volume[k]) : 1e-12;
        }
        auto distance2 = [](const std::array<double, 3>& a, const std::array<double, 3>& b) {
            const double x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
            return x * x + y * y + z * z;
        };

        // The seeds: k-means++, then Lloyd.
        const size_t k = static_cast<size_t>(std::clamp<int64_t>(params_.evalInt("count", ctx, 8), 1, static_cast<int64_t>(n)));
        uint64_t state = static_cast<uint64_t>(std::max(params_.evalInt("seed", ctx, 1), 0)) * 0x9E3779B97F4A7C15ull + 0x2545F4914F6CDD1Dull;
        std::vector<std::array<double, 3>> seeds;
        seeds.push_back(centre[std::min(static_cast<size_t>(unit(state) * static_cast<double>(n)), n - 1)]);
        std::vector<double> nearest(n, std::numeric_limits<double>::max());
        while (seeds.size() < k) {
            double total = 0.0;
            for (size_t i = 0; i < n; ++i) {
                nearest[i] = std::min(nearest[i], distance2(centre[i], seeds.back()));
                total += nearest[i] * weight[i];
            }
            if (total <= 0.0) break;  // the pieces all sit on the seeds
            double pick = unit(state) * total;
            size_t chosen = n - 1;
            for (size_t i = 0; i < n; ++i) {
                pick -= nearest[i] * weight[i];
                if (pick < 0.0) {
                    chosen = i;
                    break;
                }
            }
            seeds.push_back(centre[chosen]);
        }
        std::vector<size_t> chunk(n, 0);
        auto assign = [&] {
            for (size_t i = 0; i < n; ++i) {
                double best = std::numeric_limits<double>::max();
                for (size_t s = 0; s < seeds.size(); ++s) {
                    const double d = distance2(centre[i], seeds[s]);
                    if (d < best) {
                        best = d;
                        chunk[i] = s;
                    }
                }
            }
        };
        for (int round = 0; round < 8; ++round) {
            assign();
            std::vector<std::array<double, 3>> sum(seeds.size(), {0.0, 0.0, 0.0});
            std::vector<double> mass(seeds.size(), 0.0);
            for (size_t i = 0; i < n; ++i) {
                for (size_t a = 0; a < 3; ++a) sum[chunk[i]][a] += weight[i] * centre[i][a];
                mass[chunk[i]] += weight[i];
            }
            for (size_t s = 0; s < seeds.size(); ++s) {
                if (mass[s] <= 0.0) continue;  // none nearest: where it was
                for (size_t a = 0; a < 3; ++a) seeds[s][a] = sum[s][a] / mass[s];
            }
        }
        assign();
        // Numbered 1 and up in the order of the pieces.
        std::vector<int32_t> number(seeds.size(), 0);
        int32_t next = 0;
        for (size_t i = 0; i < n; ++i) {
            if (number[chunk[i]] == 0) number[chunk[i]] = ++next;
        }

        const float strength = std::max(params_.evalFloat("strength", ctx, 5.0f), 0.0f);
        auto primCluster = geo->primitives().create("cluster", AttrType::Int).write<int32_t>();
        auto primGlue = geo->primitives().create("clusterglue", AttrType::Float).write<float>();
        std::vector<int32_t> ofPoint(geo->pointCount(), 0);
        for (size_t prim = 0; prim < geo->primitiveCount(); ++prim) {
            const int32_t c = number[chunk[slot[piece[prim]]]];
            primCluster[prim] = c;
            primGlue[prim] = strength;
            for (const uint32_t pt : geo->primitivePoints(prim)) ofPoint[pt] = c;
        }
        auto pointCluster = geo->points().create("cluster", AttrType::Int).write<int32_t>();
        std::copy(ofPoint.begin(), ofPoint.end(), pointCluster.begin());
        return geo;
    }
};

}  // namespace

void registerClusterNodes() {
    NodeRegistry::instance().add("rbdcluster", [](const std::string& n) { return std::make_unique<RbdClusterNode>(n); });
}

}  // namespace pg
