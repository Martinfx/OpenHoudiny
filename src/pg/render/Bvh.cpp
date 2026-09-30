#include "pg/render/Bvh.h"

#include "pg/core/Parallel.h"

#include <numeric>

namespace pg::render {
namespace {

constexpr int kBins = 12;

struct Range {
    uint32_t node, begin, end;
    int depth;
};

/// Splits `root` -- its items in `items`, its node in `nodes` -- into boxes
/// within boxes. A range of fewer than `deferBelow` items is left for later,
/// in `deferred`, when there is one.
void split(std::vector<BvhNode>& nodes, std::vector<uint32_t>& items, std::span<const Box> boxes,
           const std::vector<Vec3>& centers, const Range& root, int leafSize, uint32_t deferBelow,
           std::vector<Range>* deferred) {
    std::vector<Range> todo{root};
    while (!todo.empty()) {
        const Range r = todo.back();
        todo.pop_back();
        const uint32_t count = r.end - r.begin;
        if (deferred && count < deferBelow) {
            deferred->push_back(r);
            continue;
        }
        Box box, middles;
        for (uint32_t i = r.begin; i < r.end; ++i) {
            box.grow(boxes[items[i]]);
            middles.grow(centers[items[i]]);
        }
        BvhNode& node = nodes[r.node];
        node.lo = box.lo;
        node.hi = box.hi;
        auto leaf = [&]() {
            BvhNode& l = nodes[r.node];
            l.first = r.begin;
            l.count = count;
        };
        if (count <= static_cast<uint32_t>(leafSize) || r.depth >= kMaxDepth) {
            leaf();
            continue;
        }
        // The best split over bins along each axis, by the surface area heuristic.
        int bestAxis = -1, bestSplit = 0;
        float bestCost = 1e30f;
        for (int axis = 0; axis < 3; ++axis) {
            const float lo = middles.lo[axis], extent = middles.hi[axis] - lo;
            if (extent <= 1e-12f) continue;
            const float scale = static_cast<float>(kBins) / extent;
            Box binBox[kBins];
            uint32_t binCount[kBins] = {};
            for (uint32_t i = r.begin; i < r.end; ++i) {
                const uint32_t item = items[i];
                const int b = std::min(kBins - 1, static_cast<int>((centers[item][axis] - lo) * scale));
                binBox[b].grow(boxes[item]);
                ++binCount[b];
            }
            float rightArea[kBins];
            uint32_t rightCount[kBins];
            Box acc;
            uint32_t accCount = 0;
            for (int b = kBins - 1; b > 0; --b) {
                acc.grow(binBox[b]);
                accCount += binCount[b];
                rightArea[b] = acc.area();
                rightCount[b] = accCount;
            }
            Box left;
            uint32_t leftCount = 0;
            for (int cut = 1; cut < kBins; ++cut) {
                left.grow(binBox[cut - 1]);
                leftCount += binCount[cut - 1];
                if (leftCount == 0 || rightCount[cut] == 0) continue;
                const float cost = left.area() * static_cast<float>(leftCount) +
                                   rightArea[cut] * static_cast<float>(rightCount[cut]);
                if (cost < bestCost) {
                    bestCost = cost;
                    bestAxis = axis;
                    bestSplit = cut;
                }
            }
        }
        uint32_t middle = r.begin;
        if (bestAxis >= 0) {
            // Worth it only when cheaper than a leaf of them all -- unless too many.
            if (bestCost >= box.area() * static_cast<float>(count) && count <= 16u) {
                leaf();
                continue;
            }
            const float lo = middles.lo[bestAxis];
            const float scale = static_cast<float>(kBins) / (middles.hi[bestAxis] - lo);
            const auto mid = std::partition(items.begin() + r.begin, items.begin() + r.end, [&](uint32_t item) {
                return std::min(kBins - 1, static_cast<int>((centers[item][bestAxis] - lo) * scale)) < bestSplit;
            });
            middle = static_cast<uint32_t>(mid - items.begin());
        }
        if (middle == r.begin || middle == r.end) {
            // All in one place: halves by their order.
            middle = r.begin + count / 2;
        }
        const uint32_t first = static_cast<uint32_t>(nodes.size());
        nodes[r.node].first = first;
        nodes[r.node].count = 0;
        nodes.emplace_back();
        nodes.emplace_back();
        todo.push_back({first + 1, middle, r.end, r.depth + 1});
        todo.push_back({first, r.begin, middle, r.depth + 1});
    }
}

}  // namespace

Bvh buildBvh(std::span<const Box> boxes, int leafSize) {
    Bvh bvh;
    const uint32_t n = static_cast<uint32_t>(boxes.size());
    if (n == 0) return bvh;
    leafSize = std::max(leafSize, 1);
    bvh.items.resize(n);
    std::iota(bvh.items.begin(), bvh.items.end(), 0u);
    std::vector<Vec3> centers(n);
    parallelFor(n, 16384, [&](size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i) centers[i] = boxes[i].center();
    });
    bvh.nodes.reserve(2 * static_cast<size_t>(n) / static_cast<size_t>(leafSize) + 2);
    bvh.nodes.emplace_back();
    // The top of the hierarchy here; below, where the ranges are small
    // enough, subtrees built side by side -- each over its own items, the
    // same whatever the threads -- and joined in the order they were left.
    constexpr uint32_t kSplitAlone = 8192;
    std::vector<Range> deferred;
    const uint32_t deferBelow = n >= 4 * kSplitAlone ? std::max(kSplitAlone, n / 64) : 0;
    split(bvh.nodes, bvh.items, boxes, centers, {0, 0, n, 0}, leafSize, deferBelow, deferBelow ? &deferred : nullptr);
    if (deferred.empty()) return bvh;
    std::vector<std::vector<BvhNode>> subtrees(deferred.size());
    parallelFor(deferred.size(), 1, [&](size_t begin, size_t end) {
        for (size_t k = begin; k < end; ++k) {
            std::vector<BvhNode>& local = subtrees[k];
            local.emplace_back();
            Range root = deferred[k];
            root.node = 0;
            split(local, bvh.items, boxes, centers, root, leafSize, 0, nullptr);
        }
    });
    for (size_t k = 0; k < deferred.size(); ++k) {
        const std::vector<BvhNode>& local = subtrees[k];
        // Local node i > 0 lands at base + i - 1; the root takes the node left for it.
        const uint32_t base = static_cast<uint32_t>(bvh.nodes.size());
        auto moved = [&](BvhNode node) {
            if (node.count == 0) node.first = base + node.first - 1;
            return node;
        };
        bvh.nodes[deferred[k].node] = moved(local[0]);
        for (size_t i = 1; i < local.size(); ++i) bvh.nodes.push_back(moved(local[i]));
    }
    return bvh;
}

}  // namespace pg::render
