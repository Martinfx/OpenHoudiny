//
// Invariant I4: lazy pull evaluation with dirty propagation.
//
// The headline assertion is the 100-node chain: editing halfway down must cost
// half the graph, not all of it. That is what makes an interactive viewport
// possible at all, and it is checked here rather than eyeballed in a profiler.
//
#include "pg/core/CookEngine.h"
#include "pg/core/Graph.h"

#include "test_framework.h"

using namespace pg;

namespace {

constexpr size_t kBigBudget = 1ull << 30;

uint64_t totalCookCount(const Graph& g) {
    uint64_t total = 0;
    for (Node* n : g.nodes()) total += n->cookCount();
    return total;
}

void resetCookCounts(const Graph& g) {
    for (Node* n : g.nodes()) n->resetCookCount();
}

struct Chain {
    Graph graph;
    Node* source = nullptr;
    std::vector<Node*> links;  // links[i] is transform number i+1
};

/// grid -> transform x `length`
std::unique_ptr<Chain> buildChain(int length) {
    auto c = std::make_unique<Chain>();
    c->source = c->graph.create("grid", "src");
    c->source->setInt("rows", 4);
    c->source->setInt("cols", 4);

    Node* prev = c->source;
    for (int i = 1; i <= length; ++i) {
        Node* t = c->graph.create("transform", "t" + std::to_string(i));
        t->setInput(0, prev);
        c->links.push_back(t);
        prev = t;
    }
    return c;
}

}  // namespace

TEST(editing_the_middle_of_a_100_node_chain_recooks_only_downstream) {
    auto c = buildChain(100);
    CookEngine engine(kBigBudget);
    const CookContext ctx;

    engine.cook(*c->links.back(), ctx);
    CHECK_EQ(totalCookCount(c->graph), 101u);  // grid + 100 transforms

    resetCookCounts(c->graph);
    c->links[49]->setVec3("t", Vec3(1, 0, 0));  // t50
    engine.cook(*c->links.back(), ctx);

    // t50 through t100 inclusive.
    CHECK_EQ(totalCookCount(c->graph), 51u);
    CHECK_EQ(c->source->cookCount(), 0u);
    CHECK_EQ(c->links[48]->cookCount(), 0u);  // t49 untouched
    CHECK_EQ(c->links[49]->cookCount(), 1u);
}

TEST(recooking_an_unchanged_graph_does_no_work) {
    auto c = buildChain(20);
    CookEngine engine(kBigBudget);
    const CookContext ctx;

    engine.cook(*c->links.back(), ctx);
    resetCookCounts(c->graph);

    engine.cook(*c->links.back(), ctx);
    engine.cook(*c->links.back(), ctx);
    CHECK_EQ(totalCookCount(c->graph), 0u);
}

TEST(setting_a_parameter_to_its_current_value_dirties_nothing) {
    auto c = buildChain(10);
    CookEngine engine(kBigBudget);
    const CookContext ctx;

    c->links[5]->setVec3("t", Vec3(2, 0, 0));
    engine.cook(*c->links.back(), ctx);
    resetCookCounts(c->graph);

    c->links[5]->setVec3("t", Vec3(2, 0, 0));  // same value
    engine.cook(*c->links.back(), ctx);
    CHECK_EQ(totalCookCount(c->graph), 0u);

    c->links[5]->setVec3("t", Vec3(3, 0, 0));  // different
    engine.cook(*c->links.back(), ctx);
    CHECK_EQ(totalCookCount(c->graph), 5u);  // t6..t10
}

TEST(rewiring_an_input_dirties_downstream) {
    Graph g;
    Node* a = g.create("grid", "a");
    Node* b = g.create("grid", "b");
    Node* out = g.create("transform", "out");
    out->setInput(0, a);

    CookEngine engine(kBigBudget);
    const CookContext ctx;
    engine.cook(*out, ctx);
    resetCookCounts(g);

    out->setInput(0, b);
    engine.cook(*out, ctx);
    CHECK_EQ(out->cookCount(), 1u);
}

TEST(time_dependency_is_inferred_and_propagates_downstream) {
    Graph g;
    Node* grid = g.create("grid", "grid");
    Node* animated = g.create("transform", "animated");
    Node* after = g.create("transform", "after");
    animated->setInput(0, grid);
    after->setInput(0, animated);
    animated->setExpression("t.x", [](const CookContext& c) { return c.time; });

    CookEngine engine(kBigBudget);
    CHECK(!engine.isTimeDependent(*grid));
    CHECK(engine.isTimeDependent(*animated));
    CHECK(engine.isTimeDependent(*after));

    for (int f = 1; f <= 5; ++f) {
        engine.cook(*after, CookContext{f / 24.0, f, 24.0});
    }
    // The time-independent source is cooked once and reused for every frame.
    CHECK_EQ(grid->cookCount(), 1u);
    CHECK_EQ(animated->cookCount(), 5u);
    CHECK_EQ(after->cookCount(), 5u);

    // Revisiting a frame hits the per-frame cache.
    engine.cook(*after, CookContext{3 / 24.0, 3, 24.0});
    CHECK_EQ(after->cookCount(), 5u);
}

TEST(an_expression_actually_drives_the_parameter) {
    Graph g;
    Node* grid = g.create("grid", "grid");
    grid->setInt("rows", 2);
    grid->setInt("cols", 2);
    Node* xf = g.create("transform", "xf");
    xf->setInput(0, grid);
    xf->setExpression("t.x", [](const CookContext& c) { return c.time * 10.0; });

    CookEngine engine(kBigBudget);
    GeometryPtr at1 = engine.cook(*xf, CookContext{1.0, 1, 24.0});
    GeometryPtr at2 = engine.cook(*xf, CookContext{2.0, 2, 24.0});

    CHECK_NEAR(at2->positions()[0].x - at1->positions()[0].x, 10.0, 1e-4);
}

TEST(a_connection_that_would_close_a_loop_is_refused) {
    Graph g;
    Node* a = g.create("null", "a");
    Node* b = g.create("null", "b");
    Node* c = g.create("null", "c");

    CHECK(b->setInput(0, a));
    CHECK(c->setInput(0, b));
    CHECK(!a->setInput(0, c));   // would close a -> b -> c -> a
    CHECK_EQ(a->input(0), static_cast<Node*>(nullptr));
    CHECK(!a->setInput(0, a));   // self-loop
}

TEST(merge_concatenates_all_connected_inputs) {
    Graph g;
    Node* m = g.create("merge", "m");
    for (int i = 0; i < 3; ++i) {
        Node* src = g.create("grid", "g" + std::to_string(i));
        src->setInt("rows", 2);
        src->setInt("cols", 2);
        m->setInput(static_cast<size_t>(i), src);
    }

    CookEngine engine(kBigBudget);
    GeometryPtr out = engine.cook(*m, CookContext{});

    CHECK_EQ(out->pointCount(), 12u);     // 3 x 4
    CHECK_EQ(out->primitiveCount(), 3u);  // 3 x 1 quad
    CHECK_EQ(out->primitivePoints(2)[0], 8u);
}

TEST(a_shared_upstream_node_is_cooked_once_per_evaluation) {
    // Diamond: src feeds two transforms that meet in a merge. With branch
    // parallelism off the shared source must be cooked exactly once.
    Graph g;
    Node* src = g.create("grid", "src");
    Node* left = g.create("transform", "left");
    Node* right = g.create("transform", "right");
    Node* m = g.create("merge", "m");
    left->setInput(0, src);
    right->setInput(0, src);
    m->setInput(0, left);
    m->setInput(1, right);

    CookEngine engine(kBigBudget);
    engine.setParallelBranches(false);
    engine.cook(*m, CookContext{});
    CHECK_EQ(src->cookCount(), 1u);
}
