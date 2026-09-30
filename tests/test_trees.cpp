//
// Trees as they grow (Tree.h) and the Tree node: the same tree for the same
// seed, moved and sized with its base and scale; each branch from its
// parent's centre line, thinner than it; the trunk forking into leaders; the
// crown's shape in how long the first branches are; leaves on the twigs and
// a tuft at the tips, facing as they say; tubes of bark closed and turned
// out; a tree on each point, the same whatever the threads; the skeleton's
// leaf points turning a leaf onto each; the examples.
//
#include "pg/core/Parallel.h"
#include "pg/core/Tree.h"
#include "pg/nodes/Nodes.h"
#include "pg/sim/Display.h"
#include "pg/sim/GeometryGraph.h"
#include "pg/sim/Network.h"

#include "test_framework.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <set>

using namespace pg;
using namespace pg::sim;

namespace {

bool near(const Vec3& a, const Vec3& b, float eps) { return length(a - b) <= eps; }

/// Where on a stem `t` of its length is, and its radius there.
Vec3 pointOn(const TreeStem& st, float t, float& radius) {
    const size_t n = st.points.size() - 1;
    const float f = std::clamp(t, 0.0f, 1.0f) * static_cast<float>(n);
    const size_t i = std::min(static_cast<size_t>(f), n - 1);
    const float u = f - static_cast<float>(i);
    radius = st.radius[i] * (1.0f - u) + st.radius[i + 1] * u;
    return st.points[i] * (1.0f - u) + st.points[i + 1] * u;
}

bool sameTree(const Tree& a, const Tree& b) {
    if (a.stems.size() != b.stems.size() || a.leaves.size() != b.leaves.size()) return false;
    for (size_t i = 0; i < a.stems.size(); ++i) {
        if (a.stems[i].points != b.stems[i].points || a.stems[i].radius != b.stems[i].radius) return false;
    }
    for (size_t i = 0; i < a.leaves.size(); ++i) {
        if (!(a.leaves[i].at == b.leaves[i].at) || !(a.leaves[i].facing == b.leaves[i].facing)) return false;
    }
    return true;
}

/// Each tree's foot: the middle of the points of flex 0, the ring round
/// the trunk's base -- by the primitive attribute tree.
std::vector<Vec3> footRings(const Geometry& geo) {
    const auto tree = geo.primitives().find("tree")->read<int32_t>();
    const auto flex = geo.points().find("flex")->read<float>();
    std::map<int32_t, std::set<uint32_t>> ring;
    for (size_t p = 0; p < geo.primitiveCount(); ++p) {
        for (const uint32_t q : geo.primitivePoints(p)) {
            if (flex[q] == 0.0f) ring[tree[p]].insert(q);
        }
    }
    std::vector<Vec3> out;
    for (const auto& [k, points] : ring) {
        Vec3 sum;
        for (const uint32_t q : points) sum += geo.positions()[q];
        out.push_back(sum * (1.0f / static_cast<float>(points.size())));
    }
    return out;
}

/// How high each tree's trunk reaches.
std::vector<float> trunkTops(const Geometry& geo) {
    const auto tree = geo.primitives().find("tree")->read<int32_t>();
    const auto stem = geo.primitives().find("stem")->read<int32_t>();
    std::map<int32_t, float> top;
    for (size_t p = 0; p < geo.primitiveCount(); ++p) {
        if (stem[p] != 0) continue;
        for (const uint32_t q : geo.primitivePoints(p)) {
            auto [it, fresh] = top.emplace(tree[p], geo.positions()[q].y);
            if (!fresh) it->second = std::max(it->second, geo.positions()[q].y);
        }
    }
    std::vector<float> out;
    for (const auto& [k, y] : top) out.push_back(y);
    return out;
}

std::unique_ptr<pg::Node> treeNode() {
    registerBuiltinNodes();
    return NodeRegistry::instance().create("tree", "tree");
}

/// Points at `at`, with pscale and id when given.
GeometryPtr pointsAt(const std::vector<Vec3>& at, const std::vector<float>& pscale = {},
                     const std::vector<int32_t>& id = {}) {
    auto geo = std::make_shared<Geometry>();
    geo->addPoints(at.size());
    std::copy(at.begin(), at.end(), geo->positionsForWrite().begin());
    if (!pscale.empty()) {
        auto out = geo->points().create("pscale", AttrType::Float).write<float>();
        std::copy(pscale.begin(), pscale.end(), out.begin());
    }
    if (!id.empty()) {
        auto out = geo->points().create("id", AttrType::Int).write<int32_t>();
        std::copy(id.begin(), id.end(), out.begin());
    }
    return geo;
}

}  // namespace

TEST(tree_is_the_same_for_the_same_seed_moved_and_sized_with_its_base) {
    const TreeSettings s;
    const Tree a = growTree(s, Vec3(), 1.0f, 7), b = growTree(s, Vec3(), 1.0f, 7), c = growTree(s, Vec3(), 1.0f, 8);
    CHECK(sameTree(a, b));
    CHECK(!sameTree(a, c));
    CHECK_EQ(a.stems[0].level, 0);
    CHECK(a.stems.size() > 100 && a.leaves.size() > 1000);
    CHECK_NEAR(a.height, s.height, 1e-6);
    // Stood elsewhere, twice as big: the same tree, moved and sized.
    const Vec3 base(2.0f, 0.5f, -1.0f);
    const Tree d = growTree(s, base, 2.0f, 7);
    CHECK_EQ(d.stems.size(), a.stems.size());
    CHECK_EQ(d.leaves.size(), a.leaves.size());
    for (size_t i = 0; i < a.stems.size(); ++i) {
        CHECK(near(d.stems[i].points.back(), base + a.stems[i].points.back() * 2.0f, 2e-3f));
        CHECK_NEAR(d.stems[i].radius[0], 2.0f * a.stems[i].radius[0], 1e-5);
    }
    CHECK_NEAR(d.height, 2.0f * a.height, 1e-5);
}

TEST(tree_branches_grow_from_their_parents_thinner_level_by_level) {
    const TreeSettings s;
    const Tree t = growTree(s, Vec3(), 1.0f, 3);
    std::array<size_t, 4> perLevel{};
    int lastLevel = 0;
    for (const TreeStem& st : t.stems) {
        CHECK(st.level >= lastLevel);  // level by level
        lastLevel = st.level;
        perLevel[static_cast<size_t>(st.level)]++;
        CHECK_EQ(st.points.size(), st.radius.size());
        // Thinner towards its tip; its points as far apart all along.
        const float piece = st.length / static_cast<float>(st.points.size() - 1);
        for (size_t i = 1; i < st.points.size(); ++i) {
            CHECK(st.radius[i] <= st.radius[i - 1] + 1e-7f);
            CHECK_NEAR(length(st.points[i] - st.points[i - 1]), piece, 1e-3 * piece + 1e-6);
        }
        if (st.parent < 0) continue;
        // From its parent's centre line, inside it: as thick as Thickness of it there.
        const TreeStem& parent = t.stems[static_cast<size_t>(st.parent)];
        CHECK_EQ(st.level, parent.level + 1);
        float r = 0.0f;
        CHECK(near(st.points[0], pointOn(parent, st.along, r), 1e-4f));
        CHECK_NEAR(st.radius[0], s.thickness * r, 1e-6);
        CHECK_NEAR(st.path, parent.path + st.along * parent.length, 1e-4);
    }
    CHECK_EQ(perLevel[0], size_t(1));
    CHECK_EQ(perLevel[1], size_t(s.branches[0]));  // none too short to grow
    CHECK_EQ(perLevel[2], perLevel[1] * size_t(s.branches[1]));
    // The shortest twigs, shorter than half a leaf, do not grow.
    CHECK(perLevel[3] > perLevel[2] * 3 && perLevel[3] <= perLevel[2] * size_t(s.branches[2]));
}

TEST(tree_trunk_forks_into_leaders_that_share_the_first_branches) {
    TreeSettings s;
    s.forks = 3;
    s.forkHeight = 0.4f;
    s.forkAngle = 30.0f;
    const Tree t = growTree(s, Vec3(), 1.0f, 5);
    const TreeStem& trunk = t.stems[0];
    const Vec3 fork = trunk.points[trunk.points.size() - 2];
    CHECK_NEAR(fork.y, 0.4f * s.height, 0.5f * s.segment);
    std::vector<size_t> leaders;
    for (size_t i = 1; i < t.stems.size(); ++i) {
        if (t.stems[i].level == 0) leaders.push_back(i);
    }
    CHECK_EQ(leaders.size(), size_t(3));
    for (const size_t i : leaders) {
        const TreeStem& leader = t.stems[i];
        CHECK_EQ(leader.parent, 0);
        CHECK(near(leader.points[0], fork, 1e-5f));
        // As thick as their areas add up to the trunk's, and a little more.
        CHECK_NEAR(leader.radius[0], trunk.radius[trunk.points.size() - 2] * 1.1f / std::sqrt(3.0f), 1e-6);
        // Diverging from the way the trunk went.
        const Vec3 up = normalize(fork - trunk.points[trunk.points.size() - 3]);
        const float angle = std::acos(std::clamp(dot(normalize(leader.points[1] - leader.points[0]), up), -1.0f, 1.0f));
        CHECK(angle > 0.25f && angle < 0.8f);
        CHECK(leader.points.back().y > fork.y + 0.3f * s.height);
        CHECK_NEAR(leader.path, fork.y, 0.05);
    }
    // The first branches on the trunk above the crown's foot and on the
    // leaders: as many as Branches say, all told.
    size_t first = 0;
    for (const TreeStem& st : t.stems) first += st.level == 1 ? 1 : 0;
    CHECK_EQ(first, size_t(s.branches[0]));
    // No leaves where it forks.
    for (const TreeLeaf& leaf : t.leaves) CHECK(leaf.stem != 0);
}

TEST(tree_crown_shape_is_how_long_the_first_branches_are_up_the_trunk) {
    // The mean length of the first branches in the crown's lower, middle
    // and upper third.
    auto thirds = [](TreeSettings::Shape shape) {
        TreeSettings s;
        s.shape = shape;
        s.levels = 1;
        s.branches = {90, 0, 0};
        s.wobble = 0.0f;
        s.crown = 0.2f;
        const Tree t = growTree(s, Vec3(), 1.0f, 11);
        std::array<float, 3> sum{}, n{};
        for (const TreeStem& st : t.stems) {
            if (st.level != 1) continue;
            const size_t third = static_cast<size_t>(std::clamp(static_cast<int>((st.along - s.crown) / (1.0f - s.crown) * 3.0f), 0, 2));
            sum[third] += st.length;
            n[third] += 1.0f;
        }
        return std::array<float, 3>{sum[0] / n[0], sum[1] / n[1], sum[2] / n[2]};
    };
    using Shape = TreeSettings::Shape;
    const auto conical = thirds(Shape::Conical), spherical = thirds(Shape::Spherical);
    const auto umbrella = thirds(Shape::Umbrella), cylindrical = thirds(Shape::Cylindrical);
    const auto flame = thirds(Shape::Flame), dome = thirds(Shape::Hemispherical);
    CHECK(conical[0] > conical[1] && conical[1] > conical[2]);
    CHECK(umbrella[0] < umbrella[1] && umbrella[1] < umbrella[2]);
    CHECK(spherical[1] > spherical[0] && spherical[1] > spherical[2]);
    CHECK(flame[0] > flame[2] && flame[1] > flame[2]);
    CHECK(dome[0] > dome[2] && dome[1] > dome[2]);
    const float most = std::max({cylindrical[0], cylindrical[1], cylindrical[2]});
    const float least = std::min({cylindrical[0], cylindrical[1], cylindrical[2]});
    CHECK(most < 1.15f * least);
}

TEST(tree_leaves_on_the_twigs_a_tuft_at_the_tips_facing_up) {
    TreeSettings s;
    s.leaves = 9;
    const Tree t = growTree(s, Vec3(), 1.0f, 2);
    std::vector<int> children(t.stems.size(), 0), leaves(t.stems.size(), 0);
    for (const TreeStem& st : t.stems) {
        if (st.parent >= 0) children[static_cast<size_t>(st.parent)]++;
    }
    size_t up = 0;
    for (const TreeLeaf& leaf : t.leaves) {
        leaves[static_cast<size_t>(leaf.stem)]++;
        CHECK_NEAR(length(leaf.along), 1.0, 1e-4);
        CHECK_NEAR(length(leaf.facing), 1.0, 1e-4);
        CHECK_NEAR(dot(leaf.along, leaf.facing), 0.0, 1e-4);
        CHECK(leaf.size >= 0.8f * s.leafSize - 1e-6f && leaf.size <= 1.2f * s.leafSize + 1e-6f);
        up += leaf.facing.y > 0.0f ? 1 : 0;
        // At its stem, on its bark.
        const TreeStem& st = t.stems[static_cast<size_t>(leaf.stem)];
        const float along = (leaf.path - st.path) / st.length;
        CHECK(along >= 0.0f && along <= 1.0f + 1e-5f);
        float r = 0.0f;
        CHECK_NEAR(length(leaf.at - pointOn(st, along, r)), r, 1e-4);
    }
    CHECK(up > t.leaves.size() * 9 / 10);
    // Nine on each twig, three at the tip of each stem that bears others.
    for (size_t i = 0; i < t.stems.size(); ++i) CHECK_EQ(leaves[i], children[i] == 0 ? 9 : 3);
    // None without Leaves.
    s.leaves = 0;
    CHECK(growTree(s, Vec3(), 1.0f, 2).leaves.empty());
}

TEST(tree_grows_no_more_than_its_most_stems_and_leaves) {
    // 200 on each of 200 on each of 200 would be eight million branches.
    TreeSettings s;
    s.branches = {200, 200, 200};
    s.length = {1.0f, 1.0f, 1.0f};
    s.leaves = 0;
    CHECK_EQ(growTree(s, Vec3(), 1.0f, 1).stems.size(), size_t(200000));
    // 500 leaves on each of 2 500 twigs would be a million and a quarter.
    s.levels = 2;
    s.branches = {50, 50, 0};
    s.leaves = 500;
    const Tree t = growTree(s, Vec3(), 1.0f, 1);
    CHECK_EQ(t.stems.size(), size_t(1 + 50 + 2500));
    CHECK_EQ(t.leaves.size(), size_t(1000000));
}

TEST(tree_mesh_tubes_are_closed_and_turned_out_the_leaves_face_their_way) {
    TreeSettings s;
    s.levels = 2;
    s.branches = {6, 3, 0};
    s.leaves = 4;
    const Tree t = growTree(s, Vec3(), 1.0f, 4);
    Geometry geo;
    meshTree(t, s, 3, geo);
    const auto level = geo.primitives().find("level")->read<int32_t>();
    const auto stem = geo.primitives().find("stem")->read<int32_t>();
    const auto tree = geo.primitives().find("tree")->read<int32_t>();
    const auto P = geo.positions();
    CHECK(geo.points().find("Cd") != nullptr);
    // Each stem's faces: every edge between two of them, once each way --
    // closed and wound alike -- but round a branch's base, inside its parent.
    std::map<int, std::map<std::pair<uint32_t, uint32_t>, int>> edges;
    size_t leafPrims = 0;
    for (size_t p = 0; p < geo.primitiveCount(); ++p) {
        CHECK_EQ(tree[p], 3);
        CHECK(geo.primitiveClosed(p));
        const auto corners = geo.primitivePoints(p);
        if (level[p] < 0) {
            // A leaf: facing as its leaf does.
            const TreeLeaf& leaf = t.leaves[leafPrims++];
            CHECK_EQ(stem[p], leaf.stem);
            CHECK(dot(polygonNormal(geo, corners), leaf.facing) > 0.0f);
            continue;
        }
        CHECK_EQ(level[p], t.stems[static_cast<size_t>(stem[p])].level);
        for (size_t k = 0; k < corners.size(); ++k) edges[stem[p]][{corners[k], corners[(k + 1) % corners.size()]}]++;
        // Turned out: away from the stem's centre line.
        if (corners.size() == 4) {
            const TreeStem& st = t.stems[static_cast<size_t>(stem[p])];
            const Vec3 middle = (P[corners[0]] + P[corners[1]] + P[corners[2]] + P[corners[3]]) * 0.25f;
            Vec3 axis = st.points[0];
            for (const Vec3& q : st.points) {
                if (length(q - middle) < length(axis - middle)) axis = q;
            }
            CHECK(dot(polygonNormal(geo, corners), middle - axis) > 0.0f);
        }
    }
    CHECK_EQ(leafPrims, t.leaves.size());
    for (const auto& [id, e] : edges) {
        const TreeStem& st = t.stems[static_cast<size_t>(id)];
        size_t open = 0;
        for (const auto& [ab, n] : e) {
            CHECK_EQ(n, 1);
            open += e.count({ab.second, ab.first}) == 0 ? 1 : 0;
        }
        size_t sides = static_cast<size_t>(std::max(3, s.sides - 2 * st.level));
        if (sides == 6) sides = 7;
        CHECK_EQ(open, st.level == 0 ? size_t(0) : sides);
    }
    // flex: 0 at the trunk's foot, 1 at its top, more out along the branches.
    const auto flex = geo.points().find("flex")->read<float>();
    float lowest = 1e9f, highest = 0.0f;
    for (size_t i = 0; i < flex.size(); ++i) {
        lowest = std::min(lowest, flex[i]);
        highest = std::max(highest, flex[i]);
        if (P[i].y < 0.01f) CHECK_NEAR(flex[i], 0.0, 1e-6);
    }
    CHECK_NEAR(lowest, 0.0, 1e-6);
    CHECK(highest > 1.0f);
}

TEST(tree_node_grows_one_on_each_point_whatever_the_threads) {
    auto node = treeNode();
    node->setInt("levels", 2);
    node->setInt("branches1", 10);
    node->setInt("branches2", 4);
    node->setFloat("sizevariation", 0.0f);
    const std::vector<Vec3> at = {Vec3(0, 0, 0), Vec3(8, 0, 0), Vec3(0, 1, 8), Vec3(-8, 0, 3)};
    const GeometryPtr points = pointsAt(at, {1.0f, 2.0f, 0.5f, 1.0f});
    const GeometryPtr in[] = {points};
    const unsigned saved = TaskPool::instance().threadCount();
    TaskPool::instance().setThreadCount(1);
    const GeometryPtr one = node->cookNode(CookContext{}, in);
    TaskPool::instance().setThreadCount(4);
    const GeometryPtr four = node->cookNode(CookContext{}, in);
    TaskPool::instance().setThreadCount(saved);
    CHECK_EQ(one->hash(), four->hash());

    // Each tree on its point -- the ring of its foot round it -- its trunk
    // as tall as its pscale says; bark and leaves the primitive groups.
    const auto feet = footRings(*one);
    CHECK_EQ(feet.size(), size_t(4));
    for (size_t k = 0; k < 4; ++k) CHECK(near(feet[k], at[k], 1e-3f));
    const auto top = trunkTops(*one);
    CHECK_NEAR((top[1] - at[1].y) / (top[0] - at[0].y), 2.0, 0.04);
    CHECK_NEAR((top[2] - at[2].y) / (top[0] - at[0].y), 0.5, 0.01);
    const Group* bark = one->findGroup("bark");
    const Group* leaves = one->findGroup("leaves");
    CHECK(bark && leaves);
    const auto level = one->primitives().find("level")->read<int32_t>();
    for (size_t p = 0; p < one->primitiveCount(); ++p) {
        CHECK(bark->contains(p) == (level[p] >= 0));
        CHECK(leaves->contains(p) == (level[p] < 0));
    }

    // A point's id grows its tree, wherever it is in the list: two points
    // swapped, their ids with them, the same two trees.
    const GeometryPtr ab = pointsAt({Vec3(0, 0, 0), Vec3(9, 0, 0)}, {}, {10, 20});
    const GeometryPtr ba = pointsAt({Vec3(9, 0, 0), Vec3(0, 0, 0)}, {}, {20, 10});
    const GeometryPtr inAb[] = {ab}, inBa[] = {ba};
    const GeometryPtr treesAb = node->cookNode(CookContext{}, inAb);
    const GeometryPtr treesBa = node->cookNode(CookContext{}, inBa);
    auto sumOf = [](const Geometry& g, int which) {
        const auto t = g.primitives().find("tree")->read<int32_t>();
        const auto Q = g.positions();
        std::set<uint32_t> seen;
        Vec3 sum;
        for (size_t p = 0; p < g.primitiveCount(); ++p) {
            if (t[p] != which) continue;
            for (const uint32_t q : g.primitivePoints(p)) {
                if (seen.insert(q).second) sum += Q[q];
            }
        }
        return std::make_pair(seen.size(), sum);
    };
    CHECK_EQ(treesAb->pointCount(), treesBa->pointCount());
    CHECK_EQ(sumOf(*treesAb, 0).first, sumOf(*treesBa, 1).first);
    CHECK(near(sumOf(*treesAb, 0).second, sumOf(*treesBa, 1).second, 1e-2f));
    CHECK(!near(sumOf(*treesAb, 0).second * (1.0f / static_cast<float>(sumOf(*treesAb, 0).first)),
                sumOf(*treesAb, 1).second * (1.0f / static_cast<float>(sumOf(*treesAb, 1).first)) - Vec3(9, 0, 0), 1e-3f));

    // Without points, one tree at Center.
    node->setVec3("center", Vec3(1.0f, 2.0f, 3.0f));
    const GeometryPtr none[] = {nullptr};
    const GeometryPtr alone = node->cookNode(CookContext{}, none);
    const auto foot = footRings(*alone);
    CHECK_EQ(foot.size(), size_t(1));
    CHECK(near(foot[0], Vec3(1.0f, 2.0f, 3.0f), 1e-3f));
}

TEST(tree_skeleton_is_polylines_and_leaf_points_that_turn_a_leaf_onto_each) {
    TreeSettings s;
    s.levels = 2;
    s.branches = {8, 4, 0};
    s.leaves = 5;
    const Tree t = growTree(s, Vec3(), 1.0f, 9);
    Geometry geo;
    skeletonTree(t, 0, geo);
    CHECK_EQ(geo.primitiveCount(), t.stems.size());
    const auto pscale = geo.points().find("pscale")->read<float>();
    const auto N = geo.points().find("N")->read<Vec3>();
    const auto orient = geo.points().find("orient")->read<Vec4>();
    const auto parent = geo.primitives().find("parent")->read<int32_t>();
    for (size_t p = 0; p < geo.primitiveCount(); ++p) {
        CHECK(!geo.primitiveClosed(p));
        const TreeStem& st = t.stems[p];
        CHECK_EQ(parent[p], st.parent);
        const auto corners = geo.primitivePoints(p);
        CHECK_EQ(corners.size(), st.points.size());
        for (size_t i = 0; i < corners.size(); ++i) {
            CHECK(geo.positions()[corners[i]] == st.points[i]);
            CHECK_EQ(pscale[corners[i]], st.radius[i]);
        }
    }
    // The leaves: the points after the stems', each turning a leaf lying
    // flat -- +y up, along +z -- onto its own.
    const size_t first = geo.pointCount() - t.leaves.size();
    for (size_t i = 0; i < t.leaves.size(); ++i) {
        const TreeLeaf& leaf = t.leaves[i];
        const size_t q = first + i;
        CHECK(geo.positions()[q] == leaf.at);
        CHECK(N[q] == leaf.facing);
        CHECK_EQ(pscale[q], leaf.size);
        const Vec4 o = orient[q];
        CHECK_NEAR(o.x * o.x + o.y * o.y + o.z * o.z + o.w * o.w, 1.0, 1e-4);
        const Vec3 u(o.x, o.y, o.z);
        auto turned = [&](const Vec3& v) {
            const Vec3 w = cross(u, v) * 2.0f;
            return v + w * o.w + cross(u, w);
        };
        CHECK(near(turned(Vec3(0.0f, 1.0f, 0.0f)), leaf.facing, 1e-4f));
        CHECK(near(turned(Vec3(0.0f, 0.0f, 1.0f)), leaf.along, 1e-4f));
    }

    // Through the node: the leaves' points -- those of no primitive -- in
    // the group leaves.
    auto node = treeNode();
    node->setInt("output", 1);
    const GeometryPtr none[] = {nullptr};
    const GeometryPtr out = node->cookNode(CookContext{}, none);
    const Group* leaves = out->findGroup("leaves");
    CHECK(leaves != nullptr);
    std::vector<uint8_t> used(out->pointCount(), 0);
    for (const uint32_t q : out->vertexPoints()) used[q] = 1;
    size_t loose = 0;
    for (size_t q = 0; q < used.size(); ++q) {
        CHECK(leaves->contains(q) == !used[q]);
        loose += used[q] ? 0 : 1;
    }
    CHECK(loose > 1000);
}

TEST(tree_examples_cook_and_the_forest_sways_from_its_feet) {
    Network shapes;
    CHECK(Network::example("tree_shapes", shapes));
    GeometryGraph g;
    g.sync(shapes);
    const GeometryPtr all = g.cook(shapes.displayed(), 1);
    CHECK(all != nullptr);
    std::set<int32_t> kinds;
    for (const int32_t k : all->primitives().find("tree")->read<int32_t>()) kinds.insert(k);
    CHECK_EQ(kinds.size(), size_t(1));  // each node's one tree is its tree 0
    CHECK(all->findGroup("leaves") && all->findGroup("bark"));

    // The forest, a few trees of it: the wind moves the crowns, not the feet.
    Network forest;
    CHECK(Network::example("forest", forest));
    int spots = -1, wind = -1;
    for (const auto& n : forest.nodes()) {
        if (n.name == "spots") spots = n.id;
        if (n.name == "wind") wind = n.id;
    }
    CHECK(spots >= 0 && wind >= 0);
    CHECK(forest.setParam(spots, "count", "4"));
    GeometryGraph h;
    h.sync(forest);
    const GeometryPtr a = h.cook(wind, 1), b = h.cook(wind, 30);
    CHECK(a && b && a->pointCount() == b->pointCount() && a->pointCount() > 0);
    const auto flex = a->points().find("flex")->read<float>();
    const auto Pa = a->positions(), Pb = b->positions();
    float feet = 0.0f, crowns = 0.0f;
    for (size_t i = 0; i < Pa.size(); ++i) {
        const float moved = length(Pb[i] - Pa[i]);
        if (flex[i] < 1e-6f) feet = std::max(feet, moved);
        if (flex[i] > 1.0f) crowns = std::max(crowns, moved);
    }
    CHECK(feet < 1e-6f);
    CHECK(crowns > 0.02f);
    // Only the points move: the viewport sends only their places again.
    DisplayMesher mesher;
    DisplayMesh mesh;
    CHECK(mesher.make(a, mesh) == DisplayMesher::Made::Anew);
    CHECK(mesher.make(b, mesh) == DisplayMesher::Made::Moved);
}
