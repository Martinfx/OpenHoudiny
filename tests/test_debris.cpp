//
// Debris as particles (src/pg/sim/Rigid.cpp, the grit): what a break throws
// out are particles of their own -- out of the crack, from the rim of the
// face that broke. They fly turning, held back by the air, knock into the
// pieces and what does not move, bounce off and lie still on them, riding
// what they lie on until it goes; the pieces that came loose leave dust
// behind them as they fly. How each bit is turned goes with it into the
// frames and RBD Pieces (test_rigid.cpp), the cache (test_export.cpp), USD
// (test_usd.cpp) and Copy to Points (test_sops.cpp).
//
#include "pg/core/CookEngine.h"
#include "pg/core/Graph.h"
#include "pg/nodes/Nodes.h"
#include "pg/sim/Cache.h"
#include "pg/sim/Frame.h"
#include "pg/sim/Rigid.h"

#include "test_framework.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <map>
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

void setInt(Geometry& geo, const char* name, int32_t value) {
    auto a = geo.primitives().create(name, AttrType::Int).write<int32_t>();
    std::fill(a.begin(), a.end(), value);
}

void setVec3(Geometry& geo, const char* name, Vec3 value) {
    auto a = geo.primitives().create(name, AttrType::Vec3).write<Vec3>();
    std::fill(a.begin(), a.end(), value);
}

GeometryPtr together(std::initializer_list<Geometry> parts) {
    auto out = std::make_shared<Geometry>();
    for (const Geometry& g : parts) out->append(g);
    return out;
}

/// A block `size` across at `center`, blown to dust at `when` seconds: a
/// burst of grit.
Geometry blownBlock(Vec3 center, float size, float when) {
    Geometry block = boxPiece(center, Vec3(size), 0, true);
    setFloat(block, "release", when);
    setInt(block, "vanish", 1);
    return block;
}

/// A bit of the grit of a frame.
struct Bit {
    Vec3 at, velocity;
    float size = 0.0f;
    Vec4 turn;
};

/// The grit of `f`, by the bits' numbers.
std::map<uint32_t, Bit> bitsOf(const RigidFrame& f) {
    std::map<uint32_t, Bit> out;
    const size_t n = f.debris.size() / 4;
    CHECK_EQ(f.debrisIds.size(), n);
    CHECK_EQ(f.debrisVelocity.size(), 3 * n);
    CHECK_EQ(f.debrisOrient.size(), 4 * n);
    if (f.debrisIds.size() != n || f.debrisVelocity.size() != 3 * n || f.debrisOrient.size() != 4 * n) return out;
    for (size_t i = 0; i < n; ++i) {
        Bit b;
        b.at = Vec3(f.debris[4 * i], f.debris[4 * i + 1], f.debris[4 * i + 2]);
        b.size = f.debris[4 * i + 3];
        b.velocity = Vec3(f.debrisVelocity[3 * i], f.debrisVelocity[3 * i + 1], f.debrisVelocity[3 * i + 2]);
        b.turn = Vec4(f.debrisOrient[4 * i], f.debrisOrient[4 * i + 1], f.debrisOrient[4 * i + 2], f.debrisOrient[4 * i + 3]);
        out[f.debrisIds[i]] = b;
    }
    return out;
}

float norm(const Vec4& q) { return std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w); }

bool same(const Vec4& a, const Vec4& b) { return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w; }

/// Where the point at `p` now is in the frame of a body posed so: turned
/// back, the other way round than RigidPose::apply.
Vec3 unapply(const RigidPose& pose, const Vec3& p) {
    const Vec3 u(-pose.rotation.x, -pose.rotation.y, -pose.rotation.z), v = p - pose.position;
    const Vec3 t = cross(u, v) * 2.0f;
    return v + t * pose.rotation.w + cross(u, t);
}

float along(const Vec3& v) { return std::sqrt(v.x * v.x + v.z * v.z); }

}  // namespace

TEST(debris_grit_lies_on_a_ledge_not_in_it) {
    CHECK(rigidAvailable());
    // A block blown to dust over a slab that does not move: the grit comes
    // down on the slab and lies still on it -- not in it, not through it on
    // the floor -- or, flown past its edge, on the floor.
    RigidScene scene;
    scene.pieces = together({blownBlock(Vec3(0.0f, 1.6f, 0.0f), 0.5f, 0.1f),
                             boxPiece(Vec3(0.0f, 1.0f, 0.0f), Vec3(3.0f, 0.2f, 3.0f), 1, false)});
    scene.solver.debris = 4.0f;
    RigidSolver solver(scene);
    for (int i = 0; i < 150; ++i) solver.step();
    const auto bits = bitsOf(solver.capture());
    CHECK(bits.size() > 30);
    size_t onSlab = 0, onFloor = 0;
    for (const auto& [id, b] : bits) {
        const bool over = std::fabs(b.at.x) < 1.5f && std::fabs(b.at.z) < 1.5f;
        CHECK(!(over && b.at.y > 0.9f + 1e-3f && b.at.y < 1.1f - 1e-3f));  // never in it
        CHECK(b.at.y > -1e-3f);                                           // nor under the floor
        if (over && b.at.y >= 1.1f - 1e-3f && b.at.y < 1.1f + b.size) ++onSlab;
        if (b.at.y < b.size) ++onFloor;
        CHECK_EQ(length(b.velocity), 0.0f);  // all lying still by now
    }
    CHECK(onSlab > 10);
    CHECK(onFloor > 0);
    // Lying still, turned as it came to rest: as it was.
    for (int i = 0; i < 5; ++i) solver.step();
    for (const auto& [id, b] : bitsOf(solver.capture())) {
        const auto was = bits.find(id);
        CHECK(was != bits.end());
        if (was == bits.end()) continue;
        CHECK(was->second.at == b.at);
        CHECK(same(was->second.turn, b.turn));
    }
}

TEST(debris_grit_out_of_a_piece_goes_through_what_moves_not_what_stands) {
    CHECK(rigidAvailable());
    // Two boxes glued side by side on a slab that does not move, raised off
    // the floor; a charge kicks one of them off. The grit starts in them --
    // out of the crack between them and out of the kicked one -- and goes
    // through them until it is out; into the slab it knocks, and lies on it
    // or past its edge on the floor: none in the slab, none under it.
    Geometry left = boxPiece(Vec3(-0.25f, 1.3f, 0.0f), Vec3(0.5f), 0, true);
    Geometry right = boxPiece(Vec3(0.25f, 1.3f, 0.0f), Vec3(0.5f), 1, true);
    setFloat(right, "release", 0.1f);
    setVec3(right, "kick", Vec3(2.0f, 0.0f, 0.0f));
    Geometry slab = boxPiece(Vec3(0.0f, 1.0f, 0.0f), Vec3(3.0f, 0.1f, 3.0f), 2, false);
    for (Geometry* g : {&left, &right, &slab}) setFloat(*g, "glue", g == &slab ? 0.0f : 1.0f);
    RigidScene scene;
    scene.pieces = together({left, right, slab});
    scene.solver.glue = 1e6f;
    scene.solver.debris = 5.0f;
    RigidSolver solver(scene);
    for (int i = 0; i < 120; ++i) solver.step();
    const RigidFrame f = solver.capture();
    CHECK_EQ(f.broken, 1u);
    const auto bits = bitsOf(f);
    CHECK(bits.size() > 50);
    size_t onSlab = 0;
    for (const auto& [id, b] : bits) {
        if (std::fabs(b.at.x) > 1.2f || std::fabs(b.at.z) > 1.2f) continue;  // over the slab, well in from its edge
        CHECK(b.at.y > 1.05f - 1e-3f);
        onSlab += b.at.y < 1.05f + b.size;
    }
    CHECK(onSlab > 20);
}

TEST(debris_grit_rides_what_it_lies_on_and_falls_when_that_goes) {
    CHECK(rigidAvailable());
    // Grit comes down on a slab lying on the floor. Pushed along slowly by a
    // keyed box, the slab takes it along; blown to dust, it lets it fall to
    // the floor.
    Geometry slab = boxPiece(Vec3(0.0f, 0.1f, 0.0f), Vec3(3.0f, 0.2f, 3.0f), 1, true);
    setFloat(slab, "release", 3.0f);
    setInt(slab, "vanish", 1);
    RigidScene scene;
    scene.pieces = together({blownBlock(Vec3(0.0f, 0.7f, 0.0f), 0.4f, 0.1f), slab});
    scene.solver.debris = 3.0f;
    Collider pusher;  // low: under the grit on the slab
    pusher.shape = Shape::Box;
    pusher.size = Vec3(0.3f, 0.15f, 3.4f);
    pusher.center = Vec3(-1.66f, 0.075f, 0.0f);
    pusher.node = 7;
    scene.colliders.push_back(pusher);
    RigidSolver solver(scene);
    const float dt = scene.solver.timeStep;
    auto step = [&](int frames, Vec3 velocity) {
        for (int i = 0; i < frames; ++i) {
            pusher.velocity = velocity;
            pusher.center += velocity * dt;
            solver.setColliders({pusher});
            solver.step();
        }
    };
    step(60, Vec3());
    RigidFrame f = solver.capture();
    const RigidPose before = f.poses[1];
    std::map<uint32_t, Vec3> riding;  // where each bit lies, in the slab's frame
    for (const auto& [id, b] : bitsOf(f)) {
        if (std::fabs(b.at.x) < 1.4f && std::fabs(b.at.z) < 1.4f && b.at.y > 0.2f - 1e-3f && b.at.y < 0.2f + b.size &&
            length(b.velocity) < 1e-3f) {
            riding[id] = unapply(before, b.at);
        }
    }
    CHECK(riding.size() > 10);
    // Pushed at 0.6 m/s for half a second, then let go.
    step(15, Vec3(0.6f, 0.0f, 0.0f));
    step(10, Vec3());
    f = solver.capture();
    const RigidPose after = f.poses[1];
    CHECK(after.position.x - before.position.x > 0.2f);
    const auto moved = bitsOf(f);
    size_t along = 0;
    for (const auto& [id, local] : riding) {
        const auto it = moved.find(id);
        CHECK(it != moved.end());
        if (it == moved.end()) continue;
        CHECK(length(it->second.at - after.apply(local)) < 1e-3f);
        along += it->second.at.x - (before.apply(local)).x > 0.2f;
    }
    CHECK_EQ(along, riding.size());
    // Blown to dust at three seconds: what lay on it lies on the floor.
    step(50, Vec3());
    f = solver.capture();
    CHECK(!f.vanished.empty());
    const auto fallen = bitsOf(f);
    for (const auto& [id, local] : riding) {
        const auto it = fallen.find(id);
        CHECK(it != fallen.end());
        if (it != fallen.end()) CHECK(it->second.at.y < it->second.size);
    }
}

TEST(debris_grit_flies_turning_and_the_air_holds_it_back) {
    CHECK(rigidAvailable());
    // A burst of grit high up: as it flies, the air holds each bit back --
    // along the ground it goes slower frame by frame, and the smaller it is,
    // the sooner -- and it tumbles: how it is turned, a unit quaternion,
    // changes from frame to frame.
    RigidScene scene;
    scene.pieces = together({blownBlock(Vec3(0.0f, 30.0f, 0.0f), 0.5f, 0.05f)});
    scene.solver.debris = 2.0f;
    RigidSolver solver(scene);
    std::map<uint32_t, Bit> before;
    int slowed = 0, turned = 0;
    float smallLoss = 0.0f, bigLoss = 0.0f;
    int small = 0, big = 0;
    for (int i = 0; i < 20; ++i) {
        solver.step();
        const auto now = bitsOf(solver.capture());
        for (const auto& [id, b] : now) {
            CHECK(std::fabs(norm(b.turn) - 1.0f) < 1e-4f);
            const auto was = before.find(id);
            if (was == before.end()) continue;
            const float h0 = along(was->second.velocity), h1 = along(b.velocity);
            if (h0 > 0.05f) {
                CHECK(h1 < h0);
                ++slowed;
                // How much of its speed along the ground it lost, for its speed.
                const float loss = (1.0f - h1 / h0) / std::max(length(b.velocity), 1e-3f);
                if (b.size < 0.03f) {
                    smallLoss += loss;
                    ++small;
                } else if (b.size > 0.05f) {
                    bigLoss += loss;
                    ++big;
                }
            }
            turned += !same(was->second.turn, b.turn);
        }
        before = now;
    }
    CHECK(slowed > 100);
    CHECK(turned > 100);
    CHECK(small > 0 && big > 0);
    if (small > 0 && big > 0) CHECK(smallLoss / static_cast<float>(small) > 1.5f * bigLoss / static_cast<float>(big));
    // No gravity: nothing but the air slows a bit down -- it goes slower,
    // on the way it went.
    RigidScene still = scene;
    still.solver.gravity = Vec3();
    RigidSolver drift(still);
    for (int i = 0; i < 4; ++i) drift.step();
    const auto first = bitsOf(drift.capture());
    for (int i = 0; i < 10; ++i) drift.step();
    for (const auto& [id, b] : bitsOf(drift.capture())) {
        const auto was = first.find(id);
        if (was == first.end() || length(was->second.velocity) < 0.05f) continue;
        CHECK(length(b.velocity) < length(was->second.velocity));
        CHECK(dot(b.velocity, was->second.velocity) > 0.0f);  // on the way it went
    }
}

TEST(debris_grit_is_turned_as_it_was_through_the_cache) {
    CHECK(rigidAvailable());
    // The cache keeps how each bit is turned in half floats: read back, it
    // is as it was to a thousandth, of unit length again.
    RigidScene scene;
    scene.pieces = together({blownBlock(Vec3(0.0f, 1.0f, 0.0f), 0.5f, 0.05f)});
    RigidSolver solver(scene);
    for (int i = 0; i < 10; ++i) solver.step();
    Frame frame;
    frame.rigid = solver.capture();
    CHECK(!frame.rigid.debrisOrient.empty());
    Frame back;
    std::string error;
    CHECK(parseFrame(formatFrame(frame), back, error));
    const std::vector<float>& was = frame.rigid.debrisOrient;
    const std::vector<float>& now = back.rigid.debrisOrient;
    CHECK_EQ(now.size(), was.size());
    for (size_t i = 0; i + 3 < now.size() && i + 3 < was.size(); i += 4) {
        CHECK(std::fabs(norm(Vec4(now[i], now[i + 1], now[i + 2], now[i + 3])) - 1.0f) < 1e-6f);
        for (size_t k = i; k < i + 4; ++k) CHECK(std::fabs(now[k] - was[k]) < 2e-3f);
    }
}

TEST(debris_grit_comes_out_of_the_crack_from_the_rim_of_the_face_that_broke) {
    CHECK(rigidAvailable());
    // Two boxes glued face to face, the lower one still; a charge in it
    // breaks the joint -- the grit is what comes out of the crack: from the
    // rim of the face, in its plane, flying out across it.
    Geometry low = boxPiece(Vec3(0.0f, 0.5f, 0.0f), Vec3(1.0f), 0, false);
    setFloat(low, "release", 0.1f);
    RigidScene scene;
    scene.pieces = together({low, boxPiece(Vec3(0.0f, 1.5f, 0.0f), Vec3(1.0f), 1, true)});
    scene.solver.glue = 1e7f;
    RigidSolver solver(scene);
    RigidFrame f;
    for (int i = 0; i < 10 && f.debris.empty(); ++i) {
        solver.step();
        f = solver.capture();
    }
    CHECK_EQ(f.broken, 1u);
    const auto bits = bitsOf(f);
    CHECK_EQ(bits.size(), 10u);  // a square metre of face: as many as a crack gives
    for (const auto& [id, b] : bits) {
        CHECK(std::fabs(b.at.y - 1.0f) < 0.08f);  // in the plane of the face, a step out of it at most
        const float out = along(b.at);
        CHECK(out > 0.3f && out < 0.75f);  // at its rim
        CHECK(b.velocity.x * b.at.x + b.velocity.z * b.at.z > 0.0f);  // flying out of it
    }
    // No grit wanted: none.
    scene.solver.debris = 0.0f;
    RigidSolver clean(scene);
    for (int i = 0; i < 10; ++i) clean.step();
    CHECK_EQ(clean.capture().broken, 1u);
    CHECK(clean.capture().debris.empty());
}

TEST(debris_pieces_come_loose_leave_a_trail_of_dust) {
    CHECK(rigidAvailable());
    // A box glued to one that does not move, thrown up by a charge: for as
    // long as it flies fast, not long after it came loose, it leaves dust
    // behind it. Trail 0: once the puff of the charge is gone, none.
    struct Flight {
        std::vector<RigidDust> dust;
        float top = 0.0f;  ///< how high the box's middle is
    };
    auto fly = [](float trail) {
        Geometry thrown = boxPiece(Vec3(0.0f, 1.5f, 0.0f), Vec3(1.0f), 1, true);
        setFloat(thrown, "release", 0.1f);
        setVec3(thrown, "kick", Vec3(0.0f, 9.0f, 0.0f));
        RigidScene scene;
        scene.pieces = together({boxPiece(Vec3(0.0f, 0.5f, 0.0f), Vec3(1.0f), 0, false), thrown});
        scene.solver.glue = 1e7f;
        scene.solver.trail = trail;
        scene.dustIntoGas = true;
        RigidSolver solver(scene);
        // From when the charge's puff has faded -- a third of a second after
        // it -- while the box still flies up fast.
        for (int i = 0; i < 14; ++i) solver.step();
        Flight out;
        for (int i = 0; i < 6; ++i) {
            solver.step();
            const std::vector<RigidDust> now = solver.dust();
            out.dust.insert(out.dust.end(), now.begin(), now.end());
        }
        const RigidFrame f = solver.capture();
        out.top = f.poses[1].apply(Vec3(0.0f, 1.5f, 0.0f)).y;
        CHECK(f.poses[1].velocity.y > 2.5f);
        return out;
    };
    const Flight bare = fly(0.0f);
    CHECK(bare.dust.empty());
    const Flight trailed = fly(1.0f);
    CHECK(trailed.dust.size() >= 6u);  // a puff a step at least
    CHECK(std::fabs(trailed.top - bare.top) < 1e-4f);  // the dust does not touch how it flies
    for (const RigidDust& d : trailed.dust) {
        CHECK(d.amount > 0.0f);
        CHECK(along(d.at) < 0.05f);  // on its way up
        CHECK(d.at.y < trailed.top);  // behind it
        CHECK(d.velocity.y > 0.0f);   // drawn after it
    }
    // Out of range: made safe.
    RigidScene wrong;
    wrong.solver.trail = -2.0f;
    CHECK_EQ(wrong.sanitized().solver.trail, 0.0f);
    wrong.solver.trail = std::nanf("");
    CHECK_EQ(wrong.sanitized().solver.trail, RigidSettings().trail);
}

TEST(debris_joints_know_which_way_their_faces_go) {
    // A joint's normal: across the faces that touch, from its one body to
    // the other -- the plane the grit of the crack flies out in.
    const GeometryPtr two = together({boxPiece(Vec3(0.0f, 0.5f, 0.0f), Vec3(1.0f), 0, true),
                                      boxPiece(Vec3(1.0f, 0.5f, 0.5f), Vec3(1.0f), 1, true)});
    const auto layout = rigidLayout(*two, "piece");
    const auto glue = rigidGlue(*two, *layout, "piece");
    CHECK_EQ(glue->joints.size(), 1u);
    if (glue->joints.size() != 1) return;
    const RigidJoint& j = glue->joints[0];
    CHECK(j.a < j.b);
    CHECK(length(j.normal - Vec3(1.0f, 0.0f, 0.0f)) < 1e-4f);  // body 0 at x 0, body 1 at x 1
    // As a network (RBD Constraints) and back: the same.
    const auto network = rigidNetwork(*glue, "piece");
    const auto again = rigidGlue(*two, *layout, "piece", network.get());
    CHECK_EQ(again->joints.size(), 1u);
    if (again->joints.size() == 1) CHECK(length(again->joints[0].normal - j.normal) < 1e-6f);
}
