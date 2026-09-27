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

TEST(rigid_glue_holds_the_pieces_together_and_breaks_under_a_pull) {
    // A beam that hangs over the edge of a table -- most of it on the table,
    // so that it does not tip: glued, it holds; loose, the part over the
    // edge falls.
    auto beam = [](float glue) {
        RigidScene scene;
        scene.pieces = fracturedBox(Vec3(0.0f, 1.05f, 0.0f), Vec3(2.0f, 0.1f, 0.2f), 10, 3);
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
    RigidSolver glued(beam(1e9f));
    CHECK(glued.error().empty());
    RigidFrame start = glued.capture();
    CHECK(start.joints > 0u);
    for (int i = 0; i < 60; ++i) glued.step();
    RigidFrame held = glued.capture();
    CHECK_EQ(held.broken, 0u);
    Vec3 lo, hi;
    bounds(*posedPieces(held), lo, hi);
    CHECK(lo.y > 0.8f);  // the whole beam stays up, held by the glued part on the table

    RigidSolver loose(beam(0.0f));
    for (int i = 0; i < 60; ++i) loose.step();
    RigidFrame fallen = loose.capture();
    CHECK_EQ(fallen.joints, 0u);
    bounds(*posedPieces(fallen), lo, hi);
    CHECK(lo.y < -1.0f);  // the pieces over the edge are gone down

    // Weak glue: the weight of the overhang tears it, and dust puffs where
    // it broke.
    RigidSolver weak(beam(50.0f));
    CHECK(weak.capture().joints > 0u);
    bool puffed = false;
    for (int i = 0; i < 60; ++i) {
        weak.step();
        puffed = puffed || !weak.dust().empty();
    }
    RigidFrame torn = weak.capture();
    CHECK(torn.broken > 0u);
    CHECK(puffed);
    bounds(*posedPieces(torn), lo, hi);
    CHECK(lo.y < -1.0f);
}

TEST(rigid_a_keyed_object_knocks_the_pieces_over) {
    // A glued wall, and a ball that swings through it: the glue breaks and
    // the wall goes.
    RigidScene scene;
    scene.pieces = fracturedBox(Vec3(0.0f, 0.5f, 0.0f), Vec3(1.0f, 1.0f, 0.2f), 12, 5);
    scene.solver.glue = 2000.0f;
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
    scene.solver.glue = 300.0f;
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

    // Drawn: the first face keeps its Cd, the cut face gets the inside colour.
    const GeometryPtr drawn = drawnPieces(f, Vec3(0.5f), Vec3(0.2f, 0.3f, 0.4f), "inside");
    const auto dc = drawn->primitives().find("Cd")->read<Vec3>();
    CHECK(dc[0] == Vec3(1.0f, 0.0f, 0.0f));
    CHECK(dc[1] == Vec3(0.2f, 0.3f, 0.4f));
    // No Cd of its own: the colour given.
    geo->primitives().erase("Cd");
    const GeometryPtr plain = drawnPieces(f, Vec3(0.5f), Vec3(0.2f, 0.3f, 0.4f), "inside");
    const auto pc = plain->primitives().find("Cd")->read<Vec3>();
    CHECK(pc[0] == Vec3(0.5f));
    CHECK(pc[1] == Vec3(0.2f, 0.3f, 0.4f));
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
    CHECK_EQ(c.world.rigid.solver.glue, 500.0f);
    CHECK(c.world.rigid.solver.gravity == Vec3(0.0f, -5.0f, 0.0f));
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
