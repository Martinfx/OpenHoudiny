//
// The glue as geometry (src/pg/sim/Rigid.h, rigidGlue and rigidNetwork):
// RBD Constraints makes a point a body and a line a joint, as strong as the
// pieces' attributes say; linked into the RBD Solver as it is, it glues as
// the pieces do; weakened, deleted or drawn where nothing touches, it
// breaks and holds where it says. A frame gives it back where the bodies
// are, the joints that broke marked, through the cache too; the nodes in a
// network, and the example that cracks a wall along a line.
//
#include "pg/core/CookEngine.h"
#include "pg/core/Graph.h"
#include "pg/nodes/Nodes.h"
#include "pg/sim/Cache.h"
#include "pg/sim/GeometryGraph.h"
#include "pg/sim/Network.h"
#include "pg/sim/Rigid.h"
#include "pg/sim/World.h"

#include "test_framework.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <set>

using namespace pg;
using namespace pg::sim;

namespace {

/// A box of `size` at `center`, cut into `count` Voronoi pieces.
std::shared_ptr<Geometry> fracturedBox(Vec3 center, Vec3 size, int count, int seed = 1) {
    registerBuiltinNodes();
    Graph g;
    pg::Node* box = g.create("box", "box");
    box->setInt("divisions", 1);
    box->setVec3("size", size);
    box->setVec3("center", center);
    pg::Node* fracture = g.create("voronoifracture", "fracture");
    fracture->setInt("count", count);
    fracture->setInt("seed", seed);
    fracture->setInput(0, box);
    CookEngine engine;
    return std::make_shared<Geometry>(*engine.cook(*fracture, CookContext{}));
}

/// A box that is a piece of its own: number `piece`, still (active 0) or not.
Geometry boxPiece(Vec3 center, Vec3 size, int32_t piece, bool moves) {
    registerBuiltinNodes();
    Graph g;
    pg::Node* box = g.create("box", "box");
    box->setInt("divisions", 1);
    box->setVec3("size", size);
    box->setVec3("center", center);
    CookEngine engine;
    Geometry geo = *engine.cook(*box, CookContext{});
    auto p = geo.primitives().create("piece", AttrType::Int).write<int32_t>();
    std::fill(p.begin(), p.end(), piece);
    auto a = geo.primitives().create("active", AttrType::Int).write<int32_t>();
    std::fill(a.begin(), a.end(), moves ? 1 : 0);
    return geo;
}

/// The network of `pieces`, as RBD Constraints makes it.
std::shared_ptr<Geometry> networkOf(const Geometry& pieces) {
    const auto layout = rigidLayout(pieces, "piece");
    return rigidNetwork(*rigidGlue(pieces, *layout, "piece"), "piece");
}

/// A wall of Voronoi pieces on a plinth that does not move, and a ball
/// keyed through it: `network` its glue, when given.
RigidScene wallAndBall(std::shared_ptr<const Geometry> network = nullptr) {
    auto pieces = fracturedBox(Vec3(0.0f, 0.8f, 0.0f), Vec3(2.4f, 1.2f, 0.2f), 40, 3);
    {
        auto active = pieces->primitives().create("active", AttrType::Int).write<int32_t>();
        std::fill(active.begin(), active.end(), 1);
    }
    pieces->append(boxPiece(Vec3(0.0f, 0.1f, 0.0f), Vec3(2.8f, 0.2f, 0.5f), 1000, false));
    RigidScene s;
    s.pieces = pieces;
    s.constraints = std::move(network);
    s.solver.glue = 400e3f;
    s.solver.spread = 0.3f;
    s.solver.substeps = 4;
    Collider ball;
    ball.shape = Shape::Sphere;
    ball.size = Vec3(0.5f);
    ball.center = Vec3(-0.5f, 1.0f, -0.6f);
    ball.velocity = Vec3(0.0f, 0.0f, 6.0f);
    s.colliders.push_back(ball);
    return s;
}

/// `s` stepped `frames` times, its ball going on as keyed.
RigidFrame run(RigidScene s, int frames) {
    RigidSolver solver(s.sanitized());
    for (int i = 0; i < frames; ++i) {
        if (!s.colliders.empty()) {
            s.colliders[0].center += s.colliders[0].velocity * (1.0f / 30.0f);
            solver.setColliders(s.colliders);
        }
        solver.step();
    }
    return solver.capture();
}

/// A beam of pieces lying across the edge of a table -- most of it on the
/// table, so that it does not tip -- `network` its glue, when given.
RigidScene beamOnTable(std::shared_ptr<const Geometry> network = nullptr) {
    RigidScene s;
    s.pieces = fracturedBox(Vec3(0.0f, 1.05f, 0.0f), Vec3(2.0f, 0.1f, 0.2f), 10, 3);
    s.constraints = std::move(network);
    s.solver.glue = 1e6f;
    s.solver.floor = false;
    Collider table;
    table.shape = Shape::Box;
    table.center = Vec3(-0.7f, 0.5f, 0.0f);
    table.size = Vec3(1.8f, 1.0f, 1.0f);  // under x < 0.2
    table.node = 7;
    s.colliders.push_back(table);
    return s;
}

/// The lowest y of the pieces of `f` whose points rest right of `x` (or left of it).
float lowestBeyond(const RigidFrame& f, float x, bool right) {
    const GeometryPtr posed = posedPieces(f);
    const auto P = f.pieces->positions(), Q = posed->positions();
    float lowest = 1e30f;
    for (size_t i = 0; i < P.size(); ++i) {
        if ((P[i].x > x) == right) lowest = std::min(lowest, Q[i].y);
    }
    return lowest;
}

}  // namespace

TEST(rbd_constraints_make_a_point_a_body_and_a_line_a_joint) {
    // Twelve pieces, numbered 100, 107, 114...; the first held half as
    // hard (glue 0.5); the next two one chunk held ten times as hard.
    auto pieces = fracturedBox(Vec3(0.0f, 1.0f, 0.0f), Vec3(2.0f, 1.0f, 0.3f), 12);
    {
        auto piece = pieces->primitives().find("piece")->write<int32_t>();
        auto glue = pieces->primitives().create("glue", AttrType::Float).write<float>();
        auto cluster = pieces->primitives().create("cluster", AttrType::Int).write<int32_t>();
        auto clusterGlue = pieces->primitives().create("clusterglue", AttrType::Float).write<float>();
        for (size_t prim = 0; prim < piece.size(); ++prim) {
            const int32_t k = piece[prim];
            glue[prim] = k == 0 ? 0.5f : 1.0f;
            cluster[prim] = k == 1 || k == 2 ? 1 : 0;
            clusterGlue[prim] = 10.0f;
            piece[prim] = 100 + 7 * k;
        }
    }
    const auto layout = rigidLayout(*pieces, "piece");
    const auto glue = rigidGlue(*pieces, *layout, "piece");
    const GeometryPtr net = rigidNetwork(*glue, "piece");
    CHECK_EQ(static_cast<int>(net->pointCount()), layout->bodies);
    CHECK_EQ(net->primitiveCount(), layout->contacts.size());
    CHECK(layout->contacts.size() > 15);
    CHECK(net->points().find("part") == nullptr);  // a body a piece
    // A point at the middle of each body, with the piece it is.
    const auto P = net->positions();
    const auto named = net->points().find("piece")->read<int32_t>();
    std::map<int32_t, int> bodyOfPiece;
    for (int b = 0; b < layout->bodies; ++b) {
        const uint32_t prim = layout->prims[static_cast<size_t>(b)].front();
        CHECK_EQ(named[static_cast<size_t>(b)], pieces->primitives().find("piece")->read<int32_t>()[prim]);
        bodyOfPiece[named[static_cast<size_t>(b)]] = b;
        Vec3 lo(1e30f, 1e30f, 1e30f), hi(-1e30f, -1e30f, -1e30f);
        for (const uint32_t p : layout->prims[static_cast<size_t>(b)]) {
            for (const uint32_t i : pieces->primitivePoints(p)) {
                for (int a = 0; a < 3; ++a) {
                    lo[a] = std::min(lo[a], pieces->positions()[i][a]);
                    hi[a] = std::max(hi[a], pieces->positions()[i][a]);
                }
            }
        }
        CHECK(length(P[static_cast<size_t>(b)] - (lo + hi) * 0.5f) < 1e-5f);
    }
    // A line for each place two touch, as strong as their attributes say --
    // the weaker glue, the chunk ten times as strong -- and as big.
    const auto strength = net->primitives().find("strength")->read<float>();
    const auto area = net->primitives().find("area")->read<float>();
    const auto cd = net->primitives().find("Cd")->read<Vec3>();
    for (size_t i = 0; i < layout->contacts.size(); ++i) {
        const RigidLayout::Contact& c = layout->contacts[i];
        CHECK(!net->primitiveClosed(i));
        const auto ends = net->primitivePoints(i);
        CHECK_EQ(ends.size(), size_t(2));
        CHECK_EQ(static_cast<int>(ends[0]), c.a);
        CHECK_EQ(static_cast<int>(ends[1]), c.b);
        CHECK_EQ(area[i], c.area);
        const bool first = named[static_cast<size_t>(c.a)] == 100 || named[static_cast<size_t>(c.b)] == 100;
        const bool chunk = std::set<int32_t>{named[static_cast<size_t>(c.a)], named[static_cast<size_t>(c.b)]} ==
                           std::set<int32_t>{107, 114};
        const float want = chunk ? 10.0f : first ? 0.5f : 1.0f;
        CHECK_EQ(strength[i], want);
        // Green as the Glue holds, yellow weaker, blue stronger.
        if (want == 1.0f) CHECK(cd[i] == Vec3(0.3f, 0.85f, 0.35f));
        if (want < 1.0f) CHECK(cd[i].x > 0.3f && cd[i].z < 0.35f);
        if (want > 1.0f) CHECK(cd[i].z > 0.5f && cd[i].x < 0.3f);
    }
    // The same every time; the node makes it of what it is linked to.
    CHECK_EQ(networkOf(*pieces)->hash(), net->hash());
    registerBuiltinNodes();
    registerSimGeometryNodes();
    Graph g;
    pg::Node* box = g.create("box", "box");
    box->setVec3("size", Vec3(2.0f, 1.0f, 0.3f));
    pg::Node* fracture = g.create("voronoifracture", "fracture");
    fracture->setInt("count", 12);
    fracture->setInput(0, box);
    pg::Node* node = g.create("rbd_constraints", "glue");
    CHECK(node != nullptr);
    if (node) {
        node->setInput(0, fracture);
        CookEngine engine;
        CHECK_EQ(engine.cook(*node, CookContext{})->hash(), networkOf(*engine.cook(*fracture, CookContext{}))->hash());
    }
    // A piece of two bodies -- parts that do not touch -- is two points,
    // part 0 and 1.
    Geometry apart = boxPiece(Vec3(0.0f, 0.5f, 0.0f), Vec3(0.2f), 5, true);
    apart.append(boxPiece(Vec3(1.0f, 0.5f, 0.0f), Vec3(0.2f), 5, true));
    const GeometryPtr two = networkOf(apart);
    CHECK_EQ(two->pointCount(), size_t(2));
    CHECK(two->points().find("part") != nullptr);
    if (const AttributeArray* part = two->points().find("part")) {
        CHECK_EQ(part->read<int32_t>()[0], 0);
        CHECK_EQ(part->read<int32_t>()[1], 1);
    }
    CHECK_EQ(two->primitiveCount(), size_t(0));
}

TEST(rbd_constraints_linked_in_as_they_are_glue_as_the_pieces_do) {
    CHECK(rigidAvailable());
    const RigidScene plain = wallAndBall();
    const RigidFrame a = run(plain, 20);
    const RigidFrame b = run(wallAndBall(networkOf(*plain.pieces)), 20);
    CHECK(a.broken > 5);
    CHECK(a.poses == b.poses);
    CHECK_EQ(a.joints, b.joints);
    CHECK_EQ(a.broken, b.broken);
    CHECK(a.jointState == b.jointState);
    CHECK(a.jointTime == b.jointTime);
    // What became of each joint: those of the plinth to itself -- none --
    // never held; so many broke, each when it did.
    size_t broke = 0;
    for (size_t i = 0; i < a.jointState.size(); ++i) {
        if (a.jointState[i] == RigidFrame::kJointBroken) {
            ++broke;
            CHECK(a.jointTime[i] > 0.0f && a.jointTime[i] <= 20.0f / 30.0f + 1e-4f);
        }
    }
    CHECK_EQ(broke, a.broken);
    CHECK_EQ(a.jointState.size(), a.glue->joints.size());
}

TEST(rbd_constraints_weakened_or_deleted_break_there) {
    CHECK(rigidAvailable());
    // As built, the beam stands, the part over the edge too.
    const RigidScene plain = beamOnTable();
    CHECK(lowestBeyond(run(plain, 60), 0.2f, true) > 0.9f);
    // The lines across the table's edge deleted: the part over it tips off,
    // whole -- glued as it was -- and the rest stays on the table.
    const GeometryPtr full = networkOf(*plain.pieces);
    const auto P = full->positions();
    auto across = [&](size_t prim) {
        const auto ends = full->primitivePoints(prim);
        return (P[ends[0]].x - 0.2f) * (P[ends[1]].x - 0.2f) < 0.0f;
    };
    auto cut = std::make_shared<Geometry>(*full);
    std::vector<uint8_t> keep(full->primitiveCount(), 1);
    size_t crossing = 0;
    for (size_t prim = 0; prim < keep.size(); ++prim) {
        if (across(prim)) {
            keep[prim] = 0;
            ++crossing;
        }
    }
    CHECK(crossing > 0);
    cut->deletePrimitives(keep, false);
    const RigidFrame fell = run(beamOnTable(cut), 60);
    CHECK(lowestBeyond(fell, 0.4f, true) < 0.0f);
    CHECK(lowestBeyond(fell, 0.0f, false) > 0.99f);
    CHECK_EQ(fell.broken, size_t(0));  // nothing broke: those joints were not there
    std::set<std::tuple<float, float, float>> overhang;
    const auto centres = rigidGlue(*plain.pieces, *rigidLayout(*plain.pieces, "piece"), "piece")->centres;
    for (size_t b = 0; b < fell.poses.size(); ++b) {
        if (centres[b].x > 0.2f) {
            const Vec3& p = fell.poses[b].position;
            overhang.insert({std::round(p.x * 1e3f), std::round(p.y * 1e3f), std::round(p.z * 1e3f)});
        }
    }
    CHECK_EQ(overhang.size(), size_t(1));
    // Weakened to nothing rather than deleted: the same, those joints never held.
    auto weak = std::make_shared<Geometry>(*full);
    {
        auto strength = weak->primitives().find("strength")->write<float>();
        for (size_t prim = 0; prim < strength.size(); ++prim) {
            if (across(prim)) strength[prim] = 0.0f;
        }
    }
    const RigidFrame weakened = run(beamOnTable(weak), 60);
    CHECK(weakened.poses == fell.poses);
    for (size_t i = 0; i < weakened.jointState.size(); ++i) {
        CHECK_EQ(weakened.jointState[i], across(i) ? RigidFrame::kJointNone : RigidFrame::kJointHolds);
    }
}

TEST(rbd_constraints_drawn_between_pieces_that_do_not_touch_hold_them) {
    CHECK(rigidAvailable());
    // A hook that does not move, and a weight hanging five centimetres under it.
    auto pieces = std::make_shared<Geometry>(boxPiece(Vec3(0.0f, 2.0f, 0.0f), Vec3(0.3f), 1, false));
    pieces->append(boxPiece(Vec3(0.0f, 1.6f, 0.0f), Vec3(0.3f, 0.25f, 0.3f), 2, true));
    RigidScene s;
    s.pieces = pieces;
    s.solver.floor = false;
    // Nothing touches: no glue, it falls.
    const RigidFrame loose = run(s, 20);
    CHECK_EQ(loose.joints, size_t(0));
    CHECK(loose.poses[1].position.y < -0.5f);
    // A line drawn from the hook to the weight: it hangs.
    auto drawn = [](bool named, float strength) {
        auto net = std::make_shared<Geometry>();
        net->addPoints(2);
        auto P = net->positionsForWrite();
        P[0] = Vec3(0.0f, 2.0f, 0.0f);
        P[1] = Vec3(0.0f, 1.6f, 0.0f);
        if (named) {
            auto piece = net->points().create("piece", AttrType::Int).write<int32_t>();
            piece[0] = 1;
            piece[1] = 2;
        }
        const uint32_t ends[2] = {1, 0};
        net->addPrimitive(ends, false);
        auto st = net->primitives().create("strength", AttrType::Float).write<float>();
        st[0] = strength;
        return net;
    };
    for (const bool named : {true, false}) {
        s.constraints = drawn(named, 1.0f);
        const RigidFrame held = run(s, 20);
        CHECK_EQ(held.joints, size_t(1));
        CHECK(length(held.poses[1].position) < 1e-3f);
        // As big as a hand's breadth square, halfway between them.
        CHECK(held.glue != nullptr);
        if (held.glue && held.glue->joints.size() == 1) {
            CHECK_EQ(held.glue->joints[0].area, 0.01f);
            CHECK(length(held.glue->joints[0].at - Vec3(0.0f, 1.8f, 0.0f)) < 1e-5f);
            // Across it: from the hook's middle to the weight's.
            CHECK(length(held.glue->joints[0].normal - Vec3(0.0f, -1.0f, 0.0f)) < 1e-5f);
        }
    }
    // Of no strength, or naming a piece there is not: nothing holds.
    s.constraints = drawn(true, 0.0f);
    CHECK(run(s, 20).poses[1].position.y < -0.5f);
    auto stray = drawn(true, 1.0f);
    stray->points().find("piece")->write<int32_t>()[0] = 77;
    s.constraints = stray;
    RigidSolver solver(s.sanitized());
    CHECK_EQ(solver.glue()->skipped, size_t(1));
    CHECK(solver.glue()->joints.empty());
}

TEST(rbd_pieces_give_the_glue_back_where_the_pieces_are) {
    CHECK(rigidAvailable());
    const RigidFrame f = run(wallAndBall(), 20);
    const GeometryPtr net = rigidNetwork(f);
    CHECK(f.glue != nullptr);
    CHECK_EQ(net->pointCount(), f.poses.size());
    // The points where the bodies' middles have gone, moving as they do.
    const auto P = net->positions();
    const auto v = net->points().find("v")->read<Vec3>();
    for (size_t b = 0; b < f.poses.size(); ++b) {
        CHECK(length(P[b] - f.poses[b].apply(f.glue->centres[b])) < 1e-5f);
        CHECK(length(v[b] - f.poses[b].velocityAt(f.glue->centres[b])) < 1e-4f);
    }
    // A line for each joint that held: broken where it broke, when, red.
    size_t held = 0;
    for (const uint8_t s : f.jointState) held += s != RigidFrame::kJointNone;
    CHECK_EQ(net->primitiveCount(), held);
    CHECK_EQ(held, f.joints);
    const auto broken = net->primitives().find("broken")->read<int32_t>();
    const auto time = net->primitives().find("time")->read<float>();
    const auto cd = net->primitives().find("Cd")->read<Vec3>();
    size_t count = 0;
    for (size_t i = 0; i < net->primitiveCount(); ++i) {
        if (broken[i]) {
            ++count;
            CHECK(time[i] > 0.0f);
            CHECK(cd[i] == Vec3(1.0f, 0.15f, 0.1f));
        } else {
            CHECK_EQ(time[i], -1.0f);
        }
    }
    CHECK_EQ(count, f.broken);
    // A frame that knows no glue gives none.
    RigidFrame none = f;
    none.glue = nullptr;
    CHECK_EQ(rigidNetwork(none)->primitiveCount(), size_t(0));
}

TEST(rbd_constraints_go_through_the_cache) {
    CHECK(rigidAvailable());
    const RigidScene scene = wallAndBall();
    auto network = networkOf(*scene.pieces);
    {
        auto strength = network->primitives().find("strength")->write<float>();
        for (size_t i = 0; i < strength.size(); i += 3) strength[i] *= 0.5f;
    }
    RigidScene weakened = wallAndBall(network);
    weakened.pieces = scene.pieces;
    Frame frame;
    frame.number = 20;
    frame.rigid = run(weakened, 20);
    Frame back;
    std::string error;
    CHECK(parseFrame(formatFrame(frame), back, error));
    CHECK(back.rigid.jointState == frame.rigid.jointState);
    CHECK(back.rigid.jointTime == frame.rigid.jointTime);
    CHECK(back.rigid.glue == nullptr);
    std::shared_ptr<const RigidLayout> layout;
    std::shared_ptr<const RigidRebar> bars;
    std::shared_ptr<const RigidGlue> glue;
    adoptPieces(back, weakened, &layout, &bars, &glue);
    CHECK(back.rigid.glue != nullptr);
    CHECK(glue == back.rigid.glue);
    CHECK_EQ(rigidNetwork(back.rigid)->hash(), rigidNetwork(frame.rigid)->hash());
    // The world's network of other lines -- fewer joints than the frame
    // says: no glue.
    RigidScene other = weakened;
    auto fewer = std::make_shared<Geometry>(*network);
    std::vector<uint8_t> keep(fewer->primitiveCount(), 1);
    keep[0] = 0;
    fewer->deletePrimitives(keep, false);
    other.constraints = fewer;
    Frame elsewhere;
    CHECK(parseFrame(formatFrame(frame), elsewhere, error));
    adoptPieces(elsewhere, other);
    CHECK(elsewhere.rigid.glue == nullptr);
    CHECK_EQ(rigidNetwork(elsewhere.rigid)->primitiveCount(), size_t(0));
}

TEST(rbd_constraints_in_a_network) {
    Network net;
    const int box = net.add("box");
    const int fracture = net.add("voronoi_fracture");
    const int glue = net.add("rbd_constraints");
    const int weaken = net.add("primitive_wrangle");
    const int rbd = net.add("rbd_solver");
    const int output = net.add("output");
    CHECK(net.setParam(box, "size", "2 1 0.3"));
    CHECK(net.setParam(box, "center", "0 0.5 0"));
    CHECK(net.setParam(fracture, "count", "12"));
    CHECK(net.setParam(weaken, "snippet", "f@strength *= 0.5;"));
    CHECK(net.connect(box, "geometry", fracture, "geometry"));
    CHECK(net.connect(fracture, "geometry", glue, "geometry"));
    CHECK(net.connect(glue, "geometry", weaken, "geometry"));
    CHECK(net.connect(fracture, "geometry", rbd, "pieces"));
    CHECK(net.connect(weaken, "geometry", rbd, "constraints"));
    CHECK(net.connect(rbd, "look", output, "look"));
    Compiled c = net.compile();
    CHECK(c.ok);
    for (const Problem& p : c.problems) CHECK(p.level != Problem::Level::Error && p.message.find("Constraints") == std::string::npos);
    CHECK(c.world.rigid.constraints != nullptr);
    if (c.world.rigid.constraints) {
        const auto s = c.world.rigid.constraints->primitives().find("strength")->read<float>();
        CHECK(!s.empty());
        for (const float x : s) CHECK_EQ(x, 0.5f);
    }
    // Simulated, RBD Pieces gives it back at the frame.
    if (rigidAvailable()) {
        const int back = net.add("rbd_pieces");
        CHECK(net.setParam(back, "output", "constraints"));
        CHECK(net.connect(rbd, "rigid", back, "rigid"));
        const Compiled simulated = net.compile();
        WorldSolver solver(simulated.world);
        for (int i = 0; i < 3; ++i) solver.step();
        const auto frame = std::make_shared<const Frame>(solver.capture());
        GeometryGraph graph;
        graph.sync(net);
        graph.setFrames([&](int) { return frame; });
        const GeometryPtr joints = graph.cook(back, 3, simulated.world.timeStep);
        CHECK_EQ(joints->hash(), rigidNetwork(frame->rigid)->hash());
        CHECK(joints->primitiveCount() > 10);
    }
    // A network of other pieces: its lines that join none of these are said.
    const int renumber = net.add("point_wrangle");
    CHECK(net.setParam(renumber, "snippet", "if (@ptnum < 3) i@piece += 1000;"));
    CHECK(net.connect(glue, "geometry", renumber, "geometry"));
    CHECK(net.connect(renumber, "geometry", rbd, "constraints"));
    const Compiled other = net.compile();
    bool said = false;
    for (const Problem& p : other.problems) said = said || p.message.find("not in Pieces") != std::string::npos;
    CHECK(said);
    // The example: a crack along a line up the wall -- the joints across it
    // a fiftieth as strong.
    Network example;
    CHECK(Network::example("constraint_network", example));
    Compiled e = example.compile(PG_SIM_EXAMPLES_DIR);
    CHECK(e.ok);
    CHECK(e.world.rigid.constraints != nullptr);
    if (e.world.rigid.constraints) {
        const auto s = e.world.rigid.constraints->primitives().find("strength")->read<float>();
        const size_t weak = static_cast<size_t>(std::count(s.begin(), s.end(), 0.02f));
        CHECK(s.size() > 300);
        CHECK(weak > 10 && weak < s.size() / 5);
    }
}
