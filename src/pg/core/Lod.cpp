#include "pg/core/Lod.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <vector>

namespace pg {

Geometry plantDetail(const Geometry& plant, float keep) {
    Geometry out = plant;
    if (!(keep < 1.0f)) return out;
    keep = std::max(keep, 1e-3f);
    const AttributeArray* through = plant.primitives().find("translucency");
    if (!through || through->type() != AttrType::Float) return out;
    const auto lets = through->read<float>();
    const AttributeArray* bladeAttr = plant.primitives().find("blade");
    const auto blades = bladeAttr && bladeAttr->type() == AttrType::Int ? bladeAttr->read<int32_t>() : std::span<const int32_t>();
    const AttributeArray* levelAttr = plant.primitives().find("level");
    const auto levels = levelAttr && levelAttr->type() == AttrType::Int ? levelAttr->read<int32_t>() : std::span<const int32_t>();
    const AttributeArray* flexAttr = plant.points().find("flex");
    const auto flex = flexAttr && flexAttr->type() == AttrType::Float ? flexAttr->read<float>() : std::span<const float>();

    // The foliage in pieces: a leaf a face, a blade its faces.
    const size_t prims = plant.primitiveCount();
    std::vector<int64_t> pieceOf(prims, -1);
    std::vector<std::vector<uint32_t>> pieces;
    std::map<int32_t, size_t> bladePiece;
    for (size_t i = 0; i < prims; ++i) {
        if (!(lets[i] > 0.0f)) continue;
        if (!blades.empty()) {
            const auto [it, added] = bladePiece.emplace(blades[i], pieces.size());
            if (added) pieces.emplace_back();
            pieceOf[i] = static_cast<int64_t>(it->second);
        } else {
            pieceOf[i] = static_cast<int64_t>(pieces.size());
            pieces.emplace_back();
        }
        pieces[static_cast<size_t>(pieceOf[i])].push_back(static_cast<uint32_t>(i));
    }

    // Which stay: evenly, one in every 1/keep along them.
    std::vector<uint8_t> stays(pieces.size(), 0);
    for (size_t k = 0; k < pieces.size(); ++k) {
        stays[k] = std::floor(static_cast<double>(k + 1) * keep) > std::floor(static_cast<double>(k) * keep);
    }

    // Each kept one grown.
    auto P = out.positionsForWrite();
    const float grow = 1.0f / std::sqrt(keep), widen = 1.0f / keep;
    std::vector<uint8_t> moved(plant.pointCount(), 0);
    for (size_t k = 0; k < pieces.size(); ++k) {
        if (!stays[k]) continue;
        std::vector<uint32_t> points;
        for (const uint32_t prim : pieces[k]) {
            for (const uint32_t p : plant.primitivePoints(prim)) {
                if (!moved[p]) {
                    moved[p] = 1;
                    points.push_back(p);
                }
            }
        }
        if (points.empty()) continue;
        if (!blades.empty() && !flex.empty()) {
            // A blade: each row from its middle.
            std::map<float, std::pair<Vec3, int>> rows;
            for (const uint32_t p : points) {
                auto& row = rows[flex[p]];
                row.first = row.first + P[p];
                ++row.second;
            }
            for (const uint32_t p : points) {
                const auto& row = rows[flex[p]];
                const Vec3 middle = row.first / static_cast<float>(row.second);
                P[p] = middle + (P[p] - middle) * widen;
            }
        } else {
            // A leaf: about its base.
            const Vec3 base = P[plant.primitivePoints(pieces[k].front())[0]];
            for (const uint32_t p : points) P[p] = base + (P[p] - base) * grow;
        }
    }

    // What goes: the foliage not kept, and the twigs when few are.
    std::vector<uint8_t> keepPrim(prims, 1);
    for (size_t i = 0; i < prims; ++i) {
        if (pieceOf[i] >= 0) keepPrim[i] = stays[static_cast<size_t>(pieceOf[i])];
        else if (keep < 0.5f && !levels.empty() && levels[i] >= 2) keepPrim[i] = 0;
    }
    out.deletePrimitives(keepPrim, true);
    return out;
}

}  // namespace pg
