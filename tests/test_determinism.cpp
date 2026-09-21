//
// Invariant I5: identical output regardless of thread count.
//
// A studio renders the same scene on hundreds of machines with different core
// counts. If a cook is scheduling-dependent, frames differ, and the bug is
// found months later in a render. These tests are cheap; that class of bug is
// not.
//
#include "pg/core/CookEngine.h"
#include "pg/core/Graph.h"
#include "pg/core/Parallel.h"

#include "test_framework.h"

#include <cstring>

using namespace pg;

namespace {

/// Restores the pool size however the test exits.
struct ThreadCountGuard {
    unsigned saved = TaskPool::instance().threadCount();
    ~ThreadCountGuard() { TaskPool::instance().setThreadCount(saved); }
};

bool bitwiseEqual(float a, float b) {
    return std::memcmp(&a, &b, sizeof(float)) == 0;
}

/// grid -> wrangle -> transform, exercising per-point parallel work.
uint64_t cookSceneHash() {
    Graph g;
    Node* grid = g.create("grid", "grid");
    grid->setInt("rows", 64);
    grid->setInt("cols", 64);
    Node* w = g.create("pointwrangle", "w");
    w->setInput(0, grid);
    w->setString("snippet",
                 "@P.y = noise(@P * 4.0) * 0.5;"
                 "@Cd = vec3(@P.y, @ptnum / @numpt, 0.5);"
                 "@dist = length(@P);");
    Node* xf = g.create("transform", "xf");
    xf->setInput(0, w);
    xf->setVec3("r", Vec3(10, 20, 30));

    CookEngine engine(1ull << 30);
    return engine.cook(*xf, CookContext{})->hash();
}

}  // namespace

TEST(chunking_does_not_depend_on_the_thread_count) {
    ThreadCountGuard guard;

    TaskPool::instance().setThreadCount(1);
    const auto a = chunkRanges(1000000, 4096);
    TaskPool::instance().setThreadCount(4);
    const auto b = chunkRanges(1000000, 4096);

    CHECK_EQ(a.size(), b.size());
    for (size_t i = 0; i < a.size(); ++i) {
        CHECK_EQ(a[i].first, b[i].first);
        CHECK_EQ(a[i].second, b[i].second);
    }
}

TEST(chunking_is_a_complete_disjoint_cover) {
    const auto chunks = chunkRanges(100003, 1000);
    size_t expected = 0;
    for (const auto& [begin, end] : chunks) {
        CHECK_EQ(begin, expected);
        CHECK(end > begin);
        expected = end;
    }
    CHECK_EQ(expected, 100003u);
}

TEST(float_reduction_is_bitwise_identical_across_thread_counts) {
    ThreadCountGuard guard;

    // Magnitudes chosen so that a different summation order really would give
    // a different result: this catches a naive parallel reduction.
    std::vector<float> data(1000000);
    for (size_t i = 0; i < data.size(); ++i) {
        data[i] = (i % 2 == 0) ? 1.0e7f : 1.0e-3f;
    }

    auto sum = [&] {
        return parallelReduce<float>(
            data.size(), 4096, 0.0f,
            [&](size_t b, size_t e) {
                float acc = 0.0f;
                for (size_t i = b; i < e; ++i) acc += data[i];
                return acc;
            },
            [](float a, float b) { return a + b; });
    };

    TaskPool::instance().setThreadCount(1);
    const float serial = sum();
    TaskPool::instance().setThreadCount(2);
    const float two = sum();
    TaskPool::instance().setThreadCount(4);
    const float four = sum();

    CHECK(bitwiseEqual(serial, two));
    CHECK(bitwiseEqual(serial, four));
}

TEST(a_whole_cook_hashes_identically_across_thread_counts) {
    ThreadCountGuard guard;

    TaskPool::instance().setThreadCount(1);
    const uint64_t serial = cookSceneHash();
    TaskPool::instance().setThreadCount(2);
    const uint64_t two = cookSceneHash();
    TaskPool::instance().setThreadCount(4);
    const uint64_t four = cookSceneHash();

    CHECK_EQ(serial, two);
    CHECK_EQ(serial, four);
}

TEST(the_point_cloud_generator_is_index_derived_not_sequential) {
    ThreadCountGuard guard;

    auto build = [] {
        Graph g;
        Node* pc = g.create("pointcloud", "pc");
        pc->setInt("count", 200000);
        pc->setInt("seed", 7);
        CookEngine engine(1ull << 30);
        return engine.cook(*pc, CookContext{})->hash();
    };

    TaskPool::instance().setThreadCount(1);
    const uint64_t serial = build();
    TaskPool::instance().setThreadCount(4);
    const uint64_t parallel = build();
    CHECK_EQ(serial, parallel);
}

TEST(parallel_branch_cooking_does_not_change_the_result) {
    Graph g;
    Node* m = g.create("merge", "m");
    for (int i = 0; i < 4; ++i) {
        Node* src = g.create("grid", "g" + std::to_string(i));
        src->setInt("rows", 8 + i);
        src->setInt("cols", 8);
        Node* xf = g.create("transform", "x" + std::to_string(i));
        xf->setInput(0, src);
        xf->setVec3("t", Vec3(static_cast<float>(i), 0, 0));
        m->setInput(static_cast<size_t>(i), xf);
    }

    CookEngine serialEngine(1ull << 30);
    serialEngine.setParallelBranches(false);
    const uint64_t serial = serialEngine.cook(*m, CookContext{})->hash();

    CookEngine parallelEngine(1ull << 30);
    parallelEngine.setParallelBranches(true);
    const uint64_t parallel = parallelEngine.cook(*m, CookContext{})->hash();

    CHECK_EQ(serial, parallel);
}
