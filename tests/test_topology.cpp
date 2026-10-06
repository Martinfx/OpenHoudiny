//
// The nodes that change the mesh itself (src/pg/nodes/Topology.cpp):
// Connectivity, Fuse, PolyExtrude, Subdivide, Clip, Attribute Transfer; and
// For-Each loops (src/pg/sim/ForEach.h), which run nodes piece by piece.
//
#include "pg/core/CookEngine.h"
#include "pg/core/Dissolve.h"
#include "pg/core/Graph.h"
#include "pg/core/Parallel.h"
#include "pg/nodes/Nodes.h"
#include "pg/sim/Asset.h"
#include "pg/sim/ForEach.h"
#include "pg/sim/GeometryGraph.h"
#include "pg/sim/Network.h"

#include "subdivision_cases.h"
#include "test_framework.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <span>
#include <utility>

using namespace pg;

namespace {

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

/// The volume a closed surface holds (the divergence theorem over its fans).
float volumeOf(const Geometry& geo) {
    const auto P = geo.positions();
    double v = 0.0;
    for (size_t prim = 0; prim < geo.primitiveCount(); ++prim) {
        const auto f = geo.primitivePoints(prim);
        for (size_t i = 1; i + 1 < f.size(); ++i) {
            const Vec3 &a = P[f[0]], &b = P[f[i]], &c = P[f[i + 1]];
            v += static_cast<double>(dot(a, cross(b, c))) / 6.0;
        }
    }
    return static_cast<float>(v);
}

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

GeometryPtr cookBox(CookEngine& engine, Graph& g, int divisions = 1) {
    Node* box = g.create("box", "box" + std::to_string(divisions));
    box->setInt("divisions", divisions);
    box->setVec3("size", Vec3(1.0f, 1.0f, 1.0f));
    box->setVec3("center", Vec3(0.0f, 0.5f, 0.0f));  // on the floor
    return engine.cook(*box, CookContext{});
}

/// A node of type `type` fed `in`, cooked.
GeometryPtr run(const std::string& type, std::vector<GeometryPtr> in, const std::function<void(Node&)>& set = {}) {
    registerBuiltinNodes();
    struct Given : Node {
        GeometryPtr g;
        explicit Given(GeometryPtr geo) : Node("given", "given"), g(std::move(geo)) { setInputCount(0); }
        GeometryPtr cookNode(const CookContext&, std::span<const GeometryPtr>) override { return g; }
    };
    std::vector<std::unique_ptr<Given>> sources;
    auto node = NodeRegistry::instance().create(type, "probe");
    for (size_t i = 0; i < in.size(); ++i) {
        sources.push_back(std::make_unique<Given>(in[i]));
        node->setInput(i, sources.back().get());
    }
    if (set) set(*node);
    CookEngine engine;
    return engine.cook(*node, CookContext{});
}

}  // namespace

TEST(topology_connectivity_numbers_the_pieces) {
    Graph g;
    CookEngine engine;
    auto two = std::make_shared<Geometry>(*cookBox(engine, g));
    two->append(*cookBox(engine, g, 2));
    GeometryPtr out = run("connectivity", {two});
    const auto cls = out->primitives().find("class")->read<int32_t>();
    CHECK_EQ(cls.size(), 6u + 24u);
    for (size_t p = 0; p < 6; ++p) CHECK_EQ(cls[p], 0);
    for (size_t p = 6; p < cls.size(); ++p) CHECK_EQ(cls[p], 1);
    out = run("connectivity", {two}, [](Node& n) {
        n.setInt("class", 1);
        n.setString("attribute", "piece");
    });
    const auto pts = out->points().find("piece")->read<int32_t>();
    CHECK_EQ(pts[0], 0);
    CHECK_EQ(pts[pts.size() - 1], 1);
}

TEST(topology_fuse_welds_what_is_near) {
    // Two quads side by side with their shared edge's points twice, and a
    // triangle two of whose corners are one point.
    auto geo = std::make_shared<Geometry>();
    geo->addPoints(11);
    auto P = geo->positionsForWrite();
    const Vec3 at[11] = {{0, 0, 0}, {1, 0, 0}, {1, 0, 1}, {0, 0, 1},       // quad a
                         {1, 0, 0}, {2, 0, 0}, {2, 0, 1}, {1, 0.0005f, 1},  // quad b, its left edge a's right
                         {5, 0, 0}, {6, 0, 0}, {5, 0, 0.0001f}};            // a triangle that folds
    for (size_t i = 0; i < 11; ++i) P[i] = at[i];
    const uint32_t a[4] = {0, 3, 2, 1}, b[4] = {4, 7, 6, 5}, t[3] = {8, 9, 10};
    geo->addPrimitive(a);
    geo->addPrimitive(b);
    geo->addPrimitive(t);
    auto cd = geo->points().create("Cd", AttrType::Vec3).write<Vec3>();
    cd[1] = Vec3(1.0f, 0.0f, 0.0f);
    GeometryPtr out = run("fuse", {geo}, [](Node& n) { n.setFloat("distance", 0.001f); });
    CHECK_EQ(out->pointCount(), 8u);  // two welded on the seam, one in the triangle
    CHECK_EQ(out->primitiveCount(), 2u);  // the triangle folded to a line: gone
    // The seam's points at their middle; the first's attributes kept.
    CHECK(std::fabs(out->positions()[2].y - 0.00025f) < 1e-6f);
    CHECK_EQ(out->points().find("Cd")->read<Vec3>()[1].x, 1.0f);
    // The quads share an edge now.
    const auto qa = out->primitivePoints(0), qb = out->primitivePoints(1);
    int shared = 0;
    for (const uint32_t x : qa) {
        for (const uint32_t y : qb) shared += x == y;
    }
    CHECK_EQ(shared, 2);
}

TEST(topology_polyextrude_pushes_faces_out) {
    Graph g;
    CookEngine engine;
    const GeometryPtr box = cookBox(engine, g);
    // Every face of the box, out by 0.25 and inset 0.1: six blocks on it.
    GeometryPtr out = run("polyextrude", {box}, [](Node& n) {
        n.setFloat("distance", 0.25f);
        n.setFloat("inset", 0.1f);
    });
    CHECK_EQ(out->primitiveCount(), 6u * 5u);
    CHECK_EQ(out->pointCount(), 8u + 24u);
    CHECK_EQ(out->findGroup("extrudeFront")->memberCount(), 6u);
    CHECK_EQ(out->findGroup("extrudeSide")->memberCount(), 24u);
    Vec3 lo, hi;
    bounds(*out, lo, hi);
    CHECK(std::fabs(hi.y - 1.25f) < 1e-5f && std::fabs(lo.x + 0.75f) < 1e-5f);
    // The top's front: 0.8 wide, 0.25 up.
    const GeometryPtr top = out;
    const Group* front = top->findGroup("extrudeFront");
    for (size_t p = 0; p < top->primitiveCount(); ++p) {
        if (!front->contains(p)) continue;
        Vec3 flo(1e9f, 1e9f, 1e9f), fhi(-1e9f, -1e9f, -1e9f);
        for (const uint32_t q : top->primitivePoints(p)) {
            for (int a = 0; a < 3; ++a) {
                flo[a] = std::min(flo[a], top->positions()[q][a]);
                fhi[a] = std::max(fhi[a], top->positions()[q][a]);
            }
        }
        if (fhi.y > 1.2f && flo.y > 1.2f) CHECK(std::fabs((fhi.x - flo.x) - 0.8f) < 1e-4f);
    }
    // One face, its back kept: a closed block, turned out.
    auto quad = std::make_shared<Geometry>();
    quad->addPoints(4);
    auto P = quad->positionsForWrite();
    P[0] = Vec3(0, 0, 0);
    P[1] = Vec3(0, 0, 1);
    P[2] = Vec3(1, 0, 1);
    P[3] = Vec3(1, 0, 0);  // anticlockwise seen from +y
    const uint32_t f[4] = {0, 1, 2, 3};
    quad->addPrimitive(f);
    out = run("polyextrude", {quad}, [](Node& n) {
        n.setFloat("distance", 0.5f);
        n.setBool("outputback", true);
    });
    CHECK_EQ(out->primitiveCount(), 6u);
    CHECK(watertight(*out));
    CHECK(std::fabs(volumeOf(*out) - 0.5f) < 1e-4f);
    // Inward (a window): walls that face the hole, the volume the other way.
    out = run("polyextrude", {quad}, [](Node& n) {
        n.setFloat("distance", -0.5f);
        n.setBool("outputback", true);
    });
    CHECK(watertight(*out));
    CHECK(std::fabs(volumeOf(*out) + 0.5f) < 1e-4f);
}

TEST(topology_subdivide_rounds_a_box_and_keeps_a_grid_flat) {
    Graph g;
    CookEngine engine;
    const GeometryPtr box = cookBox(engine, g);
    GeometryPtr once = run("subdivide", {box});
    CHECK_EQ(once->primitiveCount(), 24u);
    CHECK_EQ(once->pointCount(), 8u + 12u + 6u);
    CHECK(watertight(*once));
    GeometryPtr twice = run("subdivide", {box}, [](Node& n) { n.setInt("iterations", 2); });
    CHECK_EQ(twice->primitiveCount(), 96u);
    CHECK(watertight(*twice));
    // Rounder: less than the box holds, inside it, still around its middle.
    const float v0 = volumeOf(*box), v2 = volumeOf(*twice);
    CHECK(v2 > 0.3f * v0 && v2 < 0.8f * v0);
    Vec3 lo, hi;
    bounds(*twice, lo, hi);
    CHECK(lo.x > -0.5f && hi.x < 0.5f && lo.y > 0.0f && hi.y < 1.0f);
    CHECK(std::fabs(lo.x + hi.x) < 1e-5f);
    // A grid: its corners stay, its edges stay on its sides, it stays flat.
    Node* grid = g.create("grid", "grid");
    grid->setInt("rows", 3);
    grid->setInt("cols", 3);
    const GeometryPtr flat = engine.cook(*grid, CookContext{});
    GeometryPtr fine = run("subdivide", {flat}, [](Node& n) { n.setInt("iterations", 2); });
    Vec3 flo, fhi, glo, ghi;
    bounds(*flat, glo, ghi);
    bounds(*fine, flo, fhi);
    CHECK(std::fabs(flo.x - glo.x) < 1e-5f && std::fabs(fhi.z - ghi.z) < 1e-5f);
    CHECK(std::fabs(fhi.y) < 1e-6f && std::fabs(flo.y) < 1e-6f);
    CHECK_EQ(fine->primitiveCount(), 4u * 16u);
}

namespace {

/// A case's mesh: the sharpness of its edges on their corners
/// (f@creaseweight, each the edge to the next corner) and of its points
/// (f@cornerweight); "edge only" in i@subd 2.
Geometry cageOf(const opensubdiv::Case& c) {
    Geometry geo;
    geo.addPoints(c.points.size());
    std::copy(c.points.begin(), c.points.end(), geo.positionsForWrite().begin());
    for (const auto& f : c.faces) geo.addPrimitive(f, true);
    std::map<std::pair<uint32_t, uint32_t>, float> sharp;
    for (const auto& [a, b, s] : c.edges) sharp[std::minmax(a, b)] = s;
    auto crease = geo.vertices().create("creaseweight", AttrType::Float).write<float>();
    for (size_t prim = 0; prim < geo.primitiveCount(); ++prim) {
        const auto f = geo.primitivePoints(prim);
        for (size_t i = 0; i < f.size(); ++i) {
            const auto it = sharp.find(std::minmax(f[i], f[(i + 1) % f.size()]));
            crease[geo.primitiveVertexStart(prim) + i] = it == sharp.end() ? 0.0f : it->second;
        }
    }
    auto corner = geo.points().create("cornerweight", AttrType::Float).write<float>();
    for (const auto& [i, s] : c.corners) corner[i] = s;
    auto subd = geo.primitives().create("subd", AttrType::Int).write<int32_t>();
    std::fill(subd.begin(), subd.end(), c.edgeOnly ? 2 : 1);
    return geo;
}

}  // namespace

TEST(topology_subdivide_is_as_opensubdiv_has_it_with_sharp_edges_and_points) {
    const std::vector<opensubdiv::Case> cases = opensubdiv::cases();
    CHECK_EQ(cases.size(), 7u);
    for (const opensubdiv::Case& c : cases) {
        const GeometryPtr out = subdivideGeometry(cageOf(c), c.levels, "subd");
        const auto P = out->positions();
        CHECK_EQ(P.size(), c.result.size());
        const float off = std::max(opensubdiv::furthestFrom(c.result, P), opensubdiv::furthestFrom(P, c.result));
        if (off > 2e-5f) std::printf("  %s: %g off\n", c.name.c_str(), off);
        CHECK(off < 2e-5f);
    }
}

TEST(topology_subdivide_carries_the_sharpness_on_and_only_what_it_is_asked) {
    Graph g;
    CookEngine engine;
    const GeometryPtr box = cookBox(engine, g);
    // Smooth, a corner of the box comes to 5/9 of the way out from the middle.
    const GeometryPtr round = subdivideGeometry(*box, 1);
    CHECK(std::fabs(std::fabs(round->positions()[0].x) - 0.5f * 5.0f / 9.0f) < 1e-6f);
    CHECK(std::fabs(std::fabs(round->positions()[0].y - 0.5f) - 0.5f * 5.0f / 9.0f) < 1e-6f);
    // Every edge infinitely sharp: a box stays a box, its edges so.
    Geometry sharp = *box;
    auto crease = sharp.vertices().create("creaseweight", AttrType::Float).write<float>();
    std::fill(crease.begin(), crease.end(), 10.0f);
    const GeometryPtr still = subdivideGeometry(sharp, 2);
    Vec3 lo, hi;
    bounds(*still, lo, hi);
    CHECK(std::fabs(lo.x + 0.5f) < 1e-6f && std::fabs(hi.y - 1.0f) < 1e-6f);
    CHECK(std::fabs(volumeOf(*still) - volumeOf(*box)) < 1e-5f);
    // The edges along the box's sharp still, those across its faces smooth.
    const auto edges = still->vertices().find("creaseweight")->read<float>();
    const auto P = still->positions();
    auto alongTheBox = [](const Vec3& p) {
        const int ends = (std::fabs(std::fabs(p.x) - 0.5f) < 1e-5f ? 1 : 0) +
                         (std::fabs(std::fabs(p.y - 0.5f) - 0.5f) < 1e-5f ? 1 : 0) +
                         (std::fabs(std::fabs(p.z) - 0.5f) < 1e-5f ? 1 : 0);
        return ends >= 2;
    };
    size_t along = 0;
    for (size_t prim = 0; prim < still->primitiveCount(); ++prim) {
        const auto f = still->primitivePoints(prim);
        for (size_t i = 0; i < 4; ++i) {
            const bool sharpEdge = edges[still->primitiveVertexStart(prim) + i] == 10.0f;
            CHECK_EQ(sharpEdge, alongTheBox(0.5f * (P[f[i]] + P[f[(i + 1) % 4]])));
            along += sharpEdge ? 1 : 0;
        }
    }
    CHECK_EQ(along, 12u * 4u * 2u);  // each edge of the box in four, from both sides
    // As sharp as 2.5: 1.5 a step on, 0.5 the next.
    std::fill(crease.begin(), crease.end(), 2.5f);
    const GeometryPtr once = subdivideGeometry(sharp, 1), twice = subdivideGeometry(sharp, 2);
    CHECK(once->vertices().find("creaseweight")->read<float>()[0] == 1.5f);
    CHECK(twice->vertices().find("creaseweight")->read<float>()[0] == 0.5f);
    // Only the faces asked for: the others as they were, the box closed.
    Geometry half = *box;
    auto subd = half.primitives().create("subd", AttrType::Int).write<int32_t>();
    std::fill(subd.begin(), subd.end(), 0);
    subd[0] = subd[1] = 1;
    const GeometryPtr part = subdivideGeometry(half, 1, "subd");
    CHECK_EQ(part->primitiveCount(), 4u + 4u + 4u);
    CHECK_EQ(part->primitiveVertexCount(11), 4u);
    const GeometryPtr none = subdivideGeometry(half, 1, "none such");
    CHECK_EQ(none->primitiveCount(), 24u);
}

TEST(topology_subdivision_normals_are_smooth_but_across_sharp_edges) {
    Graph g;
    CookEngine engine;
    Geometry box = *cookBox(engine, g);
    // Smooth: the corners at a point face alike, out of the box's middle.
    subdivisionNormals(box);
    const auto N = box.vertices().find("N")->read<Vec3>();
    const auto P = box.positions();
    for (size_t prim = 0; prim < box.primitiveCount(); ++prim) {
        for (size_t i = 0; i < 4; ++i) {
            const size_t v = box.primitiveVertexStart(prim) + i;
            const Vec3 out = normalize(P[box.primitivePoints(prim)[i]] - Vec3(0.0f, 0.5f, 0.0f));
            CHECK(dot(N[v], out) > 0.999f);
        }
    }
    // Every edge as sharp as 1: each face its own way.
    auto crease = box.vertices().create("creaseweight", AttrType::Float).write<float>();
    std::fill(crease.begin(), crease.end(), 1.0f);
    subdivisionNormals(box);
    const auto flat = box.vertices().find("N")->read<Vec3>();
    for (size_t prim = 0; prim < box.primitiveCount(); ++prim) {
        const Vec3 face = normalize(polygonNormal(box, box.primitivePoints(prim)));
        for (size_t i = 0; i < 4; ++i) CHECK(dot(flat[box.primitiveVertexStart(prim) + i], face) > 0.9999f);
    }
}

TEST(topology_clip_cuts_and_closes) {
    Graph g;
    CookEngine engine;
    const GeometryPtr box = cookBox(engine, g, 2);
    GeometryPtr top = run("clip", {box}, [](Node& n) {
        n.setVec3("origin", Vec3(0.0f, 0.3f, 0.0f));
        n.setVec3("dir", Vec3(0.0f, 1.0f, 0.0f));
    });
    Vec3 lo, hi;
    bounds(*top, lo, hi);
    CHECK(std::fabs(lo.y - 0.3f) < 1e-5f && std::fabs(hi.y - 1.0f) < 1e-5f);
    CHECK(watertight(*top));
    CHECK(std::fabs(volumeOf(*top) - 0.7f) < 1e-4f);
    CHECK(top->findGroup("cut")->memberCount() >= 1u);
    // Below, slanted: what is left and what was cut make the whole.
    auto slant = [&](int keep) {
        return run("clip", {box}, [keep](Node& n) {
            n.setVec3("origin", Vec3(0.1f, 0.5f, 0.0f));
            n.setVec3("dir", Vec3(1.0f, 1.0f, 0.3f));
            n.setInt("keep", keep);
        });
    };
    const GeometryPtr above = slant(0), below = slant(1);
    CHECK(watertight(*above) && watertight(*below));
    CHECK(std::fabs(volumeOf(*above) + volumeOf(*below) - 1.0f) < 1e-4f);
    // A sphere cut through the middle: a hemisphere, closed.
    Node* ball = g.create("sphere", "ball");
    ball->setFloat("radius", 0.5f);
    ball->setVec3("center", Vec3(0.0f, 0.5f, 0.0f));
    const GeometryPtr sphere = engine.cook(*ball, CookContext{});
    const GeometryPtr half = run("clip", {sphere}, [](Node& n) { n.setVec3("origin", Vec3(0.0f, 0.5f, 0.0f)); });
    CHECK(watertight(*half));
    CHECK(std::fabs(volumeOf(*half) * 2.0f - volumeOf(*sphere)) < 0.01f * volumeOf(*sphere));
    // Without the cap: open where it was cut.
    const GeometryPtr open = run("clip", {sphere}, [](Node& n) {
        n.setVec3("origin", Vec3(0.0f, 0.5f, 0.0f));
        n.setBool("cap", false);
    });
    CHECK(!watertight(*open));
    CHECK_EQ(open->primitiveCount() + 1, half->primitiveCount());
    // A concave cut: an L of two boxes, capped by triangles that cover it.
    auto ell = std::make_shared<Geometry>(*box);
    Node* side = g.create("box", "side");
    side->setVec3("size", Vec3(1.0f, 1.0f, 1.0f));
    side->setVec3("center", Vec3(1.0f, 0.5f, 0.0f));
    ell->append(*engine.cook(*side, CookContext{}));
    const GeometryPtr cutEll = run("clip", {ell}, [](Node& n) { n.setVec3("origin", Vec3(0.0f, 0.25f, 0.0f)); });
    CHECK(std::fabs(volumeOf(*cutEll) - 1.5f) < 1e-4f);
    // A box with a hollow in it, cut through the hollow: the cap a ring,
    // a face with a hole.
    auto hollow = std::make_shared<Geometry>(*box);
    Node* core = g.create("box", "core");
    core->setVec3("size", Vec3(0.5f, 0.5f, 0.5f));
    core->setVec3("center", Vec3(0.0f, 0.5f, 0.0f));
    const GeometryPtr inner = engine.cook(*core, CookContext{});
    {
        // Turned inside out: its faces round the other way.
        Geometry turned;
        turned.addPoints(inner->pointCount());
        auto tp = turned.positionsForWrite();
        for (size_t i = 0; i < inner->pointCount(); ++i) tp[i] = inner->positions()[i];
        for (size_t prim = 0; prim < inner->primitiveCount(); ++prim) {
            std::vector<uint32_t> f(inner->primitivePoints(prim).begin(), inner->primitivePoints(prim).end());
            std::reverse(f.begin(), f.end());
            turned.addPrimitive(f);
        }
        hollow->append(turned);
    }
    CHECK(std::fabs(volumeOf(*hollow) - (1.0f - 0.125f)) < 1e-5f);
    const GeometryPtr ring = run("clip", {hollow}, [](Node& n) { n.setVec3("origin", Vec3(0.0f, 0.6f, 0.0f)); });
    CHECK(watertight(*ring));
    CHECK(std::fabs(volumeOf(*ring) - (0.4f - 0.25f * 0.15f)) < 1e-4f);
}

TEST(topology_attribute_transfer_colours_what_is_near) {
    Graph g;
    CookEngine engine;
    Node* grid = g.create("grid", "grid");
    grid->setInt("rows", 11);
    grid->setInt("cols", 11);
    const GeometryPtr target = engine.cook(*grid, CookContext{});
    auto source = std::make_shared<Geometry>();
    source->addPoints(1);
    source->positionsForWrite()[0] = Vec3(0.0f, 0.0f, 0.0f);
    source->points().create("Cd", AttrType::Vec3).write<Vec3>()[0] = Vec3(1.0f, 0.0f, 0.0f);
    source->points().create("id", AttrType::Int).write<int32_t>()[0] = 7;
    GeometryPtr out = run("attribtransfer", {target, source}, [](Node& n) {
        n.setString("attributes", "Cd id");
        n.setFloat("distance", 0.35f);
        n.setFloat("blend", 0.3f);
    });
    const auto P = out->positions();
    const auto cd = out->points().find("Cd")->read<Vec3>();
    const auto id = out->points().find("id")->read<int32_t>();
    for (size_t i = 0; i < P.size(); ++i) {
        const float d = length(P[i]);
        if (d <= 0.35f) CHECK(std::fabs(cd[i].x - 1.0f) < 1e-5f && id[i] == 7);
        else if (d >= 0.65f) CHECK(cd[i].x == 0.0f && id[i] == 0);
        else CHECK(cd[i].x > 0.0f && cd[i].x < 1.0f);
    }
}

// --- For-Each loops ----------------------------------------------------------------------------

namespace {

/// Three boxes on a line, numbered by Connectivity, into a loop whose body
/// lifts each by its iteration: nodes box, line, copies, pieces, begin,
/// lift, end.
sim::Network loopOverPieces(std::map<std::string, int>& id) {
    sim::Network net;
    id["box"] = net.add("box");
    id["line"] = net.add("line");
    id["copies"] = net.add("copy_to_points");
    id["pieces"] = net.add("connectivity");
    id["begin"] = net.add("foreach_begin");
    id["lift"] = net.add("point_wrangle");
    id["end"] = net.add("foreach_end");
    std::string error;
    net.setParam(id["line"], "points", sim::ParamValue{3.0f, 0.0f, 0.0f});
    net.setParam(id["line"], "length", sim::ParamValue{4.0f, 0.0f, 0.0f});
    net.connect(id["box"], "geometry", id["copies"], "geometry", &error);
    net.connect(id["line"], "geometry", id["copies"], "points", &error);
    net.connect(id["copies"], "geometry", id["pieces"], "geometry", &error);
    net.connect(id["pieces"], "geometry", id["begin"], "geometry", &error);
    net.connect(id["begin"], "geometry", id["lift"], "geometry", &error);
    net.connect(id["lift"], "geometry", id["end"], "geometry", &error);
    net.setText(id["lift"], "snippet", "@P.y += detail(0, \"iteration\") + 10 * (detail(0, \"numiterations\") - 3);");
    net.setDisplay(id["end"]);
    return net;
}

float highest(const GeometryPtr& g) {
    float y = -1e9f;
    for (const Vec3& p : g->positions()) y = std::max(y, p.y);
    return y;
}

}  // namespace

TEST(topology_foreach_runs_the_body_once_a_piece) {
    std::map<std::string, int> id;
    sim::Network net = loopOverPieces(id);
    sim::GeometryGraph geo;
    geo.sync(net);
    GeometryPtr out = geo.cook(id["end"], 1);
    CHECK_EQ(geo.error(id["end"]), std::string());
    CHECK_EQ(out->pointCount(), 24u);
    CHECK_EQ(out->primitiveCount(), 18u);
    CHECK(std::fabs(highest(out) - 3.0f) < 1e-5f);  // the third box, 2 up
    CHECK(out->detail().find("iteration") == nullptr);
    // Each box by itself: the one at x = 4 lifted by 2.
    for (const Vec3& p : out->positions()) {
        if (p.x > 3.0f) CHECK(p.y >= 2.0f - 1e-5f);
        if (p.x < 1.0f) CHECK(p.y <= 1.0f + 1e-5f);
    }
    // Cooked alone, the Begin gives the first piece; the body shows it.
    CHECK_EQ(geo.cook(id["begin"], 1)->pointCount(), 8u);
    CHECK(std::fabs(highest(geo.cook(id["lift"], 1)) - 1.0f) < 1e-5f);

    // The same on one thread and on four.
    const uint64_t many = out->hash();
    const unsigned threads = TaskPool::instance().threadCount();
    TaskPool::instance().setThreadCount(1);
    sim::GeometryGraph single;
    single.sync(net);
    CHECK_EQ(single.cook(id["end"], 1)->hash(), many);
    TaskPool::instance().setThreadCount(threads);

    // Count: the whole, three times, each lifted by its number.
    net.setParam(id["begin"], "method", sim::ParamValue{3.0f, 0.0f, 0.0f});
    net.setParam(id["begin"], "count", sim::ParamValue{3.0f, 0.0f, 0.0f});
    geo.sync(net);
    out = geo.cook(id["end"], 1);
    CHECK_EQ(out->pointCount(), 3u * 24u);
    CHECK(std::fabs(highest(out) - 3.0f) < 1e-5f);
    // Primitives: each face of each box on its own.
    net.setParam(id["begin"], "method", sim::ParamValue{1.0f, 0.0f, 0.0f});
    geo.sync(net);
    out = geo.cook(id["end"], 1);
    CHECK_EQ(out->primitiveCount(), 18u);
    CHECK_EQ(out->pointCount(), 18u * 4u);
}

TEST(topology_foreach_feedback_and_errors) {
    sim::Network net;
    const int box = net.add("box");
    const int begin = net.add("foreach_begin");
    const int move = net.add("transform");
    const int end = net.add("foreach_end");
    std::string error;
    net.connect(box, "geometry", begin, "geometry", &error);
    net.connect(begin, "geometry", move, "geometry", &error);
    net.connect(move, "geometry", end, "geometry", &error);
    net.setParam(begin, "method", sim::ParamValue{4.0f, 0.0f, 0.0f});  // feedback
    net.setParam(begin, "count", sim::ParamValue{5.0f, 0.0f, 0.0f});
    net.setParam(move, "t", sim::ParamValue{0.0f, 1.0f, 0.0f});
    sim::GeometryGraph geo;
    geo.sync(net);
    GeometryPtr out = geo.cook(end, 1);
    CHECK_EQ(geo.error(end), std::string());
    CHECK_EQ(out->pointCount(), 8u);
    CHECK(std::fabs(highest(out) - 6.0f) < 1e-5f);  // up one, five times

    // What goes wrong is said on the End.
    net.setParam(begin, "method", sim::ParamValue{0.0f, 0.0f, 0.0f});  // pieces, but no class
    geo.sync(net);
    geo.cook(end, 1);
    CHECK(geo.error(end).find("Connectivity") != std::string::npos);
    geo.cook(begin, 1);
    CHECK(geo.error(begin).find("class") != std::string::npos);
    sim::Network lone;
    const int b2 = lone.add("box");
    const int e2 = lone.add("foreach_end");
    lone.connect(b2, "geometry", e2, "geometry", &error);
    sim::GeometryGraph g2;
    g2.sync(lone);
    g2.cook(e2, 1);
    CHECK(g2.error(e2).find("no For-Each Begin") != std::string::npos);
    // Begin named that is not one.
    lone.setText(e2, "begin", "box1");
    g2.sync(lone);
    g2.cook(e2, 1);
    CHECK(g2.error(e2).find("box1") != std::string::npos);
}

TEST(topology_foreach_follows_changes_in_its_body_and_input) {
    std::map<std::string, int> id;
    sim::Network net = loopOverPieces(id);
    sim::GeometryGraph geo;
    geo.sync(net);
    CHECK(std::fabs(highest(geo.cook(id["end"], 1)) - 3.0f) < 1e-5f);
    // The body changed: cooked again.
    net.setText(id["lift"], "snippet", "@P.y += 2 * detail(0, \"iteration\");");
    geo.sync(net);
    CHECK(std::fabs(highest(geo.cook(id["end"], 1)) - 5.0f) < 1e-5f);
    // What comes in changed: four boxes now.
    net.setParam(id["line"], "points", sim::ParamValue{4.0f, 0.0f, 0.0f});
    geo.sync(net);
    CHECK(std::fabs(highest(geo.cook(id["end"], 1)) - 7.0f) < 1e-5f);
    // Nothing changed: nothing cooks.
    const uint64_t before = geo.coreNode(id["end"])->cookCount();
    geo.sync(net);
    geo.cook(id["end"], 1);
    CHECK_EQ(geo.coreNode(id["end"])->cookCount(), before);
    // The file keeps it.
    const std::string text = net.save();
    sim::Network back;
    std::string error;
    CHECK(sim::Network::load(text, back, error));
    CHECK_EQ(back.save(), text);
}

// --- cooking on a thread of its own ----------------------------------------------------------------

#include "pg/sim/Cooker.h"

#include <chrono>
#include <thread>

TEST(cooker_cooks_on_its_own_thread_and_gives_up_what_is_not_wanted) {
    sim::Network net;
    const int box = net.add("box");
    const int slow = net.add("detail_wrangle");
    std::string error;
    net.connect(box, "geometry", slow, "geometry", &error);
    net.setText(slow, "snippet", "int n = chi(\"n\"); float s = 0; for (int i = 0; i < n; i++) s += sin(i); @sum = s;");
    net.setParam(slow, "n", sim::ParamValue{300000000.0f, 0.0f, 0.0f});  // seconds of work
    net.setDisplay(slow);
    sim::Cooker cooker;
    auto ask = [&](const sim::Network& n) {
        sim::Cooker::Request r;
        r.levels.push_back({std::make_shared<const sim::Network>(n), "", 0});
        r.nodes = {slow, box};
        return cooker.submit(std::move(r));
    };
    const auto start = std::chrono::steady_clock::now();
    ask(net);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));  // under way
    net.setParam(slow, "n", sim::ParamValue{10.0f, 0.0f, 0.0f});
    const uint64_t wanted = ask(net);
    cooker.wait();
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    CHECK(seconds < 3.0);  // the long one was given up
    CHECK(!cooker.busy());
    sim::Cooker::Result r;
    CHECK(cooker.take(r));
    CHECK_EQ(r.serial, wanted);
    CHECK(!cooker.take(r));  // one result, the last
    CHECK(r.geometry.count(slow) && r.geometry.count(box));
    const float sum = r.geometry[slow]->detail().find("sum")->read<float>()[0];
    float expected = 0.0f;
    for (int i = 0; i < 10; ++i) expected += std::sin(static_cast<float>(i));
    CHECK(std::fabs(sum - expected) < 1e-4f);
    CHECK(r.errors.empty());

    // What went wrong is said, per node.
    net.setText(slow, "snippet", "@sum = nosuch(1);");
    ask(net);
    cooker.wait();
    CHECK(cooker.take(r));
    CHECK(r.errors.count(slow) && r.errors[slow].find("nosuch") != std::string::npos);

    // An asset's inside, with the instance's inputs.
    sim::Network def;
    const int in = def.add("asset_input");
    const int move = def.add("transform");
    def.connect(in, "geometry", move, "geometry", &error);
    def.setParam(move, "t", sim::ParamValue{0.0f, 5.0f, 0.0f});
    def.setDisplay(move);
    sim::AssetInfo info;
    info.name = "cooker_lift";
    def.setAsset(info);
    CHECK(sim::AssetLibrary::instance().add(def, "", error));
    sim::Network scene;
    const int b = scene.add("box");
    const int inst = scene.add("cooker_lift");
    scene.connect(b, "geometry", inst, "geometry", &error);
    sim::Cooker::Request inside;
    inside.levels.push_back({std::make_shared<const sim::Network>(scene), "", inst});
    inside.levels.push_back({std::make_shared<const sim::Network>(def), "", 0});
    inside.nodes = {move};
    cooker.submit(std::move(inside));
    cooker.wait();
    CHECK(cooker.take(r));
    Vec3 lo, hi;
    bounds(*r.geometry[move], lo, hi);
    CHECK(std::fabs(lo.y - 5.0f) < 1e-5f && std::fabs(hi.y - 6.0f) < 1e-5f);  // the box, from the scene, lifted
}

// --- Voronoi Fracture --------------------------------------------------------------------------------

TEST(fracture_cuts_a_solid_into_closed_pieces_that_make_the_whole) {
    Graph g;
    CookEngine engine;
    const GeometryPtr box = cookBox(engine, g, 2);
    const float whole = volumeOf(*box);
    GeometryPtr pieces = run("voronoifracture", {box}, [](Node& n) { n.setInt("count", 12); });
    const auto piece = pieces->primitives().find("piece")->read<int32_t>();
    int count = 0;
    for (const int32_t p : piece) count = std::max(count, p + 1);
    CHECK_EQ(count, 12);
    CHECK(pieces->findGroup("inside")->memberCount() > 0u);
    // Each piece closed, all of them the box.
    float sum = 0.0f;
    for (int k = 0; k < count; ++k) {
        std::vector<uint8_t> keep(piece.size());
        for (size_t p = 0; p < piece.size(); ++p) keep[p] = piece[p] == k;
        auto one = std::make_shared<Geometry>(*pieces);
        one->deletePrimitives(keep, true);
        CHECK(watertight(*one));
        const float v = volumeOf(*one);
        CHECK(v > 0.0f);
        sum += v;
    }
    CHECK(std::fabs(sum - whole) < 1e-4f);
    // The points carry it too.
    CHECK(pieces->points().find("piece") != nullptr);

    // The same on one thread and on four; another seed, other pieces.
    const unsigned saved = TaskPool::instance().threadCount();
    TaskPool::instance().setThreadCount(1);
    const uint64_t one = run("voronoifracture", {box}, [](Node& n) { n.setInt("count", 12); })->hash();
    TaskPool::instance().setThreadCount(4);
    const uint64_t four = run("voronoifracture", {box}, [](Node& n) { n.setInt("count", 12); })->hash();
    TaskPool::instance().setThreadCount(saved);
    CHECK_EQ(one, four);
    CHECK(run("voronoifracture", {box}, [](Node& n) {
              n.setInt("count", 12);
              n.setInt("seed", 2);
          })->hash() != one);

    // Points given: a cell each, those outside it none.
    auto seeds = std::make_shared<Geometry>();
    seeds->addPoints(3);
    auto S = seeds->positionsForWrite();
    S[0] = Vec3(-0.25f, 0.5f, 0.0f);
    S[1] = Vec3(0.25f, 0.5f, 0.0f);
    S[2] = Vec3(5.0f, 0.5f, 0.0f);  // outside: its cell holds nothing of the box
    pieces = run("voronoifracture", {box, seeds});
    int given = 0;
    for (const int32_t p : pieces->primitives().find("piece")->read<int32_t>()) given = std::max(given, p + 1);
    CHECK_EQ(given, 2);
    CHECK(std::fabs(volumeOf(*pieces) - whole) < 1e-4f);
}

namespace {

/// A closed box from `lo` to `hi`: eight points, six quads turned out.
Geometry block(Vec3 lo, Vec3 hi) {
    Geometry g;
    g.addPoints(8);
    const auto P = g.positionsForWrite();
    for (uint32_t k = 0; k < 8; ++k) P[k] = Vec3(k & 1 ? hi.x : lo.x, k & 2 ? hi.y : lo.y, k & 4 ? hi.z : lo.z);
    const uint32_t faces[6][4] = {{0, 2, 3, 1}, {4, 5, 7, 6}, {0, 1, 5, 4}, {2, 6, 7, 3}, {0, 4, 6, 2}, {1, 3, 7, 5}};
    for (const auto& f : faces) g.addPrimitive(std::span<const uint32_t>(f, 4));
    return g;
}

/// Seed i's cell as it was made before: all of the mesh, cut by the plane
/// half way to every other seed, nearest first.
std::shared_ptr<Geometry> wholeCell(const GeometryPtr& mesh, const std::vector<Vec3>& seeds, size_t i) {
    std::vector<size_t> others(seeds.size());
    for (size_t k = 0; k < others.size(); ++k) others[k] = k;
    others.erase(others.begin() + static_cast<long>(i));
    const Vec3 s = seeds[i];
    std::stable_sort(others.begin(), others.end(),
                     [&](size_t a, size_t b) { return length(seeds[a] - s) < length(seeds[b] - s); });
    std::shared_ptr<Geometry> piece = std::make_shared<Geometry>(*mesh);
    for (const size_t j : others) {
        const Vec3 dir = s - seeds[j];
        if (length(dir) < 1e-7f) continue;
        const Vec3 origin = (s + seeds[j]) * 0.5f;
        const Vec3 n = normalize(dir);
        float least = 1e30f;
        for (const Vec3& p : piece->positions()) least = std::min(least, dot(p - origin, n));
        if (least >= 0.0f) continue;
        piece = clipGeometry(*piece, origin, n, true, "inside");
        if (piece->primitiveCount() == 0) break;
    }
    return piece;
}

}  // namespace

TEST(fracture_cuts_each_cell_out_of_the_parts_near_it_as_out_of_the_whole) {
    // Blocks side by side and apart -- a mesh of many parts, as a building
    // of slabs and walls is -- and seeds in and between them, two on one
    // place. Each cell, cut out of the parts near it by the seeds near it,
    // is what cutting all of the mesh by every seed made of it: to the bit.
    auto mesh = std::make_shared<Geometry>();
    for (int k = 0; k < 8; ++k) {
        const Vec3 lo(static_cast<float>(k % 4) * 1.0f + (k >= 4 ? 0.3f : 0.0f), static_cast<float>(k / 4) * 0.8f,
                      (k % 2) * 0.25f);
        mesh->append(block(lo, lo + Vec3(k % 3 == 2 ? 1.0f : 0.9f, 0.8f, 0.5f)));
    }
    std::vector<Vec3> seeds;
    uint64_t state = 7;
    auto unit = [&]() {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<float>(state >> 40) / static_cast<float>(1ull << 24);
    };
    for (int k = 0; k < 60; ++k) seeds.push_back(Vec3(unit() * 4.4f - 0.1f, unit() * 1.7f - 0.05f, unit() * 0.8f - 0.05f));
    seeds.push_back(seeds[5]);  // two on one place: the first keeps it
    const GeometryPtr whole = mesh;
    const VoronoiCells cells(whole, seeds);
    int made = 0;
    for (size_t i = 0; i < seeds.size(); ++i) {
        const std::shared_ptr<Geometry> fast = cells.cell(i, "inside");
        const std::shared_ptr<Geometry> slow = wholeCell(whole, seeds, i);
        CHECK_EQ(fast->primitiveCount(), slow->primitiveCount());
        CHECK_EQ(fast->hash(), slow->hash());
        if (fast->primitiveCount() > 0) ++made;
    }
    CHECK(made > 40);
    // One part alone -- a box: the same.
    const GeometryPtr one = std::make_shared<Geometry>(block(Vec3(0.0f), Vec3(1.0f)));
    const std::vector<Vec3> few = {Vec3(0.2f, 0.3f, 0.4f), Vec3(0.7f, 0.6f, 0.5f), Vec3(0.5f, 0.1f, 0.9f)};
    const VoronoiCells boxCells(one, few);
    for (size_t i = 0; i < few.size(); ++i) CHECK_EQ(boxCells.cell(i, "inside")->hash(), wholeCell(one, few, i)->hash());
}

TEST(fracture_breaks_the_building) {
    sim::AssetLibrary::instance().loadDefaults();
    const auto def = sim::AssetLibrary::instance().find("building");
    CHECK(def != nullptr);
    if (!def) return;
    sim::GeometryGraph inside;
    inside.sync(*def->net);
    const GeometryPtr body = inside.cook(def->net->named("paint")->id, 1);
    const GeometryPtr pieces = run("voronoifracture", {body}, [](Node& n) { n.setInt("count", 24); });
    CHECK(std::fabs(volumeOf(*pieces) - volumeOf(*body)) < 1e-3f * volumeOf(*body));
    int count = 0;
    for (const int32_t p : pieces->primitives().find("piece")->read<int32_t>()) count = std::max(count, p + 1);
    CHECK_EQ(count, 24);
    CHECK(pieces->primitives().find("Cd") != nullptr);  // the walls' colours go with them
}


namespace {

/// A grid of `nx` by `nz` quads a metre each, facing up: point x + (nx + 1) z;
/// each quad's id its number, each corner's uv where it is.
Geometry quads(int nx, int nz) {
    Geometry geo;
    geo.addPoints(static_cast<size_t>((nx + 1) * (nz + 1)));
    auto P = geo.positionsForWrite();
    for (int z = 0; z <= nz; ++z) {
        for (int x = 0; x <= nx; ++x) P[static_cast<size_t>(x + (nx + 1) * z)] = Vec3(static_cast<float>(x), 0.0f, static_cast<float>(z));
    }
    for (int z = 0; z < nz; ++z) {
        for (int x = 0; x < nx; ++x) {
            const uint32_t a = static_cast<uint32_t>(x + (nx + 1) * z);
            const uint32_t q[4] = {a, a + static_cast<uint32_t>(nx + 1), a + static_cast<uint32_t>(nx + 2), a + 1};
            geo.addPrimitive(q, true);
        }
    }
    auto id = geo.primitives().create("id", AttrType::Int).write<int32_t>();
    for (size_t p = 0; p < geo.primitiveCount(); ++p) id[p] = static_cast<int32_t>(p);
    auto uv = geo.vertices().create("uv", AttrType::Vec3).write<Vec3>();
    for (size_t v = 0; v < geo.vertexCount(); ++v) {
        const Vec3 p = geo.positions()[geo.vertexPoint(v)];
        uv[v] = Vec3(p.x / static_cast<float>(nx), p.z / static_cast<float>(nz), 0.0f);
    }
    return geo;
}

float areaOf(const Geometry& geo, size_t prim) {
    const auto P = geo.positions();
    const auto f = geo.primitivePoints(prim);
    Vec3 sum(0.0f);
    for (size_t i = 1; i + 1 < f.size(); ++i) sum += cross(P[f[i]] - P[f[0]], P[f[i + 1]] - P[f[0]]);
    return 0.5f * length(sum);
}

}  // namespace

TEST(topology_dissolve_makes_the_faces_an_edge_parts_one) {
    // Two quads of a 3 x 3 grid made one: the point left inline on the
    // border goes, the one where other quads meet stays; the polygon keeps
    // the lower quad's id, each corner its uv.
    const Geometry grid = quads(3, 3);
    DissolveCount count;
    const Geometry out = dissolveEdges(grid, std::vector<Edge>{{1, 5}}, DissolveSettings{}, &count);
    CHECK_EQ(count.polygons, size_t(1));
    CHECK_EQ(count.merged, size_t(2));
    CHECK_EQ(out.primitiveCount(), size_t(8));
    CHECK_EQ(out.pointCount(), size_t(15));
    const size_t last = out.primitiveCount() - 1;
    CHECK_EQ(out.primitiveVertexCount(last), size_t(5));
    CHECK(std::fabs(areaOf(out, last) - 2.0f) < 1e-5f);
    CHECK_EQ(out.primitives().find("id")->read<int32_t>()[last], 0);
    const auto uv = out.vertices().find("uv")->read<Vec3>();
    for (size_t v = out.primitiveVertexStart(last); v < out.vertexCount(); ++v) {
        const Vec3 p = out.positions()[out.vertexPoint(v)];
        CHECK(length(uv[v] - Vec3(p.x / 3.0f, p.z / 3.0f, 0.0f)) < 1e-6f);
    }
    // Without taking the inline point out: it stays a corner.
    DissolveSettings keep;
    keep.inlinePoints = false;
    const Geometry kept = dissolveEdges(grid, std::vector<Edge>{{1, 5}}, keep);
    CHECK_EQ(kept.pointCount(), size_t(16));
    CHECK_EQ(kept.primitiveVertexCount(kept.primitiveCount() - 1), size_t(6));
}

TEST(topology_dissolve_a_block_into_one_face_but_not_a_ring_round_a_hole) {
    // A 2 x 2 block, its four inner edges: one quad -- the middle point,
    // used by nothing now, and the ones inline on its sides gone.
    const Geometry block = quads(2, 2);
    const Geometry one = dissolveEdges(block, edgesOf(block).size() ? std::vector<Edge>{{1, 4}, {3, 4}, {4, 5}, {4, 7}} : std::vector<Edge>{});
    CHECK_EQ(one.primitiveCount(), size_t(1));
    CHECK_EQ(one.pointCount(), size_t(4));
    CHECK_EQ(one.primitiveVertexCount(0), size_t(4));
    CHECK(std::fabs(areaOf(one, 0) - 4.0f) < 1e-5f);

    // A 3 x 3 grid: the eight round the middle quad joined by the edges
    // between them -- a ring round a hole, not one polygon: they stay.
    const Geometry grid = quads(3, 3);
    const std::vector<Edge> ring = {{1, 5}, {2, 6}, {4, 5}, {6, 7}, {8, 9}, {10, 11}, {9, 13}, {10, 14}};
    DissolveCount count;
    const Geometry same = dissolveEdges(grid, ring, DissolveSettings{}, &count);
    CHECK_EQ(same.primitiveCount(), size_t(9));
    CHECK(count.kept >= 1);
    CHECK_EQ(count.polygons, size_t(0));

    // A border edge, a side of one quad only: nothing to dissolve.
    const Geometry border = dissolveEdges(grid, std::vector<Edge>{{0, 1}}, DissolveSettings{}, &count);
    CHECK_EQ(border.primitiveCount(), size_t(9));
    CHECK_EQ(count.kept, size_t(1));
}

TEST(topology_dissolve_node_takes_edges_or_faces) {
    // Edges as the viewport writes them; faces picked: the sides they share.
    auto grid = std::make_shared<Geometry>(quads(3, 3));
    const GeometryPtr byEdges = run("dissolve", {grid}, [](Node& n) { n.setString("group", "p1-5 p5-9"); });
    CHECK(byEdges && byEdges->primitiveCount() == 7);
    const GeometryPtr byFaces = run("dissolve", {grid}, [](Node& n) {
        n.setString("group", "0 1 3 4");
        n.setInt("class", 1);
    });
    CHECK(byFaces && byFaces->primitiveCount() == 6);
    // The four faces a square: one quad of 4 m^2.
    const size_t last = byFaces->primitiveCount() - 1;
    CHECK(std::fabs(areaOf(*byFaces, last) - 4.0f) < 1e-5f);
    // Wound unlike: two quads sharing a side the same way -- not one.
    Geometry odd;
    odd.addPoints(6);
    {
        auto P = odd.positionsForWrite();
        for (int i = 0; i < 6; ++i) P[static_cast<size_t>(i)] = Vec3(static_cast<float>(i % 3), 0.0f, static_cast<float>(i / 3));
        const uint32_t a[4] = {0, 3, 4, 1}, b[4] = {1, 4, 5, 2};
        odd.addPrimitive(a, true);
        // The second turned the other way round.
        const uint32_t c[4] = {1, 2, 5, 4};
        (void)b;
        odd.addPrimitive(c, true);
    }
    DissolveCount count;
    const Geometry still = dissolveEdges(odd, std::vector<Edge>{{1, 4}}, DissolveSettings{}, &count);
    CHECK_EQ(still.primitiveCount(), size_t(2));
}
