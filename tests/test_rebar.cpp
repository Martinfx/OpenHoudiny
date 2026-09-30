//
// Reinforced concrete: the Rebar node (src/pg/nodes/Rebar.cpp) lays bars in
// a block as they are laid before it is poured -- a mesh in a wall, a cage
// with stirrups in a beam, turned as the block lies, Cover in from its
// faces; rigidRebar (src/pg/sim/Rigid.h) follows each bar through the
// pieces; the RBD Solver holds the pieces on the bars where the glue no
// longer does -- they bend and stay bent, tear pulled harder than the
// steel holds, slide out of a piece that anchors them less -- the same
// every time; what became of them goes through the cache; the network
// compiles them in; the look draws them as steel.
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
#include <filesystem>
#include <functional>
#include <map>
#include <set>

using namespace pg;
using namespace pg::sim;

namespace {

/// What `last` cooks to, in a graph `build` makes.
GeometryPtr cooked(const std::function<pg::Node*(Graph&)>& build) {
    registerBuiltinNodes();
    Graph g;
    pg::Node* last = build(g);
    CookEngine engine;
    return engine.cook(*last, CookContext{});
}

/// A box of `size` at `center`: the piece `piece`, moving or not.
Geometry block(Vec3 size, Vec3 center, int piece, bool moves) {
    Geometry geo = *cooked([&](Graph& g) {
        pg::Node* box = g.create("box", "box");
        box->setVec3("size", size);
        box->setVec3("center", center);
        return box;
    });
    auto p = geo.primitives().create("piece", AttrType::Int).write<int32_t>();
    std::fill(p.begin(), p.end(), piece);
    auto a = geo.primitives().create("active", AttrType::Int).write<int32_t>();
    std::fill(a.begin(), a.end(), moves ? 1 : 0);
    return geo;
}

/// Polylines through `lines`, `width` across.
std::shared_ptr<Geometry> barsAlong(const std::vector<std::vector<Vec3>>& lines, float width) {
    auto geo = std::make_shared<Geometry>();
    std::vector<uint32_t> ids;
    for (const std::vector<Vec3>& line : lines) {
        ids.clear();
        const size_t first = geo->addPoints(line.size());
        auto P = geo->positionsForWrite();
        for (size_t i = 0; i < line.size(); ++i) {
            P[first + i] = line[i];
            ids.push_back(static_cast<uint32_t>(first + i));
        }
        geo->addPrimitive(ids, false);
    }
    auto w = geo->points().create("width", AttrType::Float).write<float>();
    std::fill(w.begin(), w.end(), width);
    return geo;
}

/// The Rebar node over `input`, set up by `setup`.
GeometryPtr rebarOf(const std::function<pg::Node*(Graph&)>& input, const std::function<void(pg::Node&)>& setup = {}) {
    return cooked([&](Graph& g) {
        pg::Node* in = input(g);
        pg::Node* bars = g.create("rebar", "rebar");
        bars->setInput(0, in);
        if (setup) setup(*bars);
        return bars;
    });
}

/// A cantilever: a still block, and three pieces in a row off its side --
/// no glue -- with a bar along the top and one along the bottom through
/// them all.
RigidScene cantilever(float strength, float stretch, bool withBars) {
    auto pieces = std::make_shared<Geometry>(block(Vec3(0.5f, 0.3f, 0.3f), Vec3(-0.25f, 2.0f, 0.0f), 0, false));
    for (int k = 0; k < 3; ++k) {
        pieces->append(block(Vec3(0.4f, 0.3f, 0.3f), Vec3(0.2f + 0.4f * static_cast<float>(k), 2.0f, 0.0f), k + 1, true));
    }
    RigidScene scene;
    scene.pieces = pieces;
    scene.solver.glue = 0.0f;
    scene.solver.rebarStrength = strength;
    scene.solver.stretch = stretch;
    if (withBars) {
        scene.rebar = barsAlong({{Vec3(-0.45f, 2.1f, 0.0f), Vec3(1.15f, 2.1f, 0.0f)},
                                 {Vec3(-0.45f, 1.9f, 0.0f), Vec3(1.15f, 1.9f, 0.0f)}},
                                0.012f);
    }
    return scene;
}

/// A piece hanging under a still block on one bar down through both,
/// running `into` into the piece; heavier by `density`.
RigidScene hanging(float strength, float into, float density) {
    auto pieces = std::make_shared<Geometry>(block(Vec3(0.3f, 0.5f, 0.3f), Vec3(0.0f, 2.25f, 0.0f), 0, false));
    Geometry below = block(Vec3(0.3f, 0.5f, 0.3f), Vec3(0.0f, 1.75f, 0.0f), 1, true);
    auto d = below.primitives().create("density", AttrType::Float).write<float>();
    std::fill(d.begin(), d.end(), density);
    pieces->append(below);
    RigidScene scene;
    scene.pieces = pieces;
    scene.solver.glue = 0.0f;
    scene.solver.rebarStrength = strength;
    scene.rebar = barsAlong({{Vec3(0.0f, 2.45f, 0.0f), Vec3(0.0f, 2.0f - into, 0.0f)}}, 0.012f);
    return scene;
}

RigidFrame run(const RigidScene& scene, int frames) {
    RigidSolver solver(scene.sanitized());
    for (int i = 0; i < frames; ++i) solver.step();
    return solver.capture();
}

/// Where the middle of body `b` of `f` is: its first point's pose applied
/// to the middle of its box at rest.
Vec3 middleOf(const RigidFrame& f, int b) {
    const RigidLayout& L = *f.layout;
    const auto P = f.pieces->positions();
    Vec3 lo(1e30f, 1e30f, 1e30f), hi(-1e30f, -1e30f, -1e30f);
    for (const uint32_t prim : L.prims[static_cast<size_t>(b)]) {
        for (const uint32_t p : f.pieces->primitivePoints(prim)) {
            for (int a = 0; a < 3; ++a) {
                lo[a] = std::min(lo[a], P[p][a]);
                hi[a] = std::max(hi[a], P[p][a]);
            }
        }
    }
    return f.poses[static_cast<size_t>(b)].apply((lo + hi) * 0.5f);
}

/// The body of the piece numbered `piece`.
int bodyOfPiece(const RigidFrame& f, int piece) {
    const auto p = f.pieces->primitives().find("piece")->read<int32_t>();
    for (size_t prim = 0; prim < p.size(); ++prim) {
        if (p[prim] == piece) return f.layout->bodyOf[prim];
    }
    return -1;
}

}  // namespace

TEST(rebar_lays_a_mesh_in_a_wall_and_a_cage_in_a_beam) {
    const float cover = 0.035f, d = 0.012f;
    // A wall: a mesh both ways, a layer near each face.
    const GeometryPtr mesh = rebarOf([](Graph& g) {
        pg::Node* box = g.create("box", "wall");
        box->setVec3("size", Vec3(5.0f, 3.0f, 0.3f));
        box->setVec3("center", Vec3(0.0f, 1.7f, 0.0f));
        return box;
    });
    // 16 bars along it (3 m less the cover, 0.2 apart) and 26 up it, twice.
    CHECK_EQ(mesh->primitiveCount(), size_t(2 * (16 + 26)));
    std::set<float> depths;
    const auto P = mesh->positions();
    const auto w = mesh->points().find("width")->read<float>();
    for (size_t i = 0; i < P.size(); ++i) {
        CHECK_NEAR(w[i], d, 1e-6f);
        // Cover of concrete over them, on every side.
        CHECK(std::fabs(P[i].x) <= 2.5f - cover + 1e-4f);
        CHECK(P[i].y >= 0.2f + cover - 1e-4f && P[i].y <= 3.2f - cover + 1e-4f);
        CHECK(std::fabs(P[i].z) <= 0.15f - cover - 0.5f * d + 1e-4f);
        depths.insert(std::round(P[i].z * 1000.0f) / 1000.0f);
    }
    // One way on the other, near each face.
    CHECK_EQ(depths.size(), size_t(4));
    CHECK_NEAR(*depths.rbegin(), 0.15f - cover - 0.5f * d, 1e-3f);
    CHECK_NEAR(*std::next(depths.rbegin()), 0.15f - cover - 1.5f * d, 1e-3f);
    for (size_t prim = 0; prim < mesh->primitiveCount(); ++prim) {
        CHECK(!mesh->primitiveClosed(prim));
        CHECK_EQ(mesh->primitivePoints(prim).size(), size_t(2));
    }
    // One layer: in the middle.
    const GeometryPtr one = rebarOf(
        [](Graph& g) {
            pg::Node* box = g.create("box", "wall");
            box->setVec3("size", Vec3(5.0f, 3.0f, 0.3f));
            return box;
        },
        [](pg::Node& n) { n.setInt("layers", 1); });
    CHECK_EQ(one->primitiveCount(), size_t(16 + 26));

    // A beam, turned as it hangs: bars along it round a cage, and stirrups
    // round them -- square to the beam, whichever way it lies.
    auto beam = [](Graph& g) {
        pg::Node* box = g.create("box", "beam");
        box->setVec3("size", Vec3(3.0f, 0.4f, 0.4f));
        box->setVec3("center", Vec3(0.0f, 0.0f, 0.0f));
        pg::Node* broken = g.create("voronoifracture", "fracture");
        broken->setInt("count", 20);
        broken->setInput(0, box);
        pg::Node* turn = g.create("transform", "turn");
        turn->setVec3("t", Vec3(0.3f, 3.4f, -0.2f));
        turn->setVec3("r", Vec3(0.0f, 20.0f, 6.0f));
        turn->setInput(0, broken);
        return turn;
    };
    const GeometryPtr cage = rebarOf(beam);
    // 3 bars a side round 0.4 m less the cover and the stirrups: 8; the
    // stirrups every 0.2 m along 3 m less the cover: 16.
    size_t along = 0, stirrups = 0;
    const Mat4 turn = rotationXYZ(Vec3(0.0f, 20.0f, 6.0f));
    const auto Q = cage->positions();
    const auto qw = cage->points().find("width")->read<float>();
    for (size_t prim = 0; prim < cage->primitiveCount(); ++prim) {
        const auto c = cage->primitivePoints(prim);
        if (c.size() == 2) {
            ++along;
            CHECK_NEAR(qw[c[0]], d, 1e-6f);
        } else {
            ++stirrups;
            CHECK_EQ(c.size(), size_t(5));
            CHECK(length(Q[c.front()] - Q[c.back()]) < 1e-5f);  // round to where it started
            CHECK_NEAR(qw[c[0]], 0.008f, 1e-6f);
        }
        for (const uint32_t q : c) {
            // Back in the beam's own axes: inside it, Cover in from its faces.
            const Vec3 local = Q[q] - Vec3(0.3f, 3.4f, -0.2f);
            const Vec3 ax = transformDirection(turn, Vec3(1.0f, 0.0f, 0.0f));
            const Vec3 ay = transformDirection(turn, Vec3(0.0f, 1.0f, 0.0f));
            const Vec3 az = transformDirection(turn, Vec3(0.0f, 0.0f, 1.0f));
            CHECK(std::fabs(dot(local, ax)) <= 1.5f - cover + 1e-3f);
            CHECK(std::fabs(dot(local, ay)) <= 0.2f - cover + 1e-3f);
            CHECK(std::fabs(dot(local, az)) <= 0.2f - cover + 1e-3f);
            CHECK(std::fabs(dot(local, ay)) >= 0.1f || std::fabs(dot(local, az)) >= 0.1f || c.size() == 2);
        }
    }
    CHECK_EQ(along, size_t(8));
    CHECK_EQ(stirrups, size_t(16));
    // Nothing in, nothing out.
    const GeometryPtr none = rebarOf([](Graph& g) { return g.create("merge", "nothing"); });
    CHECK_EQ(none->primitiveCount(), size_t(0));
}

TEST(rebar_model_follows_each_bar_through_the_pieces) {
    // A slab of Voronoi pieces and a bar through it, and one past it.
    const GeometryPtr pieces = cooked([](Graph& g) {
        pg::Node* box = g.create("box", "slab");
        box->setVec3("size", Vec3(2.0f, 0.3f, 1.0f));
        box->setVec3("center", Vec3(0.0f, 0.15f, 0.0f));
        pg::Node* broken = g.create("voronoifracture", "fracture");
        broken->setInt("count", 16);
        broken->setInput(0, box);
        return broken;
    });
    const auto layout = rigidLayout(*pieces, "piece");
    const auto bars = barsAlong({{Vec3(-0.95f, 0.15f, 0.1f), Vec3(0.95f, 0.15f, 0.1f)},
                                 {Vec3(-0.95f, 0.8f, 0.1f), Vec3(0.95f, 0.8f, 0.1f)}},
                                0.012f);
    const auto model = rigidRebar(*pieces, *layout, *bars);
    CHECK_EQ(model->bars.size(), size_t(2));
    const RigidRebar::Bar& through = model->bars[0];
    CHECK_NEAR(through.along.back(), 1.9f, 1e-5f);
    CHECK(through.count >= 2u);
    CHECK_EQ(model->bars[1].count, 0u);  // above the slab: in no piece
    // In order along it, one after another -- the cells fill the slab, so
    // together all of it -- and each in the piece it says.
    float covered = 0.0f;
    for (uint32_t s = through.first; s < through.first + through.count; ++s) {
        const RigidRebar::Station& st = model->stations[s];
        CHECK(st.out > st.in);
        covered += st.out - st.in;
        if (s > through.first) {
            const RigidRebar::Station& before = model->stations[s - 1];
            CHECK(st.in >= before.out - 1e-5f);
            CHECK(st.in - before.out < 1e-3f);
            CHECK(st.body != before.body);
        }
        Geometry one;
        const auto P = pieces->positions();
        std::vector<uint32_t> ids;
        std::map<uint32_t, uint32_t> local;
        for (const uint32_t prim : layout->prims[static_cast<size_t>(st.body)]) {
            ids.clear();
            for (const uint32_t p : pieces->primitivePoints(prim)) {
                const auto [it, added] = local.try_emplace(p, static_cast<uint32_t>(local.size()));
                if (added) {
                    one.addPoints(1);
                    one.positionsForWrite()[it->second] = P[p];
                }
                ids.push_back(it->second);
            }
            one.addPrimitive(ids, true);
        }
        CHECK(insideMesh(one, RigidRebar::at(through, 0.5f * (st.in + st.out))));
    }
    CHECK_NEAR(covered, 1.9f, 2e-3f);
    // Where it runs, and which way.
    CHECK(length(RigidRebar::at(through, 0.95f) - Vec3(0.0f, 0.15f, 0.1f)) < 1e-5f);
    CHECK(length(RigidRebar::tangent(through, 0.3f) - Vec3(1.0f, 0.0f, 0.0f)) < 1e-6f);
    // Bars and pieces made the same, the same stretches.
    const auto again = rigidRebar(*pieces, *layout, *bars);
    CHECK_EQ(again->stations.size(), model->stations.size());
    for (size_t s = 0; s < model->stations.size(); ++s) {
        CHECK_EQ(again->stations[s].body, model->stations[s].body);
        CHECK_EQ(again->stations[s].in, model->stations[s].in);
    }
}

TEST(rebar_holds_the_pieces_the_glue_no_longer_does) {
    if (!rigidAvailable()) return;
    // No glue: without bars the pieces fall off the block's side...
    const RigidFrame loose = run(cantilever(500e6f, 0.1f, false), 45);
    const int far = bodyOfPiece(loose, 3);
    CHECK(middleOf(loose, far).y < 0.5f);
    // ... with them they hold, as a reinforced beam does.
    const RigidFrame held = run(cantilever(500e6f, 0.1f, true), 45);
    CHECK(held.rebar != nullptr);
    CHECK_EQ(held.rebarState.size(), held.rebar->stations.size());
    CHECK(middleOf(held, far).y > 1.85f);
    for (const uint8_t s : held.rebarState) CHECK_EQ(s, 0);
    CHECK(length(held.poses[static_cast<size_t>(far)].velocity) < 0.05f);
}

TEST(rebar_bends_and_stays_bent) {
    if (!rigidAvailable()) return;
    // Bars too weak for the pieces' weight -- stretching far before they
    // tear: the pieces swing down on them, bending them, and stay where
    // they came to rest; bent bars do not spring back.
    const RigidFrame bent = run(cantilever(20e6f, 5.0f, true), 90);
    const int far = bodyOfPiece(bent, 3);
    const Vec3 at = middleOf(bent, far);
    CHECK(at.y < 1.8f);  // it bent down
    CHECK(at.y > 0.5f);  // ... and hangs on
    for (const uint8_t s : bent.rebarState) CHECK(!(s & RigidFrame::kRebarTorn));
    RigidScene scene = cantilever(20e6f, 5.0f, true);
    RigidSolver solver(scene.sanitized());
    for (int i = 0; i < 90; ++i) solver.step();
    const Vec3 was = middleOf(solver.capture(), far);
    for (int i = 0; i < 30; ++i) solver.step();
    const RigidFrame later = solver.capture();
    CHECK(length(middleOf(later, far) - was) < 0.02f);
    // The same every time.
    const RigidFrame again = run(cantilever(20e6f, 5.0f, true), 90);
    CHECK(again.poses == bent.poses);
    CHECK(again.rebarState == bent.rebarState);
}

TEST(rebar_tears_pulled_harder_than_the_steel_holds) {
    if (!rigidAvailable()) return;
    // 90 kg on a 12 mm bar: 500 MPa steel holds it (57 kN)...
    const RigidFrame holds = run(hanging(500e6f, 0.45f, 2000.0f), 40);
    CHECK(middleOf(holds, bodyOfPiece(holds, 1)).y > 1.7f);
    // ... steel of 5 MPa holds 565 N: it yields, stretches and tears -- the
    // piece anchors it along 45 cm, far harder than that.
    const RigidFrame torn = run(hanging(5e6f, 0.45f, 2000.0f), 40);
    CHECK(middleOf(torn, bodyOfPiece(torn, 1)).y < 1.0f);
    CHECK(std::any_of(torn.rebarState.begin(), torn.rebarState.end(), [](uint8_t s) { return s & RigidFrame::kRebarTorn; }));
    for (const uint8_t s : torn.rebarState) CHECK(!(s & RigidFrame::kRebarLoose));
    // Drawn torn: two stretches of bar, each in its piece.
    CHECK_EQ(rebarBars(torn)->primitiveCount(), size_t(2));
    CHECK_EQ(rebarBars(holds)->primitiveCount(), size_t(1));
}

TEST(rebar_slides_out_of_a_piece_that_anchors_it_less) {
    if (!rigidAvailable()) return;
    // 3 cm of the bar in a piece of 900 kg: its bond (5.7 kN) holds less
    // than the steel, and less than the weight -- the bar slides out of it.
    const RigidFrame out = run(hanging(500e6f, 0.03f, 20000.0f), 40);
    const int below = bodyOfPiece(out, 1);
    CHECK(middleOf(out, below).y < 1.0f);
    const RigidRebar& model = *out.rebar;
    bool left = false;
    for (size_t s = 0; s < model.stations.size(); ++s) {
        CHECK(!(out.rebarState[s] & RigidFrame::kRebarTorn));
        if (model.stations[s].body == below) left = out.rebarState[s] & RigidFrame::kRebarLoose;
    }
    CHECK(left);
    // Lighter, it holds.
    const RigidFrame held = run(hanging(500e6f, 0.03f, 2000.0f), 40);
    CHECK(middleOf(held, bodyOfPiece(held, 1)).y > 1.7f);
    // Bond 0: bars hold nothing.
    RigidScene smooth = hanging(500e6f, 0.45f, 2000.0f);
    smooth.solver.bond = 0.0f;
    CHECK(middleOf(run(smooth, 40), 1).y < 1.0f);
}

TEST(rebar_goes_through_the_cache_and_is_drawn_as_steel) {
    if (!rigidAvailable()) return;
    const RigidScene scene = cantilever(20e6f, 5.0f, true);
    RigidSolver solver(scene.sanitized());
    for (int i = 0; i < 20; ++i) solver.step();
    Frame frame;
    frame.number = 20;
    frame.rigid = solver.capture();
    const std::string folder = (std::filesystem::temp_directory_path() / "pg_test_rebar_cache").string();
    std::filesystem::remove_all(folder);
    std::string error;
    CHECK(writeFrame(frame, folder, error));
    Frame back;
    CHECK(readFrame(folder, 20, back, error));
    CHECK(back.rigid.rebarState == frame.rigid.rebarState);
    World world;
    world.rigid = scene;
    std::shared_ptr<const RigidLayout> layout;
    std::shared_ptr<const RigidRebar> bars;
    adoptPieces(back, world.rigid, &layout, &bars);
    CHECK(back.rigid.rebar != nullptr);
    CHECK(bars == back.rigid.rebar);
    const GeometryPtr a = rebarBars(frame.rigid), b = rebarBars(back.rigid);
    CHECK_EQ(a->pointCount(), b->pointCount());
    for (size_t i = 0; i < a->pointCount(); ++i) CHECK(length(a->positions()[i] - b->positions()[i]) < 1e-6f);
    // Another world's bars -- fewer stretches than the frame says: none.
    RigidScene other = scene;
    other.rebar = barsAlong({{Vec3(-0.45f, 2.0f, 0.0f), Vec3(0.3f, 2.0f, 0.0f)}}, 0.012f);
    Frame elsewhere;
    CHECK(readFrame(folder, 20, elsewhere, error));
    adoptPieces(elsewhere, other);
    CHECK(elsewhere.rigid.rebar == nullptr);
    std::filesystem::remove_all(folder);

    // Drawn: the pieces, and a tube of six sides round each stretch of bar,
    // in the colour of steel.
    const Vec3 steel(0.3f, 0.25f, 0.21f);
    const GeometryPtr drawn = drawnPieces(frame.rigid, Vec3(0.5f), Vec3(0.6f), "inside", steel);
    const GeometryPtr plain = drawnPieces(RigidFrame{frame.rigid.pieces, frame.rigid.layout, frame.rigid.attribute,
                                                     frame.rigid.poses},
                                          Vec3(0.5f), Vec3(0.6f), "inside", steel);
    const GeometryPtr centre = rebarBars(frame.rigid);
    size_t segments = 0;
    for (size_t prim = 0; prim < centre->primitiveCount(); ++prim) segments += centre->primitivePoints(prim).size() - 1;
    CHECK_EQ(drawn->primitiveCount(), plain->primitiveCount() + 6 * segments);
    const auto cd = drawn->vertices().find("Cd")->read<Vec3>();
    CHECK(cd.back() == steel);
    CHECK(cd.front() == Vec3(0.5f));
    // As thick as the bars: every corner of a tube 6 mm from its bar's line.
    const auto P = drawn->positions();
    const auto C = centre->positions();
    const size_t firstTube = plain->pointCount();
    for (size_t i = firstTube; i < firstTube + 6; ++i) CHECK_NEAR(length(P[i] - C[0]), 0.006f, 1e-4f);
}

TEST(rebar_in_a_network_compiles_into_the_solver_and_its_look) {
    Network net;
    const int box = net.add("box");
    const int fracture = net.add("voronoi_fracture");
    const int bars = net.add("rebar");
    const int rbd = net.add("rbd_solver");
    const int output = net.add("output");
    CHECK(net.setParam(box, "size", "3 0.4 0.4"));
    CHECK(net.setParam(box, "center", "0 1 0"));
    CHECK(net.setParam(fracture, "count", "12"));
    CHECK(net.connect(box, "geometry", fracture, "geometry"));
    CHECK(net.connect(box, "geometry", bars, "geometry"));
    CHECK(net.connect(fracture, "geometry", rbd, "pieces"));
    CHECK(net.connect(bars, "geometry", rbd, "rebar"));
    CHECK(net.connect(rbd, "look", output, "look"));
    CHECK(net.setParam(bars, "layout", "cage"));
    CHECK(net.setParam(rbd, "rebar_strength", "400"));
    CHECK(net.setParam(rbd, "bond", "4"));
    CHECK(net.setParam(rbd, "stretch", "0.2"));
    CHECK(net.setParam(rbd, "rebar_color", "0.4 0.3 0.2"));
    const Compiled c = net.compile();
    CHECK(c.ok);
    for (const Problem& p : c.problems) CHECK(p.level != Problem::Level::Error);
    CHECK(c.world.rigid.rebar != nullptr);
    CHECK_EQ(c.world.rigid.rebar->primitiveCount(), size_t(8 + 16));
    CHECK_NEAR(c.world.rigid.solver.rebarStrength, 400e6f, 1.0f);
    CHECK_NEAR(c.world.rigid.solver.bond, 4e6f, 1.0f);
    CHECK_NEAR(c.world.rigid.solver.stretch, 0.2f, 1e-6f);
    CHECK(c.look.pieces);
    CHECK(c.look.rebarColor == Vec3(0.4f, 0.3f, 0.2f));
    // Simulated, RBD Pieces brings the bars back too: a polyline for each
    // stretch of one, as thick as it is.
    if (rigidAvailable()) {
        const int back = net.add("rbd_pieces");
        CHECK(net.connect(rbd, "rigid", back, "rigid"));
        const Compiled simulated = net.compile();
        WorldSolver solver(simulated.world);
        for (int i = 0; i < 5; ++i) solver.step();
        const auto frame = std::make_shared<const Frame>(solver.capture());
        CHECK(frame->rigid.rebar != nullptr);
        GeometryGraph graph;
        graph.sync(net);
        graph.setFrames([&](int) { return frame; });
        const size_t prims = graph.cook(back, 5, simulated.world.timeStep)->primitiveCount();
        CHECK(net.setParam(back, "rebar", "1"));
        graph.sync(net);
        const GeometryPtr withBars = graph.cook(back, 5, simulated.world.timeStep);
        const GeometryPtr bars = rebarBars(frame->rigid);
        CHECK(bars->primitiveCount() >= size_t(8 + 16));
        CHECK_EQ(withBars->primitiveCount(), prims + bars->primitiveCount());
        CHECK(withBars->points().find("width") != nullptr);
        CHECK(!withBars->primitiveClosed(withBars->primitiveCount() - 1));
        CHECK(net.remove(back));
    }
    // Without bars linked, none; bars with no lines: said.
    CHECK(net.disconnect(Link{bars, "geometry", rbd, "rebar"}));
    CHECK(net.compile().world.rigid.rebar == nullptr);
    const int empty = net.add("merge");
    CHECK(net.connect(empty, "geometry", rbd, "rebar"));
    const Compiled none = net.compile();
    CHECK(none.world.rigid.rebar == nullptr);
    CHECK(std::any_of(none.problems.begin(), none.problems.end(),
                      [](const Problem& p) { return p.message.find("no lines") != std::string::npos; }));
    // Out of range, made safe.
    RigidScene odd;
    odd.solver.rebarStrength = -5.0f;
    odd.solver.bond = std::nanf("");
    odd.solver.stretch = -1.0f;
    CHECK_EQ(odd.sanitized().solver.rebarStrength, 1e6f);
    CHECK_EQ(odd.sanitized().solver.bond, RigidSettings().bond);
    CHECK_EQ(odd.sanitized().solver.stretch, 0.0f);
    // Written and read back the same.
    const std::string text = net.save();
    Network again;
    std::string error;
    CHECK(Network::load(text, again, error));
    CHECK_EQ(again.save(), text);
}
