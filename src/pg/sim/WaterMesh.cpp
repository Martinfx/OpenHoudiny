#include "pg/sim/WaterMesh.h"

#include "pg/core/Parallel.h"
#include "pg/nodes/Nodes.h"

#include <algorithm>
#include <cmath>

namespace pg::sim {

namespace {

/// Trilinear between the centres of a grid of n[0] x n[1] x n[2] cells of
/// `voxel` from `origin`, the nearest cell's beyond them.
template <class At>
float sampleClamped(const Vec3& p, const Vec3& origin, float voxel, const int n[3], At&& at) {
    const Vec3 g = (p - origin) * (1.0f / voxel) - Vec3(0.5f, 0.5f, 0.5f);
    const float x[3] = {g.x, g.y, g.z};
    int i0[3];
    float t[3];
    for (int a = 0; a < 3; ++a) {
        const float c = std::clamp(x[a], 0.0f, static_cast<float>(std::max(n[a] - 1, 0)));
        i0[a] = std::min(static_cast<int>(c), std::max(n[a] - 2, 0));
        t[a] = n[a] > 1 ? c - static_cast<float>(i0[a]) : 0.0f;
    }
    float v = 0.0f;
    for (int c = 0; c < 8; ++c) {
        const int di = c & 1, dj = (c >> 1) & 1, dk = (c >> 2) & 1;
        const float w = (di ? t[0] : 1.0f - t[0]) * (dj ? t[1] : 1.0f - t[1]) * (dk ? t[2] : 1.0f - t[2]);
        if (w == 0.0f) continue;
        v += w * at(std::min(i0[0] + di, n[0] - 1), std::min(i0[1] + dj, n[1] - 1), std::min(i0[2] + dk, n[2] - 1));
    }
    return v;
}

}  // namespace

std::shared_ptr<Geometry> waterMesh(const WaterFrame& water, const RainFrame* ripples) {
    const Domain& d = water.domain;
    if (water.empty() || water.cells.size() != 2 * d.cellCount()) return std::make_shared<Geometry>();
    const int n[3] = {d.cells[0], d.cells[1], d.cells[2]};

    // The distance to the surface, as a volume: below 0 in the water.
    std::vector<float> distance(d.cellCount());
    pg::parallelFor(static_cast<size_t>(n[2]), 1, [&](size_t begin, size_t end) {
        for (int k = static_cast<int>(begin); k < static_cast<int>(end); ++k) {
            for (int j = 0; j < n[1]; ++j) {
                for (int i = 0; i < n[0]; ++i) {
                    distance[static_cast<size_t>(i) + static_cast<size_t>(n[0]) * (static_cast<size_t>(j) + static_cast<size_t>(n[1]) * static_cast<size_t>(k))] =
                        water.distance(i, j, k);
                }
            }
        }
    });
    const Volume field = Volume::make("surface", d.origin(), d.voxel, n[0], n[1], n[2], std::move(distance));
    std::shared_ptr<Geometry> geo = volumeToMesh(field, 0.0f, true);
    const size_t points = geo->pointCount();
    if (points == 0) return geo;

    auto P = geo->positionsForWrite();
    auto N = geo->points().find("N")->write<Vec3>();
    // How fast it goes, where the frame knows (not in one cached before it did).
    const bool moving = !water.flow.empty();
    std::span<Vec3> v;
    if (moving) v = geo->points().create("v", AttrType::Vec3).write<Vec3>();
    auto foam = geo->points().create("foam", AttrType::Float).write<float>();
    // The ripples: heights over the water's xz, from the rain.
    const bool rippled = ripples && ripples->rippleCells[0] > 0 && ripples->rippleCells[1] > 0 &&
                         ripples->ripples.size() == static_cast<size_t>(ripples->rippleCells[0]) *
                                                        static_cast<size_t>(ripples->rippleCells[1]) &&
                         ripples->rippleCell > 0.0f;
    auto height = [&](float x, float z) {
        const RainFrame& r = *ripples;
        const int m[3] = {r.rippleCells[0], 1, r.rippleCells[1]};
        const Vec3 at(x, r.rippleOrigin.y, z);
        return sampleClamped(at, r.rippleOrigin, r.rippleCell, m, [&](int i, int, int k) {
            return fromHalf(r.ripples[static_cast<size_t>(i) + static_cast<size_t>(r.rippleCells[0]) * static_cast<size_t>(k)]);
        });
    };
    pg::parallelFor(points, 4096, [&](size_t begin, size_t end) {
        for (size_t p = begin; p < end; ++p) {
            if (moving) v[p] = water.flowAt(P[p]);
            foam[p] = std::clamp(sampleClamped(P[p], d.origin(), d.voxel, n, [&](int i, int j, int k) { return water.foam(i, j, k); }),
                                 0.0f, 1.0f);
            if (!rippled || N[p].y <= 0.0f) continue;
            // The top raised as the ripples are, and tilted by their slope:
            // all of it where it faces up, none where it stands upright.
            const float up = N[p].y * N[p].y;
            const float e = ripples->rippleCell;
            const float h = height(P[p].x, P[p].z);
            const float hx = (height(P[p].x + e, P[p].z) - height(P[p].x - e, P[p].z)) / (2.0f * e);
            const float hz = (height(P[p].x, P[p].z + e) - height(P[p].x, P[p].z - e)) / (2.0f * e);
            P[p].y += up * h;
            N[p] = normalize(N[p] + Vec3(-hx, 0.0f, -hz) * up);
        }
    });
    return geo;
}

}  // namespace pg::sim
