#include "pg/core/Geometry.h"

#include "test_framework.h"

using namespace pg;

namespace {

/// Two triangles sharing no points: 6 points, 2 primitives.
Geometry twoTriangles() {
    Geometry g;
    g.addPoints(6);
    auto P = g.positionsForWrite();
    for (size_t i = 0; i < 6; ++i) P[i] = Vec3(static_cast<float>(i), 0, 0);
    const uint32_t a[3] = {0, 1, 2};
    const uint32_t b[3] = {3, 4, 5};
    g.addPrimitive(std::span<const uint32_t>(a, 3));
    g.addPrimitive(std::span<const uint32_t>(b, 3));
    return g;
}

}  // namespace

TEST(building_points_and_primitives_tracks_counts) {
    Geometry g = twoTriangles();
    CHECK_EQ(g.pointCount(), 6u);
    CHECK_EQ(g.vertexCount(), 6u);
    CHECK_EQ(g.primitiveCount(), 2u);
    CHECK_EQ(g.primitivePoints(1)[0], 3u);
    CHECK(g.primitiveClosed(0));
}

TEST(append_rebases_topology_and_merges_attributes) {
    Geometry a = twoTriangles();
    a.points().create("mass", AttrType::Float).write<float>()[0] = 4.0f;

    Geometry b = twoTriangles();
    b.points().create("charge", AttrType::Float).write<float>()[5] = 8.0f;

    a.append(b);

    CHECK_EQ(a.pointCount(), 12u);
    CHECK_EQ(a.primitiveCount(), 4u);
    // The third primitive is b's first, its point indices shifted by 6.
    CHECK_EQ(a.primitivePoints(2)[0], 6u);
    CHECK_EQ(a.primitivePoints(3)[2], 11u);
    CHECK_EQ(a.points().find("mass")->read<float>()[0], 4.0f);
    CHECK_EQ(a.points().find("charge")->read<float>()[11], 8.0f);
    // mass is absent on b's side, so those elements are zero.
    CHECK_EQ(a.points().find("mass")->read<float>()[6], 0.0f);
}

TEST(deleting_a_point_removes_the_primitives_that_used_it) {
    Geometry g = twoTriangles();
    std::vector<uint8_t> keep(6, 1);
    keep[4] = 0;  // belongs to the second triangle

    g.deletePoints(keep);

    CHECK_EQ(g.pointCount(), 5u);
    CHECK_EQ(g.primitiveCount(), 1u);   // second triangle gone
    CHECK_EQ(g.vertexCount(), 3u);
    // Surviving points keep their relative order.
    CHECK_EQ(g.positions()[3].x, 3.0f);
    CHECK_EQ(g.positions()[4].x, 5.0f);
}

TEST(delete_remaps_groups_and_attributes_consistently) {
    Geometry g = twoTriangles();
    g.points().create("mass", AttrType::Float);
    {
        auto m = g.points().find("mass")->write<float>();
        for (size_t i = 0; i < 6; ++i) m[i] = static_cast<float>(i) * 10.0f;
    }
    Group& sel = g.createGroup("sel", AttrClass::Point);
    sel.set(0, true);
    sel.set(5, true);

    std::vector<uint8_t> keep{1, 0, 1, 1, 1, 1};
    g.deletePoints(keep);

    CHECK_EQ(g.pointCount(), 5u);
    auto m = g.points().find("mass")->read<float>();
    CHECK_EQ(m[0], 0.0f);
    CHECK_EQ(m[1], 20.0f);  // old index 2
    CHECK_EQ(m[4], 50.0f);  // old index 5
    CHECK(g.findGroup("sel")->contains(0));
    CHECK(g.findGroup("sel")->contains(4));
    CHECK_EQ(g.findGroup("sel")->memberCount(), 2u);
}

TEST(hash_is_stable_and_content_sensitive) {
    Geometry a = twoTriangles();
    Geometry b = twoTriangles();
    CHECK_EQ(a.hash(), b.hash());
    CHECK_EQ(a.hash(), a.hash());  // pure

    b.positionsForWrite()[3].y = 0.001f;
    CHECK_NE(a.hash(), b.hash());

    Geometry c = twoTriangles();
    c.points().create("extra", AttrType::Float);
    CHECK_NE(a.hash(), c.hash());

    Geometry d = twoTriangles();
    d.createGroup("sel", AttrClass::Point).set(0, true);
    CHECK_NE(a.hash(), d.hash());
}

TEST(P_always_exists_on_a_fresh_geometry) {
    Geometry g;
    CHECK(g.points().contains("P"));
    CHECK_EQ(g.points().find("P")->type(), AttrType::Vec3);
    CHECK_EQ(g.pointCount(), 0u);
}
