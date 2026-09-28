//
// Concrete Fracture (src/pg/nodes/Concrete.cpp): closed pieces that make
// the whole as they are and as their proxy has them, the rough faces of
// each crack the same on both sides, spalls small pieces of their own,
// the smallest pieces round the impact, the same on any thread count; and
// the RBD Solver with them (src/pg/sim/Rigid.h): simulated as the proxy
// has them -- glued where the plain cuts meet, standing as built -- drawn
// rough, and knocked apart no further than Spread and Rings let a knock go.
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
#include <numeric>
#include <unordered_map>

using namespace pg;
using namespace pg::sim;

namespace {

/// A box of `size` at `center`, broken by a Concrete Fracture that `setup`
/// sets up -- and, when `after` names a node type, that node after it.
GeometryPtr concrete(Vec3 size, Vec3 center, const std::function<void(pg::Node&)>& setup,
                     const std::string& after = "", const std::function<void(pg::Node&)>& afterSetup = {}) {
    registerBuiltinNodes();
    Graph g;
    pg::Node* box = g.create("box", "box");
    box->setVec3("size", size);
    box->setVec3("center", center);
    pg::Node* fracture = g.create("concretefracture", "concrete");
    fracture->setInput(0, box);
    setup(*fracture);
    pg::Node* last = fracture;
    if (!after.empty()) {
        last = g.create(after, "after");
        last->setInput(0, fracture);
        if (afterSetup) afterSetup(*last);
    }
    CookEngine engine;
    return engine.cook(*last, CookContext{});
}

/// Every edge of a closed surface is shared by two faces, turned opposite ways.
bool watertight(const Geometry& geo) {
    std::map<std::pair<uint32_t, uint32_t>, int> edges;
    for (size_t prim = 0; prim < geo.primitiveCount(); ++prim) {
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

/// The volume the primitives `prims` of `geo` hold with their points at `P`.
double volumeOf(const Geometry& geo, std::span<const Vec3> P, const std::vector<uint32_t>& prims) {
    double v = 0.0;
    for (const uint32_t prim : prims) {
        const auto f = geo.primitivePoints(prim);
        for (size_t i = 1; i + 1 < f.size(); ++i) v += static_cast<double>(dot(P[f[0]], cross(P[f[i]], P[f[i + 1]]))) / 6.0;
    }
    return v;
}

double areaOf(const Geometry& geo, std::span<const Vec3> P, uint32_t prim) {
    const auto f = geo.primitivePoints(prim);
    Vec3 sum;
    for (size_t i = 1; i + 1 < f.size(); ++i) sum += cross(P[f[i]] - P[f[0]], P[f[i + 1]] - P[f[0]]);
    return 0.5 * static_cast<double>(length(sum));
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

/// Points that lie within `tolerance` of each other get one number.
std::vector<uint32_t> welded(std::span<const Vec3> P, float tolerance) {
    std::vector<uint32_t> parent(P.size());
    std::iota(parent.begin(), parent.end(), 0u);
    auto find = [&](uint32_t i) {
        while (parent[i] != i) i = parent[i] = parent[parent[i]];
        return i;
    };
    const float cell = tolerance * 16.0f;
    auto key = [&](int x, int y, int z) {
        return (static_cast<uint64_t>(static_cast<uint32_t>(x) & 0x1FFFFFu) << 42) |
               (static_cast<uint64_t>(static_cast<uint32_t>(y) & 0x1FFFFFu) << 21) |
               static_cast<uint64_t>(static_cast<uint32_t>(z) & 0x1FFFFFu);
    };
    std::unordered_map<uint64_t, std::vector<uint32_t>> grid;
    for (uint32_t i = 0; i < P.size(); ++i) {
        const int x = static_cast<int>(std::floor(P[i].x / cell)), y = static_cast<int>(std::floor(P[i].y / cell)),
                  z = static_cast<int>(std::floor(P[i].z / cell));
        for (int dx = -1; dx <= 1; ++dx) {
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dz = -1; dz <= 1; ++dz) {
                    const auto it = grid.find(key(x + dx, y + dy, z + dz));
                    if (it == grid.end()) continue;
                    for (const uint32_t j : it->second) {
                        if (length(P[i] - P[j]) <= tolerance) parent[find(i)] = find(j);
                    }
                }
            }
        }
        grid[key(x, y, z)].push_back(i);
    }
    std::vector<uint32_t> out(P.size());
    for (uint32_t i = 0; i < P.size(); ++i) out[i] = find(i);
    return out;
}

bool hasProxy(const Geometry& geo) {
    const AttributeArray* a = geo.points().find("proxy");
    return a && a->type() == AttrType::Vec3;
}

}  // namespace

TEST(concrete_pieces_are_closed_and_make_the_whole) {
    const GeometryPtr pieces = concrete(Vec3(3.0f, 2.0f, 0.3f), Vec3(0.0f, 1.0f, 0.0f), [](pg::Node& n) {
        n.setInt("count", 30);
        n.setVec3("impact", Vec3(0.5f, 1.2f, 0.0f));
        n.setFloat("focus", 0.6f);
        n.setFloat("chips", 0.3f);
    });
    CHECK(hasProxy(*pieces));
    CHECK(pieces->primitives().find("piece") != nullptr && pieces->points().find("piece") != nullptr);
    CHECK(pieces->primitives().find("chip") != nullptr);
    CHECK(pieces->findGroup("inside") != nullptr && pieces->findGroup("inside")->memberCount() > 0u);
    CHECK(pieces->findGroup("__spall") == nullptr);  // the node's own, gone
    CHECK(pieces->points().find("N") == nullptr);    // the renderer's to make

    // Each piece closed; together -- as they are and as their proxy has
    // them -- the box: the cracks' faces meet, whatever roughness they have.
    const auto P = pieces->positions();
    const auto proxy = pieces->points().find("proxy")->read<Vec3>();
    const auto byPiece = primsOfPieces(*pieces);
    CHECK(byPiece.size() > 30u);  // the spalls are pieces too
    double rough = 0.0, plain = 0.0;
    for (const std::vector<uint32_t>& prims : byPiece) {
        CHECK(!prims.empty());
        std::vector<uint8_t> keep(pieces->primitiveCount(), 0);
        for (const uint32_t prim : prims) keep[prim] = 1;
        Geometry one(*pieces);
        one.deletePrimitives(keep, true);
        CHECK(watertight(one));
        const double v = volumeOf(*pieces, P, prims), w = volumeOf(*pieces, proxy, prims);
        CHECK(v > 0.0 && w > 0.0);
        rough += v;
        plain += w;
    }
    CHECK(std::fabs(rough - 1.8) < 2e-4);
    CHECK(std::fabs(plain - 1.8) < 2e-4);

    // Nothing pokes out of the box; the rough faces moved, no further than
    // Rough, the outside not at all.
    float moved = 0.0f;
    for (size_t i = 0; i < P.size(); ++i) {
        CHECK(P[i].x >= -1.5f - 1e-5f && P[i].x <= 1.5f + 1e-5f);
        CHECK(P[i].y >= -1e-5f && P[i].y <= 2.0f + 1e-5f);
        CHECK(P[i].z >= -0.15f - 1e-5f && P[i].z <= 0.15f + 1e-5f);
        moved = std::max(moved, length(P[i] - proxy[i]));
    }
    CHECK(moved > 0.005f);
    CHECK(moved <= std::sqrt(3.0f) * 0.02f + 1e-5f);  // each way, along each axis
    const Group& inside = *pieces->findGroup("inside");
    for (size_t prim = 0; prim < pieces->primitiveCount(); ++prim) {
        if (inside.contains(prim)) continue;
        for (const uint32_t pt : pieces->primitivePoints(prim)) CHECK(P[pt] == proxy[pt]);
    }
}

TEST(concrete_cracks_are_the_same_on_both_sides) {
    // No spalls: every face of every crack is one of another piece, point
    // for point, turned the other way.
    const GeometryPtr pieces = concrete(Vec3(2.0f, 1.5f, 0.4f), Vec3(0.0f, 0.75f, 0.0f), [](pg::Node& n) {
        n.setInt("count", 20);
        n.setFloat("chips", 0.0f);
        n.setFloat("rough", 0.03f);
    });
    const auto P = pieces->positions();
    const std::vector<uint32_t> id = welded(P, 1e-5f);
    const auto piece = pieces->primitives().find("piece")->read<int32_t>();
    const Group& inside = *pieces->findGroup("inside");
    std::map<std::vector<uint32_t>, std::vector<std::pair<int32_t, uint32_t>>> faces;  // welded corners -> piece, prim
    size_t count = 0;
    for (size_t prim = 0; prim < pieces->primitiveCount(); ++prim) {
        if (!inside.contains(prim)) continue;
        std::vector<uint32_t> key;
        for (const uint32_t pt : pieces->primitivePoints(prim)) key.push_back(id[pt]);
        std::sort(key.begin(), key.end());
        faces[key].push_back({piece[prim], static_cast<uint32_t>(prim)});
        ++count;
    }
    CHECK(count > 1000u);  // made rough: many triangles
    size_t matched = 0;
    for (const auto& [key, owners] : faces) {
        if (owners.size() == 2 && owners[0].first != owners[1].first) {
            matched += 2;
            // Turned the other way: their normals opposite.
            const auto a = pieces->primitivePoints(owners[0].second);
            const auto b = pieces->primitivePoints(owners[1].second);
            const Vec3 na = cross(P[a[1]] - P[a[0]], P[a[2]] - P[a[0]]);
            const Vec3 nb = cross(P[b[1]] - P[b[0]], P[b[2]] - P[b[0]]);
            CHECK(dot(na, nb) < 0.0f);
        }
    }
    CHECK_EQ(matched, count);
    // And the rough faces are rough: not in one plane.
    float offPlane = 0.0f;
    const auto proxy = pieces->points().find("proxy")->read<Vec3>();
    for (size_t i = 0; i < P.size(); ++i) offPlane = std::max(offPlane, length(P[i] - proxy[i]));
    CHECK(offPlane > 0.01f);
}

TEST(concrete_spalls_are_small_pieces_of_their_own) {
    auto make = [](float chips) {
        return concrete(Vec3(2.0f, 1.0f, 0.5f), Vec3(0.0f, 0.5f, 0.0f), [chips](pg::Node& n) {
            n.setInt("count", 12);
            n.setFloat("chips", chips);
            n.setFloat("chipsize", 0.08f);
        });
    };
    // Without spalls: a piece a cell.
    CHECK_EQ(primsOfPieces(*make(0.0f)).size(), 12u);
    const GeometryPtr pieces = make(0.6f);
    const auto byPiece = primsOfPieces(*pieces);
    const auto chip = pieces->primitives().find("chip")->read<int32_t>();
    const Group& inside = *pieces->findGroup("inside");
    const auto P = pieces->positions();
    size_t spalls = 0;
    for (const std::vector<uint32_t>& prims : byPiece) {
        const int32_t first = chip[prims.front()];
        bool cut = false;
        Vec3 lo(1e30f, 1e30f, 1e30f), hi(-1e30f, -1e30f, -1e30f);
        for (const uint32_t prim : prims) {
            CHECK_EQ(chip[prim], first);  // the whole piece is a spall, or none of it
            cut = cut || inside.contains(prim);
            for (const uint32_t pt : pieces->primitivePoints(prim)) {
                for (int a = 0; a < 3; ++a) {
                    lo[a] = std::min(lo[a], P[pt][a]);
                    hi[a] = std::max(hi[a], P[pt][a]);
                }
            }
        }
        if (first != 1) continue;
        ++spalls;
        CHECK(cut);                                  // glued on by the face it came off
        CHECK(length(hi - lo) <= 3.0f * 0.08f + 1e-4f);  // a spall, not a slice
    }
    CHECK(spalls > 3u);
    CHECK_EQ(byPiece.size(), 12u + spalls);
}

TEST(concrete_is_smallest_round_the_impact_and_the_same_on_any_thread_count) {
    // A long wall struck near its left end: more pieces there than at the right.
    const GeometryPtr pieces = concrete(Vec3(4.0f, 1.0f, 0.3f), Vec3(0.0f, 0.5f, 0.0f), [](pg::Node& n) {
        n.setInt("count", 60);
        n.setFloat("uneven", 0.0f);
        n.setFloat("chips", 0.0f);
        n.setVec3("impact", Vec3(-1.5f, 0.5f, 0.0f));
        n.setFloat("focus", 1.0f);
        n.setFloat("reach", 0.5f);
    });
    const auto proxy = pieces->points().find("proxy")->read<Vec3>();
    int left = 0, right = 0;
    for (const std::vector<uint32_t>& prims : primsOfPieces(*pieces)) {
        Vec3 sum;
        int n = 0;
        for (const uint32_t prim : prims) {
            for (const uint32_t pt : pieces->primitivePoints(prim)) {
                sum += proxy[pt];
                ++n;
            }
        }
        const float x = sum.x / static_cast<float>(n);
        left += x < -1.0f;
        right += x > 1.0f;
    }
    CHECK(left >= 3 * std::max(right, 1));

    // The same on one thread and on four; another seed, other pieces.
    auto hashOf = [](int seed) {
        return concrete(Vec3(2.0f, 1.0f, 0.3f), Vec3(0.0f, 0.5f, 0.0f), [seed](pg::Node& n) {
                   n.setInt("count", 15);
                   n.setInt("seed", seed);
                   n.setFloat("chips", 0.4f);
               })->hash();
    };
    const unsigned saved = TaskPool::instance().threadCount();
    TaskPool::instance().setThreadCount(1);
    const uint64_t one = hashOf(1);
    TaskPool::instance().setThreadCount(4);
    const uint64_t four = hashOf(1);
    const uint64_t other = hashOf(2);
    TaskPool::instance().setThreadCount(saved);
    CHECK_EQ(one, four);
    CHECK(other != one);
}

TEST(concrete_proxy_goes_where_the_pieces_go) {
    // Transform moves the proxy with the points.
    auto setup = [](pg::Node& n) {
        n.setInt("count", 8);
        n.setFloat("chips", 0.3f);
    };
    const GeometryPtr before = concrete(Vec3(1.0f, 1.0f, 0.3f), Vec3(0.0f, 0.5f, 0.0f), setup);
    const GeometryPtr after = concrete(Vec3(1.0f, 1.0f, 0.3f), Vec3(0.0f, 0.5f, 0.0f), setup, "transform", [](pg::Node& t) {
        t.setVec3("t", Vec3(1.0f, 2.0f, 3.0f));
        t.setVec3("r", Vec3(0.0f, 90.0f, 0.0f));
    });
    const Mat4 m = Mat4::rotate(Vec3(0.0f, 90.0f, 0.0f)) * Mat4::translate(Vec3(1.0f, 2.0f, 3.0f));
    const auto a = before->points().find("proxy")->read<Vec3>();
    const auto b = after->points().find("proxy")->read<Vec3>();
    CHECK_EQ(a.size(), b.size());
    for (size_t i = 0; i < a.size(); ++i) CHECK(length(m.transformPoint(a[i]) - b[i]) < 1e-5f);

    // The solver's places: the proxy where there is one; merged with
    // pieces that had none (a proxy of 0 far from the origin), the points.
    std::vector<Vec3> places = rigidPositions(*before);
    for (size_t i = 0; i < a.size(); ++i) CHECK(places[i] == a[i]);
    Geometry merged(*before);
    registerBuiltinNodes();
    Graph g;
    pg::Node* box = g.create("box", "plain");
    box->setVec3("center", Vec3(5.0f, 0.5f, 0.0f));
    CookEngine engine;
    merged.append(*engine.cook(*box, CookContext{}));
    places = rigidPositions(merged);
    const auto P = merged.positions();
    for (size_t i = a.size(); i < P.size(); ++i) CHECK(places[i] == P[i]);
    CHECK(rigidPositions(*engine.cook(*box, CookContext{})).size() == 8u);  // no proxy at all: the points
}

TEST(rigid_concrete_stands_glued_as_its_proxy_and_is_drawn_rough) {
    CHECK(rigidAvailable());
    RigidScene scene;
    scene.pieces = concrete(Vec3(2.0f, 1.2f, 0.2f), Vec3(0.0f, 0.6f, 0.0f), [](pg::Node& n) {
        n.setInt("count", 16);
        n.setFloat("chips", 0.3f);
    });
    const Geometry& pieces = *scene.pieces;
    // Glued where the plain cuts meet: the faces of every crack, cut into
    // triangles, are found as one -- as much glued area as there are cracks.
    const auto layout = rigidLayout(pieces, "piece");
    double glued = 0.0;
    for (const RigidLayout::Contact& c : layout->contacts) glued += c.area;
    const auto proxy = pieces.points().find("proxy")->read<Vec3>();
    const Group& inside = *pieces.findGroup("inside");
    double cracks = 0.0;
    for (size_t prim = 0; prim < pieces.primitiveCount(); ++prim) {
        if (inside.contains(prim)) cracks += areaOf(pieces, proxy, static_cast<uint32_t>(prim));
    }
    CHECK(std::fabs(glued - 0.5 * cracks) < 0.02 * 0.5 * cracks);
    CHECK_EQ(static_cast<size_t>(layout->bodies), primsOfPieces(pieces).size());

    // As built, it stands: nothing breaks, nothing moves.
    RigidSolver solver(scene);
    CHECK(solver.error().empty());
    CHECK_EQ(solver.capture().joints, layout->contacts.size());
    for (int i = 0; i < 30; ++i) solver.step();
    const RigidFrame rest = solver.capture();
    CHECK_EQ(rest.broken, 0u);
    for (const RigidPose& p : rest.poses) CHECK(length(p.velocity) < 0.05f && length(p.position) < 0.01f);

    // Posed, the pieces are rough -- the proxy stays behind.
    const GeometryPtr posed = posedPieces(rest);
    CHECK(posed->points().find("proxy") == nullptr);
    const auto P = pieces.positions();
    const auto Q = posed->positions();
    float far = 0.0f;
    for (size_t i = 0; i < P.size(); ++i) far = std::max(far, length(P[i] - Q[i]));
    CHECK(far < 0.01f);
    // Drawn, the faces of the cracks keep their own points: the shading of
    // the outside does not bleed into them.
    const GeometryPtr drawn = drawnPieces(rest, Vec3(0.5f, 0.5f, 0.5f), Vec3(0.6f, 0.6f, 0.6f), "inside");
    const Group* cut = drawn->findGroup("inside");
    CHECK(cut != nullptr);
    std::vector<uint8_t> use(drawn->pointCount(), 0);
    for (size_t prim = 0; prim < drawn->primitiveCount(); ++prim) {
        const uint8_t mark = cut->contains(prim) ? 1 : 2;
        for (const uint32_t pt : drawn->primitivePoints(prim)) use[pt] |= mark;
    }
    CHECK(std::none_of(use.begin(), use.end(), [](uint8_t u) { return u == 3; }));
}

TEST(rigid_spread_and_rings_keep_a_knock_near_where_it_lands) {
    // A concrete wall on a foundation -- a piece of its own that does not
    // move, the wall's foot glued to it -- and a keyed ball through its
    // middle: an unstoppable knock. What it breaks spreads as far as
    // Spread and Rings let it.
    auto wall = std::make_shared<Geometry>(*concrete(Vec3(4.0f, 1.6f, 0.2f), Vec3(0.0f, 0.8f, 0.0f), [](pg::Node& n) {
        n.setInt("count", 50);
        n.setFloat("chips", 0.0f);
        n.setFloat("uneven", 0.0f);
    }));
    const size_t wallPoints = wall->pointCount();
    {
        auto active = wall->primitives().create("active", AttrType::Int).write<int32_t>();
        std::fill(active.begin(), active.end(), 1);
        registerBuiltinNodes();
        Graph g;
        pg::Node* box = g.create("box", "foundation");
        box->setVec3("size", Vec3(4.4f, 0.2f, 0.4f));
        box->setVec3("center", Vec3(0.0f, -0.1f, 0.0f));
        CookEngine engine;
        Geometry base = *engine.cook(*box, CookContext{});
        auto piece = base.primitives().create("piece", AttrType::Int).write<int32_t>();
        std::fill(piece.begin(), piece.end(), 1000);
        base.primitives().create("active", AttrType::Int);  // 0: it stays
        wall->append(base);
    }
    auto knock = [&](float spread, int rings, RigidFrame& after) {
        RigidScene scene;
        scene.pieces = wall;
        scene.solver.glue = 500000.0f;
        scene.solver.spread = spread;
        scene.solver.rings = rings;
        Collider ball;
        ball.shape = Shape::Sphere;
        ball.center = Vec3(0.0f, 0.8f, 2.0f);
        ball.size = Vec3(0.5f, 0.5f, 0.5f);
        ball.node = 3;
        scene.colliders.push_back(ball);
        RigidSolver solver(scene.sanitized());
        for (int i = 0; i < 20; ++i) {
            ball.center.z -= 6.0f / 30.0f;
            ball.velocity = Vec3(0.0f, 0.0f, -6.0f);
            solver.setColliders({ball});
            solver.step();
        }
        after = solver.capture();
        return after.broken;
    };
    // How far the ends of the wall went.
    auto ends = [&](const RigidFrame& f) {
        const GeometryPtr posed = posedPieces(f);
        const auto P = wall->positions();
        const auto Q = posed->positions();
        float far = 0.0f;
        for (size_t i = 0; i < wallPoints; ++i) {
            if (std::fabs(P[i].x) > 1.6f) far = std::max(far, length(P[i] - Q[i]));
        }
        return far;
    };
    RigidFrame all, one, little, none;
    const size_t everywhere = knock(0.5f, 0, all);
    const size_t ring = knock(0.5f, 1, one);
    const size_t weak = knock(0.05f, 0, little);
    const size_t nothing = knock(0.0f, 0, none);
    CHECK(everywhere > 0u);
    CHECK(ring > 0u && ring < everywhere);
    CHECK(weak > 0u && weak < everywhere);
    CHECK(nothing > 0u && nothing <= weak);
    // Half of it on and on: the whole wall comes down. One ring: a hole,
    // and the ends of the wall stand where they stood.
    CHECK(ends(all) > 0.1f);
    CHECK(ends(one) < 1e-4f);
    CHECK(ends(none) < 1e-4f);
    // Out of range, made safe.
    RigidScene odd;
    odd.solver.spread = 3.0f;
    odd.solver.rings = -2;
    CHECK_EQ(odd.sanitized().solver.spread, 1.0f);
    CHECK_EQ(odd.sanitized().solver.rings, 0);
    odd.solver.spread = std::nanf("");
    CHECK_EQ(odd.sanitized().solver.spread, RigidSettings().spread);
}

TEST(concrete_fracture_and_rings_in_a_network) {
    Network net;
    const int box = net.add("box");
    const int fracture = net.add("concrete_fracture");
    const int rbd = net.add("rbd_solver");
    const int output = net.add("output");
    CHECK(net.setParam(box, "center", "0 0.5 0"));
    CHECK(net.setParam(fracture, "count", "10"));
    CHECK(net.setParam(fracture, "chips", "0"));
    CHECK(net.connect(box, "geometry", fracture, "geometry"));
    CHECK(net.connect(fracture, "geometry", rbd, "pieces"));
    CHECK(net.connect(rbd, "look", output, "look"));
    CHECK(net.setParam(rbd, "spread", "0.25"));
    CHECK(net.setParam(rbd, "rings", "2"));
    const Compiled c = net.compile();
    CHECK(c.ok);
    CHECK(c.world.rigid.pieces != nullptr);
    CHECK(hasProxy(*c.world.rigid.pieces));
    CHECK_EQ(primsOfPieces(*c.world.rigid.pieces).size(), 10u);
    CHECK_EQ(c.world.rigid.solver.spread, 0.25f);
    CHECK_EQ(c.world.rigid.solver.rings, 2);
    for (const Problem& p : c.problems) CHECK(p.level != Problem::Level::Error);
    // Written and read back the same.
    const std::string text = net.save();
    Network again;
    std::string error;
    CHECK(Network::load(text, again, error));
    CHECK_EQ(again.save(), text);
}
