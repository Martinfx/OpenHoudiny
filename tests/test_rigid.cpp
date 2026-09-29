//
// Rigid bodies (src/pg/sim/Rigid.h): the pieces of a fractured box fall
// and come to rest on the floor; glue holds them together and breaks under
// a pull -- a wrecking ball -- puffing dust; the same frames on any number
// of threads and from one solver to the next; the pieces posed, drawn and
// brought back as geometry; the RBD Solver in a network, its pieces into
// the water and the gas, and its frames through the cache.
//
#include "pg/core/CookEngine.h"
#include "pg/core/Graph.h"
#include "pg/core/Parallel.h"
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
GeometryPtr fracturedBox(Vec3 center, Vec3 size, int count, int seed = 1) {
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
    return engine.cook(*fracture, CookContext{});
}

int pieceCount(const Geometry& geo) {
    int count = 0;
    pieceOfPrimitives(geo, "piece", count);
    return count;
}

/// The lowest and highest point of the posed pieces.
void bounds(const Geometry& geo, Vec3& lo, Vec3& hi) {
    lo = Vec3(1e30f, 1e30f, 1e30f);
    hi = Vec3(-1e30f, -1e30f, -1e30f);
    for (const Vec3& p : geo.positions()) {
        for (int a = 0; a < 3; ++a) {
            lo[a] = std::min(lo[a], p[a]);
            hi[a] = std::max(hi[a], p[a]);
        }
    }
}

float fastest(const RigidFrame& f) {
    float v = 0.0f;
    for (const RigidPose& p : f.poses) v = std::max(v, length(p.velocity));
    return v;
}

bool samePoses(const RigidFrame& a, const RigidFrame& b) { return a.poses == b.poses && a.broken == b.broken; }

struct ThreadCountGuard {
    unsigned saved = TaskPool::instance().threadCount();
    ~ThreadCountGuard() { TaskPool::instance().setThreadCount(saved); }
};

}  // namespace

TEST(rigid_pieces_fall_and_come_to_rest_on_the_floor) {
    CHECK(rigidAvailable());
    RigidScene scene;
    scene.pieces = fracturedBox(Vec3(0.0f, 2.0f, 0.0f), Vec3(1.0f, 1.0f, 1.0f), 8);
    scene.solver.glue = 0.0f;  // loose pieces
    RigidSolver solver(scene);
    CHECK(solver.error().empty());
    CHECK_EQ(solver.pieceCount(), static_cast<size_t>(pieceCount(*scene.pieces)));
    // At the start: where they were made.
    RigidFrame first = solver.capture();
    CHECK_EQ(first.poses.size(), solver.pieceCount());
    for (const RigidPose& p : first.poses) {
        CHECK(p.position == Vec3());
        CHECK(p.rotation.w == 1.0f);
    }
    CHECK_EQ(first.joints, 0u);
    // A second in: falling.
    for (int i = 0; i < 15; ++i) solver.step();
    RigidFrame falling = solver.capture();
    Vec3 lo, hi;
    bounds(*posedPieces(falling), lo, hi);
    CHECK(hi.y < 2.5f);
    CHECK(fastest(falling) > 1.0f);
    // Four seconds in: on the floor, still.
    for (int i = 0; i < 105; ++i) solver.step();
    RigidFrame rest = solver.capture();
    bounds(*posedPieces(rest), lo, hi);
    CHECK(lo.y > -0.05f);
    CHECK(lo.y < 0.05f);
    CHECK(hi.y < 1.2f);  // spread out: lower than the box was tall
    CHECK(fastest(rest) < 0.05f);
    // Every piece is where it says: the rest points moved and turned keep
    // their distances.
    const GeometryPtr posed = posedPieces(rest);
    const auto P = scene.pieces->positions();
    const auto Q = posed->positions();
    const auto piece = scene.pieces->points().find("piece")->read<int32_t>();
    for (size_t i = 1; i < P.size(); ++i) {
        if (piece[i] != piece[0]) continue;
        CHECK(std::fabs(length(P[i] - P[0]) - length(Q[i] - Q[0])) < 1e-3f);
    }
    // The velocity of each point is there: v, near zero at rest.
    const AttributeArray* v = posed->points().find("v");
    CHECK(v != nullptr && v->type() == AttrType::Vec3);
}

TEST(rigid_glue_holds_as_built_and_breaks_under_what_comes_on_top) {
    // A beam that hangs over the edge of a table -- most of it on the table,
    // so that it does not tip -- and, above its end, a small weight that
    // falls onto it.
    auto beam = [](float glue, bool weight) {
        RigidScene scene;
        auto geo = std::make_shared<Geometry>(*fracturedBox(Vec3(0.0f, 1.05f, 0.0f), Vec3(2.0f, 0.1f, 0.2f), 10, 3));
        if (weight) {
            registerBuiltinNodes();
            Graph g;
            pg::Node* box = g.create("box", "weight");
            box->setInt("divisions", 1);
            box->setVec3("size", Vec3(0.2f, 0.2f, 0.2f));
            box->setVec3("center", Vec3(0.8f, 2.4f, 0.0f));
            CookEngine engine;
            Geometry w = *engine.cook(*box, CookContext{});
            auto a = w.primitives().create("piece", AttrType::Int).write<int32_t>();
            std::fill(a.begin(), a.end(), 100);
            geo->append(w);
        }
        scene.pieces = geo;
        scene.solver.glue = glue;
        scene.solver.floor = false;
        Collider table;
        table.shape = Shape::Box;
        table.center = Vec3(-0.7f, 0.5f, 0.0f);
        table.size = Vec3(1.8f, 1.0f, 1.0f);  // under x < 0.2
        table.node = 7;
        scene.colliders.push_back(table);
        return scene;
    };
    auto lowest = [](const RigidSolver& s) {
        Vec3 lo, hi;
        RigidFrame f = s.capture();
        // The beam's pieces only: the weight is the last body.
        f.poses.back() = RigidPose();
        bounds(*posedPieces(f), lo, hi);
        return lo.y;
    };
    // Weak glue: as built, it stands -- the overhang too.
    RigidSolver weak(beam(50.0f, false));
    CHECK(weak.capture().joints > 0u);
    for (int i = 0; i < 60; ++i) weak.step();
    CHECK_EQ(weak.capture().broken, 0u);
    CHECK(lowest(weak) > 0.9f);
    // ... until the weight lands on it: torn where it broke, dust puffs.
    RigidSolver hit(beam(50.0f, true));
    bool puffed = false;
    for (int i = 0; i < 60; ++i) {
        hit.step();
        puffed = puffed || !hit.dust().empty();
    }
    CHECK(hit.capture().broken > 0u);
    CHECK(puffed);
    CHECK(lowest(hit) < 0.0f);
    // Strong glue: the weight bounces off, the beam holds -- in one piece.
    RigidSolver strong(beam(1e9f, true));
    for (int i = 0; i < 60; ++i) strong.step();
    CHECK_EQ(strong.capture().broken, 0u);
    // No glue: the part over the edge falls.
    RigidSolver loose(beam(0.0f, false));
    for (int i = 0; i < 60; ++i) loose.step();
    CHECK_EQ(loose.capture().joints, 0u);
    CHECK(lowest(loose) < -1.0f);
}

TEST(rigid_a_keyed_object_knocks_the_pieces_over) {
    // A glued wall, and a ball that swings through it: the glue breaks and
    // the wall goes.
    RigidScene scene;
    scene.pieces = fracturedBox(Vec3(0.0f, 0.5f, 0.0f), Vec3(1.0f, 1.0f, 0.2f), 12, 5);
    scene.solver.glue = 20000.0f;
    Collider ball;
    ball.shape = Shape::Sphere;
    ball.center = Vec3(0.0f, 0.5f, 3.0f);
    ball.size = Vec3(0.6f, 0.6f, 0.6f);
    ball.node = 3;
    scene.colliders.push_back(ball);
    RigidSolver solver(scene);
    CHECK(solver.capture().joints > 0u);
    // Still: nothing moves.
    for (int i = 0; i < 20; ++i) solver.step();
    RigidFrame still = solver.capture();
    CHECK_EQ(still.broken, 0u);
    CHECK(fastest(still) < 0.05f);
    // The ball comes through at 8 m/s.
    for (int i = 0; i < 40; ++i) {
        ball.center.z -= 8.0f / 30.0f;
        ball.velocity = Vec3(0.0f, 0.0f, -8.0f);
        solver.setColliders({ball});
        solver.step();
    }
    RigidFrame hit = solver.capture();
    CHECK(hit.broken > 0u);
    Vec3 lo, hi;
    bounds(*posedPieces(hit), lo, hi);
    CHECK(lo.z < -0.5f);  // pieces thrown the way the ball went
    // The pieces as colliders of the water and the gas: a mesh each, moving.
    RigidScene wet = scene;
    wet.intoWater = true;
    RigidSolver meshed(wet);
    meshed.step();
    const std::vector<Collider> colliders = meshed.colliders();
    CHECK_EQ(colliders.size(), meshed.pieceCount());
    for (const Collider& c : colliders) {
        CHECK(c.shape == Shape::Mesh);
        CHECK(c.mesh != nullptr);
        CHECK_EQ(c.node, scene.node);
    }
    CHECK(solver.colliders().empty());  // not asked for: none made
}

TEST(rigid_frames_are_the_same_on_any_thread_count_and_from_solver_to_solver) {
    ThreadCountGuard guard;
    RigidScene scene;
    scene.pieces = fracturedBox(Vec3(0.3f, 1.5f, -0.2f), Vec3(1.0f, 0.8f, 0.6f), 9, 2);
    scene.solver.glue = 3000.0f;
    auto frames = [&](unsigned threads) {
        TaskPool::instance().setThreadCount(threads);
        RigidSolver solver(scene);
        std::vector<RigidFrame> out;
        for (int i = 0; i < 40; ++i) {
            solver.step();
            out.push_back(solver.capture());
        }
        return out;
    };
    const std::vector<RigidFrame> one = frames(1), four = frames(4), again = frames(4);
    CHECK_EQ(one.size(), four.size());
    for (size_t k = 0; k < one.size(); ++k) {
        CHECK(samePoses(one[k], four[k]));
        CHECK(samePoses(one[k], again[k]));
    }
    // Something happened along the way.
    CHECK(!samePoses(one.front(), one.back()));
}

TEST(rigid_pieces_are_posed_and_drawn_in_their_colours) {
    // Two triangles, two pieces; the second is the cut face.
    auto geo = std::make_shared<Geometry>();
    geo->addPoints(6);
    auto P = geo->positionsForWrite();
    P[0] = Vec3(0, 0, 0), P[1] = Vec3(1, 0, 0), P[2] = Vec3(0, 1, 0);
    P[3] = Vec3(0, 0, 1), P[4] = Vec3(1, 0, 1), P[5] = Vec3(0, 1, 1);
    const uint32_t first[3] = {0, 1, 2}, second[3] = {3, 4, 5};
    geo->addPrimitive(first, true);
    geo->addPrimitive(second, true);
    auto piece = geo->primitives().create("piece", AttrType::Int).write<int32_t>();
    piece[0] = 0;
    piece[1] = 1;
    auto cd = geo->primitives().create("Cd", AttrType::Vec3).write<Vec3>();
    cd[0] = Vec3(1.0f, 0.0f, 0.0f);
    cd[1] = Vec3(0.0f, 1.0f, 0.0f);
    geo->createGroup("inside", AttrClass::Primitive).set(1, true);

    RigidFrame f;
    f.pieces = geo;
    f.attribute = "piece";
    RigidPose moved;
    moved.position = Vec3(0.0f, 2.0f, 0.0f);
    moved.velocity = Vec3(1.0f, 0.0f, 0.0f);
    RigidPose turned;
    turned.rotation = Vec4(0.0f, 0.7071068f, 0.0f, 0.7071068f);  // 90 degrees about y
    turned.spin = Vec3(0.0f, 1.0f, 0.0f);
    f.poses = {moved, turned};
    const GeometryPtr posed = posedPieces(f);
    const auto Q = posed->positions();
    CHECK(length(Q[1] - Vec3(1.0f, 2.0f, 0.0f)) < 1e-5f);
    CHECK(length(Q[4] - Vec3(1.0f, 0.0f, -1.0f)) < 1e-5f);  // x goes to -z
    const auto v = posed->points().find("v")->read<Vec3>();
    CHECK(length(v[0] - Vec3(1.0f, 0.0f, 0.0f)) < 1e-5f);
    CHECK(length(v[4] - cross(Vec3(0.0f, 1.0f, 0.0f), Q[4])) < 1e-5f);

    // Drawn: the first face keeps its Cd, the cut face gets the inside
    // colour -- on the corners, so that each face has its own.
    const GeometryPtr drawn = drawnPieces(f, Vec3(0.5f), Vec3(0.2f, 0.3f, 0.4f), "inside");
    CHECK(drawn->primitives().find("Cd") == nullptr);
    const auto dc = drawn->vertices().find("Cd")->read<Vec3>();
    CHECK(dc[0] == Vec3(1.0f, 0.0f, 0.0f));
    CHECK(dc[3] == Vec3(0.2f, 0.3f, 0.4f));
    // No Cd of its own: the colour given.
    geo->primitives().erase("Cd");
    const GeometryPtr plain = drawnPieces(f, Vec3(0.5f), Vec3(0.2f, 0.3f, 0.4f), "inside");
    const auto pc = plain->vertices().find("Cd")->read<Vec3>();
    CHECK(pc[0] == Vec3(0.5f));
    CHECK(pc[4] == Vec3(0.2f, 0.3f, 0.4f));
    // Grit: loose points of its size, in a shade of the inside.
    RigidFrame gritty = f;
    gritty.debris = {0.5f, 0.1f, 0.5f, 0.04f, 1.0f, 0.0f, 1.0f, 0.1f};
    const GeometryPtr withGrit = drawnPieces(gritty, Vec3(0.5f), Vec3(0.2f, 0.3f, 0.4f), "inside");
    CHECK_EQ(withGrit->pointCount(), geo->pointCount() + 2);
    CHECK(withGrit->points().find("pscale")->read<float>()[7] == 0.05f);
    // Point colours: the corners take them, the cut face's corners the inside colour.
    auto pcd = geo->points().create("Cd", AttrType::Vec3).write<Vec3>();
    for (size_t i = 0; i < 6; ++i) pcd[i] = Vec3(0.0f, 0.0f, static_cast<float>(i));
    const GeometryPtr corners = drawnPieces(f, Vec3(0.5f), Vec3(0.2f, 0.3f, 0.4f), "inside");
    CHECK(corners->points().find("Cd") == nullptr);
    const auto vc = corners->vertices().find("Cd")->read<Vec3>();
    CHECK(vc[1] == Vec3(0.0f, 0.0f, 1.0f));
    CHECK(vc[4] == Vec3(0.2f, 0.3f, 0.4f));
    // Nothing: nothing.
    CHECK_EQ(posedPieces(RigidFrame())->pointCount(), 0u);
}

TEST(rigid_pieces_without_an_attribute_are_what_touches_what) {
    auto geo = std::make_shared<Geometry>();
    geo->addPoints(7);
    auto P = geo->positionsForWrite();
    for (size_t i = 0; i < 7; ++i) P[i] = Vec3(static_cast<float>(i), 0.0f, 0.0f);
    const uint32_t a0[3] = {0, 1, 2}, a1[3] = {2, 3, 0}, a2[3] = {4, 5, 6};
    geo->addPrimitive(a0, true);
    geo->addPrimitive(a1, true);  // shares points with the first
    geo->addPrimitive(a2, true);  // alone
    int count = 0;
    const std::vector<int32_t> of = pieceOfPrimitives(*geo, "piece", count);
    CHECK_EQ(count, 2);
    CHECK_EQ(of[0], of[1]);
    CHECK(of[2] != of[0]);
    // With an attribute on the primitives: its values, numbered in order.
    auto a = geo->primitives().create("piece", AttrType::Int).write<int32_t>();
    a[0] = 7, a[1] = 3, a[2] = 7;
    const std::vector<int32_t> by = pieceOfPrimitives(*geo, "piece", count);
    CHECK_EQ(count, 2);
    CHECK_EQ(by[0], 1);
    CHECK_EQ(by[1], 0);
    CHECK_EQ(by[2], 1);
}

TEST(rigid_settings_out_of_range_are_made_safe) {
    RigidScene s;
    s.solver.density = -5.0f;
    s.solver.bounce = 3.0f;
    s.solver.substeps = 0;
    s.solver.glue = std::nanf("");
    s.solver.dustSize = 0.0f;
    const RigidScene r = s.sanitized();
    CHECK(r.solver.density >= 1.0f);
    CHECK(r.solver.bounce == 1.0f);
    CHECK(r.solver.substeps == 1);
    CHECK(r.solver.glue == RigidSettings().glue);
    CHECK(r.solver.dustSize > 0.0f);
    // No pieces: the solver says so, and does nothing.
    RigidSolver none{RigidScene()};
    CHECK(!none.error().empty());
    none.step();
    CHECK(none.capture().empty());
    CHECK_EQ(none.pieceCount(), 0u);
}

TEST(rigid_solver_is_a_node_of_the_network) {
    Network net;
    const int box = net.add("box");
    const int fracture = net.add("voronoi_fracture");
    const int rbd = net.add("rbd_solver");
    const int object = net.add("object");
    const int output = net.add("output");
    CHECK(net.setParam(box, "center", "0 1 0"));
    CHECK(net.setParam(fracture, "count", "6"));
    CHECK(net.connect(box, "geometry", fracture, "geometry"));
    CHECK(net.connect(fracture, "geometry", rbd, "pieces"));
    CHECK(net.connect(object, "collider", rbd, "colliders"));
    CHECK(net.connect(rbd, "look", output, "look"));
    CHECK(net.setParam(rbd, "glue", "500"));
    CHECK(net.setParam(rbd, "gravity", "5"));
    CHECK(net.setParam(rbd, "air", "2"));
    CHECK(net.setParam(rbd, "color", "1 0 0"));
    CHECK(net.setText(rbd, "inside_group", "cut"));
    // The Rigid output links only to a Rigid input.
    std::string why;
    CHECK(!net.connect(rbd, "rigid", output, "look", &why));
    const int pieces = net.add("rbd_pieces");
    CHECK(net.connect(rbd, "rigid", pieces, "rigid"));

    Compiled c = net.compile();
    CHECK(c.ok);
    CHECK(c.world.hasRigid);
    CHECK_EQ(c.rigid, rbd);
    CHECK(c.world.rigid.pieces != nullptr);
    CHECK_EQ(pieceCount(*c.world.rigid.pieces), 6);
    CHECK_EQ(c.world.rigid.solver.glue, 500000.0f);  // kPa
    CHECK(c.world.rigid.solver.gravity == Vec3(0.0f, -5.0f, 0.0f));
    CHECK_EQ(c.world.rigid.solver.air, 2.0f);
    CHECK_EQ(c.world.rigid.colliders.size(), 1u);
    CHECK_EQ(c.world.rigid.node, rbd);
    CHECK(!c.world.rigid.intoWater && !c.world.rigid.intoGas && !c.world.rigid.dustIntoGas);
    CHECK(c.look.pieces);
    CHECK(c.look.piecesColor == Vec3(1.0f, 0.0f, 0.0f));
    CHECK_EQ(c.look.insideGroup, "cut");
    CHECK(c.isActive(rbd) && c.isActive(fracture) && c.isActive(box) && c.isActive(object));
    for (const Problem& p : c.problems) CHECK(p.level != Problem::Level::Error);

    // Simulated: the frames carry the pieces, and RBD Pieces brings them back.
    WorldSolver solver(c.world);
    for (int i = 0; i < 30; ++i) solver.step();
    const auto frame = std::make_shared<const Frame>(solver.capture());
    CHECK(!frame->rigid.empty());
    CHECK_EQ(frame->rigid.poses.size(), 6u);
    CHECK(!frame->empty());
    GeometryGraph graph;
    graph.sync(net);
    graph.setFrames([&](int) { return frame; });
    const GeometryPtr back = graph.cook(pieces, 30, c.world.timeStep);
    CHECK(back != nullptr);
    CHECK_EQ(back->primitiveCount(), c.world.rigid.pieces->primitiveCount());
    Vec3 lo, hi;
    bounds(*back, lo, hi);
    CHECK(hi.y < 1.5f);  // fallen from where the box stood
    CHECK(back->points().find("v") != nullptr);
    // With the grit: a point more for each bit, as wide as it is, numbered.
    CHECK(net.setParam(pieces, "grit", "1"));
    graph.sync(net);
    const GeometryPtr gritty = graph.cook(pieces, 30, c.world.timeStep);
    const size_t bits = frame->rigid.debris.size() / 4;
    CHECK_EQ(gritty->pointCount(), back->pointCount() + bits);
    if (bits > 0) {
        CHECK(gritty->points().find("pscale") && gritty->points().find("id"));
        const auto id = gritty->points().find("id")->read<int32_t>();
        CHECK_EQ(static_cast<uint32_t>(id[back->pointCount()]), frame->rigid.debrisIds[0]);
        // How each bit is turned: orient, as the frame has it.
        const AttributeArray* orient = gritty->points().find("orient");
        CHECK(orient && orient->type() == AttrType::Vec4);
        if (orient) {
            const Vec4 q = orient->read<Vec4>()[back->pointCount() + bits - 1];
            const float* w = frame->rigid.debrisOrient.data() + 4 * (bits - 1);
            CHECK(q.x == w[0] && q.y == w[1] && q.z == w[2] && q.w == w[3]);
        }
    }
    CHECK(net.setParam(pieces, "grit", "0"));
    graph.sync(net);
    // Without a frame, nothing.
    graph.setFrames({});
    CHECK_EQ(graph.cook(pieces, 30, c.world.timeStep)->pointCount(), 0u);

    // Through the cache: the poses are in the file, the pieces come from the world.
    const std::string bytes = formatFrame(*frame);
    Frame read;
    std::string error;
    CHECK(parseFrame(bytes, read, error));
    CHECK(read.rigid.poses == frame->rigid.poses);
    CHECK(read.rigid.pieces == nullptr);
    adoptPieces(read, c.world.rigid);
    CHECK(read.rigid.pieces == c.world.rigid.pieces);
    CHECK_EQ(read.rigid.attribute, "piece");
    // Not these pieces: left alone.
    Frame other = read;
    other.rigid.pieces = nullptr;
    other.rigid.poses.pop_back();
    adoptPieces(other, c.world.rigid);
    CHECK(other.rigid.pieces == nullptr);

    // Files: read back the same.
    const std::string text = net.save();
    Network again;
    CHECK(Network::load(text, again, error));
    CHECK_EQ(again.save(), text);
}

TEST(rigid_pieces_go_into_the_water_the_gas_and_the_rain) {
    Network net;
    const int box = net.add("box");
    const int fracture = net.add("voronoi_fracture");
    const int rbd = net.add("rbd_solver");
    const int liquid = net.add("liquid_solver");
    const int waterLook = net.add("water_look");
    const int pyro = net.add("pyro_solver");
    const int volumeLook = net.add("volume_look");
    const int rain = net.add("rain");
    const int output = net.add("output");
    CHECK(net.setParam(box, "center", "0 1.5 0"));
    CHECK(net.setParam(box, "size", "0.4 0.4 0.4"));
    CHECK(net.setParam(fracture, "count", "4"));
    CHECK(net.connect(box, "geometry", fracture, "geometry"));
    CHECK(net.connect(fracture, "geometry", rbd, "pieces"));
    CHECK(net.connect(rbd, "collider", liquid, "colliders"));
    CHECK(net.connect(rbd, "collider", pyro, "colliders"));
    CHECK(net.connect(rbd, "dust", pyro, "sources"));
    CHECK(net.connect(rbd, "collider", rain, "colliders"));
    CHECK(net.connect(liquid, "liquid", waterLook, "liquid"));
    CHECK(net.connect(pyro, "gas", volumeLook, "gas"));
    CHECK(net.connect(waterLook, "look", output, "look"));
    CHECK(net.connect(volumeLook, "look", output, "look"));
    CHECK(net.connect(rain, "look", output, "look"));
    // Water below, a small gas box, low resolutions: quick.
    const int water = net.add("water_source");
    CHECK(net.setParam(water, "center", "0 0.15 0"));
    CHECK(net.setParam(water, "size", "1.8 0.3 1"));
    CHECK(net.setParam(water, "shape", "box"));
    CHECK(net.connect(water, "water", liquid, "sources"));
    CHECK(net.setParam(liquid, "resolution", "16"));
    CHECK(net.setParam(pyro, "resolution", "16"));
    CHECK(net.setParam(pyro, "size", "2 2 2"));
    CHECK(net.setParam(output, "frames", "10"));
    Compiled c = net.compile();
    CHECK(c.ok);
    CHECK(c.world.hasRigid && c.world.hasWater && c.world.hasGas && c.world.hasRain);
    CHECK(c.world.rigid.intoWater);
    CHECK(c.world.rigid.intoGas);
    CHECK(c.world.rigid.dustIntoGas);
    CHECK(c.world.rigid.intoRain);
    CHECK(!c.look.pieces);  // not linked into the Output: not drawn, simulated all the same
    CHECK(c.world.water.colliders.empty());  // the pieces come in as the world steps, not here
    for (const Problem& p : c.problems) CHECK(p.level != Problem::Level::Error);
    // The gas has no source but the dust: no warning that nothing will appear.
    for (const Problem& p : c.problems) CHECK(p.message.find("No sources") == std::string::npos);

    WorldSolver solver(c.world);
    for (int i = 0; i < 10; ++i) solver.step();
    const Frame f = solver.capture();
    CHECK(!f.rigid.empty());
    CHECK(!f.water.empty());
    CHECK(!f.fields.empty());
    // The water's colliders are the pieces where they are: as many as pieces.
    CHECK_EQ(solver.water()->scene().colliders.size(), 4u);
    CHECK(solver.water()->scene().colliders.front().shape == Shape::Mesh);
    CHECK(solver.rain()->scene().colliders.size() == 4u);
}

TEST(rigid_solver_says_what_is_wrong) {
    Network net;
    const int rbd = net.add("rbd_solver");
    const int output = net.add("output");
    CHECK(net.connect(rbd, "look", output, "look"));
    Compiled c = net.compile();
    CHECK(c.ok);  // it runs, with nothing in it
    bool noPieces = false;
    for (const Problem& p : c.problems) noPieces = noPieces || (p.node == rbd && p.message.find("No pieces") != std::string::npos);
    CHECK(noPieces);
    // Two solvers: the second is not simulated.
    const int second = net.add("rbd_solver");
    CHECK(net.connect(second, "look", output, "look"));
    c = net.compile();
    CHECK_EQ(c.rigid, rbd);
    bool another = false;
    for (const Problem& p : c.problems) another = another || (p.node == second && p.message.find("Another RBD Solver") != std::string::npos);
    CHECK(another);
    // Pieces from a simulation: nothing to simulate.
    const int points = net.add("liquid_points");
    CHECK(net.connect(points, "geometry", rbd, "pieces"));
    c = net.compile();
    bool fromSim = false;
    for (const Problem& p : c.problems) fromSim = fromSim || (p.node == rbd && p.message.find("come from a simulation") != std::string::npos);
    CHECK(fromSim);
    CHECK(c.world.rigid.pieces == nullptr);
}

namespace {

/// A closed box of `size` at `center`, its primitives in piece `piece`.
Geometry boxPiece(Vec3 center, Vec3 size, int32_t piece) {
    registerBuiltinNodes();
    Graph g;
    pg::Node* box = g.create("box", "box");
    box->setInt("divisions", 1);
    box->setVec3("size", size);
    box->setVec3("center", center);
    CookEngine engine;
    Geometry out = *engine.cook(*box, CookContext{});
    auto a = out.primitives().create("piece", AttrType::Int).write<int32_t>();
    std::fill(a.begin(), a.end(), piece);
    return out;
}

GeometryPtr together(std::initializer_list<Geometry> parts) {
    auto out = std::make_shared<Geometry>();
    for (const Geometry& g : parts) out->append(g);
    return out;
}

}  // namespace

TEST(rigid_bodies_are_parts_that_touch_and_glue_where_faces_meet) {
    // Two boxes side by side, half their faces against each other.
    const GeometryPtr two = together({boxPiece(Vec3(0.0f, 0.5f, 0.0f), Vec3(1.0f), 0),
                                      boxPiece(Vec3(1.0f, 0.5f, 0.5f), Vec3(1.0f), 1)});
    auto L = rigidLayout(*two, "piece");
    CHECK_EQ(L->bodies, 2);
    CHECK_EQ(L->contacts.size(), 1u);
    const RigidLayout::Contact& c = L->contacts.front();
    CHECK(std::fabs(c.area - 0.5f) < 1e-4f);
    CHECK(length(c.normal - Vec3(1.0f, 0.0f, 0.0f)) < 1e-4f);
    CHECK(length(c.at - Vec3(0.5f, 0.5f, 0.25f)) < 1e-3f);
    // One piece: one body of two parts -- it collides as both.
    const GeometryPtr one = together({boxPiece(Vec3(0.0f, 0.5f, 0.0f), Vec3(1.0f), 0),
                                      boxPiece(Vec3(1.0f, 0.5f, 0.5f), Vec3(1.0f), 0)});
    L = rigidLayout(*one, "piece");
    CHECK_EQ(L->bodies, 1);
    CHECK_EQ(L->parts[0].size(), 2u);
    CHECK(L->contacts.empty());
    // Apart, though of one piece: two bodies.
    const GeometryPtr apart = together({boxPiece(Vec3(0.0f, 0.5f, 0.0f), Vec3(1.0f), 0),
                                        boxPiece(Vec3(3.0f, 0.5f, 0.0f), Vec3(1.0f), 0)});
    L = rigidLayout(*apart, "piece");
    CHECK_EQ(L->bodies, 2);
    // Only an edge in common: no glue.
    const GeometryPtr edge = together({boxPiece(Vec3(0.0f, 0.5f, 0.0f), Vec3(1.0f), 0),
                                       boxPiece(Vec3(1.0f, 1.5f, 0.0f), Vec3(1.0f), 1)});
    CHECK(rigidLayout(*edge, "piece")->contacts.empty());

    // An L of one piece falls as one body, its two parts together.
    RigidScene scene;
    scene.pieces = together({boxPiece(Vec3(0.0f, 2.5f, 0.0f), Vec3(1.0f), 0),
                             boxPiece(Vec3(1.0f, 2.5f, 0.0f), Vec3(1.0f), 0),
                             boxPiece(Vec3(1.0f, 3.5f, 0.0f), Vec3(1.0f), 0)});
    RigidSolver solver(scene);
    CHECK_EQ(solver.pieceCount(), 1u);
    for (int i = 0; i < 90; ++i) solver.step();
    Vec3 lo, hi;
    bounds(*posedPieces(solver.capture()), lo, hi);
    CHECK(lo.y > -0.05f && lo.y < 0.1f);  // on the floor, not through it
}

TEST(rigid_charges_break_the_glue_on_time_and_kick_the_pieces) {
    // A column of three glued boxes; a charge in the middle one at 0.5 s.
    Geometry low = boxPiece(Vec3(0.0f, 0.5f, 0.0f), Vec3(1.0f), 0);
    Geometry middle = boxPiece(Vec3(0.0f, 1.5f, 0.0f), Vec3(1.0f), 1);
    Geometry top = boxPiece(Vec3(0.0f, 2.5f, 0.0f), Vec3(1.0f), 2);
    {
        auto r = middle.primitives().create("release", AttrType::Float).write<float>();
        std::fill(r.begin(), r.end(), 0.5f);
        auto k = middle.primitives().create("kick", AttrType::Vec3).write<Vec3>();
        std::fill(k.begin(), k.end(), Vec3(6.0f, 0.0f, 0.0f));
        // The foundation stays where it is; the others move (a merge fills
        // what a geometry lacks with 0: all of them say).
        for (Geometry* g : {&low, &middle, &top}) {
            auto a = g->primitives().create("active", AttrType::Int).write<int32_t>();
            std::fill(a.begin(), a.end(), g == &low ? 0 : 1);
        }
    }
    RigidScene scene;
    scene.pieces = together({low, middle, top});
    scene.solver.glue = 1e7f;
    scene.dustIntoGas = true;
    RigidSolver solver(scene);
    RigidFrame f = solver.capture();
    CHECK_EQ(f.joints, 2u);
    for (int i = 0; i < 12; ++i) solver.step();
    f = solver.capture();
    CHECK_EQ(f.broken, 0u);  // standing, glued
    CHECK(fastest(f) < 0.01f);
    bool puffed = false;
    for (int i = 0; i < 6; ++i) {
        solver.step();
        puffed = puffed || !solver.dust().empty();
    }
    f = solver.capture();
    CHECK_EQ(f.broken, 2u);  // the charge went off
    CHECK(puffed);
    CHECK(f.poses[1].velocity.x > 1.0f);  // kicked out
    for (int i = 0; i < 60; ++i) solver.step();
    f = solver.capture();
    CHECK(f.poses[1].apply(Vec3(0.0f, 1.5f, 0.0f)).x > 0.5f);
    CHECK(f.poses[0].position == Vec3());  // the foundation did not move
    // The top came down: a knock, dust, grit.
    CHECK(f.poses[2].apply(Vec3(0.0f, 2.5f, 0.0f)).y < 1.6f);
    CHECK(!f.debris.empty());
    CHECK_EQ(f.debris.size() % 4, 0u);
}

TEST(rigid_knocks_puff_dust_and_throw_grit) {
    RigidScene scene;
    scene.pieces = together({boxPiece(Vec3(0.0f, 4.0f, 0.0f), Vec3(1.0f), 0)});
    scene.dustIntoGas = true;
    RigidSolver solver(scene);
    bool puffed = false;
    for (int i = 0; i < 30; ++i) {
        solver.step();
        if (!solver.dust().empty()) puffed = true;
    }
    CHECK(puffed);
    const RigidFrame f = solver.capture();
    CHECK(!f.debris.empty());
    // The grit lands and lies on the floor.
    RigidSolver later(scene);
    for (int i = 0; i < 120; ++i) later.step();
    const RigidFrame rest = later.capture();
    for (size_t i = 0; i + 3 < rest.debris.size(); i += 4) CHECK(rest.debris[i + 1] < 0.2f);
    // No grit wanted: none.
    scene.solver.debris = 0.0f;
    RigidSolver clean(scene);
    for (int i = 0; i < 30; ++i) clean.step();
    CHECK(clean.capture().debris.empty());
}

TEST(rigid_grit_keeps_its_number_and_its_velocity_from_frame_to_frame) {
    // A block blown to dust by a charge throws a burst of grit. Each bit has
    // a number of its own, the same as long as it is there -- a renderer
    // follows it by that, to blur it as it moves -- and its velocity: a bit
    // in the air is where its velocity took it.
    Geometry block = boxPiece(Vec3(0.0f, 1.0f, 0.0f), Vec3(1.0f), 0);
    {
        auto r = block.primitives().create("release", AttrType::Float).write<float>();
        std::fill(r.begin(), r.end(), 0.1f);
        auto v = block.primitives().create("vanish", AttrType::Int).write<int32_t>();
        std::fill(v.begin(), v.end(), 1);
    }
    RigidScene scene;
    scene.pieces = together({block});
    RigidSolver solver(scene);
    const float dt = scene.solver.timeStep;
    std::map<uint32_t, Vec3> before;
    std::set<uint32_t> gone;
    int followed = 0;
    for (int i = 0; i < 30; ++i) {
        solver.step();
        const RigidFrame f = solver.capture();
        const size_t n = f.debris.size() / 4;
        CHECK_EQ(f.debrisIds.size(), n);
        CHECK_EQ(f.debrisVelocity.size(), 3 * n);
        std::map<uint32_t, Vec3> now;
        for (size_t k = 0; k < n; ++k) {
            const uint32_t id = f.debrisIds[k];
            const Vec3 at(f.debris[4 * k], f.debris[4 * k + 1], f.debris[4 * k + 2]);
            const Vec3 v(f.debrisVelocity[3 * k], f.debrisVelocity[3 * k + 1], f.debrisVelocity[3 * k + 2]);
            CHECK(now.emplace(id, at).second);  // one of each number
            CHECK(!gone.count(id));             // a number is never given again
            const auto was = before.find(id);
            // Above the floor both frames -- higher than a bit is wide: it did
            // not bounce in between.
            if (was != before.end() && was->second.y > 0.12f && at.y > 0.12f) {
                CHECK(length(at - (was->second + v * dt)) < 1e-4f);
                ++followed;
            }
        }
        for (const auto& [id, at] : before) {
            if (!now.count(id)) gone.insert(id);
        }
        before = std::move(now);
    }
    CHECK(followed > 20);
}

TEST(rigid_knocks_squeeze_out_the_air_that_swells_the_dust) {
    // A box lands hard: the air under it is squeezed out, and the puff of its
    // knock swells -- the gas pushes the dust out along the floor.
    RigidScene scene;
    scene.pieces = together({boxPiece(Vec3(0.0f, 4.0f, 0.0f), Vec3(1.0f), 0)});
    scene.dustIntoGas = true;
    auto most = [](RigidScene s) {
        RigidSolver solver(s);
        float swell = -1.0f;
        for (int i = 0; i < 30; ++i) {
            solver.step();
            for (const RigidDust& d : solver.dust()) swell = std::max(swell, d.expansion);
        }
        return swell;
    };
    const float swell = most(scene);
    CHECK(swell > 0.0f);
    CHECK(swell <= 20.0f);  // kept to what the gas takes
    // No air: the puffs are dust alone.
    scene.solver.air = 0.0f;
    CHECK_EQ(most(scene), 0.0f);
    // Out of range: made safe.
    scene.solver.air = -1.0f;
    CHECK_EQ(scene.sanitized().solver.air, 0.0f);
}

TEST(rigid_heavier_pieces_by_attribute) {
    // A light box and a heavy one on a see-saw of glue: the heavy one wins.
    Geometry light = boxPiece(Vec3(-1.0f, 2.5f, 0.0f), Vec3(1.0f), 0);
    Geometry heavy = boxPiece(Vec3(1.0f, 2.5f, 0.0f), Vec3(1.0f), 1);
    Geometry beam = boxPiece(Vec3(0.0f, 1.5f, 0.0f), Vec3(4.0f, 1.0f, 1.0f), 2);
    auto d = heavy.primitives().create("density", AttrType::Float).write<float>();
    std::fill(d.begin(), d.end(), 8000.0f);
    RigidScene scene;
    scene.pieces = together({light, heavy, beam});
    scene.solver.glue = 1e9f;
    Collider pivot;
    pivot.shape = Shape::Box;
    pivot.center = Vec3(0.0f, 0.5f, 0.0f);
    pivot.size = Vec3(0.2f, 1.0f, 2.0f);
    pivot.node = 9;
    scene.colliders.push_back(pivot);
    RigidSolver solver(scene);
    for (int i = 0; i < 45; ++i) solver.step();
    const GeometryPtr posed = posedPieces(solver.capture());
    Vec3 lo, hi;
    bounds(*posed, lo, hi);
    // Tipped towards +x: the heavy end is down.
    const RigidPose p = solver.capture().poses[1];  // a copy: the frame is gone after this line
    CHECK(p.apply(Vec3(1.0f, 2.5f, 0.0f)).y < 2.0f);
}
