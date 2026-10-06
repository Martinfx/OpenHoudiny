#pragma once
//
// For the nodes that make new points of old ones -- Clip, Subdivide, Fuse,
// the fractures: a new point is a blend of points that were (Blends), and
// every attribute of the points -- P, Cd, uv, N -- is blended by the same
// weights: numbers weighted, integers and strings from the heaviest. So the
// nodes know nothing of the attributes they carry.
//
#include "pg/core/Geometry.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace pg {

/// New elements, each a weighted sum of old ones: the terms of element e are
/// index/weight[start[e], start[e + 1]).
struct Blends {
    std::vector<uint32_t> start{0};
    std::vector<uint32_t> index;
    std::vector<float> weight;

    size_t size() const { return start.size() - 1; }
    void one(uint32_t i) {
        index.push_back(i);
        weight.push_back(1.0f);
        start.push_back(static_cast<uint32_t>(index.size()));
    }
    /// (1 - t) a + t b -- as add() has them: the lower first, one if the same.
    void two(uint32_t a, uint32_t b, float t) {
        if (a == b) {
            index.push_back(a);
            weight.push_back(1.0f);
        } else {
            const bool inOrder = a < b;
            index.push_back(inOrder ? a : b);
            weight.push_back(inOrder ? 1.0f - t : t);
            index.push_back(inOrder ? b : a);
            weight.push_back(inOrder ? t : 1.0f - t);
        }
        start.push_back(static_cast<uint32_t>(index.size()));
    }
    /// Terms naming the same element are summed.
    void add(std::vector<std::pair<uint32_t, float>>& terms) {
        std::sort(terms.begin(), terms.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
        for (size_t i = 0; i < terms.size();) {
            const uint32_t at = terms[i].first;
            float w = 0.0f;
            while (i < terms.size() && terms[i].first == at) w += terms[i++].second;
            index.push_back(at);
            weight.push_back(w);
        }
        start.push_back(static_cast<uint32_t>(index.size()));
    }
    void none() { start.push_back(static_cast<uint32_t>(index.size())); }
    /// The term with the most weight, the first of equals; -1 for none.
    int64_t heaviest(size_t e) const {
        int64_t best = -1;
        float w = -1.0f;
        for (uint32_t k = start[e]; k < start[e + 1]; ++k) {
            if (weight[k] > w) {
                w = weight[k];
                best = index[k];
            }
        }
        return best;
    }
};

/// How many floats an attribute of this type holds; 0 for integers and strings.
int floatsOf(AttrType t);

/// The attributes of `from` for elements blended from its own.
AttributeSet blended(const AttributeSet& from, const Blends& b);

/// The attributes of `from` gathered: element e is from's `source[e]`.
AttributeSet gathered(const AttributeSet& from, std::span<const uint32_t> source);

/// The groups of `src` onto `dst`, whose points are `points` blended from
/// src's (members where every term is one) and whose primitives are
/// `prims` gathered from src's.
void carryGroups(const Geometry& src, Geometry& dst, const Blends& points, std::span<const uint32_t> prims);

/// A geometry of `pointCount` points and the primitives `faces` (point
/// lists, `closed`), its attributes from `src`: points blended, vertices
/// blended, primitives gathered by `sourcePrim`; groups carried; detail and
/// volumes kept, and the prototypes the instance points stand for.
std::shared_ptr<Geometry> rebuild(const Geometry& src, const Blends& points, const std::vector<std::vector<uint32_t>>& faces,
                                  const std::vector<uint8_t>& closed, const Blends& vertices,
                                  const std::vector<uint32_t>& sourcePrim);

/// An edge between two points, the same key whichever way round.
uint64_t edgeKey(uint32_t a, uint32_t b);

}  // namespace pg
