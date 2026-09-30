#include "pg/core/Soft.h"

#include "pg/core/Parallel.h"
#include "pg/core/Spatial.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <queue>

namespace pg {

float falloff(Falloff shape, float x) {
    x = std::clamp(x, 0.0f, 1.0f);
    switch (shape) {
        case Falloff::Smooth: {
            const float f = 1.0f - x * x;
            return f * f;
        }
        case Falloff::Linear: return 1.0f - x;
        case Falloff::Sharp: {
            const float f = 1.0f - x;
            return f * f;
        }
        case Falloff::Sphere: return std::sqrt(std::max(0.0f, 1.0f - x * x));
        case Falloff::Constant: return x < 1.0f ? 1.0f : 0.0f;
    }
    return 0.0f;
}

std::vector<float> softWeights(const Geometry& geo, std::span<const uint8_t> picked, float radius, SoftDistance distance,
                               Falloff shape) {
    const size_t n = geo.pointCount();
    const auto P = geo.positions();
    std::vector<float> weight(n, 0.0f);
    std::vector<Vec3> sources;
    for (size_t i = 0; i < n && i < picked.size(); ++i) {
        if (!picked[i]) continue;
        weight[i] = 1.0f;
        sources.push_back(P[i]);
    }
    if (sources.empty() || !(radius > 0.0f)) return weight;

    if (distance == SoftDistance::Space) {
        // How far each other point is from the nearest one picked.
        const PointTree tree(sources);
        parallelFor(n, 4096, [&](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i) {
                if (weight[i] > 0.0f) continue;
                const int32_t j = tree.nearest(P[i], radius);
                if (j < 0) continue;
                weight[i] = falloff(shape, length(P[i] - sources[static_cast<size_t>(j)]) / radius);
            }
        });
        return weight;
    }

    // Along the surface: out from every point picked at once, nearest
    // first, through the edges. A point reached keeps the one picked its
    // way began at: straight from that one while that is no nearer than
    // where the way has got to, else the edges' length on from there.
    // Equally far, the lower number first: the same answer every time.
    Adjacency adjacency;
    adjacency.build(geo);
    constexpr float kFar = std::numeric_limits<float>::infinity();
    std::vector<float> dist(n, kFar);
    std::vector<uint32_t> from(n, 0);
    using Item = std::pair<float, uint32_t>;
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> front;
    for (size_t i = 0; i < n && i < picked.size(); ++i) {
        if (!picked[i]) continue;
        dist[i] = 0.0f;
        from[i] = static_cast<uint32_t>(i);
        front.push({0.0f, static_cast<uint32_t>(i)});
    }
    while (!front.empty()) {
        const auto [d, u] = front.top();
        front.pop();
        if (d > dist[u]) continue;  // reached nearer since
        for (const int32_t nv : adjacency.neighbours(u)) {
            const uint32_t v = static_cast<uint32_t>(nv);
            if (v >= n) continue;
            float reach = d + length(P[v] - P[u]);
            const float straight = length(P[v] - P[from[u]]);
            if (straight >= d) reach = std::min(reach, straight);
            if (reach >= dist[v] || reach >= radius) continue;
            dist[v] = reach;
            from[v] = from[u];
            front.push({reach, v});
        }
    }
    for (size_t i = 0; i < n; ++i) {
        if (weight[i] > 0.0f || !(dist[i] < radius)) continue;
        weight[i] = falloff(shape, dist[i] / radius);
    }
    return weight;
}

}  // namespace pg
