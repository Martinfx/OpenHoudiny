//
// Pieces that break as they are knocked (RigidSettings::fracture) and wood
// (Wood Fracture): a block dropped hard breaks where it lands -- into
// fragments that together are the block, most of them small round the
// knock -- and one dropped softly does not; a weight goes on through the
// beam it breaks; a piece breaks only so many times and not below a size; the fragments are made again, to the bit,
// from the breaks a frame keeps; the same on any number of threads and
// through the cache; wood breaks into long splints along its fibres, and
// so does a piece of it the solver breaks.
//
#include "pg/core/CookEngine.h"
#include "pg/core/Graph.h"
#include "pg/core/Parallel.h"
#include "pg/nodes/Nodes.h"
#include "pg/sim/Cache.h"
#include "pg/sim/Network.h"
#include "pg/sim/Rigid.h"
#include "pg/sim/World.h"

#include "test_framework.h"

#include <algorithm>
#include <cmath>
#include <filesystem>

using namespace pg;
using namespace pg::sim;

namespace {

/// A closed box of `size` at `center`, one piece.
GeometryPtr block(Vec3 center, Vec3 size) {
    registerBuiltinNodes();
    Graph g;
    pg::Node* box = g.create("box", "box");
    box->setInt("divisions", 1);
    box->setVec3("size", size);
    box->setVec3("center", center);
    CookEngine engine;
    return engine.cook(*box, CookContext{});
}

/// A box of `size` at `center` broken by a Wood Fracture of `count` pieces.
GeometryPtr wood(Vec3 center, Vec3 size, int count) {
    registerBuiltinNodes();
    Graph g;
    pg::Node* box = g.create("box", "box");
    box->setInt("divisions", 1);
    box->setVec3("size", size);
    box->setVec3("center", center);
    pg::Node* fracture = g.create("woodfracture", "wood");
    fracture->setInt("count", count);
    fracture->setInput(0, box);
    CookEngine engine;
    return engine.cook(*fracture, CookContext{});
}

/// A block dropped from `height` (its bottom) onto the floor, breaking
/// harder than `fracture` pascals a square metre of its section holds.
RigidScene dropped(float height, float fracture) {
    RigidScene s;
    s.pieces = block(Vec3(0.0f, height + 0.25f, 0.0f), Vec3(0.8f, 0.5f, 0.5f));
    s.solver.fracture = fracture;
    s.solver.glue = 0.0f;
    s.solver.substeps = 4;
    s.solver.fracturePieces = 10;
    return s;
}

double volumeOf(const Geometry& geo, const RigidLayout& layout, const std::vector<uint32_t>& gone, size_t from = 0) {
    // The signed volumes of the faces' fans: closed meshes, each body's.
    const auto P = geo.positions();
    double sum = 0.0;
    for (size_t b = from; b < static_cast<size_t>(layout.bodies); ++b) {
        if (std::binary_search(gone.begin(), gone.end(), static_cast<uint32_t>(b))) continue;
        for (const uint32_t prim : layout.prims[b]) {
            const auto c = geo.primitivePoints(prim);
            for (size_t i = 1; i + 1 < c.size(); ++i) {
                const Vec3 a = P[c[0]], q = P[c[i]], r = P[c[i + 1]];
                sum += static_cast<double>(dot(a, cross(q, r))) / 6.0;
            }
        }
    }
    return sum;
}

/// A box of `size` at `center`, piece `piece`: still or not, so heavy, and
/// breaking so easily (f@fracture).
GeometryPtr part(Vec3 center, Vec3 size, int piece, bool moves, float density, float fracture) {
    auto g = std::make_shared<Geometry>(*block(center, size));
    auto number = g->primitives().create("piece", AttrType::Int).write<int32_t>();
    std::fill(number.begin(), number.end(), piece);
    for (const auto& [name, value] :
         {std::pair<const char*, float>{"active", moves ? 1.0f : 0.0f}, {"density", density}, {"fracture", fracture}}) {
        auto w = g->primitives().create(name, AttrType::Float).write<float>();
        std::fill(w.begin(), w.end(), value);
    }
    return g;
}

/// A steel block dropped onto the middle of a beam lying on two still
/// posts, the beam breaking harder than `fracture` pascals a square metre
/// of its section holds.
RigidScene punched(float fracture) {
    auto pieces = std::make_shared<Geometry>(*part(Vec3(0.0f, 1.0f, 0.0f), Vec3(2.4f, 0.16f, 0.3f), 0, true, 600.0f, 1.0f));
    pieces->append(*part(Vec3(-1.0f, 0.46f, 0.0f), Vec3(0.3f, 0.92f, 0.4f), 1, false, 2400.0f, 1.0f));
    pieces->append(*part(Vec3(1.0f, 0.46f, 0.0f), Vec3(0.3f, 0.92f, 0.4f), 2, false, 2400.0f, 1.0f));
    pieces->append(*part(Vec3(0.0f, 2.6f, 0.0f), Vec3(0.3f, 0.3f, 0.3f), 3, true, 7800.0f, 0.0f));
    RigidScene s;
    s.pieces = pieces;
    s.solver.fracture = fracture;
    s.solver.glue = 0.0f;
    s.solver.substeps = 4;
    s.solver.fractureDepth = 1;
    return s;
}

std::vector<RigidFrame> run(const RigidScene& scene, int frames) {
    RigidSolver solver(scene);
    std::vector<RigidFrame> out;
    for (int f = 0; f < frames; ++f) {
        solver.step();
        out.push_back(solver.capture());
    }
    return out;
}

}  // namespace

TEST(shatter_a_block_dropped_hard_breaks_where_it_lands) {
    if (!rigidAvailable()) return;
    const std::vector<RigidFrame> frames = run(dropped(3.0f, 2e5f), 40);
    const RigidFrame& last = frames.back();
    CHECK_EQ(last.shatters.size() >= 1, true);
    const RigidShatter& first = last.shatters.front();
    CHECK_EQ(first.body, 0u);
    // Where it was knocked: at its bottom, as it rests.
    CHECK(std::fabs(first.at.y - 3.0f) < 0.1f);
    // Into fragments, the block gone: as much of it as there was.
    CHECK(last.poses.size() > 4);
    CHECK(std::binary_search(last.vanished.begin(), last.vanished.end(), 0u));
    const double before = 0.8 * 0.5 * 0.5;
    CHECK(std::fabs(volumeOf(*last.pieces, *last.layout, last.vanished) - before) < 0.02 * before);
    // They lie on the floor, none under it.
    const std::shared_ptr<Geometry> posed = posedPieces(last);
    float lowest = 1e30f;
    for (const Vec3& p : posed->positions()) lowest = std::min(lowest, p.y);
    CHECK(lowest > -0.03f);
    // The faces of the cracks are the inside.
    const Group* inside = last.pieces->findGroup("inside");
    CHECK(inside != nullptr);
    size_t cut = 0;
    for (size_t p = 0; p < last.pieces->primitiveCount(); ++p) cut += inside->contains(p) ? 1 : 0;
    CHECK(cut > 10);
}

TEST(shatter_a_soft_drop_breaks_nothing_and_fracture_0_never_does) {
    if (!rigidAvailable()) return;
    const std::vector<RigidFrame> soft = run(dropped(0.05f, 2e5f), 30);
    CHECK(soft.back().shatters.empty());
    CHECK_EQ(soft.back().poses.size(), size_t(1));
    const std::vector<RigidFrame> never = run(dropped(3.0f, 0.0f), 40);
    CHECK(never.back().shatters.empty());
    // The same as a solver without it, to the bit.
    RigidScene plain = dropped(3.0f, 0.0f);
    plain.solver.fracturePieces = 3;
    plain.solver.fractureDepth = 5;
    const std::vector<RigidFrame> same = run(plain, 40);
    CHECK(same.back().poses == never.back().poses);
}

TEST(shatter_fragments_break_only_so_often_and_not_below_a_size) {
    if (!rigidAvailable()) return;
    RigidScene once = dropped(6.0f, 1e4f);
    once.solver.fractureDepth = 1;
    const RigidFrame a = run(once, 60).back();
    CHECK_EQ(a.shatters.size(), size_t(1));
    RigidScene twice = once;
    twice.solver.fractureDepth = 2;
    twice.solver.fractureMinSize = 0.05f;
    const RigidFrame b = run(twice, 60).back();
    CHECK(b.shatters.size() > 1);
    for (const RigidShatter& s : b.shatters) {
        // A fragment breaks: its body is one the first break made.
        CHECK(s.body == 0 || s.body >= 1);
    }
    RigidScene small = twice;
    small.solver.fractureMinSize = 5.0f;  // bigger than the block
    CHECK(run(small, 60).back().shatters.empty());
}

TEST(shatter_a_weight_goes_through_the_beam_it_breaks) {
    if (!rigidAvailable()) return;
    // How fast the weight falls when it has passed the beam's top -- or,
    // when the beam holds, lies on it.
    auto fallen = [](const std::vector<RigidFrame>& frames, float& lowest) {
        float speed = 0.0f;
        lowest = 1e30f;
        for (const RigidFrame& f : frames) {
            const RigidPose& w = f.poses[3];
            const float y = 2.6f + w.position.y;  // its middle: it turns little
            lowest = std::min(lowest, y);
            if (y < 1.0f && speed == 0.0f) speed = -w.velocity.y;
        }
        return speed;
    };
    float through = 0.0f, held = 0.0f;
    const std::vector<RigidFrame> broken = run(punched(1e5f), 30);
    CHECK(!broken.back().shatters.empty());
    CHECK_EQ(broken.back().shatters.front().body, 0u);
    // It falls on through what is left of the beam, still fast...
    const float fast = fallen(broken, through);
    CHECK(fast > 3.0f);
    CHECK(through < 0.6f);
    // ... and lies on the beam that does not break.
    const std::vector<RigidFrame> whole = run(punched(0.0f), 30);
    CHECK(whole.back().shatters.empty());
    CHECK_EQ(fallen(whole, held), 0.0f);
    CHECK(held > 1.1f);
}

TEST(shatter_fragments_are_made_again_from_the_breaks_to_the_bit) {
    if (!rigidAvailable()) return;
    const RigidScene scene = dropped(4.0f, 5e4f);
    const RigidFrame last = run(scene, 50).back();
    CHECK(!last.shatters.empty());
    const std::shared_ptr<const RigidBroken> again = rigidBroken(scene, last.shatters);
    CHECK(again != nullptr);
    CHECK_EQ(again->layout->bodies, last.layout->bodies);
    CHECK_EQ(again->pieces->pointCount(), last.pieces->pointCount());
    CHECK(again->pieces->hash() == last.pieces->hash());
    CHECK(again->layout->bodyOf == last.layout->bodyOf);
}

TEST(shatter_the_same_on_any_number_of_threads_and_through_the_cache) {
    if (!rigidAvailable()) return;
    const unsigned saved = TaskPool::instance().threadCount();
    World w;
    w.hasRigid = true;
    w.rigid = dropped(4.0f, 5e4f);
    TaskPool::instance().setThreadCount(1);
    WorldSolver one(w);
    for (int f = 0; f < 45; ++f) one.step();
    TaskPool::instance().setThreadCount(4);
    WorldSolver four(w);
    for (int f = 0; f < 45; ++f) four.step();
    TaskPool::instance().setThreadCount(saved);
    const Frame a = one.capture(), b = four.capture();
    CHECK(a.rigid.poses == b.rigid.poses);
    CHECK(a.rigid.shatters == b.rigid.shatters);
    CHECK(!a.rigid.shatters.empty());
    // Written and read back: the breaks, and the fragments made again.
    const std::filesystem::path folder = std::filesystem::temp_directory_path() / "pg_test_shatter_cache";
    std::filesystem::remove_all(folder);
    std::string error;
    CHECK(writeFrame(a, folder.string(), error));
    Frame back;
    CHECK(readFrame(folder.string(), a.number, back, error));
    CHECK(back.rigid.shatters == a.rigid.shatters);
    adoptPieces(back, w.rigid);
    CHECK(back.rigid.pieces != nullptr);
    CHECK_EQ(back.rigid.layout->bodies, a.rigid.layout->bodies);
    CHECK(back.rigid.pieces->hash() == a.rigid.pieces->hash());
    std::filesystem::remove_all(folder);
}

TEST(wood_breaks_into_long_splints_along_its_fibres) {
    const GeometryPtr pieces = wood(Vec3(0.0f, 0.5f, 0.0f), Vec3(3.0f, 0.2f, 0.25f), 12);
    int count = 0;
    const std::vector<int32_t> pieceOf = pieceOfPrimitives(*pieces, "piece", count);
    CHECK(count >= 8);
    // Each piece's box: longer along the beam than across it, by far.
    std::vector<Vec3> lo(static_cast<size_t>(count), Vec3(1e30f)), hi(static_cast<size_t>(count), Vec3(-1e30f));
    const std::vector<Vec3> P = rigidPositions(*pieces);  // the plain cut
    for (size_t p = 0; p < pieces->primitiveCount(); ++p) {
        for (const uint32_t q : pieces->primitivePoints(p)) {
            const size_t k = static_cast<size_t>(pieceOf[p]);
            for (int a = 0; a < 3; ++a) {
                lo[k][a] = std::min(lo[k][a], P[q][a]);
                hi[k][a] = std::max(hi[k][a], P[q][a]);
            }
        }
    }
    int splints = 0;
    for (int k = 0; k < count; ++k) {
        const Vec3 e = hi[static_cast<size_t>(k)] - lo[static_cast<size_t>(k)];
        splints += e.x > 2.5f * std::max(e.y, e.z) ? 1 : 0;
    }
    CHECK(splints * 2 > count);
    // The fibres' way on the points, and wood on the faces.
    const AttributeArray* grain = pieces->points().find("grain");
    CHECK(grain != nullptr);
    CHECK(std::fabs(grain->read<Vec3>()[0].x) > 0.99f);
    CHECK_EQ(primitiveString(*pieces, "material", 0), std::string("wood"));
    // The faces across the fibres torn: points moved along them, off the plain cut.
    const auto at = pieces->positions();
    float torn = 0.0f;
    for (size_t i = 0; i < at.size(); ++i) torn = std::max(torn, std::fabs(at[i].x - P[i].x));
    CHECK(torn > 0.01f);
    CHECK(torn < 0.06f);
}

TEST(wood_a_piece_the_solver_breaks_splits_along_its_fibres) {
    if (!rigidAvailable()) return;
    RigidScene s;
    // One splint of wood -- the fibres along x -- dropped onto the floor.
    s.pieces = wood(Vec3(0.0f, 4.0f, 0.0f), Vec3(2.0f, 0.12f, 0.12f), 1);
    s.solver.fracture = 1e4f;
    s.solver.glue = 0.0f;
    s.solver.substeps = 4;
    s.solver.fracturePieces = 8;
    s.solver.fractureDepth = 1;
    const RigidFrame last = run(s, 50).back();
    CHECK_EQ(last.shatters.size(), size_t(1));
    // Its fragments: long along x.
    int longer = 0, all = 0;
    for (size_t b = 1; b < static_cast<size_t>(last.layout->bodies); ++b) {
        Vec3 lo(1e30f), hi(-1e30f);
        for (const uint32_t prim : last.layout->prims[b]) {
            for (const uint32_t q : last.pieces->primitivePoints(prim)) {
                for (int a = 0; a < 3; ++a) {
                    lo[a] = std::min(lo[a], last.pieces->positions()[q][a]);
                    hi[a] = std::max(hi[a], last.pieces->positions()[q][a]);
                }
            }
        }
        const Vec3 e = hi - lo;
        ++all;
        longer += e.x > 1.5f * std::max(e.y, e.z) ? 1 : 0;
    }
    CHECK(all >= 4);
    CHECK(longer * 2 >= all);
}
