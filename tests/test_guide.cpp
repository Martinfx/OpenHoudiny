//
// Guided rigid bodies (src/pg/sim/Rigid.h, the guide): the pieces moved as
// the shot wants them to go lead the bodies -- each step a body is steered
// to where the guide has it, as hard as the strength says, and still knocks
// into things -- until its time is up, its glue breaks or something stops
// it further from the guide than the reach. In a network the RBD Solver
// takes the guide linked into Guide at every frame.
//
#include "pg/core/CookEngine.h"
#include "pg/core/Graph.h"
#include "pg/nodes/Nodes.h"
#include "pg/sim/Network.h"
#include "pg/sim/Rigid.h"
#include "pg/sim/World.h"

#include "test_framework.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <initializer_list>
#include <string>

using namespace pg;
using namespace pg::sim;

namespace {

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

void setFloat(Geometry& geo, const char* name, float value) {
    auto a = geo.primitives().create(name, AttrType::Float).write<float>();
    std::fill(a.begin(), a.end(), value);
}

std::shared_ptr<Geometry> together(std::initializer_list<Geometry> parts) {
    auto out = std::make_shared<Geometry>();
    for (const Geometry& g : parts) out->append(g);
    return out;
}

/// `geo` with every point moved by `move`: turned, then shifted.
std::shared_ptr<const Geometry> moved(const Geometry& geo, const std::function<Vec3(const Vec3&)>& move) {
    auto out = std::make_shared<Geometry>(geo);
    auto P = out->positionsForWrite();
    for (Vec3& p : P) p = move(p);
    return out;
}

/// `p` turned by `degrees` about the y axis through `pivot`.
Vec3 turnedAboutY(const Vec3& p, float degrees, const Vec3& pivot = Vec3()) {
    const float a = degrees * 3.14159265f / 180.0f;
    const Vec3 d = p - pivot;
    return pivot + Vec3(d.x * std::cos(a) + d.z * std::sin(a), d.y, -d.x * std::sin(a) + d.z * std::cos(a));
}

bool near(const Vec3& a, const Vec3& b, float eps) { return length(a - b) < eps; }

/// How far, at most, the points of `rest` posed by `pose` are from `there`'s.
float offBy(const RigidPose& pose, const Geometry& rest, const Geometry& there) {
    float most = 0.0f;
    const auto a = rest.positions(), b = there.positions();
    for (size_t i = 0; i < a.size(); ++i) most = std::max(most, length(pose.apply(a[i]) - b[i]));
    return most;
}

}  // namespace

TEST(guide_a_body_goes_where_the_guide_has_it_and_stays_there) {
    CHECK(rigidAvailable());
    // A box on the floor; its guide: the box two metres over, a metre up,
    // turned a quarter round. Led as hard as can be, the box is there within
    // a few steps -- held up against gravity -- and stays.
    const auto box = together({boxPiece(Vec3(0.0f, 0.5f, 0.0f), Vec3(1.0f), 0, true)});
    const auto guide = moved(*box, [](const Vec3& p) { return turnedAboutY(p, 90.0f) + Vec3(2.0f, 1.0f, 0.0f); });
    RigidScene scene;
    scene.pieces = box;
    scene.guide = guide;
    RigidSolver solver(scene);
    for (int i = 0; i < 10; ++i) solver.step();
    CHECK(offBy(solver.capture().poses[0], *box, *guide) < 0.02f);
    for (int i = 0; i < 60; ++i) solver.step();
    const RigidFrame f = solver.capture();
    CHECK(offBy(f.poses[0], *box, *guide) < 0.01f);
    CHECK(length(f.poses[0].velocity) < 0.05f);
    // Without it, the box stays on the floor.
    scene.guide = nullptr;
    RigidSolver free(scene);
    for (int i = 0; i < 30; ++i) free.step();
    CHECK(length(free.capture().poses[0].position) < 0.01f);
    // A guide that is not the pieces moved -- other points -- leads nothing.
    scene.guide = together({boxPiece(Vec3(2.0f, 1.5f, 0.0f), Vec3(1.0f), 0, true),
                            boxPiece(Vec3(4.0f, 1.5f, 0.0f), Vec3(1.0f), 1, true)});
    RigidSolver wrong(scene);
    for (int i = 0; i < 30; ++i) wrong.step();
    CHECK(length(wrong.capture().poses[0].position) < 0.01f);
}

TEST(guide_a_body_follows_a_guide_that_moves_and_lags_when_led_softly) {
    CHECK(rigidAvailable());
    // The guide goes round a circle a metre across, turning as it goes: led
    // at full strength the box keeps to it within a centimetre; at a
    // fifth, further behind, but it follows.
    const auto box = together({boxPiece(Vec3(0.0f, 1.5f, 0.0f), Vec3(0.5f), 0, true)});
    auto guideAt = [&](int frame) {
        const float t = static_cast<float>(frame) / 30.0f;
        const Vec3 shift(std::sin(3.0f * t), 0.2f * t, 1.0f - std::cos(3.0f * t));
        return moved(*box, [&](const Vec3& p) { return turnedAboutY(p, 60.0f * t, Vec3(0.0f, 1.5f, 0.0f)) + shift; });
    };
    auto run = [&](float strength) {
        RigidScene scene;
        scene.pieces = box;
        scene.guide = guideAt(1);
        scene.solver.guideStrength = strength;
        RigidSolver solver(scene);
        float worst = 0.0f;
        for (int frame = 1; frame <= 60; ++frame) {
            const auto guide = guideAt(frame);
            solver.setGuide(guide, strength);
            solver.step();
            if (frame > 10) worst = std::max(worst, offBy(solver.capture().poses[0], *box, *guide));
        }
        return worst;
    };
    const float hard = run(1.0f), soft = run(0.2f);
    CHECK(hard < 0.01f);
    CHECK(soft > 2.0f * hard);
    CHECK(soft < 0.5f);
}

TEST(guide_lets_the_bodies_go_after_its_time_or_when_they_are_too_far) {
    CHECK(rigidAvailable());
    const auto box = together({boxPiece(Vec3(0.0f, 0.5f, 0.0f), Vec3(1.0f), 0, true)});
    const auto up = moved(*box, [](const Vec3& p) { return p + Vec3(0.0f, 2.0f, 0.0f); });
    // Until half a second: then the box falls back to the floor.
    RigidScene scene;
    scene.pieces = box;
    scene.guide = up;
    scene.solver.guideUntil = 0.5f;
    RigidSolver timed(scene);
    for (int i = 0; i < 15; ++i) timed.step();
    CHECK(timed.capture().poses[0].position.y > 1.9f);
    for (int i = 0; i < 45; ++i) timed.step();
    CHECK(timed.capture().poses[0].position.y < 0.05f);
    // A guide down through the floor: the floor stops the box. With a reach
    // of half a metre it goes its own way -- and the guide up high again no
    // longer takes it there; with none, it does.
    const auto down = moved(*box, [](const Vec3& p) { return p + Vec3(0.0f, -0.8f, 0.0f); });
    auto run = [&](float reach) {
        RigidScene s;
        s.pieces = box;
        s.guide = down;
        s.solver.guideStrength = 0.5f;
        s.solver.guideReach = reach;
        RigidSolver solver(s);
        for (int i = 0; i < 15; ++i) solver.step();
        solver.setGuide(up, 0.5f);
        for (int i = 0; i < 60; ++i) solver.step();
        return solver.capture().poses[0].position.y;
    };
    CHECK(run(0.5f) < 0.05f);
    CHECK(run(0.0f) > 1.9f);
}

TEST(guide_lets_go_of_a_piece_whose_glue_breaks) {
    CHECK(rigidAvailable());
    // Two boxes glued side by side, held a metre up by their guide; a charge
    // breaks the glue at a third of a second. Let Go: both fall. Held on:
    // both stay where the guide has them.
    Geometry left = boxPiece(Vec3(-0.5f, 0.5f, 0.0f), Vec3(1.0f), 0, true);
    Geometry right = boxPiece(Vec3(0.5f, 0.5f, 0.0f), Vec3(1.0f), 1, true);
    setFloat(right, "release", 0.33f);
    const auto pieces = together({left, right});
    auto run = [&](bool letGo) {
        RigidScene s;
        s.pieces = pieces;
        s.guide = moved(*pieces, [](const Vec3& p) { return p + Vec3(0.0f, 1.0f, 0.0f); });
        s.solver.glue = 1e7f;
        s.solver.guideLetGo = letGo;
        RigidSolver solver(s);
        for (int i = 0; i < 9; ++i) solver.step();
        RigidFrame f = solver.capture();
        CHECK_EQ(f.broken, 0u);
        CHECK(f.poses[0].position.y > 0.95f && f.poses[1].position.y > 0.95f);
        for (int i = 0; i < 40; ++i) solver.step();
        f = solver.capture();
        CHECK_EQ(f.broken, 1u);
        return std::max(f.poses[0].position.y, f.poses[1].position.y);
    };
    CHECK(run(true) < 0.05f);
    CHECK(run(false) > 0.95f);
}

TEST(guide_leads_each_piece_as_much_as_its_attribute_says) {
    CHECK(rigidAvailable());
    // Two boxes apart, both guided a metre up; the second's guide 0: it
    // stays on the floor. A still piece is not led either.
    Geometry led = boxPiece(Vec3(-1.0f, 0.5f, 0.0f), Vec3(1.0f), 0, true);
    Geometry not_ = boxPiece(Vec3(1.0f, 0.5f, 0.0f), Vec3(1.0f), 1, true);
    Geometry still = boxPiece(Vec3(3.0f, 0.5f, 0.0f), Vec3(1.0f), 2, false);
    setFloat(led, "guide", 1.0f);
    setFloat(not_, "guide", 0.0f);
    setFloat(still, "guide", 1.0f);
    RigidScene s;
    s.pieces = together({led, not_, still});
    s.guide = moved(*s.pieces, [](const Vec3& p) { return p + Vec3(0.0f, 1.0f, 0.0f); });
    RigidSolver solver(s);
    for (int i = 0; i < 30; ++i) solver.step();
    const RigidFrame f = solver.capture();
    CHECK(std::fabs(f.poses[0].position.y - 1.0f) < 0.01f);
    CHECK(std::fabs(f.poses[1].position.y) < 0.01f);
    CHECK(f.poses[2].position == Vec3());
    // Out of range: made safe.
    RigidScene wrong;
    wrong.solver.guideStrength = 7.0f;
    wrong.solver.guideUntil = -1.0f;
    wrong.solver.guideReach = std::nanf("");
    const RigidScene safe = wrong.sanitized();
    CHECK_EQ(safe.solver.guideStrength, 1.0f);
    CHECK_EQ(safe.solver.guideUntil, 0.0f);
    CHECK_EQ(safe.solver.guideReach, RigidSettings().guideReach);
}

TEST(guide_rbd_solver_takes_the_guide_at_every_frame) {
    CHECK(rigidAvailable());
    // A box cut in pieces, and a Transform of the pieces keyed from where
    // they stand to two metres up over twenty frames: the guide. It is
    // taken at every frame, and the pieces rise with it.
    Network net;
    const int box = net.add("box");
    const int cut = net.add("voronoi_fracture");
    const int lift = net.add("transform");
    const int rbd = net.add("rbd_solver");
    const int out = net.add("output");
    CHECK(net.setParam(box, "center", "0 0.5 0"));
    CHECK(net.setParam(cut, "count", "6"));
    CHECK(net.setParam(out, "frames", "20"));
    CHECK(net.connect(box, "geometry", cut, "geometry"));
    CHECK(net.connect(cut, "geometry", rbd, "pieces"));
    CHECK(net.connect(cut, "geometry", lift, "geometry"));
    CHECK(net.connect(lift, "geometry", rbd, "guide"));
    CHECK(net.connect(rbd, "look", out, "look"));
    CHECK(net.setKey(lift, "t", 1.0f, {0.0f, 0.0f, 0.0f}, Interp::Linear));
    CHECK(net.setKey(lift, "t", 20.0f, {0.0f, 2.0f, 0.0f}, Interp::Linear));
    CHECK(net.setParam(rbd, "guide_strength", "0.8"));
    CHECK(net.setParam(rbd, "guide_until", "2"));
    CHECK(net.setParam(rbd, "guide_reach", "1.5"));
    CHECK(net.setParam(rbd, "guide_let_go", "0"));
    Compiled c = net.compile();
    CHECK(c.ok);
    for (const Problem& p : c.problems) CHECK(p.level != Problem::Level::Error);
    const RigidScene& r = c.world.rigid;
    CHECK(r.guide != nullptr);
    CHECK_EQ(r.solver.guideStrength, 0.8f);
    CHECK_EQ(r.solver.guideUntil, 2.0f);
    CHECK_EQ(r.solver.guideReach, 1.5f);
    CHECK(!r.solver.guideLetGo);
    CHECK(!c.world.animation.empty());
    const World& last = c.world.at(20);
    CHECK(last.rigid.guide != nullptr && last.rigid.guide != r.guide);
    if (last.rigid.guide && r.pieces) {
        CHECK(near(last.rigid.guide->positions()[0], r.pieces->positions()[0] + Vec3(0.0f, 2.0f, 0.0f), 1e-4f));
    }
    WorldSolver sim(c.world);
    for (int i = 0; i < 20; ++i) sim.step();
    const RigidFrame f = sim.capture().rigid;
    for (const RigidPose& p : f.poses) CHECK(p.position.y > 1.7f);

    // A guide that is not the pieces moved: said, and nothing leads them.
    const int other = net.add("box");
    CHECK(net.connect(other, "geometry", rbd, "guide"));
    c = net.compile();
    CHECK(c.world.rigid.guide == nullptr);
    bool said = false;
    for (const Problem& p : c.problems) said = said || p.message.find("must be the pieces moved") != std::string::npos;
    CHECK(said);

    // A guide a wrangle moves by the time, nothing keyed: taken at every
    // frame all the same.
    Network timed;
    const int b = timed.add("box");
    const int v = timed.add("voronoi_fracture");
    const int w = timed.add("point_wrangle");
    const int s = timed.add("rbd_solver");
    const int o = timed.add("output");
    CHECK(timed.setParam(b, "center", "0 0.5 0"));
    CHECK(timed.setParam(v, "count", "4"));
    CHECK(timed.setText(w, "snippet", "@P.y += @Time;"));
    CHECK(timed.connect(b, "geometry", v, "geometry"));
    CHECK(timed.connect(v, "geometry", s, "pieces"));
    CHECK(timed.connect(v, "geometry", w, "geometry"));
    CHECK(timed.connect(w, "geometry", s, "guide"));
    CHECK(timed.connect(s, "look", o, "look"));
    const Compiled t = timed.compile();
    CHECK(t.guideMoves);
    CHECK(!t.world.animation.empty());
}
