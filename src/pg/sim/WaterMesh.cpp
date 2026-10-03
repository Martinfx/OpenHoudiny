#include "pg/sim/WaterMesh.h"

#include "pg/core/Half.h"
#include "pg/core/Parallel.h"
#include "pg/nodes/Nodes.h"
#include "pg/sim/SparseGrid.h"

#include <algorithm>
#include <cmath>

namespace pg::sim {

namespace {

/// Trilinear between the centres of a grid of n[0] x n[1] x n[2] cells of
/// `voxel` from `origin`, the nearest cell's beyond them.
template <class T, class At>
T sampleClamped(const Vec3& p, const Vec3& origin, float voxel, const int n[3], At&& at) {
    const Vec3 g = (p - origin) * (1.0f / voxel) - Vec3(0.5f, 0.5f, 0.5f);
    const float x[3] = {g.x, g.y, g.z};
    int i0[3];
    float t[3];
    for (int a = 0; a < 3; ++a) {
        const float c = std::clamp(x[a], 0.0f, static_cast<float>(std::max(n[a] - 1, 0)));
        i0[a] = std::min(static_cast<int>(c), std::max(n[a] - 2, 0));
        t[a] = n[a] > 1 ? c - static_cast<float>(i0[a]) : 0.0f;
    }
    T v{};
    for (int c = 0; c < 8; ++c) {
        const int di = c & 1, dj = (c >> 1) & 1, dk = (c >> 2) & 1;
        const float w = (di ? t[0] : 1.0f - t[0]) * (dj ? t[1] : 1.0f - t[1]) * (dk ? t[2] : 1.0f - t[2]);
        if (w == 0.0f) continue;
        v = v + at(std::min(i0[0] + di, n[0] - 1), std::min(i0[1] + dj, n[1] - 1), std::min(i0[2] + dk, n[2] - 1)) * w;
    }
    return v;
}

/// Where a cell of a frame's grid is among its values: x fastest through
/// every cell, or -- sparse -- 512 a tile in the order of the tiles kept,
/// by a table of every tile's place in that order, for many lookups; -1 in
/// a tile not kept.
class CellPlaces {
public:
    CellPlaces(const Domain& d, const std::vector<uint32_t>& tiles) : n_{d.cells[0], d.cells[1], d.cells[2]} {
        if (tiles.empty()) return;
        for (int a = 0; a < 3; ++a) t_[a] = static_cast<size_t>((n_[a] + Tiles::kSide - 1) / Tiles::kSide);
        slot_.assign(t_[0] * t_[1] * t_[2], -1);
        for (size_t s = 0; s < tiles.size(); ++s) slot_[tiles[s]] = static_cast<int32_t>(s);
    }
    int64_t operator()(int i, int j, int k) const {
        if (slot_.empty()) {
            return static_cast<int64_t>(static_cast<size_t>(i) +
                                        static_cast<size_t>(n_[0]) * (static_cast<size_t>(j) + static_cast<size_t>(n_[1]) * static_cast<size_t>(k)));
        }
        const int32_t s = slot_[static_cast<size_t>(i >> Tiles::kLog) +
                                t_[0] * (static_cast<size_t>(j >> Tiles::kLog) + t_[1] * static_cast<size_t>(k >> Tiles::kLog))];
        return s < 0 ? -1 : static_cast<int64_t>(s) * Tiles::kCells + static_cast<int64_t>(SparseGrid::local(i, j, k));
    }

private:
    int n_[3];
    size_t t_[3] = {0, 0, 0};
    std::vector<int32_t> slot_;  // sparse: per tile of the grid, its place among those kept, or -1
};

}  // namespace

std::shared_ptr<Geometry> waterMesh(const WaterFrame& water, const RainFrame* ripples) {
    const Domain& d = water.domain;
    if (water.empty() || !water.fits()) return std::make_shared<Geometry>();
    const int n[3] = {d.cells[0], d.cells[1], d.cells[2]};
    const uint8_t* cells = water.cells.data();
    // As WaterFrame::distance() has it.
    auto distance = [&](size_t c) { return (static_cast<float>(cells[2 * c]) / 255.0f * 2.0f - 1.0f) * water.band; };

    // The distance to the surface, as a volume: below 0 in the water. Sparse,
    // as tiles, the rest as far as the band.
    std::shared_ptr<Geometry> geo;
    if (water.tiles.empty()) {
        std::vector<float> field(d.cellCount());
        pg::parallelFor(field.size(), 65536, [&](size_t begin, size_t end) {
            for (size_t c = begin; c < end; ++c) field[c] = distance(c);
        });
        geo = volumeToMesh(Volume::make("surface", d.origin(), d.voxel, n[0], n[1], n[2], std::move(field)), 0.0f, true);
    } else {
        TiledVolume field;
        field.origin = d.origin();
        field.voxel = d.voxel;
        for (int a = 0; a < 3; ++a) field.res[a] = n[a];
        field.tiles = water.tiles;
        field.values.resize(Tiles::kCells * water.tiles.size());
        field.background = water.band;
        pg::parallelFor(field.values.size(), 65536, [&](size_t begin, size_t end) {
            for (size_t c = begin; c < end; ++c) field.values[c] = distance(c);
        });
        geo = volumeToMesh(field, 0.0f, true);
    }
    const size_t points = geo->pointCount();
    if (points == 0) return geo;

    auto P = geo->positionsForWrite();
    auto N = geo->points().find("N")->write<Vec3>();
    // How fast it goes, where the frame knows (not in one cached before it did).
    const bool moving = water.hasFlow();
    std::span<Vec3> v;
    if (moving) v = geo->points().create("v", AttrType::Vec3).write<Vec3>();
    const CellPlaces cellAt(d, water.tiles);
    const Domain flowGrid = water.flowDomain();
    const int m[3] = {flowGrid.cells[0], flowGrid.cells[1], flowGrid.cells[2]};
    const CellPlaces flowAt(flowGrid, water.flowTiles);
    const uint16_t* flow = water.flow.data();
    auto foam = geo->points().create("foam", AttrType::Float).write<float>();
    // The ripples: heights over the water's xz, from the rain.
    const bool rippled = ripples && ripples->rippleCells[0] > 0 && ripples->rippleCells[1] > 0 &&
                         ripples->ripples.size() == static_cast<size_t>(ripples->rippleCells[0]) *
                                                        static_cast<size_t>(ripples->rippleCells[1]) &&
                         ripples->rippleCell > 0.0f;
    auto height = [&](float x, float z) {
        const RainFrame& r = *ripples;
        const int cells[3] = {r.rippleCells[0], 1, r.rippleCells[1]};
        const Vec3 at(x, r.rippleOrigin.y, z);
        return sampleClamped<float>(at, r.rippleOrigin, r.rippleCell, cells, [&](int i, int, int k) {
            return fromHalf(r.ripples[static_cast<size_t>(i) + static_cast<size_t>(r.rippleCells[0]) * static_cast<size_t>(k)]);
        });
    };
    pg::parallelFor(points, 4096, [&](size_t begin, size_t end) {
        for (size_t p = begin; p < end; ++p) {
            // As WaterFrame::flowAt() and foam() have them: a cell of a tile
            // not kept still, without foam.
            if (moving) {
                v[p] = sampleClamped<Vec3>(P[p], flowGrid.origin(), flowGrid.voxel, m, [&](int i, int j, int k) {
                    const int64_t c = flowAt(i, j, k);
                    if (c < 0) return Vec3();
                    const uint16_t* h = flow + 3 * c;
                    return Vec3(fromHalf(h[0]), fromHalf(h[1]), fromHalf(h[2]));
                });
            }
            foam[p] = std::clamp(sampleClamped<float>(P[p], d.origin(), d.voxel, n, [&](int i, int j, int k) {
                                     const int64_t c = cellAt(i, j, k);
                                     return c < 0 ? 0.0f : static_cast<float>(cells[2 * c + 1]) / 255.0f;
                                 }),
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
