//
// Foliage as the renderers see it: the uv of trees and grass -- round and up
// the bark in whole pictures, square at a stem's foot; each leaf in its
// quarter of the leaf picture; once across and up a blade of grass -- none
// mirrored, and the library's bark, leaves and grass laid on by it.
//
#include "pg/core/Grass.h"
#include "pg/core/Material.h"
#include "pg/core/Tree.h"
#include "pg/render/Scene.h"

#include "test_framework.h"

#include <algorithm>
#include <cmath>
#include <set>

using namespace pg;

namespace {

/// The uv of the corners of primitive `prim` of `geo`.
std::vector<Vec3> uvOf(const Geometry& geo, size_t prim) {
    const auto uv = geo.vertices().find("uv")->read<Vec3>();
    std::vector<Vec3> out;
    const size_t first = geo.primitiveVertexStart(prim);
    for (size_t k = 0; k < geo.primitiveVertexCount(prim); ++k) out.push_back(uv[first + k]);
    return out;
}

/// Twice the area of a polygon in uv, anticlockwise positive.
float uvArea(const std::vector<Vec3>& uv) {
    float a = 0.0f;
    for (size_t k = 0; k < uv.size(); ++k) {
        const Vec3& p = uv[k];
        const Vec3& q = uv[(k + 1) % uv.size()];
        a += p.x * q.y - q.x * p.y;
    }
    return a;
}

}  // namespace

TEST(foliage_trees_and_grass_have_uv_none_mirrored) {
    TreeSettings s;
    s.levels = 2;
    s.leaves = 6;
    Geometry geo;
    const Tree tree = growTree(s, Vec3(), 1.0f, 7);
    meshTree(tree, s, 0, geo);
    const AttributeArray* uvs = geo.vertices().find("uv");
    CHECK(uvs && uvs->type() == AttrType::Vec3 && uvs->size() == geo.vertexCount());
    const auto level = geo.primitives().find("level")->read<int32_t>();
    const auto P = geo.positions();

    // The bark: no face across the seam (less than a picture round it, and
    // up it), every face's uv anticlockwise as its corners are; at the
    // trunk's foot the pictures square -- as many metres round a unit of u
    // as up a unit of v.
    std::set<float> broadCells;
    size_t bark = 0, leaves = 0;
    for (size_t i = 0; i < geo.primitiveCount(); ++i) {
        const std::vector<Vec3> uv = uvOf(geo, i);
        float lo = 1e9f, hi = -1e9f;
        for (const Vec3& q : uv) lo = std::min(lo, q.x), hi = std::max(hi, q.x);
        if (level[i] >= 0) {
            ++bark;
            CHECK(hi - lo < 1.0f);
            CHECK(uvArea(uv) > 0.0f);
        } else {
            // A leaf: in a quarter of the picture, its base at the middle of
            // the quarter's bottom, its tip at the middle of its top.
            ++leaves;
            const float cx = uv[0].x < 0.5f ? 0.0f : 0.5f, cy = uv[0].y < 0.5f ? 0.0f : 0.5f;
            CHECK(cy == 0.5f);  // broad leaves: the top two
            broadCells.insert(cx);
            for (const Vec3& q : uv) CHECK(q.x >= cx && q.x <= cx + 0.5f && q.y >= cy && q.y <= cy + 0.5f);
            CHECK(std::fabs(uv[0].x - (cx + 0.25f)) < 1e-6f && std::fabs(uv[0].y - cy) < 1e-6f);
            float top = 0.0f;
            for (const Vec3& q : uv) top = std::max(top, q.y);
            CHECK(std::fabs(top - (cy + 0.5f)) < 1e-6f);
            CHECK(uvArea(uv) > 0.0f);
        }
    }
    CHECK(bark > 0 && leaves == tree.leaves.size());
    CHECK(broadCells.size() == 2);  // either broad leaf
    {
        // The trunk's first quad: round it and up it the same metres a unit.
        const std::vector<Vec3> uv = uvOf(geo, 0);
        const size_t first = geo.primitiveVertexStart(0);
        const auto corner = [&](size_t k) { return P[geo.vertexPoints()[first + k]]; };
        const float round = length(corner(1) - corner(0)) / (uv[1].x - uv[0].x);
        const float up = length(corner(3) - corner(0)) / (uv[3].y - uv[0].y);
        CHECK(std::fabs(round / up - 1.0f) < 0.1f);
        // Whole pictures round it: the last face round ends on a whole u.
        const float pictures = uvOf(geo, static_cast<size_t>(s.sides) - 1)[1].x;
        CHECK(pictures >= 1.0f && std::fabs(pictures - std::round(pictures)) < 1e-5f);
    }

    // Narrow leaves bottom left, needles bottom right.
    for (const auto [shape, cell] : {std::pair{TreeSettings::Leaf::Narrow, 0.0f}, std::pair{TreeSettings::Leaf::Needles, 0.5f}}) {
        TreeSettings t = s;
        t.leaf = shape;
        Geometry g;
        meshTree(growTree(t, Vec3(), 1.0f, 7), t, 0, g);
        const auto lv = g.primitives().find("level")->read<int32_t>();
        bool all = true;
        for (size_t i = 0; i < g.primitiveCount(); ++i) {
            if (lv[i] >= 0) continue;
            for (const Vec3& q : uvOf(g, i)) all = all && q.y <= 0.5f && q.x >= cell && q.x <= cell + 0.5f;
        }
        CHECK(all);
    }

    // Grass: once across a blade and up it, root to tip.
    const Geometry clump = growGrassClump(GrassSettings(), 3);
    CHECK(clump.vertices().find("uv"));
    float lowest = 1.0f, highest = 0.0f;
    for (size_t i = 0; i < clump.primitiveCount(); ++i) {
        const std::vector<Vec3> uv = uvOf(clump, i);
        CHECK(uvArea(uv) > 0.0f);
        for (const Vec3& q : uv) lowest = std::min(lowest, q.y), highest = std::max(highest, q.y);
    }
    CHECK(lowest == 0.0f && highest == 1.0f);

    // The renderers: bark and leaves laid on by it, none mirrored -- every
    // tangent the way of the faces' own.
    for (const Geometry* g : {static_cast<const Geometry*>(&geo), &clump}) {
        const auto mesh = render::meshOf(*g);
        CHECK(!mesh->materials.empty());
        for (const render::Material& m : mesh->materials) CHECK(m.byUv && laidByUv(m.preset));
        CHECK_EQ(mesh->tangents.size(), 3 * mesh->count());
        size_t mirrored = 0;
        for (const Vec4& t : mesh->tangents) mirrored += t.w < 0.0f;
        CHECK_EQ(mirrored, 0u);
    }
}
