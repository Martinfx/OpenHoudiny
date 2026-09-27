#pragma once
//
// What a run asks of its inputs beyond their attributes -- the nearest
// points, who is joined to whom, the bounding box -- made the first time a
// program asks, once, whichever thread asks first.
//
#include "pg/core/Geometry.h"
#include "pg/core/Spatial.h"

#include <array>
#include <limits>
#include <mutex>

namespace pg::lang {

struct InputQuery {
    const Geometry* geo = nullptr;

    const PointTree& tree() {
        std::call_once(treeOnce_, [&] {
            if (geo) tree_.build(geo->positions());
        });
        return tree_;
    }
    const Adjacency& adjacency() {
        std::call_once(adjOnce_, [&] {
            if (geo) adj_.build(*geo);
        });
        return adj_;
    }
    /// The box round the points; an empty geometry's is 0 to 0.
    void box(Vec3& mn, Vec3& mx) {
        std::call_once(boxOnce_, [&] {
            if (!geo || geo->pointCount() == 0) return;
            mn_ = Vec3(std::numeric_limits<float>::max());
            mx_ = Vec3(-std::numeric_limits<float>::max());
            for (const Vec3& p : geo->positions()) {
                for (int a = 0; a < 3; ++a) {
                    mn_[a] = std::min(mn_[a], p[a]);
                    mx_[a] = std::max(mx_[a], p[a]);
                }
            }
        });
        mn = mn_;
        mx = mx_;
    }

private:
    std::once_flag treeOnce_, adjOnce_, boxOnce_;
    PointTree tree_;
    Adjacency adj_;
    Vec3 mn_, mx_;
};

struct Queries {
    std::array<InputQuery, 4> in;
    /// Input k, or null outside 0..3.
    InputQuery* input(int32_t k) { return k >= 0 && k < 4 ? &in[static_cast<size_t>(k)] : nullptr; }
};

}  // namespace pg::lang
