//
// Invariant I1: copy-on-write per attribute array.
//
// These are the tests that decide whether the architecture holds up at
// production scale. If they regress, memory use becomes O(nodes x attributes)
// instead of O(attributes) and long node chains stop being viable.
//
#include "pg/core/Attribute.h"
#include "pg/core/Geometry.h"

#include "test_framework.h"

#include <cmath>

using namespace pg;

namespace {

AttributeSet makeSet(size_t n) {
    AttributeSet set;
    set.setElementCount(n);
    set.create("P", AttrType::Vec3);
    set.create("N", AttrType::Vec3);
    set.create("Cd", AttrType::Vec3);
    set.create("mass", AttrType::Float);
    set.create("id", AttrType::Int);
    return set;
}

}  // namespace

TEST(copying_an_attribute_set_copies_no_element_data) {
    AttributeSet a = makeSet(100000);

    resetAttributeAllocationCount();
    AttributeSet b = a;
    CHECK_EQ(attributeAllocationCount(), 0u);

    for (const auto& name : a.names()) {
        CHECK_EQ(a.find(name)->bufferId(), b.find(name)->bufferId());
    }
}

TEST(writing_one_attribute_clones_only_that_attribute) {
    AttributeSet a = makeSet(100000);
    AttributeSet b = a;

    resetAttributeAllocationCount();
    auto P = b.find("P")->write<Vec3>();
    P[0] = Vec3(1, 2, 3);

    // Exactly one buffer allocated: P's. Not one per attribute.
    CHECK_EQ(attributeAllocationCount(), 1u);

    CHECK_NE(a.find("P")->bufferId(), b.find("P")->bufferId());
    CHECK_EQ(a.find("N")->bufferId(), b.find("N")->bufferId());
    CHECK_EQ(a.find("Cd")->bufferId(), b.find("Cd")->bufferId());
    CHECK_EQ(a.find("mass")->bufferId(), b.find("mass")->bufferId());
    CHECK_EQ(a.find("id")->bufferId(), b.find("id")->bufferId());

    // The original is untouched.
    CHECK_EQ(a.find("P")->read<Vec3>()[0].x, 0.0f);
    CHECK_EQ(b.find("P")->read<Vec3>()[0].x, 1.0f);
}

TEST(writing_an_unshared_attribute_does_not_clone) {
    AttributeSet a = makeSet(1000);
    const void* before = a.find("P")->bufferId();

    resetAttributeAllocationCount();
    a.find("P")->write<Vec3>()[0] = Vec3(1, 1, 1);
    a.find("P")->write<Vec3>()[1] = Vec3(2, 2, 2);

    CHECK_EQ(attributeAllocationCount(), 0u);
    CHECK_EQ(a.find("P")->bufferId(), before);
}

TEST(cow_survives_a_chain_of_copies) {
    // A 50-node chain in which every node writes only P must allocate exactly
    // 50 buffers, not 50 x attributeCount.
    AttributeSet base = makeSet(10000);
    resetAttributeAllocationCount();

    std::vector<AttributeSet> chain;
    chain.push_back(base);
    for (int i = 0; i < 50; ++i) {
        AttributeSet next = chain.back();
        auto P = next.find("P")->write<Vec3>();
        P[0] = Vec3(static_cast<float>(i), 0, 0);
        chain.push_back(std::move(next));
    }

    CHECK_EQ(attributeAllocationCount(), 50u);
    // Every link still shares the four untouched attributes with the source.
    for (const char* name : {"N", "Cd", "mass", "id"}) {
        CHECK_EQ(chain.back().find(name)->bufferId(), base.find(name)->bufferId());
    }
}

TEST(geometry_copy_shares_attributes_topology_and_groups) {
    Geometry g;
    g.addPoints(4);
    const uint32_t quad[4] = {0, 1, 2, 3};
    g.addPrimitive(std::span<const uint32_t>(quad, 4));
    g.points().create("Cd", AttrType::Vec3);
    g.createGroup("sel", AttrClass::Point).set(1, true);

    resetAttributeAllocationCount();
    Geometry copy = g;
    CHECK_EQ(attributeAllocationCount(), 0u);

    CHECK_EQ(copy.points().find("P")->bufferId(), g.points().find("P")->bufferId());
    CHECK_EQ(copy.points().find("Cd")->bufferId(), g.points().find("Cd")->bufferId());
    CHECK_EQ(copy.findGroup("sel")->bufferId(), g.findGroup("sel")->bufferId());
    CHECK_EQ(copy.hash(), g.hash());
}

TEST(editing_a_group_clones_only_the_group_mask) {
    Geometry g;
    g.addPoints(10);
    g.createGroup("sel", AttrClass::Point);

    Geometry copy = g;
    const void* sharedMask = g.findGroup("sel")->bufferId();

    copy.findGroup("sel")->set(3, true);

    CHECK_NE(copy.findGroup("sel")->bufferId(), sharedMask);
    CHECK_EQ(g.findGroup("sel")->bufferId(), sharedMask);
    CHECK(copy.findGroup("sel")->contains(3));
    CHECK(!g.findGroup("sel")->contains(3));
}

TEST(gather_reorders_and_shares_the_string_table) {
    AttributeArray names(AttrType::String, 4);
    {
        auto w = names.write<int32_t>();
        w[0] = names.internString("alpha");
        w[1] = names.internString("beta");
        w[2] = names.internString("alpha");
        w[3] = names.internString("gamma");
    }
    CHECK_EQ(names.stringTableSize(), 3u);  // "alpha" interned once

    const uint32_t pick[3] = {3, 1, 0};
    AttributeArray picked = names.gather(std::span<const uint32_t>(pick, 3));

    CHECK_EQ(picked.size(), 3u);
    CHECK_EQ(picked.stringValue(picked.read<int32_t>()[0]), std::string("gamma"));
    CHECK_EQ(picked.stringValue(picked.read<int32_t>()[1]), std::string("beta"));
    CHECK_EQ(picked.stringValue(picked.read<int32_t>()[2]), std::string("alpha"));
}

TEST(resize_zero_fills_and_preserves_existing_values) {
    AttributeArray a(AttrType::Float, 3);
    {
        auto w = a.write<float>();
        w[0] = 1.0f; w[1] = 2.0f; w[2] = 3.0f;
    }
    a.resize(5);
    CHECK_EQ(a.size(), 5u);
    auto r = a.read<float>();
    CHECK_EQ(r[2], 3.0f);
    CHECK_EQ(r[3], 0.0f);
    CHECK_EQ(r[4], 0.0f);

    a.resize(2);
    CHECK_EQ(a.size(), 2u);
    CHECK_EQ(a.read<float>()[1], 2.0f);
}

TEST(append_merges_attributes_and_zero_fills_the_gaps) {
    AttributeSet a;
    a.setElementCount(2);
    a.create("mass", AttrType::Float).write<float>()[0] = 5.0f;

    AttributeSet b;
    b.setElementCount(3);
    b.create("mass", AttrType::Float).write<float>()[0] = 7.0f;
    b.create("extra", AttrType::Float).write<float>()[2] = 9.0f;

    a.append(b);

    CHECK_EQ(a.elementCount(), 5u);
    CHECK_EQ(a.find("mass")->read<float>()[0], 5.0f);
    CHECK_EQ(a.find("mass")->read<float>()[2], 7.0f);
    CHECK(a.contains("extra"));
    CHECK_EQ(a.find("extra")->read<float>()[0], 0.0f);  // back-filled
    CHECK_EQ(a.find("extra")->read<float>()[4], 9.0f);
}
