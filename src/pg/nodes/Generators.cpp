#include "pg/nodes/Nodes.h"

#include "pg/core/Geometry.h"
#include "pg/core/Parallel.h"

#include <cmath>

namespace pg {
namespace {

/// Rectangular polygon grid in the XZ plane.
class GridNode : public Node {
public:
    explicit GridNode(std::string name) : Node("grid", std::move(name)) {
        setInputCount(0);
        params_.setInt("rows", 10);
        params_.setInt("cols", 10);
        params_.setFloat("sizex", 1.0f);
        params_.setFloat("sizez", 1.0f);
    }

    GeometryPtr cookNode(const CookContext& ctx,
                         std::span<const GeometryPtr>) override {
        const int rows = std::max(2, params_.getInt("rows", 10));
        const int cols = std::max(2, params_.getInt("cols", 10));
        const float sx = params_.evalFloat("sizex", ctx, 1.0f);
        const float sz = params_.evalFloat("sizez", ctx, 1.0f);

        auto geo = std::make_shared<Geometry>();
        geo->addPoints(static_cast<size_t>(rows) * cols);

        auto P = geo->positionsForWrite();
        const float dx = sx / static_cast<float>(cols - 1);
        const float dz = sz / static_cast<float>(rows - 1);
        parallelFor(static_cast<size_t>(rows), 16, [&](size_t r0, size_t r1) {
            for (size_t r = r0; r < r1; ++r) {
                for (int c = 0; c < cols; ++c) {
                    P[r * cols + c] = Vec3(-sx * 0.5f + dx * static_cast<float>(c),
                                           0.0f,
                                           -sz * 0.5f + dz * static_cast<float>(r));
                }
            }
        });

        uint32_t quad[4];
        for (int r = 0; r + 1 < rows; ++r) {
            for (int c = 0; c + 1 < cols; ++c) {
                quad[0] = static_cast<uint32_t>(r * cols + c);
                quad[1] = static_cast<uint32_t>(r * cols + c + 1);
                quad[2] = static_cast<uint32_t>((r + 1) * cols + c + 1);
                quad[3] = static_cast<uint32_t>((r + 1) * cols + c);
                geo->addPrimitive(std::span<const uint32_t>(quad, 4), true);
            }
        }
        return geo;
    }
};

/// Open polyline along +X.
class LineNode : public Node {
public:
    explicit LineNode(std::string name) : Node("line", std::move(name)) {
        setInputCount(0);
        params_.setInt("points", 10);
        params_.setFloat("length", 1.0f);
    }

    GeometryPtr cookNode(const CookContext& ctx,
                         std::span<const GeometryPtr>) override {
        const int n = std::max(2, params_.getInt("points", 10));
        const float len = params_.evalFloat("length", ctx, 1.0f);

        auto geo = std::make_shared<Geometry>();
        geo->addPoints(static_cast<size_t>(n));
        auto P = geo->positionsForWrite();
        std::vector<uint32_t> idx(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i) {
            P[i] = Vec3(len * static_cast<float>(i) / static_cast<float>(n - 1), 0.0f, 0.0f);
            idx[i] = static_cast<uint32_t>(i);
        }
        geo->addPrimitive(idx, false);
        return geo;
    }
};

/// Loose points on a deterministic pseudo-random cloud. No topology.
/// Useful as a cheap large-N source for benchmarks.
class PointCloudNode : public Node {
public:
    explicit PointCloudNode(std::string name) : Node("pointcloud", std::move(name)) {
        setInputCount(0);
        params_.setInt("count", 1000);
        params_.setInt("seed", 0);
        params_.setFloat("size", 1.0f);
    }

    GeometryPtr cookNode(const CookContext& ctx,
                         std::span<const GeometryPtr>) override {
        const size_t n = static_cast<size_t>(std::max(0, params_.getInt("count", 1000)));
        const uint32_t seed = static_cast<uint32_t>(params_.getInt("seed", 0));
        const float size = params_.evalFloat("size", ctx, 1.0f);

        auto geo = std::make_shared<Geometry>();
        geo->addPoints(n);
        auto P = geo->positionsForWrite();

        // Index-derived hashing, not a sequential RNG: point i depends only on
        // i and the seed, so the result cannot depend on how work was split.
        parallelFor(n, 8192, [&](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i) {
                auto h = [&](uint32_t salt) {
                    uint32_t x = static_cast<uint32_t>(i) * 0x9e3779b9u ^ (seed + salt * 0x85ebca6bu);
                    x ^= x >> 16; x *= 0x7feb352du;
                    x ^= x >> 15; x *= 0x846ca68bu;
                    x ^= x >> 16;
                    return static_cast<float>(x) * (1.0f / 4294967296.0f) - 0.5f;
                };
                P[i] = Vec3(h(1) * size, h(2) * size, h(3) * size);
            }
        });
        return geo;
    }
};

}  // namespace

void registerGeneratorNodes() {
    auto& r = NodeRegistry::instance();
    r.add("grid", [](const std::string& n) { return std::make_unique<GridNode>(n); });
    r.add("line", [](const std::string& n) { return std::make_unique<LineNode>(n); });
    r.add("pointcloud", [](const std::string& n) { return std::make_unique<PointCloudNode>(n); });
}

}  // namespace pg
