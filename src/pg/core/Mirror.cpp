#include "pg/core/Mirror.h"

#include "pg/core/Parallel.h"
#include "pg/core/Spatial.h"

#include <algorithm>
#include <cmath>

namespace pg {

float mirrorTolerance(const Geometry& geo) {
    const auto P = geo.positions();
    if (P.empty()) return 1e-6f;
    Vec3 lo = P[0], hi = P[0];
    for (const Vec3& p : P) lo = glm::min(lo, p), hi = glm::max(hi, p);
    return std::max(1e-4f * length(hi - lo), 1e-6f);
}

std::vector<int32_t> mirrorPoints(const Geometry& geo, Mirror m, float tolerance) {
    const auto P = geo.positions();
    std::vector<int32_t> out(P.size(), -1);
    if (m == Mirror::None || P.empty()) return out;
    const PointTree tree(P);
    parallelFor(P.size(), 4096, [&](size_t b, size_t e) {
        for (size_t i = b; i < e; ++i) out[i] = tree.nearest(mirrored(P[i], m), tolerance);
    });
    return out;
}

std::vector<uint8_t> withMirror(const Geometry& geo, std::span<const uint8_t> mask, Mirror m) {
    std::vector<uint8_t> out(mask.begin(), mask.end());
    out.resize(geo.pointCount(), 0);
    if (m == Mirror::None) return out;
    const std::vector<int32_t> image = mirrorPoints(geo, m, mirrorTolerance(geo));
    for (size_t i = 0; i < mask.size() && i < image.size(); ++i) {
        if (mask[i] && image[i] >= 0) out[static_cast<size_t>(image[i])] = 1;
    }
    return out;
}

}  // namespace pg
