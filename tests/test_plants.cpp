//
// Plants as they live: in the wind as springs (SwayingPlants) -- a stem
// driven by gusts as a damped spring is, by how near its own pace they come;
// a tree still in a steady wind, its branches flung by the trunk when it is
// not; the same whatever frames are asked for, in whatever order, on any
// number of threads; plants standing on points swaying whole.
//
#include "pg/core/CookEngine.h"
#include "pg/core/Ecosystem.h"
#include "pg/core/Graph.h"
#include "pg/core/Parallel.h"
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
