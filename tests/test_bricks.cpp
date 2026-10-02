//
// Brick Wall (src/pg/nodes/Bricks.cpp): a wall laid of bricks as a
// bricklayer lays it -- closed bricks with their mortar that make the wall,
// courses that break joint as their bond says, straight reveals round
// openings, plaster on the faces, some bricks cut in two and held harder
// -- whatever way the wall stands, the same every time; and the RBD Solver
// with them: the wall stands on its mortar, and a knock breaks it along its
// joints -- and the family house of the example house_collapse, built of
// them.
//
#include "pg/core/CookEngine.h"
#include "pg/core/Graph.h"
#include "pg/core/Parallel.h"
#include "pg/nodes/Nodes.h"
#include "pg/sim/Network.h"
#include "pg/sim/Rigid.h"

#include "test_framework.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <set>

using namespace pg;
using namespace pg::sim;

namespace {

/// Boxes of `sizes` at `centers`, merged, turned by `turn` degrees about
/// the origin: the wall; a Brick Wall over it, set up by `setup`.
GeometryPtr bricks(const std::vector<std::pair<Vec3, Vec3>>& boxes, const std::function<void(pg::Node&)>& setup,
                   Vec3 turn = Vec3()) {
    registerBuiltinNodes();
    Graph g;
    pg::Node* merge = g.create("merge", "wall");
    int k = 0;
    for (const auto& [size, center] : boxes) {
        pg::Node* box = g.create("box", "box" + std::to_string(k));
        box->setVec3("size", size);
        box->setVec3("center", center);
        merge->setInput(k++, box);
    }
    pg::Node* place = g.create("transform", "place");
    place->setInput(0, merge);
    place->setVec3("r", turn);
    pg::Node* wall = g.create("brickwall", "bricks");
    wall->setInput(0, place);
    setup(*wall);
    CookEngine engine;
    return engine.cook(*wall, CookContext{});
}

GeometryPtr plainWall(const std::function<void(pg::Node&)>& setup, Vec3 size = Vec3(2.08f, 1.2f, 0.25f)) {
    return bricks({{size, Vec3(0.0f, 0.5f * size.y, 0.0f)}}, setup);
}

/// The primitives of each piece, by the attribute piece.
std::vector<std::vector<uint32_t>> primsOfPieces(const Geometry& geo) {
    const auto piece = geo.primitives().find("piece")->read<int32_t>();
    int count = 0;
    for (const int32_t p : piece) count = std::max(count, p + 1);
    std::vector<std::vector<uint32_t>> out(static_cast<size_t>(count));
    for (size_t prim = 0; prim < piece.size(); ++prim) out[static_cast<size_t>(piece[prim])].push_back(static_cast<uint32_t>(prim));
    return out;
}

/// Every edge of the faces `prims` is shared by two of them, turned opposite ways.
bool closed(const Geometry& geo, const std::vector<uint32_t>& prims) {
    std::map<std::pair<uint32_t, uint32_t>, int> edges;
    for (const uint32_t prim : prims) {
        const auto f = geo.primitivePoints(prim);
        for (size_t i = 0; i < f.size(); ++i) ++edges[{f[i], f[(i + 1) % f.size()]}];
    }
    for (const auto& [e, count] : edges) {
        if (count != 1) return false;
        const auto back = edges.find({e.second, e.first});
        if (back == edges.end() || back->second != 1) return false;
    }
    return !edges.empty();
}

double volumeOf(const Geometry& geo, const std::vector<uint32_t>& prims, const Vec3& middle) {
    const auto P = geo.positions();
    double v = 0.0;
    for (const uint32_t prim : prims) {
        const auto f = geo.primitivePoints(prim);
        for (size_t i = 1; i + 1 < f.size(); ++i) {
            v += static_cast<double>(dot(P[f[0]] - middle, cross(P[f[i]] - middle, P[f[i + 1]] - middle))) / 6.0;
        }
    }
    return v;
}

struct Box {
    Vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
};

/// The box round each piece.
std::vector<Box> boxesOf(const Geometry& geo) {
    const auto P = geo.positions();
    std::vector<Box> out;
    for (const std::vector<uint32_t>& prims : primsOfPieces(geo)) {
        Box b;
        for (const uint32_t prim : prims) {
            for (const uint32_t p : geo.primitivePoints(prim)) {
                for (int a = 0; a < 3; ++a) {
                    b.lo[a] = std::min(b.lo[a], P[p][a]);
                    b.hi[a] = std::max(b.hi[a], P[p][a]);
                }
            }
        }
        out.push_back(b);
    }
    return out;
}

/// The courses: for each (the bottom of its bricks' boxes, to a millimetre),
/// where the bricks of the front leaf end along x -- their head joints.
std::map<long, std::set<long>> jointsByCourse(const Geometry& geo, float wallEnd) {
    std::map<long, std::set<long>> out;
    for (const Box& b : boxesOf(geo)) {
        if (b.hi.z < 0.0f) continue;  // the back leaf's
        const long course = std::lround(b.lo.y * 1000.0f);
        if (b.hi.x < wallEnd - 1e-4f) out[course].insert(std::lround(b.hi.x * 1000.0f));
    }
    return out;
}

bool about(long a, long b, long tolerance = 1) { return std::labs(a - b) <= tolerance; }

}  // namespace

TEST(brick_wall_fills_the_wall_with_closed_bricks_and_their_mortar) {
    for (const int bond : {1, 2, 3, 4}) {  // stretcher, English, Flemish, stack
        const GeometryPtr wall = plainWall([&](pg::Node& n) { n.setInt("bond", bond); });
        const std::vector<std::vector<uint32_t>> pieces = primsOfPieces(*wall);
        CHECK(pieces.size() > 250);
        double volume = 0.0;
        for (const std::vector<uint32_t>& prims : pieces) {
            CHECK(closed(*wall, prims));
            const double v = volumeOf(*wall, prims, Vec3(0.0f, 0.6f, 0.0f));
            CHECK(v > 0.0);  // turned outwards
            volume += v;
        }
        // The bricks and their mortar are the wall, to a hair.
        CHECK_NEAR(volume, 2.08 * 1.2 * 0.25, 1e-6);
        // Each face a brick's shade or the mortar's.
        const auto cd = wall->primitives().find("Cd")->read<Vec3>();
        size_t mortar = 0;
        for (const Vec3& c : cd) {
            if (c == Vec3(0.52f, 0.5f, 0.47f)) ++mortar;
            else CHECK(c.x > c.y && c.y > c.z);  // brick red, lighter or darker
        }
        CHECK(mortar > cd.size() / 4);
        CHECK(mortar < cd.size());
    }
    // A wall one leaf thick is laid in stretcher bond when Auto: every brick
    // right across it.
    const GeometryPtr thin = plainWall([](pg::Node&) {}, Vec3(2.08f, 1.2f, 0.12f));
    for (const Box& b : boxesOf(*thin)) CHECK_NEAR(b.hi.z - b.lo.z, 0.12f, 1e-5f);
    // Nothing: nothing.
    CHECK_EQ(bricks({}, [](pg::Node&) {})->primitiveCount(), size_t(0));
}

TEST(brick_wall_courses_break_joint_as_their_bond_says) {
    const float end = 1.04f;  // the wall's right end: no joint there
    // Stretcher: each course half a brick -- 13 cm -- on from the one below.
    const GeometryPtr stretcher = plainWall([](pg::Node& n) {
        n.setInt("bond", 1);  // stretcher
        n.setFloat("broken", 0.0f);
    });
    const auto sj = jointsByCourse(*stretcher, end);
    CHECK_EQ(sj.size(), size_t(16));  // 1.2 m of 75 mm courses
    for (auto it = sj.begin(); std::next(it) != sj.end(); ++it) {
        for (const long x : std::next(it)->second) {
            bool half = false;
            for (const long y : it->second) half = half || about(std::labs(x - y) % 260, 130);
            CHECK(half);
        }
    }
    // Stack: joint over joint.
    const auto st = jointsByCourse(*plainWall([](pg::Node& n) {
        n.setInt("bond", 4);  // stack
        n.setFloat("broken", 0.0f);
    }), end);
    for (const auto& [course, joints] : st) CHECK(joints == st.begin()->second);
    // English: a course of headers -- 12 cm along -- then a course of stretchers.
    const GeometryPtr english = plainWall([](pg::Node& n) {
        n.setInt("bond", 2);  // English
        n.setFloat("broken", 0.0f);
    });
    std::map<long, std::set<long>> lengths;
    for (const Box& b : boxesOf(*english)) {
        if (b.hi.x < end - 1e-4f && b.lo.x > -end + 1e-4f) lengths[std::lround(b.lo.y * 1000.0f)].insert(std::lround((b.hi.x - b.lo.x) * 1000.0f));
    }
    int headerCourses = 0, stretcherCourses = 0;
    for (const auto& [course, l] : lengths) {
        CHECK_EQ(l.size(), size_t(1));  // one kind in a course: each a brick and its joint
        if (about(*l.begin(), 130)) ++headerCourses;
        else if (about(*l.begin(), 260)) ++stretcherCourses;
    }
    CHECK_EQ(headerCourses, 8);
    CHECK_EQ(stretcherCourses, 8);
    // A header runs right across the wall; stretchers lie two leaves side by side.
    int across = 0, leaves = 0;
    for (const Box& b : boxesOf(*english)) {
        if (about(std::lround((b.hi.z - b.lo.z) * 1000.0f), 250)) ++across;
        if (about(std::lround((b.hi.z - b.lo.z) * 1000.0f), 130) || about(std::lround((b.hi.z - b.lo.z) * 1000.0f), 120)) ++leaves;
    }
    CHECK(across > 100);
    CHECK(leaves > 100);
    // Flemish: header and stretcher in turn in every course.
    const GeometryPtr flemish = plainWall([](pg::Node& n) {
        n.setInt("bond", 3);  // Flemish
        n.setFloat("broken", 0.0f);
    });
    std::map<long, std::vector<std::pair<long, long>>> rows;  // course: (start, length) of the front's bricks
    for (const Box& b : boxesOf(*flemish)) {
        if (b.hi.z < 0.0f || b.lo.x < -end + 1e-4f || b.hi.x > end - 1e-4f) continue;
        rows[std::lround(b.lo.y * 1000.0f)].push_back({std::lround(b.lo.x * 1000.0f), std::lround((b.hi.x - b.lo.x) * 1000.0f)});
    }
    for (auto& [course, row] : rows) {
        std::sort(row.begin(), row.end());
        for (size_t i = 0; i + 1 < row.size(); ++i) CHECK(about(row[i].second, 130) != about(row[i + 1].second, 130));
    }
}

TEST(brick_wall_lays_straight_reveals_round_an_opening) {
    // A wall of four boxes round a window 1.04 m wide and 0.9 m high.
    const std::vector<std::pair<Vec3, Vec3>> wall = {{Vec3(1.3f, 2.1f, 0.25f), Vec3(-1.17f, 1.05f, 0.0f)},
                                                     {Vec3(1.3f, 2.1f, 0.25f), Vec3(1.17f, 1.05f, 0.0f)},
                                                     {Vec3(1.04f, 0.6f, 0.25f), Vec3(0.0f, 0.3f, 0.0f)},
                                                     {Vec3(1.04f, 0.6f, 0.25f), Vec3(0.0f, 1.8f, 0.0f)}};
    const GeometryPtr g = bricks(wall, [](pg::Node& n) { n.setInt("bond", 3); });  // Flemish
    double volume = 0.0;
    for (const std::vector<uint32_t>& prims : primsOfPieces(*g)) {
        CHECK(closed(*g, prims));
        volume += volumeOf(*g, prims, Vec3(0.0f, 1.0f, 0.0f));
    }
    CHECK_NEAR(volume, (3.64 * 2.1 - 1.04 * 0.9) * 0.25, 1e-6);
    // Nothing in the opening: the bricks cut short at its sides.
    for (const Box& b : boxesOf(*g)) {
        const bool inside = b.lo.x < 0.52f - 1e-4f && b.hi.x > -0.52f + 1e-4f && b.lo.y < 1.5f - 1e-4f && b.hi.y > 0.6f + 1e-4f;
        CHECK(!inside);
    }
}

TEST(brick_wall_plasters_its_faces_and_breaks_some_bricks_in_two) {
    // Plaster on both faces: the faces of the wall are all plaster.
    const Vec3 plaster(0.9f, 0.1f, 0.2f);
    const GeometryPtr g = plainWall([&](pg::Node& n) {
        n.setFloat("plaster", 0.015f);
        n.setVec3("plastercolor", plaster);
        n.setFloat("broken", 0.0f);
    }, Vec3(2.08f, 1.2f, 0.28f));
    const auto P = g->positions();
    const auto cd = g->primitives().find("Cd")->read<Vec3>();
    size_t faces = 0;
    for (size_t prim = 0; prim < g->primitiveCount(); ++prim) {
        bool front = true, back = true;
        for (const uint32_t p : g->primitivePoints(prim)) {
            front = front && std::fabs(P[p].z - 0.14f) < 1e-5f;
            back = back && std::fabs(P[p].z + 0.14f) < 1e-5f;
        }
        if (!front && !back) continue;
        ++faces;
        CHECK(cd[prim] == plaster);
    }
    CHECK(faces > 200);
    CHECK(g->primitives().find("cluster") == nullptr);
    // Broken: every brick long enough cut in two, the halves one cluster,
    // held Strength times as hard.
    const GeometryPtr cut = plainWall([](pg::Node& n) {
        n.setInt("bond", 1);  // stretcher
        n.setFloat("broken", 1.0f);
        n.setFloat("strength", 12.0f);
    });
    const auto cluster = cut->primitives().find("cluster")->read<int32_t>();
    const auto glue = cut->primitives().find("clusterglue")->read<float>();
    const auto piece = cut->primitives().find("piece")->read<int32_t>();
    std::map<int32_t, std::set<int32_t>> halves;
    for (size_t prim = 0; prim < cluster.size(); ++prim) {
        if (cluster[prim] == 0) continue;
        halves[cluster[prim]].insert(piece[prim]);
        CHECK_EQ(glue[prim], 12.0f);
    }
    CHECK(halves.size() > 150);
    for (const auto& [c, pieces] : halves) CHECK_EQ(pieces.size(), size_t(2));
    // Their faces of the break are the brick inside: warmer and lighter than its face.
    size_t warm = 0;
    const auto cc = cut->primitives().find("Cd")->read<Vec3>();
    for (size_t prim = 0; prim < cc.size(); ++prim) warm += cluster[prim] > 0 && cc[prim].x > 0.5f;
    CHECK(warm > 0);
}

TEST(brick_wall_stands_as_the_wall_stands_and_is_the_same_every_time) {
    // Turned: as many bricks, as much of them, the courses level.
    const std::vector<std::pair<Vec3, Vec3>> one = {{Vec3(2.08f, 1.2f, 0.25f), Vec3(0.0f, 0.6f, 0.0f)}};
    const GeometryPtr straight = bricks(one, [](pg::Node&) {});
    const GeometryPtr turned = bricks(one, [](pg::Node&) {}, Vec3(0.0f, 37.0f, 0.0f));
    CHECK_EQ(primsOfPieces(*turned).size(), primsOfPieces(*straight).size());
    double a = 0.0, b = 0.0;
    for (const auto& prims : primsOfPieces(*straight)) a += volumeOf(*straight, prims, Vec3(0.0f, 0.6f, 0.0f));
    for (const auto& prims : primsOfPieces(*turned)) b += volumeOf(*turned, prims, Vec3(0.0f, 0.6f, 0.0f));
    CHECK_NEAR(a, b, 1e-6);
    std::set<long> bottoms;
    for (const Box& box : boxesOf(*turned)) bottoms.insert(std::lround(box.lo.y * 10000.0f));
    CHECK_EQ(bottoms.size(), size_t(16));
    // The same on one thread and four, and every time; another seed, other shades.
    auto hashOf = [](int seed) { return plainWall([seed](pg::Node& n) { n.setInt("seed", seed); })->hash(); };
    const unsigned saved = TaskPool::instance().threadCount();
    TaskPool::instance().setThreadCount(1);
    const uint64_t first = hashOf(1);
    TaskPool::instance().setThreadCount(4);
    const uint64_t again = hashOf(1), other = hashOf(2);
    TaskPool::instance().setThreadCount(saved);
    CHECK_EQ(first, again);
    CHECK(other != first);
}

TEST(rigid_brick_wall_stands_on_its_mortar_and_breaks_along_its_joints) {
    CHECK(rigidAvailable());
    // A wall 1.3 m wide and 0.9 m high, a brick thick, glued to a plinth
    // that does not move.
    auto scene = [](float strength, bool ball) {
        GeometryPtr wall = bricks({{Vec3(1.3f, 0.9f, 0.25f), Vec3(0.0f, 0.65f, 0.0f)}}, [&](pg::Node& n) {
            n.setInt("bond", 2);  // English
            n.setFloat("broken", 0.5f);
            n.setFloat("strength", strength);
        });
        auto pieces = std::make_shared<Geometry>(*wall);
        {
            auto active = pieces->primitives().create("active", AttrType::Int).write<int32_t>();
            std::fill(active.begin(), active.end(), 1);
        }
        registerBuiltinNodes();
        Graph g;
        pg::Node* box = g.create("box", "plinth");
        box->setVec3("size", Vec3(1.6f, 0.2f, 0.6f));
        box->setVec3("center", Vec3(0.0f, 0.1f, 0.0f));
        CookEngine engine;
        Geometry plinth = *engine.cook(*box, CookContext{});
        auto p = plinth.primitives().create("piece", AttrType::Int).write<int32_t>();
        std::fill(p.begin(), p.end(), 100000);
        auto still = plinth.primitives().create("active", AttrType::Int).write<int32_t>();
        std::fill(still.begin(), still.end(), 0);
        pieces->append(plinth);
        RigidScene s;
        s.pieces = pieces;
        s.solver.density = 1800.0f;
        s.solver.glue = 150e3f;
        s.solver.rings = 3;
        s.solver.substeps = 4;
        if (ball) {
            Collider c;
            c.shape = Shape::Sphere;
            c.size = Vec3(0.5f);
            c.center = Vec3(0.0f, 0.7f, -0.45f);
            c.velocity = Vec3(0.0f, 0.0f, 8.0f);
            s.colliders.push_back(c);
        }
        return s;
    };
    // Left alone it stands as it was laid: not a joint broken, not a brick moved.
    {
        RigidSolver solver(scene(8.0f, false).sanitized());
        for (int i = 0; i < 30; ++i) solver.step();
        const RigidFrame f = solver.capture();
        CHECK_EQ(f.broken, size_t(0));
        for (const RigidPose& p : f.poses) CHECK(length(p.velocity) < 0.05f && length(p.position) < 0.01f);
    }
    // A ball through it: the mortar gives -- bricks come loose -- and a
    // brick comes apart in its two halves the less often, the harder they hold.
    auto knocked = [&](float strength, size_t& split) {
        RigidScene s = scene(strength, true);
        RigidSolver solver(s.sanitized());
        for (int i = 0; i < 15; ++i) {
            // The ball goes on through, as keyed.
            s.colliders[0].center += s.colliders[0].velocity * (1.0f / 30.0f);
            solver.setColliders(s.colliders);
            solver.step();
        }
        const RigidFrame f = solver.capture();
        // The halves of each brick cut in two: apart when posed apart.
        const auto cluster = s.pieces->primitives().find("cluster")->read<int32_t>();
        std::map<int32_t, std::vector<size_t>> halves;
        for (size_t body = 0; body < f.poses.size(); ++body) {
            const int32_t c = cluster[solver.layout()->prims[body].front()];
            if (c > 0) halves[c].push_back(body);
        }
        split = 0;
        for (const auto& [c, bodies] : halves) {
            CHECK_EQ(bodies.size(), size_t(2));
            const RigidPose& a = f.poses[bodies[0]];
            const RigidPose& b = f.poses[bodies[1]];
            split += length(a.position - b.position) > 1e-3f ||
                     length(Vec3(a.rotation.x - b.rotation.x, a.rotation.y - b.rotation.y, a.rotation.z - b.rotation.z)) > 1e-3f;
        }
        return f;
    };
    // As hard as the mortar, many a brick breaks; a thousand times as hard, hardly one.
    size_t splitWeak = 0, splitStrong = 0;
    const RigidFrame weak = knocked(1.0f, splitWeak);
    const RigidFrame strong = knocked(1000.0f, splitStrong);
    CHECK(weak.broken > 100 && strong.broken > 100);
    CHECK(weak.unglued.size() > 50 && strong.unglued.size() > 50);
    CHECK(splitWeak > 10);
    CHECK(4 * splitStrong < splitWeak);
}

TEST(brick_wall_in_a_network) {
    Network net;
    CHECK(Network::example("brick_wall", net));
    Compiled c = net.compile(PG_SIM_EXAMPLES_DIR);
    CHECK(c.ok);
    CHECK(c.world.hasRigid);
    const Geometry& pieces = *c.world.rigid.pieces;
    // Bricks, the window's glass and frame, the plinth.
    CHECK(pieces.primitives().find("glass") != nullptr);
    CHECK(pieces.primitives().find("cluster") != nullptr);
    const auto piece = pieces.primitives().find("piece")->read<int32_t>();
    std::set<int32_t> all(piece.begin(), piece.end());
    CHECK(all.size() > 1500);
    CHECK(all.count(100000) == 1 && all.count(300000) == 1);
    // The column: its cage of eight bars and the stirrups round them.
    Network column;
    CHECK(Network::example("concrete_column", column));
    Compiled cc = column.compile(PG_SIM_EXAMPLES_DIR);
    CHECK(cc.ok);
    CHECK(cc.world.hasRigid);
    CHECK(cc.world.rigid.rebar != nullptr);
    if (cc.world.rigid.rebar) CHECK_EQ(cc.world.rigid.rebar->primitiveCount(), size_t(8 + 21));
}

TEST(house_collapse_in_a_network) {
    // The family house of the example: walls of blocks in pieces of masonry
    // a few blocks each, glued in chunks; slabs, gables, the roof, the
    // chimney, frames, gutters and glass; the fence along the street; the
    // plinth and the fence's posts stay. The charges are in the walls of the
    // ground floor, and nowhere else.
    Network net;
    CHECK(Network::example("house_collapse", net));
    Compiled c = net.compile(PG_SIM_EXAMPLES_DIR);
    CHECK(c.ok);
    CHECK(c.world.hasRigid && c.world.hasGas);
    const Geometry& pieces = *c.world.rigid.pieces;
    const auto& prims = pieces.primitives();
    for (const char* name : {"piece", "kind", "floor", "active", "release", "vanish", "cluster", "glass"}) {
        if (!prims.find(name)) ::testing::fail(__FILE__, __LINE__, std::string("no attribute ") + name);
    }
    const auto piece = prims.find("piece")->read<int32_t>();
    const auto kind = prims.find("kind")->read<int32_t>();
    const auto floor = prims.find("floor")->read<int32_t>();
    const auto active = prims.find("active")->read<int32_t>();
    const auto release = prims.find("release")->read<float>();
    const auto cluster = prims.find("cluster")->read<int32_t>();
    // Each piece as its first primitive has it, as the solver takes it.
    std::map<int32_t, size_t> first;
    for (size_t i = 0; i < piece.size(); ++i) first.emplace(piece[i], i);
    std::map<int32_t, int> perKind;
    std::set<int32_t> chunks;
    int still = 0, charged = 0;
    for (const auto& [p, i] : first) {
        ++perKind[kind[i]];
        if (kind[i] == 1 || kind[i] == 2) chunks.insert(cluster[i]);
        if (!active[i]) ++still;
        if (release[i] > 0.0f) {
            ++charged;
            CHECK((kind[i] == 1 || kind[i] == 2) && floor[i] == 0);
        }
    }
    CHECK(first.size() > 2000);
    CHECK_EQ(perKind[0], 1);                            // the plinth
    CHECK(perKind[1] + perKind[2] == 900);              // masonry: a few blocks a piece
    CHECK(chunks.size() > 100 && chunks.size() <= 170);  // in chunks
    CHECK(perKind[3] > 50 && perKind[4] > 20 && perKind[5] > 40 && perKind[6] > 3);
    CHECK(perKind[8] > 500);                            // the shards of the panes
    CHECK(perKind[9] > 200);                            // the fence
    CHECK(charged > 200 && charged < 500);
    CHECK(still > 10);                                  // the plinth and the fence's posts
}
