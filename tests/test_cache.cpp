#include "pg/core/CookEngine.h"
#include "pg/core/Graph.h"

#include "test_framework.h"

using namespace pg;

namespace {

/// The cache never dereferences the node pointer, so tests can use distinct
/// fake addresses as identities.
const Node* fakeNode(uintptr_t i) { return reinterpret_cast<const Node*>(i); }

GeometryPtr somePoints(size_t n) {
    auto g = std::make_shared<Geometry>();
    g->addPoints(n);
    return g;
}

}  // namespace

TEST(cache_returns_what_was_inserted_and_counts_hits) {
    CookCache cache(1 << 20);
    const CacheKey key{fakeNode(1), 1, kAnyFrame};
    GeometryPtr geo = somePoints(10);

    CHECK_EQ(cache.find(key), GeometryPtr());
    CHECK_EQ(cache.misses(), 1u);

    cache.insert(key, geo, 100);
    CHECK_EQ(cache.find(key), geo);
    CHECK_EQ(cache.hits(), 1u);
}

TEST(a_version_bump_makes_the_old_entry_unreachable) {
    CookCache cache(1 << 20);
    GeometryPtr geo = somePoints(10);
    cache.insert(CacheKey{fakeNode(1), 1, kAnyFrame}, geo, 100);

    // Same node, next version: a different key, so a miss. This is why no
    // explicit invalidation pass is needed.
    CHECK_EQ(cache.find(CacheKey{fakeNode(1), 2, kAnyFrame}), GeometryPtr());
}

TEST(eviction_is_least_recently_used_and_respects_the_budget) {
    CookCache cache(250);
    const CacheKey k1{fakeNode(1), 1, kAnyFrame};
    const CacheKey k2{fakeNode(2), 1, kAnyFrame};
    const CacheKey k3{fakeNode(3), 1, kAnyFrame};

    cache.insert(k1, somePoints(1), 100);
    cache.insert(k2, somePoints(1), 100);
    CHECK_EQ(cache.entryCount(), 2u);
    CHECK_EQ(cache.memoryUsed(), 200u);

    cache.find(k1);  // k1 becomes the most recent, so k2 is now the victim
    cache.insert(k3, somePoints(1), 100);

    CHECK(cache.memoryUsed() <= cache.memoryBudget());
    CHECK_EQ(cache.evictions(), 1u);
    CHECK_NE(cache.find(k1), GeometryPtr());
    CHECK_EQ(cache.find(k2), GeometryPtr());
    CHECK_NE(cache.find(k3), GeometryPtr());
}

TEST(an_entry_larger_than_the_whole_budget_is_still_returned) {
    CookCache cache(100);
    const CacheKey k{fakeNode(1), 1, kAnyFrame};
    cache.insert(k, somePoints(1), 1000);
    CHECK_NE(cache.find(k), GeometryPtr());
    CHECK_EQ(cache.entryCount(), 1u);
}

TEST(lowering_the_budget_evicts_immediately) {
    CookCache cache(1000);
    for (uintptr_t i = 1; i <= 8; ++i) {
        cache.insert(CacheKey{fakeNode(i), 1, kAnyFrame}, somePoints(1), 100);
    }
    CHECK_EQ(cache.entryCount(), 8u);

    cache.setMemoryBudget(300);
    CHECK(cache.memoryUsed() <= 300u);
    CHECK_EQ(cache.entryCount(), 3u);
}

TEST(a_long_frame_range_stays_inside_the_cache_budget) {
    Graph g;
    Node* grid = g.create("grid", "grid");
    grid->setInt("rows", 40);
    grid->setInt("cols", 40);
    Node* xf = g.create("transform", "xf");
    xf->setInput(0, grid);
    xf->setExpression("t.x", [](const CookContext& c) { return c.time; });

    const size_t budget = 512 * 1024;
    CookEngine engine(budget);
    for (int f = 1; f <= 200; ++f) {
        engine.cook(*xf, CookContext{f / 24.0, f, 24.0});
    }

    CHECK(engine.cache().memoryUsed() <= budget);
    CHECK(engine.cache().evictions() > 0u);
    // The time-independent source survived: it was touched on every frame.
    CHECK_EQ(grid->cookCount(), 1u);
}
