//
// Measures the claims the architecture rests on. Numbers here are what the
// ROADMAP's gate criteria are checked against.
//
#include "pg/core/Attribute.h"
#include "pg/core/CookEngine.h"
#include "pg/core/Graph.h"
#include "pg/core/Parallel.h"

#include <chrono>
#include <cstdio>
#include <string>

using namespace pg;
using Clock = std::chrono::steady_clock;

namespace {

template <class Fn>
double timeMs(Fn&& fn) {
    const auto t0 = Clock::now();
    fn();
    const auto t1 = Clock::now();
    return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

void header(const char* title) {
    std::printf("\n%s\n", title);
    for (size_t i = 0; i < std::string(title).size(); ++i) std::printf("-");
    std::printf("\n");
}

// --- 1. copy-on-write ------------------------------------------------------

void benchCow() {
    header("1. Copy-on-write: 50-node chain, 2M points, 5 attributes");

    constexpr size_t kPoints = 2'000'000;
    AttributeSet base;
    base.setElementCount(kPoints);
    for (const char* n : {"P", "N", "Cd"}) base.create(n, AttrType::Vec3);
    base.create("mass", AttrType::Float);
    base.create("id", AttrType::Int);

    const size_t bytesPerCopy = base.memoryUsage();
    resetAttributeAllocationCount();

    std::vector<AttributeSet> chain;
    chain.reserve(51);
    chain.push_back(base);
    const double ms = timeMs([&] {
        for (int i = 0; i < 50; ++i) {
            AttributeSet next = chain.back();
            next.find("P")->write<Vec3>()[0] = Vec3(static_cast<float>(i), 0, 0);
            chain.push_back(std::move(next));
        }
    });

    const uint64_t allocs = attributeAllocationCount();
    const double actualMb = static_cast<double>(allocs * kPoints * sizeof(Vec3)) / (1024 * 1024);
    const double naiveMb = static_cast<double>(50 * bytesPerCopy) / (1024 * 1024);

    std::printf("  buffer allocations   %llu  (deep copy would be %d)\n",
                static_cast<unsigned long long>(allocs), 50 * 5);
    std::printf("  memory               %.0f MB  (deep copy: %.0f MB, %.1fx more)\n",
                actualMb, naiveMb, naiveMb / actualMb);
    std::printf("  wall time            %.1f ms\n", ms);
}

// --- 2. dirty propagation --------------------------------------------------

void benchDirty() {
    header("2. Dirty propagation: 100-node chain, 250k points");

    Graph g;
    Node* src = g.create("grid", "src");
    src->setInt("rows", 500);
    src->setInt("cols", 500);

    std::vector<Node*> chain;
    Node* prev = src;
    for (int i = 1; i <= 100; ++i) {
        Node* t = g.create("transform", "t" + std::to_string(i));
        t->setVec3("t", Vec3(0.001f, 0, 0));
        t->setInput(0, prev);
        chain.push_back(t);
        prev = t;
    }

    CookEngine engine(4ull << 30);
    const double cold = timeMs([&] { engine.cook(*prev, CookContext{}); });

    for (Node* n : g.nodes()) n->resetCookCount();
    const double warm = timeMs([&] { engine.cook(*prev, CookContext{}); });

    for (Node* n : g.nodes()) n->resetCookCount();
    chain[49]->setVec3("t", Vec3(0.5f, 0, 0));
    const double mid = timeMs([&] { engine.cook(*prev, CookContext{}); });
    uint64_t cooked = 0;
    for (Node* n : g.nodes()) cooked += n->cookCount();

    for (Node* n : g.nodes()) n->resetCookCount();
    chain[98]->setVec3("t", Vec3(0.25f, 0, 0));
    const double late = timeMs([&] { engine.cook(*prev, CookContext{}); });

    std::printf("  cold cook (101 nodes)      %7.1f ms\n", cold);
    std::printf("  no-op recook               %7.3f ms   (0 nodes)\n", warm);
    std::printf("  edit at node 50            %7.1f ms   (%llu nodes, %.0f%% of cold)\n",
                mid, static_cast<unsigned long long>(cooked), 100.0 * mid / cold);
    std::printf("  edit at node 99            %7.1f ms   (2 nodes)\n", late);
}

// --- 3. per-element language ----------------------------------------------

void benchWrangle() {
    header("3. Per-element language (tree-walking interpreter)");

    struct Case { const char* label; const char* snippet; };
    const Case cases[] = {
        {"noise-bound   ", "@P.y = noise(@P * 3.0);"},
        {"arithmetic-bnd", "@P.y = @P.x * 2.0 + @P.z * 3.0 - @P.x * @P.z + "
                           "@P.x * @P.x * 0.5 - @P.z * 0.25 + 1.0;"},
    };

    for (const auto& c : cases) {
        Graph g;
        Node* src = g.create("grid", "src");
        src->setInt("rows", 1000);
        src->setInt("cols", 1000);
        Node* w = g.create("pointwrangle", "w");
        w->setInput(0, src);
        w->setString("snippet", c.snippet);

        CookEngine engine(8ull << 30);
        GeometryPtr in = engine.cook(*src, CookContext{});
        const size_t n = in->pointCount();
        const double ms = timeMs([&] { engine.cook(*w, CookContext{}); });

        std::printf("  %s  %8zu pts   %7.1f ms   %6.1f Mpts/s\n",
                    c.label, n, ms, static_cast<double>(n) / ms / 1000.0);
    }
    std::printf("\n  The noise case is dominated by a native builtin, so it flatters the\n");
    std::printf("  interpreter. The arithmetic case is the honest baseline: it is pure\n");
    std::printf("  AST walking, and it is what the M5 JIT has to beat.\n");
}

// --- 4. cache over a frame range ------------------------------------------

void benchCache() {
    header("4. Cook cache over a 240-frame range");

    Graph g;
    Node* src = g.create("grid", "src");
    src->setInt("rows", 300);
    src->setInt("cols", 300);
    Node* w = g.create("pointwrangle", "w");
    w->setInput(0, src);
    w->setString("snippet", "@P.y = noise(@P * 2.0 + vec3(@Time, 0.0, 0.0));");
    Node* xf = g.create("transform", "xf");
    xf->setInput(0, w);

    CookEngine engine(256ull * 1024 * 1024);
    const double ms = timeMs([&] {
        for (int f = 1; f <= 240; ++f) engine.cook(*xf, CookContext{f / 24.0, f, 24.0});
    });

    std::printf("  240 frames                 %7.1f ms  (%.1f ms/frame)\n", ms, ms / 240.0);
    std::printf("  source cooks               %7llu   (time-independent, reused)\n",
                static_cast<unsigned long long>(src->cookCount()));
    std::printf("  cache hits / misses        %7llu / %llu\n",
                static_cast<unsigned long long>(engine.cache().hits()),
                static_cast<unsigned long long>(engine.cache().misses()));
    std::printf("  evictions                  %7llu   (budget %zu MB, used %zu MB)\n",
                static_cast<unsigned long long>(engine.cache().evictions()),
                engine.cache().memoryBudget() / (1024 * 1024),
                engine.cache().memoryUsed() / (1024 * 1024));
}

// --- 5. thread scaling -----------------------------------------------------

void benchScaling() {
    header("5. Thread scaling and determinism");

    const unsigned saved = TaskPool::instance().threadCount();
    uint64_t reference = 0;
    double serialMs = 0.0;

    for (unsigned threads : {1u, 2u, 4u}) {
        TaskPool::instance().setThreadCount(threads);

        Graph g;
        Node* src = g.create("grid", "src");
        src->setInt("rows", 700);
        src->setInt("cols", 700);
        Node* w = g.create("pointwrangle", "w");
        w->setInput(0, src);
        w->setString("snippet", "@P.y = noise(@P * 3.0); @Cd = vec3(@P.y, 0.5, 1.0);");

        CookEngine engine(8ull << 30);
        GeometryPtr result;
        // hash() is a serial byte-at-a-time pass; timing it here would measure
        // the checksum, not the cook.
        const double ms = timeMs([&] { result = engine.cook(*w, CookContext{}); });
        const uint64_t hash = result->hash();

        if (threads == 1) { reference = hash; serialMs = ms; }
        std::printf("  %u thread(s)   %7.1f ms   speedup %4.2fx   hash %016llx  %s\n",
                    threads, ms, serialMs / ms,
                    static_cast<unsigned long long>(hash),
                    hash == reference ? "identical" : "*** DIFFERS ***");
    }
    TaskPool::instance().setThreadCount(saved);
}

}  // namespace

int main() {
    std::printf("procedural geometry core -- benchmarks\n");
    std::printf("hardware threads available: %u\n", TaskPool::instance().threadCount());
    benchCow();
    benchDirty();
    benchWrangle();
    benchCache();
    benchScaling();
    std::printf("\n");
    return 0;
}
