//
// Plants as they live: in the wind as springs (SwayingPlants) -- a stem
// driven by gusts as a damped spring is, by how near its own pace they come;
// a tree still in a steady wind, its branches flung by the trunk when it is
// not; the same whatever frames are asked for, in whatever order, on any
// number of threads; plants standing on points swaying whole. In a
// community by height (CanopyLight): a tall crown shading the short ones
// under it, shrubs as an understorey, shade bearers waiting. Among
// obstacles (TriangleTree): the nearest points and crossings as found by
// hand, a tree by a wall grown round it, trees as instances by one grown
// on their own.
//
#include "pg/core/CookEngine.h"
#include "pg/core/Ecosystem.h"
#include "pg/core/Graph.h"
#include "pg/core/Parallel.h"
#include "pg/core/Spatial.h"
#include "pg/core/Tree.h"
#include "pg/core/Wind.h"
#include "pg/nodes/Nodes.h"
#include "pg/sim/GeometryGraph.h"
#include "pg/sim/Network.h"

#include "test_framework.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <vector>

using namespace pg;

namespace {

constexpr float kPi = 3.14159265358979f;

/// A blade 2 m tall standing at the origin: a polyline, flex 0 to 1 up it.
Geometry blade() {
    Geometry geo;
    constexpr int n = 9;
    geo.addPoints(n);
    auto P = geo.positionsForWrite();
    auto flex = geo.points().create("flex", AttrType::Float).write<float>();
    std::vector<uint32_t> line;
    for (int i = 0; i < n; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(n - 1);
        P[static_cast<size_t>(i)] = Vec3(0.0f, 2.0f * t, 0.0f);
        flex[static_cast<size_t>(i)] = t;
        line.push_back(static_cast<uint32_t>(i));
    }
    geo.addPrimitive(line, false);
    geo.primitives().create("blade", AttrType::Int);
    return geo;
}

WindAt steadyAt(const WindSettings& s) {
    return [s](float) { return s; };
}

}  // namespace

TEST(plants_a_stem_in_gusts_sways_as_a_damped_spring) {
    // Gusts as waves passing the stem's foot: the bend the wind would hold
    // it at swings between none and twice the strength. A spring answers
    // each harmonic of it as 1 / (1 - r^2 + 2 i zeta r), r its pace over the
    // spring's own: far slower gusts it follows, at its own pace they rock
    // it to 1 / (2 zeta) as far, far quicker ones hardly move it.
    const Geometry geo = blade();
    SwaySettings d;
    d.damping = 0.12f;
    WindSettings s;
    s.strength = 0.05f;
    s.gust = 1.0f;
    s.turbulence = 0.0f;
    s.flutter = 0.0f;
    s.gustSize = 10.0f;
    SwayingPlants probe;
    probe.build(geo);
    CHECK_EQ(probe.stemCount(), size_t(1));
    const float own = probe.stemFrequency(0, d);
    std::printf("  a stem 2 m long sways at %.3f Hz\n", own);
    CHECK(std::fabs(own - 0.5f * std::pow(5.0f, 0.6f)) < 1e-4f);
    for (const float r : {0.3f, 1.0f, 3.0f}) {
        s.gustSpeed = r * own * s.gustSize;
        SwayingPlants plants;
        plants.build(geo);
        float lo = 1e9f, hi = -1e9f, kinLo = 1e9f, kinHi = -1e9f;
        for (float t = 15.0f; t < 25.0f; t += 1.0f / 90.0f) {
            Geometry g = geo;
            CHECK(plants.blow(g, steadyAt(s), d, t));
            const float bend = -plants.stemBends()[0].z;  // about -z: the top along +x
            const float held = -cross(Vec3(0.0f, 1.0f, 0.0f), windBend(s, Vec3(0.0f), t, 0 ^ s.seed)).z;
            lo = std::min(lo, bend), hi = std::max(hi, bend);
            kinLo = std::min(kinLo, held), kinHi = std::max(kinHi, held);
        }
        // What a spring makes of the wave 0.75 + sin(phi) - 0.25 cos(2 phi):
        // each harmonic as far as |H| says, turned by arg H.
        auto H = [&](float x) { return 1.0f / std::complex<float>(1.0f - x * x, 2.0f * d.damping * x); };
        const std::complex<float> h1 = H(r), h2 = H(2.0f * r);
        float yLo = 1e9f, yHi = -1e9f;
        for (int k = 0; k < 3600; ++k) {
            const float phi = 2.0f * kPi * static_cast<float>(k) / 3600.0f;
            const float y = std::abs(h1) * std::sin(phi + std::arg(h1)) - 0.25f * std::abs(h2) * std::cos(2.0f * phi + std::arg(h2));
            yLo = std::min(yLo, y), yHi = std::max(yHi, y);
        }
        const float expected = (yHi - yLo) / 2.0f;  // the wind's own swing is 2 strengths
        const float measured = (hi - lo) / (kinHi - kinLo);
        std::printf("  gusts at %.1f of its pace: it swings %.3f as far as the wind's bow (a spring: %.3f)\n", r, measured,
                    expected);
        CHECK(std::fabs(kinHi - kinLo - 2.0f * s.strength) < 0.02f * s.strength);
        CHECK(std::fabs(measured - expected) < 0.04f * expected);
    }
}

TEST(plants_a_tree_stands_still_in_a_steady_wind_and_its_branches_are_flung) {
    TreeSettings ts;
    ts.levels = 2;
    const Tree tree = growTree(ts, Vec3(), 1.0f, 4);
    Geometry geo;
    meshTree(tree, ts, 0, geo);
    SwayingPlants plants;
    plants.build(geo);
    // Its stems as the tree grew them, each with its parent.
    CHECK_EQ(plants.stemCount(), tree.stems.size());
    for (size_t i = 0; i < tree.stems.size(); ++i) CHECK_EQ(plants.stemParents()[i], tree.stems[i].parent);
    SwaySettings d;
    float slowest = 1e9f, quickest = 0.0f;
    for (size_t i = 0; i < plants.stemCount(); ++i) {
        slowest = std::min(slowest, plants.stemFrequency(i, d));
        quickest = std::max(quickest, plants.stemFrequency(i, d));
    }
    std::printf("  %zu stems, %.2f Hz the trunk (%.2f m), %.2f to %.2f Hz all\n", plants.stemCount(),
                plants.stemFrequency(0, d), tree.stems[0].length, slowest, quickest);
    CHECK(std::fabs(plants.stemFrequency(0, d) - slowest) < 1e-6f);

    // A steady wind: from the start each stem where the wind holds it --
    // nothing moves, the trunk's top bowed along the wind as far as the
    // bow bows it (the twigs less: each branch bends about its own base,
    // not about the foot).
    WindSettings steady;
    steady.gust = 0.0f;
    steady.turbulence = 0.0f;
    steady.flutter = 0.0f;
    Geometry a = geo, b = geo, bowed = geo;
    CHECK(plants.blow(a, steadyAt(steady), d, 0.5f));
    CHECK(plants.blow(b, steadyAt(steady), d, 4.0f));
    blowPlants(bowed, steady, 4.0f);
    float moved = 0.0f, fast = 0.0f;
    for (size_t p = 0; p < geo.pointCount(); ++p) {
        moved = std::max(moved, length(a.positions()[p] - b.positions()[p]));
        fast = std::max(fast, length(b.points().find("v")->read<Vec3>()[p]));
    }
    // The trunk's tip: the point of the most flex of stem 0's wood.
    const auto stemOf = geo.primitives().find("stem")->read<int32_t>();
    const auto flex = geo.points().find("flex")->read<float>();
    size_t top = 0;
    for (size_t prim = 0; prim < geo.primitiveCount(); ++prim) {
        if (stemOf[prim] != 0) continue;
        for (const uint32_t q : geo.primitivePoints(prim)) {
            if (flex[q] > flex[top]) top = q;  // point 0: the foot of the trunk
        }
    }
    const float sway = b.positions()[top].x - geo.positions()[top].x, bow = bowed.positions()[top].x - geo.positions()[top].x;
    std::printf("  steady: moved %.2g m in 3.5 s, %.2g m/s at most; the trunk's top %.3f m along it (bowed %.3f m)\n",
                moved, fast, sway, bow);
    CHECK(moved < 1e-4f);
    CHECK(fast < 1e-3f);
    CHECK(std::fabs(sway - bow) < 0.02f * bow);

    // Branches with no bend of their own, the trunk in gusts: each flung by
    // it -- turned and shoved under them -- yet still when it is.
    SwaySettings stiff = d;
    stiff.branches = 0.0f;
    WindSettings gusty = steady;
    gusty.gust = 1.0f;
    gusty.turbulence = 0.5f;
    auto flung = [&](const WindSettings& w) {
        SwayingPlants p;
        p.build(geo);
        float most = 0.0f;
        for (float t = 0.0f; t < 8.0f; t += 0.05f) {
            Geometry g = geo;
            CHECK(p.blow(g, steadyAt(w), stiff, t));
            for (size_t i = 0; i < p.stemCount(); ++i) {
                if (p.stemParents()[i] >= 0) most = std::max(most, length(p.stemBends()[i]));
            }
        }
        return most;
    };
    const float inGusts = flung(gusty), inSteady = flung(steady);
    std::printf("  branches flung by the trunk: %.4f rad at most in gusts, %.2g steady\n", inGusts, inSteady);
    CHECK(inGusts > 0.003f);
    CHECK(inSteady < 1e-5f);
}

TEST(plants_swaying_is_the_same_whatever_the_frames_and_threads) {
    // The forest example swaying: frames 1 to 36 one after the other, frame
    // 36 straight away, back to 20, on one thread and on four -- the same.
    sim::Network forest;
    CHECK(sim::Network::example("forest", forest));
    int spots = -1, wind = -1;
    for (const auto& n : forest.nodes()) {
        if (n.name == "spots") spots = n.id;
        if (n.name == "wind") wind = n.id;
    }
    CHECK(spots >= 0 && wind >= 0);
    CHECK(forest.setParam(spots, "count", "3"));
    CHECK(forest.setParam(wind, "dynamics", "1"));
    const unsigned saved = TaskPool::instance().threadCount();
    auto cookAt = [&](unsigned threads, const std::vector<int>& frames) {
        TaskPool::instance().setThreadCount(threads);
        sim::GeometryGraph g;
        g.sync(forest);
        GeometryPtr last;
        for (const int f : frames) last = g.cook(wind, f);
        return last;
    };
    std::vector<int> all;
    for (int f = 1; f <= 36; ++f) all.push_back(f);
    const GeometryPtr inOrder = cookAt(4, all), straight = cookAt(1, {36}), back = cookAt(4, {36, 20, 36});
    TaskPool::instance().setThreadCount(saved);
    CHECK(inOrder && straight && back);
    CHECK_EQ(inOrder->pointCount(), straight->pointCount());
    size_t same = 0;
    for (size_t p = 0; p < inOrder->pointCount(); ++p) {
        same += inOrder->positions()[p] == straight->positions()[p] && inOrder->positions()[p] == back->positions()[p] ? 1 : 0;
    }
    std::printf("  %zu points, %zu the same in order, straight away and back again\n", inOrder->pointCount(), same);
    CHECK_EQ(same, inOrder->pointCount());
}

TEST(plants_standing_on_points_sway_whole_held_where_a_steady_wind_bows_them) {
    // Clumps of grass as instances: in a steady wind each held where the bow
    // holds it -- the same shapes and tilts as without Dynamics; in gusts
    // they swing on their own.
    registerBuiltinNodes();
    Graph g;
    Node* grid = g.create("grid", "ground");
    grid->setFloat("sizex", 6.0f);
    grid->setFloat("sizez", 6.0f);
    Node* grass = g.create("grass", "grass");
    grass->setInt("variants", 2);
    CHECK(grass->setInput(0, grid));
    Node* bow = g.create("plantwind", "bow");
    Node* sway = g.create("plantwind", "sway");
    CHECK(bow->setInput(0, grass));
    CHECK(sway->setInput(0, grass));
    for (Node* n : {bow, sway}) {
        n->setFloat("gust", 0.0f);
        n->setFloat("turbulence", 0.0f);
    }
    sway->setBool("dynamics", true);
    CookEngine engine;
    CookContext at;
    at.time = 2.5;
    const GeometryPtr a = engine.cook(*bow, at), b = engine.cook(*sway, at);
    CHECK(a && b && a->pointCount() == b->pointCount() && a->pointCount() > 0);
    const auto ia = a->points().find("instance")->read<int32_t>(), ib = b->points().find("instance")->read<int32_t>();
    const auto oa = a->points().find("orient")->read<Vec4>(), ob = b->points().find("orient")->read<Vec4>();
    float off = 0.0f;
    size_t sameShape = 0;
    for (size_t p = 0; p < a->pointCount(); ++p) {
        sameShape += ia[p] == ib[p] ? 1 : 0;  // bent ahead as far the same way
        off = std::max(off, length(oa[p] - ob[p]));
    }
    std::printf("  %zu clumps: tilts at most %.2g apart in a steady wind\n", a->pointCount(), off);
    CHECK_EQ(sameShape, a->pointCount());
    CHECK(off < 1e-4f);

    // Gusts as quick as the clumps sway: the swaying ones lag and swing
    // past -- not where the bow is.
    for (Node* n : {bow, sway}) {
        n->setFloat("gust", 1.0f);
        n->setFloat("gustsize", 2.0f);
    }
    const GeometryPtr c = engine.cook(*bow, at), e = engine.cook(*sway, at);
    const auto oc = c->points().find("orient")->read<Vec4>(), oe = e->points().find("orient")->read<Vec4>();
    float apart = 0.0f;
    for (size_t p = 0; p < c->pointCount(); ++p) apart = std::max(apart, length(oc[p] - oe[p]));
    std::printf("  in gusts their tilts as much as %.3f apart\n", apart);
    CHECK(apart > 0.01f);
}

TEST(plants_by_height_a_tall_crown_shades_the_short_ones_under_it) {
    // A grown oak and a spruce seedling at its foot, a seedling out in the
    // open: the oak's top in the sky's light, the seedling under it in its
    // shade -- a lone crown, the low sky still round it -- the one in the
    // open lit, the light rising toward the crown's edge.
    Species oak;
    oak.crown = 5.0f;
    oak.height = 11.0f;
    oak.depth = 0.65f;
    oak.density = 4.0f;
    Species spruce = oak;
    spruce.crown = 3.0f;
    spruce.height = 14.5f;
    const std::vector<Species> kinds = {oak, spruce};
    const std::vector<Vec3> places = {Vec3(0.0f), Vec3(1.0f, 0.0f, 0.5f), Vec3(40.0f, 0.0f, 0.0f)};
    std::vector<EcoPlant> plants(3);
    plants[0].place = 0, plants[0].species = 0, plants[0].size = 1.0f;
    plants[1].place = 1, plants[1].species = 1, plants[1].size = 0.0f;
    plants[2].place = 2, plants[2].species = 1, plants[2].size = 0.0f;
    const CanopyLight light(places, plants, kinds);
    const float top = light.atTop(0), under = light.atTop(1), open = light.atTop(2);
    const float edge = light.at(Vec3(5.5f, 0.5f, 0.0f)), far = light.at(Vec3(80.0f, 0.5f, 0.0f));
    std::printf("  light at the oak's top %.3f, under it %.3f, at its edge %.3f, in the open %.3f (far %.3f); cells %.2f m\n",
                top, under, edge, open, far, light.cell());
    CHECK(top > 0.999f);
    CHECK(far > 0.999f);
    CHECK(under < 0.35f);
    CHECK(edge > under + 0.3f && edge < 0.95f);
    CHECK(open > 0.97f);
}

TEST(plants_by_height_shade_bearers_wait_under_the_canopy_and_shrubs_make_an_understorey) {
    // A land of 2601 places, wet along a stream; birches, oaks, spruces and
    // hazels by height for 100 years: the hazels under the trees, in their
    // shade; spruce saplings waiting in it, birch ones not -- they need the
    // light of a gap. The same on one thread and on four.
    std::vector<Vec3> places;
    std::vector<float> wet;
    for (int i = 0; i < 51; ++i) {
        for (int j = 0; j < 51; ++j) {
            const uint64_t h = (static_cast<uint64_t>(i) * 131u + static_cast<uint64_t>(j)) * 0x9E3779B97F4A7C15ull;
            const float x = -60.0f + 2.35f * static_cast<float>(i) + 1.5f * (static_cast<float>(h >> 40) / 16777216.0f - 0.5f);
            const float z = -60.0f + 2.35f * static_cast<float>(j) + 1.5f * (static_cast<float>((h >> 16) & 0xFFFFFF) / 16777216.0f - 0.5f);
            places.push_back(Vec3(x, 0.0f, z));
            const float d = std::fabs(z - 18.0f * std::sin(x * 0.035f));
            wet.push_back(std::clamp(std::exp(-d * d / 300.0f) + 0.15f, 0.0f, 1.0f));
        }
    }
    EcosystemSettings s;
    s.years = 100;
    s.byHeight = true;
    s.species = {{1.0f, 2.5f, 10.0f, 60.0f, 0.1f, 0.45f, 0.45f, 12.0f, 1.2f, 9.5f, 0.6f, 2.5f},
                 {0.6f, 5.0f, 40.0f, 300.0f, 0.35f, 0.3f, 0.3f, 6.0f, 0.4f, 11.0f, 0.65f, 4.0f},
                 {0.6f, 3.0f, 30.0f, 200.0f, 0.85f, 0.75f, 0.3f, 7.0f, 0.6f, 14.5f, 0.9f, 6.0f},
                 {0.8f, 1.5f, 6.0f, 40.0f, 0.8f, 0.5f, 0.45f, 3.0f, 1.0f, 3.0f, 0.9f, 3.0f}};
    const unsigned saved = TaskPool::instance().threadCount();
    TaskPool::instance().setThreadCount(1);
    const auto one = growEcosystem(places, wet, s);
    TaskPool::instance().setThreadCount(4);
    const auto four = growEcosystem(places, wet, s);
    TaskPool::instance().setThreadCount(saved);
    CHECK_EQ(one.size(), four.size());
    size_t same = 0;
    for (size_t i = 0; i < one.size() && i < four.size(); ++i) {
        same += one[i].place == four[i].place && one[i].species == four[i].species && one[i].size == four[i].size &&
                        one[i].light == four[i].light
                    ? 1
                    : 0;
    }
    CHECK_EQ(same, one.size());
    // Under a crown at least a metre taller.
    auto underTaller = [&](const EcoPlant& p) {
        const float hp = plantHeight(p, s.species[static_cast<size_t>(p.species)]);
        for (const EcoPlant& q : one) {
            const Species& sq = s.species[static_cast<size_t>(q.species)];
            const Vec3 d = places[q.place] - places[p.place];
            const float r = crownRadius(q, sq);
            if (&q != &p && plantHeight(q, sq) > hp + 1.0f && d.x * d.x + d.z * d.z < r * r) return true;
        }
        return false;
    };
    size_t count[4] = {0, 0, 0, 0}, under[4] = {0, 0, 0, 0}, waiting[4] = {0, 0, 0, 0};
    double light[4] = {0, 0, 0, 0}, waited[4] = {0, 0, 0, 0};
    for (const EcoPlant& p : one) {
        const size_t k = static_cast<size_t>(p.species);
        ++count[k];
        light[k] += p.light;
        under[k] += underTaller(p) ? 1 : 0;
        if (p.size < 0.3f && p.light < 0.1f) ++waiting[k], waited[k] += p.age;
    }
    const char* names[4] = {"birches", "oaks", "spruces", "hazels"};
    for (size_t k = 0; k < 4; ++k) {
        const double n = static_cast<double>(std::max<size_t>(count[k], 1));
        std::printf("  %-7s %4zu: light %.2f, %3.0f %% under a taller crown; %zu small ones in deep shade, %.1f years old\n",
                    names[k], count[k], light[k] / n, 100.0 * static_cast<double>(under[k]) / n, waiting[k],
                    waited[k] / static_cast<double>(std::max<size_t>(waiting[k], 1)));
    }
    for (size_t k = 0; k < 4; ++k) CHECK(count[k] > 0);
    CHECK(under[3] * 2 > count[3]);    // most hazels under the trees
    CHECK(light[3] / count[3] < 0.5);  // in their shade
    CHECK(light[2] / count[2] > 0.6);  // the canopy in the light
    // Spruce saplings bear the shade: more of them in it, and for longer.
    CHECK(waiting[2] * count[0] > 2 * waiting[0] * count[2]);
    CHECK(waited[2] / waiting[2] > 2.0 * waited[0] / std::max<size_t>(waiting[0], 1));
}

namespace {

/// A box `size` across standing at `center`: six closed quads, turned out.
Geometry boxAt(const Vec3& center, const Vec3& size) {
    registerBuiltinNodes();
    auto node = NodeRegistry::instance().create("box", "wall");
    node->setVec3("size", size);
    node->setVec3("center", center);
    CookEngine engine;
    return *engine.cook(*node, CookContext());
}

/// The nearest point of `geo`'s triangles to p, by looking at each.
float nearestByHand(const Geometry& geo, const Vec3& p) {
    float best = 1e30f;
    const auto P = geo.positions();
    for (size_t prim = 0; prim < geo.primitiveCount(); ++prim) {
        const auto pts = geo.primitivePoints(prim);
        for (size_t k = 1; k + 1 < pts.size(); ++k) {
            // Fine samples of the triangle: as near as any.
            const Vec3 a = P[pts[0]], b = P[pts[k]], c = P[pts[k + 1]];
            for (int i = 0; i <= 60; ++i) {
                for (int j = 0; i + j <= 60; ++j) {
                    const Vec3 q = a + (b - a) * (static_cast<float>(i) / 60.0f) + (c - a) * (static_cast<float>(j) / 60.0f);
                    best = std::min(best, length(q - p));
                }
            }
        }
    }
    return best;
}

}  // namespace

TEST(plants_triangles_nearest_and_crossing_as_found_by_hand) {
    Geometry geo = boxAt(Vec3(0.3f, 1.0f, -0.2f), Vec3(1.0f, 2.0f, 0.6f));
    geo.append(boxAt(Vec3(-1.5f, 0.5f, 0.8f), Vec3(0.4f, 1.0f, 1.2f)));
    const TriangleTree tree(geo);
    CHECK_EQ(tree.size(), size_t(24));
    uint64_t state = 7;
    auto unit = [&]() {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<float>(state >> 40) / 16777216.0f;
    };
    float worst = 0.0f;
    size_t crossings = 0, agree = 0;
    for (int n = 0; n < 200; ++n) {
        const Vec3 p(4.0f * unit() - 2.5f, 3.0f * unit() - 0.5f, 3.0f * unit() - 1.5f);
        Vec3 at;
        CHECK(tree.nearest(p, 10.0f, at));
        worst = std::max(worst, std::fabs(length(at - p) - nearestByHand(geo, p)));
        // Within a radius only: none when it is short of the nearest.
        Vec3 none;
        CHECK(!tree.nearest(p, 0.98f * length(at - p) - 1e-4f, none) || length(at - p) < 1e-4f);
        // A segment crosses a closed box when its ends are on either side
        // of its surface.
        const Vec3 q(4.0f * unit() - 2.5f, 3.0f * unit() - 0.5f, 3.0f * unit() - 1.5f);
        auto inside = [](const Vec3& x, const Vec3& c, const Vec3& h) {
            return std::fabs(x.x - c.x) < h.x && std::fabs(x.y - c.y) < h.y && std::fabs(x.z - c.z) < h.z;
        };
        const bool a1 = inside(p, Vec3(0.3f, 1.0f, -0.2f), Vec3(0.5f, 1.0f, 0.3f)), b1 = inside(q, Vec3(0.3f, 1.0f, -0.2f), Vec3(0.5f, 1.0f, 0.3f));
        const bool a2 = inside(p, Vec3(-1.5f, 0.5f, 0.8f), Vec3(0.2f, 0.5f, 0.6f)), b2 = inside(q, Vec3(-1.5f, 0.5f, 0.8f), Vec3(0.2f, 0.5f, 0.6f));
        const bool crosses = tree.crosses(p, q);
        crossings += crosses ? 1 : 0;
        if (a1 != b1 || a2 != b2) agree += crosses ? 1 : 0;  // one end in, one out: it must cross
        else agree += 1;                                        // else it may pass through both sides
    }
    std::printf("  nearest points within %.2g m of those found by hand; %zu of 200 segments cross\n", worst, crossings);
    CHECK(worst < 0.03f);  // the hand's samples are 1/60 of a face apart
    CHECK_EQ(agree, size_t(200));
    CHECK(crossings > 20);
}

TEST(plants_a_tree_by_a_wall_grows_round_it) {
    // A wall half a metre from a tree's foot: no stem goes through it or
    // comes nearer than Clearance; the branches meeting it turn along it,
    // up and aside; with Avoid 0 they end there instead.
    const Geometry wall = boxAt(Vec3(0.9f, 4.0f, 0.0f), Vec3(0.3f, 8.0f, 10.0f));
    const TriangleTree obstacles(wall);
    TreeSettings ts;
    ts.levels = 2;
    const Tree free = growTree(ts, Vec3(), 1.0f, 3);
    const Tree kept = growTree(ts, Vec3(), 1.0f, 3, &obstacles);
    TreeSettings stopping = ts;
    stopping.avoid = 0.0f;
    const Tree stopped = growTree(stopping, Vec3(), 1.0f, 3, &obstacles);
    struct Count {
        size_t through = 0, near = 0, beyond = 0, points = 0;
        float wood = 0.0f, along = 0.0f;
        size_t leaves = 0, leavesThrough = 0;
    };
    auto count = [&](const Tree& t) {
        Count c;
        for (const TreeStem& st : t.stems) {
            for (size_t i = 0; i < st.points.size(); ++i) {
                const Vec3& p = st.points[i];
                ++c.points;
                if (i > 0) {
                    c.through += obstacles.crosses(st.points[i - 1], p) ? 1 : 0;
                    c.wood += length(p - st.points[i - 1]);
                }
                Vec3 at;
                if (obstacles.nearest(p, ts.clearance + st.radius[i] - 0.02f, at)) ++c.near;
                c.beyond += p.x > 0.75f ? 1 : 0;
                // Branches sliding along it: wood within 2.5 Clearance of it.
                auto by = [&](size_t k) { return obstacles.nearest(st.points[k], 2.5f * ts.clearance + st.radius[k], at); };
                if (i > 0 && st.level > 0 && by(i) && by(i - 1)) c.along += length(p - st.points[i - 1]);
            }
        }
        c.leaves = t.leaves.size();
        for (const TreeLeaf& leaf : t.leaves) c.leavesThrough += obstacles.crosses(leaf.at, leaf.at + leaf.along * leaf.size) ? 1 : 0;
        return c;
    };
    const Count a = count(free), b = count(kept), c = count(stopped);
    std::printf("  free:     %zu points, %.0f m of wood, %zu segments through the wall, %zu points beyond it, %zu leaves (%zu through)\n",
                a.points, a.wood, a.through, a.beyond, a.leaves, a.leavesThrough);
    std::printf("  avoiding: %zu points, %.0f m of wood, %zu through, %zu nearer than Clearance, %.1f m of branches along it, "
                "%zu leaves (%zu through)\n",
                b.points, b.wood, b.through, b.near, b.along, b.leaves, b.leavesThrough);
    std::printf("  stopping: %zu points, %.0f m of wood, %zu through, %zu nearer than Clearance, %.1f m along it, %zu leaves\n",
                c.points, c.wood, c.through, c.near, c.along, c.leaves);
    CHECK(a.through > 10 && a.beyond > 100);  // without it, the crown would reach through
    CHECK_EQ(b.through, size_t(0));
    CHECK_EQ(b.beyond, size_t(0));
    CHECK_EQ(b.near, size_t(0));
    CHECK_EQ(b.leavesThrough, size_t(0));
    CHECK_EQ(c.through, size_t(0));
    CHECK_EQ(c.near, size_t(0));
    CHECK(b.along > 2.0f * c.along);  // turned along it rather than stopped
    CHECK(b.wood > c.wood);
    // The same tree where no obstacle is within its reach.
    const Geometry far = boxAt(Vec3(40.0f, 4.0f, 0.0f), Vec3(0.3f, 8.0f, 10.0f));
    const TriangleTree distant(far);
    const Tree alone = growTree(ts, Vec3(), 1.0f, 3, &distant);
    CHECK_EQ(alone.stems.size(), free.stems.size());
    bool same = alone.leaves.size() == free.leaves.size();
    for (size_t i = 0; i < alone.stems.size() && same; ++i) same = alone.stems[i].points == free.stems[i].points;
    CHECK(same);
}

TEST(plants_trees_as_instances_by_an_obstacle_are_grown_on_their_own) {
    // Six places in two rows, a wall by the first two: those two stand for
    // trees of their own, grown there clear of it; the rest for the
    // variants. Unpacked, no stem goes through the wall.
    registerBuiltinNodes();
    Graph g;
    Node* grid = g.create("grid", "spots");
    grid->setInt("rows", 2);
    grid->setInt("cols", 3);
    grid->setFloat("sizex", 24.0f);
    grid->setFloat("sizez", 1.0f);
    Node* wall = g.create("box", "wall");
    wall->setVec3("size", Vec3(0.3f, 8.0f, 4.0f));
    wall->setVec3("center", Vec3(-11.1f, 4.0f, 0.0f));
    Node* trees = g.create("tree", "trees");
    trees->setInt("output", 2);
    trees->setInt("variants", 3);
    trees->setInt("levels", 2);
    trees->setFloat("sizevariation", 0.0f);
    CHECK(trees->setInput(0, grid));
    CHECK(trees->setInput(1, wall));
    CookEngine engine;
    const GeometryPtr out = engine.cook(*trees, CookContext());
    CHECK(out && out->pointCount() == 6);
    const auto instance = out->points().find("instance")->read<int32_t>();
    const auto P = out->positions();
    size_t own = 0;
    for (size_t i = 0; i < 6; ++i) own += instance[i] >= 3 ? 1 : 0;
    const TriangleTree obstacles(*engine.cook(*wall, CookContext()));
    size_t through = 0, segments = 0;
    for (size_t i = 0; i < 6; ++i) {
        const bool near = P[i].x < -10.0f;
        CHECK_EQ(instance[i] >= 3, near);
        if (!near) continue;
        // Its own tree, upright and its own size: its points where they stand.
        const Geometry& proto = *out->prototypes()[static_cast<size_t>(instance[i])];
        const auto level = proto.primitives().find("level")->read<int32_t>();
        for (size_t prim = 0; prim < proto.primitiveCount(); ++prim) {
            if (level[prim] < 0) continue;
            const auto pts = proto.primitivePoints(prim);
            for (size_t k = 0; k + 1 < pts.size(); ++k) {
                ++segments;
                through += obstacles.crosses(P[i] + proto.positions()[pts[k]], P[i] + proto.positions()[pts[k + 1]]) ? 1 : 0;
            }
        }
    }
    std::printf("  %zu prototypes, %zu places with a tree of their own; %zu of their %zu bark edges through the wall\n",
                out->prototypeCount(), own, through, segments);
    CHECK_EQ(own, size_t(2));
    CHECK_EQ(out->prototypeCount(), size_t(5));
    CHECK_EQ(through, size_t(0));
}
