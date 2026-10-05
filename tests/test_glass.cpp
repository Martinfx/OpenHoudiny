//
// Glass: Glass Fracture (src/pg/nodes/Glass.cpp) breaks a pane as glass
// breaks where it is struck -- closed shards that make the pane, slivers at
// the blow and wide shards further out, cracks square to the pane however
// it lies, the same every time and on any number of threads; drawn
// (src/pg/sim/Display.h) apart from the rest, its chips dots below 0,
// smooth by normals of its own and else flat; and the RBD Solver
// (src/pg/sim/Rigid.h) keeps a pane whole -- no cracks -- until it breaks,
// when it throws chips of glass and little dust.
//
#include "pg/core/CookEngine.h"
#include "pg/core/Graph.h"
#include "pg/core/Parallel.h"
#include "pg/nodes/Nodes.h"
#include "pg/sim/Display.h"
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

/// A pane of `size` at `center`, turned by `turn` (degrees) about it, broken
/// by a Glass Fracture that `setup` sets up.
GeometryPtr pane(Vec3 size, Vec3 center, const std::function<void(pg::Node&)>& setup, Vec3 turn = Vec3()) {
    registerBuiltinNodes();
    Graph g;
    pg::Node* box = g.create("box", "pane");
    box->setVec3("size", size);
    pg::Node* last = box;
    pg::Node* place = g.create("transform", "place");
    place->setInput(0, box);
    place->setVec3("r", turn);
    place->setVec3("t", center);
    last = place;
    pg::Node* glass = g.create("glassfracture", "glass");
    glass->setInput(0, last);
    setup(*glass);
    CookEngine engine;
    return engine.cook(*glass, CookContext{});
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

/// Twice the area of face `prim`, along its normal as its corners go round.
Vec3 areaVector(const Geometry& geo, uint32_t prim) {
    const auto P = geo.positions();
    const auto f = geo.primitivePoints(prim);
    Vec3 sum;
    for (size_t i = 1; i + 1 < f.size(); ++i) sum += cross(P[f[i]] - P[f[0]], P[f[i + 1]] - P[f[0]]);
    return sum;
}

/// The volume the faces `prims` hold, about `middle`: positive when they
/// are turned outwards.
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

int glassOfPrim(const Geometry& geo, size_t prim) { return geo.primitives().find("glass")->read<int32_t>()[prim]; }

size_t cracksIn(const Geometry& geo) {
    const AttributeArray* a = geo.primitives().find("glass");
    if (!a) return 0;
    const auto g = a->read<int32_t>();
    return static_cast<size_t>(std::count(g.begin(), g.end(), 2));
}

}  // namespace

TEST(glass_fracture_breaks_a_pane_into_closed_shards_that_make_it) {
    const Vec3 size(1.2f, 1.5f, 0.006f), center(0.0f, 1.25f, 0.0f);
    const GeometryPtr glass = pane(size, center, [](pg::Node& n) { n.setVec3("impact", Vec3(0.1f, 1.35f, 0.0f)); });
    const std::vector<std::vector<uint32_t>> shards = primsOfPieces(*glass);
    CHECK(shards.size() > 100);
    const Group* inside = glass->findGroup("inside");
    CHECK(inside != nullptr);
    const auto cd = glass->primitives().find("Cd")->read<Vec3>();
    double volume = 0.0, faces = 0.0;
    for (const std::vector<uint32_t>& prims : shards) {
        CHECK(!prims.empty());
        CHECK(closed(*glass, prims));
        // Turned outwards: it holds a volume of its own.
        const double v = volumeOf(*glass, prims, center);
        CHECK(v > 0.0);
        volume += v;
        for (const uint32_t prim : prims) {
            // 1 a face of the pane, 2 of a crack: those the fracture cut.
            const int kind = glassOfPrim(*glass, prim);
            CHECK(kind == 1 || kind == 2);
            CHECK_EQ(kind == 2, inside && inside->contains(prim));
            CHECK(cd[prim] == Vec3(0.82f, 0.9f, 0.88f));
            const Vec3 a = areaVector(*glass, prim);
            if (kind == 1 && std::fabs(a.z) > 0.99f * length(a)) faces += 0.5 * static_cast<double>(length(a));
        }
    }
    // The shards make the pane: its volume, both of its faces.
    CHECK_NEAR(volume, static_cast<double>(size.x * size.y * size.z), 1e-6);
    CHECK_NEAR(faces, 2.0 * static_cast<double>(size.x * size.y), 1e-4);
}

TEST(glass_fracture_has_slivers_at_the_blow_and_wide_shards_further_out) {
    const Vec3 impact(0.1f, 1.35f, 0.0f);
    const GeometryPtr glass =
        pane(Vec3(1.2f, 1.5f, 0.006f), Vec3(0.0f, 1.25f, 0.0f), [&](pg::Node& n) { n.setVec3("impact", impact); });
    const auto P = glass->positions();
    double nearArea = 0.0, farArea = 0.0;
    int nearCount = 0, farCount = 0;
    for (const std::vector<uint32_t>& prims : primsOfPieces(*glass)) {
        double area = 0.0;
        Vec3 sum;
        int n = 0;
        for (const uint32_t prim : prims) {
            const Vec3 a = areaVector(*glass, prim);
            if (glassOfPrim(*glass, prim) == 1 && a.z > 0.99f * length(a)) area += 0.5 * static_cast<double>(length(a));
            for (const uint32_t p : glass->primitivePoints(prim)) {
                sum += P[p];
                ++n;
            }
        }
        const float away = length(sum * (1.0f / static_cast<float>(n)) - impact);
        if (away < 0.15f) {
            nearArea += area;
            ++nearCount;
        } else if (away > 0.45f) {
            farArea += area;
            ++farCount;
        }
    }
    CHECK(nearCount > 20);
    CHECK(farCount > 5);
    CHECK(farArea / farCount > 20.0 * (nearArea / nearCount));

    // More rays, more shards; fewer rings, fewer.
    const size_t base = primsOfPieces(*glass).size();
    const GeometryPtr more = pane(Vec3(1.2f, 1.5f, 0.006f), Vec3(0.0f, 1.25f, 0.0f), [&](pg::Node& n) {
        n.setVec3("impact", impact);
        n.setInt("radials", 24);
    });
    const GeometryPtr fewer = pane(Vec3(1.2f, 1.5f, 0.006f), Vec3(0.0f, 1.25f, 0.0f), [&](pg::Node& n) {
        n.setVec3("impact", impact);
        n.setInt("rings", 4);
    });
    CHECK(primsOfPieces(*more).size() > base);
    CHECK(primsOfPieces(*fewer).size() < base);
}

TEST(glass_fracture_cracks_run_square_to_the_pane_however_it_lies) {
    // A pane lying tilted: its web in its own plane, round where it is struck.
    const Vec3 turn(30.0f, 40.0f, 0.0f), center(0.3f, 1.0f, -0.2f);
    const GeometryPtr glass = pane(Vec3(0.8f, 0.6f, 0.01f), center, [&](pg::Node& n) { n.setVec3("impact", center); },
                                   turn);
    // The pane's normal: that of its biggest faces.
    Vec3 normal;
    float biggest = 0.0f;
    for (size_t prim = 0; prim < glass->primitiveCount(); ++prim) {
        const Vec3 a = areaVector(*glass, static_cast<uint32_t>(prim));
        if (glassOfPrim(*glass, prim) == 1 && length(a) > biggest) {
            biggest = length(a);
            normal = normalize(a);
        }
    }
    size_t cracks = 0;
    for (size_t prim = 0; prim < glass->primitiveCount(); ++prim) {
        const Vec3 a = areaVector(*glass, static_cast<uint32_t>(prim));
        if (length(a) < 1e-10f) continue;
        const float along = std::fabs(dot(normalize(a), normal));
        if (glassOfPrim(*glass, prim) == 2) {
            ++cracks;
            CHECK(along < 1e-3f);
        } else {
            CHECK(along < 1e-3f || along > 1.0f - 1e-3f);  // a face of it, or its edge
        }
    }
    CHECK(cracks > 100);
    // The rays meet where it was struck: the smallest shards are round it.
    const auto P = glass->positions();
    float nearest = 1e30f;
    for (const std::vector<uint32_t>& prims : primsOfPieces(*glass)) {
        Vec3 sum;
        int n = 0;
        for (const uint32_t prim : prims) {
            for (const uint32_t p : glass->primitivePoints(prim)) {
                sum += P[p];
                ++n;
            }
        }
        nearest = std::min(nearest, length(sum * (1.0f / static_cast<float>(n)) - center));
    }
    CHECK(nearest < 0.05f);
}

TEST(glass_fracture_is_the_same_every_time_and_on_any_number_of_threads) {
    auto hashOf = [](int seed) {
        return pane(Vec3(1.0f, 1.0f, 0.006f), Vec3(0.0f, 1.0f, 0.0f), [seed](pg::Node& n) {
                   n.setVec3("impact", Vec3(0.2f, 1.1f, 0.0f));
                   n.setInt("seed", seed);
               })->hash();
    };
    const unsigned saved = TaskPool::instance().threadCount();
    TaskPool::instance().setThreadCount(1);
    const uint64_t one = hashOf(1);
    TaskPool::instance().setThreadCount(4);
    const uint64_t four = hashOf(1), again = hashOf(1);
    const uint64_t other = hashOf(2);
    TaskPool::instance().setThreadCount(saved);
    CHECK_EQ(one, four);
    CHECK_EQ(four, again);
    CHECK(other != one);
}

TEST(glass_is_drawn_apart_from_the_rest_and_its_chips_below_zero) {
    Geometry geo = *pane(Vec3(0.4f, 0.4f, 0.006f), Vec3(0.0f, 1.0f, 0.0f), [](pg::Node& n) {
        n.setVec3("impact", Vec3(0.0f, 1.0f, 0.0f));
        n.setInt("radials", 6);
        n.setInt("rings", 3);
    });
    const size_t glassTriangles = [&] {
        size_t t = 0;
        for (size_t prim = 0; prim < geo.primitiveCount(); ++prim) t += geo.primitivePoints(prim).size() - 2;
        return t;
    }();
    // A box beside it, not of glass -- its glass 0, as a Merge fills it.
    registerBuiltinNodes();
    {
        Graph g;
        pg::Node* box = g.create("box", "box");
        box->setVec3("center", Vec3(2.0f, 0.5f, 0.0f));
        CookEngine engine;
        geo.append(*engine.cook(*box, CookContext{}));
    }
    // Two loose points, chips: one of glass.
    const size_t first = geo.addPoints(2);
    auto P = geo.positionsForWrite();
    P[first] = Vec3(0.0f, 0.1f, 0.0f);
    P[first + 1] = Vec3(0.5f, 0.1f, 0.0f);
    auto pscale = geo.points().create("pscale", AttrType::Float).write<float>();
    pscale[first] = pscale[first + 1] = 0.02f;
    auto chips = geo.points().create("glass", AttrType::Int).write<int32_t>();
    chips[first] = 1;

    const DisplayGeometry d = displayOf(geo);
    CHECK_EQ(d.triangleCount(), size_t(12));  // the box's
    CHECK_EQ(d.glassCount(), glassTriangles);
    std::set<float> kinds;
    for (size_t t = 0; t < d.glassCount(); ++t) {
        const float* c = d.glass.data() + 39 * t;
        // Flat, of no normals of its own: each corner the face's own
        // normal, as its corners go round -- and the face's, which way a ray
        // comes into it.
        const Vec3 a(c[0], c[1], c[2]), b(c[13], c[14], c[15]), e(c[26], c[27], c[28]);
        const Vec3 n = normalize(cross(b - a, e - a));
        for (int k = 0; k < 3; ++k) {
            const float* v = c + 13 * k;
            CHECK(length(Vec3(v[3], v[4], v[5]) - n) < 1e-3f);
            CHECK(Vec3(v[6], v[7], v[8]) == Vec3(0.82f, 0.9f, 0.88f));
            CHECK_EQ(v[9], c[9]);
            CHECK(length(Vec3(v[10], v[11], v[12]) - n) < 1e-3f);
        }
        kinds.insert(c[9]);
    }
    CHECK(kinds == std::set<float>({1.0f, 2.0f}));
    CHECK_EQ(d.dotCount(), size_t(2));
    CHECK_NEAR(d.dots[6], -0.02f, 1e-6f);      // the chip of glass
    CHECK_NEAR(d.dots[7 + 6], 0.02f, 1e-6f);   // the other
    // Its box takes the glass in.
    CHECK(d.lo.y <= 0.8f + 1e-4f && d.hi.y >= 1.2f - 1e-4f);
}

TEST(glass_with_normals_of_its_own_is_smooth_and_faces_as_its_corners_go_round) {
    // Two faces of glass folded along a ridge, the points' N between them
    // -- as a round piece of glass has them.
    Geometry geo;
    geo.addPoints(6);
    {
        auto P = geo.positionsForWrite();
        const Vec3 at[6] = {{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.5f, 0.3f, 1.0f},
                            {0.5f, 0.3f, 0.0f}, {1.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f}};
        for (size_t i = 0; i < 6; ++i) P[i] = at[i];
    }
    const uint32_t left[4] = {0, 1, 2, 3}, right[4] = {3, 2, 4, 5};
    geo.addPrimitive(left, true);
    geo.addPrimitive(right, true);
    auto glass = geo.primitives().create("glass", AttrType::Int).write<int32_t>();
    glass[0] = glass[1] = 1;
    const Vec3 up(0.0f, 1.0f, 0.0f), outLeft = normalize(Vec3(-0.3f, 0.5f, 0.0f)), outRight = normalize(Vec3(0.3f, 0.5f, 0.0f));
    const std::vector<Vec3> N = {outLeft, outLeft, up, up, outRight, outRight};
    {
        auto w = geo.points().create("N", AttrType::Vec3).write<Vec3>();
        for (size_t i = 0; i < 6; ++i) w[i] = N[i] * 2.0f;  // not a unit long
    }

    // Each face as its corners go round, out of the solid; a corner shaded
    // by the N it is given -- that of its point -- or, given none, flat.
    auto faceOf = [](const Vec3& a, const Vec3& b, const Vec3& c) { return normalize(cross(b - a, c - a)); };
    auto pointAt = [&](const Vec3& p) {
        size_t point = 0;
        for (size_t i = 0; i < 6; ++i) {
            if (length(geo.positions()[i] - p) < 1e-6f) point = i;
        }
        return point;
    };
    auto shadedAs = [&](const Geometry& g, const std::vector<Vec3>& given) {
        bool ok = true;
        const DisplayGeometry d = displayOf(g);
        ok = ok && d.glassCount() == 4;
        for (size_t t = 0; t < d.glassCount(); ++t) {
            const float* c = d.glass.data() + 39 * t;
            const Vec3 face = faceOf(Vec3(c[0], c[1], c[2]), Vec3(c[13], c[14], c[15]), Vec3(c[26], c[27], c[28]));
            for (int k = 0; k < 3; ++k) {
                const float* v = c + 13 * k;
                const Vec3 shade = given.empty() ? face : given[pointAt(Vec3(v[0], v[1], v[2]))];
                ok = ok && length(normalize(Vec3(v[3], v[4], v[5])) - shade) < 1e-4f;
                ok = ok && length(Vec3(v[10], v[11], v[12]) - face) < 1e-4f;
            }
        }
        // ... and as the renderers have it.
        const ShadedTriangles s = shadedTriangles(g);
        ok = ok && s.count() == 4;
        for (size_t t = 0; t < s.count(); ++t) {
            ok = ok && s.glass[t] == 1;
            const Vec3 face = faceOf(s.positions[3 * t], s.positions[3 * t + 1], s.positions[3 * t + 2]);
            for (size_t k = 0; k < 3; ++k) {
                const Vec3 shade = given.empty() ? face : given[pointAt(s.positions[3 * t + k])];
                ok = ok && length(s.normals[3 * t + k] - shade) < 1e-4f;
            }
        }
        return ok;
    };
    // Smooth by its points' N.
    CHECK(shadedAs(geo, N));
    // ... by its corners' -- before the points'.
    Geometry corners = geo;
    {
        auto w = corners.vertices().create("N", AttrType::Vec3).write<Vec3>();
        for (size_t c = 0; c < w.size(); ++c) w[c] = N[corners.vertexPoint(c)];
        auto other = corners.points().find("N")->write<Vec3>();
        for (size_t i = 0; i < other.size(); ++i) other[i] = Vec3(1.0f, 0.0f, 0.0f);
    }
    CHECK(shadedAs(corners, N));
    // Of none: flat, each face its own.
    Geometry flat = geo;
    flat.points().erase("N");
    CHECK(shadedAs(flat, {}));
}

TEST(glass_is_whole_until_it_breaks) {
    const GeometryPtr glass = pane(Vec3(0.4f, 0.4f, 0.006f), Vec3(0.0f, 1.0f, 0.0f), [](pg::Node& n) {
        n.setVec3("impact", Vec3(0.05f, 1.0f, 0.0f));
        n.setInt("radials", 6);
        n.setInt("rings", 3);
    });
    const size_t cracks = cracksIn(*glass);
    CHECK(cracks > 10);
    RigidFrame f;
    f.pieces = glass;
    f.attribute = "piece";
    f.layout = rigidLayout(*glass, "piece");
    const size_t bodies = static_cast<size_t>(f.layout->bodies);
    CHECK(bodies > 5);
    // At rest -- no poses -- every face is there.
    CHECK(wholePanes(f).empty());
    CHECK_EQ(cracksIn(*posedPieces(f)), cracks);
    // Moved, turned, all together: whole, and not a crack to be seen.
    f.poses.resize(bodies);
    for (RigidPose& p : f.poses) {
        p.position = Vec3(0.3f, -0.2f, 0.1f);
        p.rotation = Vec4(0.0f, 0.38268343f, 0.0f, 0.92387953f);
    }
    std::vector<uint8_t> whole = wholePanes(f);
    CHECK_EQ(whole.size(), bodies);
    CHECK(std::all_of(whole.begin(), whole.end(), [](uint8_t w) { return w == 1; }));
    const GeometryPtr intact = posedPieces(f);
    CHECK_EQ(cracksIn(*intact), size_t(0));
    CHECK_EQ(intact->primitiveCount(), glass->primitiveCount() - cracks);
    // One shard a millimetre off the rest: the pane has broken -- every crack there.
    f.poses[bodies / 2].position.y -= 0.001f;
    whole = wholePanes(f);
    CHECK(std::none_of(whole.begin(), whole.end(), [](uint8_t w) { return w == 1; }));
    CHECK_EQ(cracksIn(*posedPieces(f)), cracks);
    // ... and so when one is gone.
    f.poses[bodies / 2] = f.poses[0];
    CHECK_EQ(cracksIn(*posedPieces(f)), size_t(0));
    f.vanished = {1};
    CHECK_EQ(cracksIn(*posedPieces(f)), cracks - [&] {
        size_t c = 0;
        for (const uint32_t prim : f.layout->prims[1]) c += glassOfPrim(*glass, prim) == 2;
        return c;
    }());

    // A shard alone is no pane: the faces the fracture cut are its edges.
    const std::vector<uint32_t>& own = f.layout->prims[0];
    std::vector<uint8_t> keep(glass->primitiveCount(), 0);
    for (const uint32_t prim : own) keep[prim] = 1;
    auto alone = std::make_shared<Geometry>(*glass);
    alone->deletePrimitives(keep, true);
    RigidFrame one;
    one.pieces = alone;
    one.attribute = "piece";
    one.poses.resize(1);
    CHECK_EQ(wholePanes(one), std::vector<uint8_t>({0}));
    CHECK(cracksIn(*posedPieces(one)) > 0);
    CHECK_EQ(cracksIn(*posedPieces(one)), cracksIn(*alone));
}

TEST(glass_breaks_into_chips_of_glass_and_little_dust) {
    CHECK(rigidAvailable());
    // A pane dropped flat onto the floor: whole as it falls, broken as it
    // lands -- and the same of stone.
    auto dropped = [](bool glassy, float& dust, RigidFrame& falling, RigidFrame& landed) {
        GeometryPtr pieces = pane(Vec3(0.5f, 0.5f, 0.01f), Vec3(0.0f, 0.6f, 0.0f), [](pg::Node& n) {
            n.setVec3("impact", Vec3(0.05f, 0.6f, 0.0f));
            n.setInt("radials", 7);
            n.setInt("rings", 3);
        }, Vec3(90.0f, 0.0f, 0.0f));
        if (!glassy) {
            auto stone = std::make_shared<Geometry>(*pieces);
            stone->primitives().erase("glass");
            pieces = stone;
        }
        RigidScene scene;
        scene.pieces = pieces;
        scene.solver.density = 2500.0f;
        scene.solver.glue = 30e3f;  // pascals: 30 kPa
        scene.solver.substeps = 4;
        // Little dust a break: the puffs it makes stay below what one holds.
        scene.solver.dust = scene.solver.impactDust = 0.1f;
        RigidSolver solver(scene);
        dust = 0.0f;
        for (int i = 0; i < 40; ++i) {
            solver.step();
            if (i == 5) falling = solver.capture();
            for (const RigidDust& d : solver.dust()) dust += d.amount;
        }
        landed = solver.capture();
    };
    float glassDust = 0.0f, stoneDust = 0.0f;
    RigidFrame glassFalling, glassLanded, stoneFalling, stoneLanded;
    dropped(true, glassDust, glassFalling, glassLanded);
    dropped(false, stoneDust, stoneFalling, stoneLanded);
    // Falling, the pane is whole; landed, broken.
    CHECK_EQ(cracksIn(*posedPieces(glassFalling)), size_t(0));
    CHECK(glassLanded.broken > 0);
    CHECK(cracksIn(*posedPieces(glassLanded)) > 0);
    // Its grit is glass; the stone's is not.
    CHECK(!glassLanded.debris.empty());
    CHECK_EQ(glassLanded.debrisGlass.size(), glassLanded.debris.size() / 4);
    CHECK(std::all_of(glassLanded.debrisGlass.begin(), glassLanded.debrisGlass.end(), [](uint8_t g) { return g == 1; }));
    CHECK(stoneLanded.debrisGlass.empty());
    // Glass breaks clean: far less dust than stone.
    CHECK(stoneDust > 0.0f);
    CHECK(glassDust < 0.3f * stoneDust);
    // Drawn: chips of glass, of the glass's colour.
    const GeometryPtr drawn = drawnPieces(glassLanded, Vec3(0.5f), Vec3(0.4f), "inside");
    const AttributeArray* chips = drawn->points().find("glass");
    CHECK(chips != nullptr);
    if (chips) {
        const auto g = chips->read<int32_t>();
        CHECK_EQ(static_cast<size_t>(std::count(g.begin(), g.end(), 1)), glassLanded.debris.size() / 4);
    }
    const DisplayGeometry d = displayOf(*drawn);
    CHECK(d.glassCount() > 0);
    size_t below = 0;
    for (size_t i = 0; i < d.dotCount(); ++i) below += d.dots[7 * i + 6] < 0.0f;
    CHECK_EQ(below, d.dotCount());
}

TEST(glass_fracture_in_a_network) {
    // The node in a network, and the example that breaks a window with it.
    Network net;
    CHECK(Network::example("glass_window", net));
    Compiled c = net.compile(PG_SIM_EXAMPLES_DIR);
    CHECK(c.ok);
    CHECK(c.world.hasRigid);
    const Geometry& pieces = *c.world.rigid.pieces;
    CHECK(pieces.primitives().find("glass") != nullptr);
    CHECK(cracksIn(pieces) > 100);
    // The frame is no glass.
    const auto piece = pieces.primitives().find("piece")->read<int32_t>();
    for (size_t prim = 0; prim < pieces.primitiveCount(); ++prim) {
        if (piece[prim] == 1000) CHECK_EQ(glassOfPrim(pieces, prim), 0);
    }
}
