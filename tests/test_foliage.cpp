//
// Foliage as the renderers see it: the uv of trees and grass -- round and up
// the bark in whole pictures, square at a stem's foot; each leaf in its
// quarter of the leaf picture; once across and up a blade of grass -- none
// mirrored, and the library's bark, leaves and grass laid on by it; leaves
// cut out by their picture's alpha -- seen through and letting the sun
// through there, in both renderers and both ray engines; the viewport's
// mesh carrying how much light each face lets through; plants thinned for
// far away -- fewer leaves and blades, as much foliage -- and the copies
// drawn at the level of detail they look big enough for; wind bowing the
// plants from their feet, nothing stretched, gusts running along it, and
// the plants points stand for bent ahead into a few shapes.
//
#include "pg/core/CookEngine.h"
#include "pg/core/Graph.h"
#include "pg/core/Ecosystem.h"
#include "pg/core/Grass.h"
#include "pg/core/Instances.h"
#include "pg/core/Lod.h"
#include "pg/core/Material.h"
#include "pg/core/Tree.h"
#include "pg/core/Wind.h"
#include "pg/io/Picture.h"
#include "pg/nodes/Nodes.h"
#include "pg/render/Cycles.h"
#include "pg/render/Embree.h"
#include "pg/render/PathTracer.h"
#include "pg/render/Scene.h"
#include "pg/render/Textures.h"
#include "pg/sim/Display.h"
#include "pg/sim/GeometryGraph.h"
#include "pg/sim/Network.h"

#include "test_framework.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <functional>
#include <set>

using namespace pg;
namespace fs = std::filesystem;

namespace {

/// The uv of the corners of primitive `prim` of `geo`.
std::vector<Vec3> uvOf(const Geometry& geo, size_t prim) {
    const auto uv = geo.vertices().find("uv")->read<Vec3>();
    std::vector<Vec3> out;
    const size_t first = geo.primitiveVertexStart(prim);
    for (size_t k = 0; k < geo.primitiveVertexCount(prim); ++k) out.push_back(uv[first + k]);
    return out;
}

/// Twice the area of a polygon in uv, anticlockwise positive.
float uvArea(const std::vector<Vec3>& uv) {
    float a = 0.0f;
    for (size_t k = 0; k < uv.size(); ++k) {
        const Vec3& p = uv[k];
        const Vec3& q = uv[(k + 1) % uv.size()];
        a += p.x * q.y - q.x * p.y;
    }
    return a;
}

}  // namespace

TEST(foliage_trees_and_grass_have_uv_none_mirrored) {
    TreeSettings s;
    s.levels = 2;
    s.leaves = 6;
    Geometry geo;
    const Tree tree = growTree(s, Vec3(), 1.0f, 7);
    meshTree(tree, s, 0, geo);
    const AttributeArray* uvs = geo.vertices().find("uv");
    CHECK(uvs && uvs->type() == AttrType::Vec3 && uvs->size() == geo.vertexCount());
    const auto level = geo.primitives().find("level")->read<int32_t>();
    const auto P = geo.positions();

    // The bark: no face across the seam (less than a picture round it, and
    // up it), every face's uv anticlockwise as its corners are; at the
    // trunk's foot the pictures square -- as many metres round a unit of u
    // as up a unit of v.
    std::set<float> broadCells;
    size_t bark = 0, leaves = 0;
    for (size_t i = 0; i < geo.primitiveCount(); ++i) {
        const std::vector<Vec3> uv = uvOf(geo, i);
        float lo = 1e9f, hi = -1e9f;
        for (const Vec3& q : uv) lo = std::min(lo, q.x), hi = std::max(hi, q.x);
        if (level[i] >= 0) {
            ++bark;
            CHECK(hi - lo < 1.0f);
            CHECK(uvArea(uv) > 0.0f);
        } else {
            // A leaf: in a quarter of the picture, its base at the middle of
            // the quarter's bottom, its tip at the middle of its top.
            ++leaves;
            const float cx = uv[0].x < 0.5f ? 0.0f : 0.5f, cy = uv[0].y < 0.5f ? 0.0f : 0.5f;
            CHECK(cy == 0.5f);  // broad leaves: the top two
            broadCells.insert(cx);
            for (const Vec3& q : uv) CHECK(q.x >= cx && q.x <= cx + 0.5f && q.y >= cy && q.y <= cy + 0.5f);
            CHECK(std::fabs(uv[0].x - (cx + 0.25f)) < 1e-6f && std::fabs(uv[0].y - cy) < 1e-6f);
            float top = 0.0f;
            for (const Vec3& q : uv) top = std::max(top, q.y);
            CHECK(std::fabs(top - (cy + 0.5f)) < 1e-6f);
            CHECK(uvArea(uv) > 0.0f);
        }
    }
    CHECK(bark > 0 && leaves == tree.leaves.size());
    CHECK(broadCells.size() == 2);  // either broad leaf
    {
        // The trunk's first quad: round it and up it the same metres a unit.
        const std::vector<Vec3> uv = uvOf(geo, 0);
        const size_t first = geo.primitiveVertexStart(0);
        const auto corner = [&](size_t k) { return P[geo.vertexPoints()[first + k]]; };
        const float round = length(corner(1) - corner(0)) / (uv[1].x - uv[0].x);
        const float up = length(corner(3) - corner(0)) / (uv[3].y - uv[0].y);
        CHECK(std::fabs(round / up - 1.0f) < 0.1f);
        // Whole pictures round it: the last face round ends on a whole u.
        const float pictures = uvOf(geo, static_cast<size_t>(s.sides) - 1)[1].x;
        CHECK(pictures >= 1.0f && std::fabs(pictures - std::round(pictures)) < 1e-5f);
    }

    // Narrow leaves bottom left, needles bottom right.
    for (const auto [shape, cell] : {std::pair{TreeSettings::Leaf::Narrow, 0.0f}, std::pair{TreeSettings::Leaf::Needles, 0.5f}}) {
        TreeSettings t = s;
        t.leaf = shape;
        Geometry g;
        meshTree(growTree(t, Vec3(), 1.0f, 7), t, 0, g);
        const auto lv = g.primitives().find("level")->read<int32_t>();
        bool all = true;
        for (size_t i = 0; i < g.primitiveCount(); ++i) {
            if (lv[i] >= 0) continue;
            for (const Vec3& q : uvOf(g, i)) all = all && q.y <= 0.5f && q.x >= cell && q.x <= cell + 0.5f;
        }
        CHECK(all);
    }

    // Grass: once across a blade and up it, root to tip.
    const Geometry clump = growGrassClump(GrassSettings(), 3);
    CHECK(clump.vertices().find("uv"));
    float lowest = 1.0f, highest = 0.0f;
    for (size_t i = 0; i < clump.primitiveCount(); ++i) {
        const std::vector<Vec3> uv = uvOf(clump, i);
        CHECK(uvArea(uv) > 0.0f);
        for (const Vec3& q : uv) lowest = std::min(lowest, q.y), highest = std::max(highest, q.y);
    }
    CHECK(lowest == 0.0f && highest == 1.0f);

    // The renderers: bark and leaves laid on by it, none mirrored -- every
    // tangent the way of the faces' own.
    for (const Geometry* g : {static_cast<const Geometry*>(&geo), &clump}) {
        const auto mesh = render::meshOf(*g);
        CHECK(!mesh->materials.empty());
        for (const render::Material& m : mesh->materials) CHECK(m.byUv && laidByUv(m.preset));
        CHECK_EQ(mesh->tangents.size(), 3 * mesh->count());
        size_t mirrored = 0;
        for (const Vec4& t : mesh->tangents) mirrored += t.w < 0.0f;
        CHECK_EQ(mirrored, 0u);
    }
}

namespace {

/// A node of `type` set up by `setup`, cooked over `input`.
GeometryPtr cookedOver(const std::string& type, GeometryPtr input, const std::function<void(Node&)>& setup) {
    registerBuiltinNodes();
    Graph g;
    Node* node = g.create(type, "node");
    setup(*node);
    const GeometryPtr in[1] = {std::move(input)};
    return node->cookNode(CookContext{}, in);
}

/// An RGBA picture, `pixel(x, y)` its colour and alpha (the top row first).
void writeRgba(const std::string& path, int width, int height, const std::function<Vec4(int, int)>& pixel) {
    io::Picture p;
    p.width = width;
    p.height = height;
    p.rgba.resize(static_cast<size_t>(width) * height * 4);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const Vec4 c = pixel(x, y);
            float* q = &p.rgba[(static_cast<size_t>(y) * width + x) * 4];
            q[0] = c.x, q[1] = c.y, q[2] = c.z, q[3] = c.w;
        }
    }
    std::string error;
    CHECK(io::writePicture(path, p, 95, error));
}

/// A square sheet 4 m across, a metre above the ground, facing up: its
/// texture `texture` laid on by uv, u from -x to +x.
GeometryPtr sheet(const std::string& texture) {
    auto geo = std::make_shared<Geometry>();
    geo->addPoints(4);
    auto P = geo->positionsForWrite();
    P[0] = Vec3(-2.0f, 1.0f, -2.0f);
    P[1] = Vec3(-2.0f, 1.0f, 2.0f);
    P[2] = Vec3(2.0f, 1.0f, 2.0f);
    P[3] = Vec3(2.0f, 1.0f, -2.0f);
    const uint32_t quad[4] = {0, 1, 2, 3};
    geo->addPrimitive(quad, true);
    GeometryPtr laid = cookedOver("uvproject", geo, [](Node& n) {
        n.setVec3("center", Vec3(-2.0f, 1.0f, 2.0f));
        n.setFloat("scale", 4.0f);
    });
    return cookedOver("material", laid, [&](Node& n) {
        n.setInt("material", 0);
        n.setString("texture", texture);
        n.setBool("texture_tint", false);
    });
}

/// The alpha of a set, as a renderer has it.
struct SetCoverage final : render::Coverage {
    std::shared_ptr<const render::TexturePicture> alpha;
    float at(const render::Material& m, const Vec2& uv) const override {
        return m.cutout && alpha ? alpha->at(uv.x, uv.y).x : 1.0f;
    }
};

Vec3 meanOf(const render::Image& img, int x0, int y0, int x1, int y1) {
    Vec3 sum(0.0f);
    int n = 0;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const float* p = &img.pixels[3 * (static_cast<size_t>(y) * img.width + x)];
            sum = sum + Vec3(p[0], p[1], p[2]);
            ++n;
        }
    }
    return n > 0 ? sum / static_cast<float>(n) : sum;
}

}  // namespace

TEST(foliage_leaves_are_cut_out_by_their_pictures_alpha) {
    // The library's leaves: their colour's alpha; someone else's set, its
    // _opacity picture's grey.
    const render::TextureSet leaf = render::presetTextureSet(render::textureLibrary(), MaterialPreset::Leaf);
    CHECK(leaf.valid() && leaf.alphaChannel && leaf.alpha == leaf.color && leaf.tint);
    CHECK(render::presetTextureSet(render::textureLibrary(), MaterialPreset::Bark).alpha.empty());
    const render::TextureSet grass = render::presetTextureSet(render::textureLibrary(), MaterialPreset::Grass);
    CHECK(grass.valid() && grass.alpha.empty());
    const fs::path dir = fs::temp_directory_path() / "pg_test_foliage";
    fs::remove_all(dir);
    fs::create_directories(dir / "other");
    writeRgba((dir / "other" / "fern_diff.png").string(), 4, 4, [](int, int) { return Vec4(0.2f, 0.5f, 0.1f, 1.0f); });
    writeRgba((dir / "other" / "fern_opacity.png").string(), 4, 4, [](int, int) { return Vec4(1.0f); });
    const render::TextureSet other = render::textureSet((dir / "other" / "fern_diff.png").string());
    CHECK(!other.alphaChannel && other.alpha == (dir / "other" / "fern_opacity.png").string());

    // A tree's leaves cut out, its bark not.
    TreeSettings ts;
    ts.levels = 1;
    Geometry tree;
    meshTree(growTree(ts, Vec3(), 1.0f, 3), ts, 0, tree);
    for (const render::Material& m : render::meshOf(tree)->materials) CHECK_EQ(m.cutout, m.preset == MaterialPreset::Leaf);

    // A red sheet a metre above the ground, its left half not there (alpha
    // 0): rays and the sun's go through it there, in both engines -- and
    // without coverage it is whole.
    const std::string half = (dir / "half" / "half_diff.png").string();
    fs::create_directories(dir / "half");
    writeRgba(half, 16, 16, [](int x, int) { return Vec4(0.8f, 0.05f, 0.05f, x < 8 ? 0.0f : 1.0f); });
    const GeometryPtr cut = sheet(half);
    SetCoverage coverage;
    coverage.alpha = render::alphaPicture(render::textureSet(half));
    CHECK(coverage.alpha != nullptr);
    sim::Look look;
    look.lightIntensity = 2.0f;
    look.lightElevation = 90.0f;
    look.skyIntensity = 0.3f;
    look.groundColor = Vec3(0.5f);
    sim::Camera cam = sim::Camera::lookingAt(Vec3(0.0f, 6.0f, 0.01f), Vec3(0.0f, 0.0f, 0.0f));
    cam.width = cam.height = 32;
    render::SceneInput in;
    in.geometry = cut;
    in.look = look;
    in.camera = cam;
    std::vector<render::RayEngine> engines = {render::RayEngine::Own};
    if (render::embreeAvailable()) engines.push_back(render::RayEngine::Embree);
    for (const render::RayEngine engine : engines) {
        render::SceneBuilder builder(engine);
        const auto scene = builder.build(in);
        const Vec3 up(0.0f, 1.0f, 0.0f), down(0.0f, -1.0f, 0.0f);
        CHECK(scene->transmittance(Vec3(-1.0f, 0.5f, 0.0f), up, 1e30f, 0.0f, &coverage).x > 0.99f);
        CHECK(scene->transmittance(Vec3(1.0f, 0.5f, 0.0f), up, 1e30f, 0.0f, &coverage).x == 0.0f);
        CHECK(scene->transmittance(Vec3(-1.0f, 0.5f, 0.0f), up, 1e30f, 0.0f).x == 0.0f);
        render::Hit hit;
        CHECK(scene->intersect(Vec3(-1.0f, 3.0f, 0.0f), down, 1e30f, 0.3f, hit, 0.0f, &coverage) && hit.floor &&
              std::fabs(hit.t - 3.0f) < 1e-3f);
        CHECK(scene->intersect(Vec3(1.0f, 3.0f, 0.0f), down, 1e30f, 0.3f, hit, 0.0f, &coverage) && !hit.floor &&
              std::fabs(hit.t - 2.0f) < 1e-3f);
        CHECK(scene->intersect(Vec3(-1.0f, 3.0f, 0.0f), down, 1e30f, 0.3f, hit) && !hit.floor);
    }

    // From above, in both renderers: the ground through the left half, lit
    // by the sun as where there is no sheet at all; the red sheet on the
    // right.
    render::SceneBuilder builder;
    const auto scene = builder.build(in);
    render::SceneInput bare = in;
    bare.geometry = std::make_shared<Geometry>();
    const auto ground = builder.build(bare);
    render::Settings s;
    s.width = s.height = 32;
    s.samples = 16;
    s.denoise = false;
    s.sky = render::Settings::Sky::Look;
    auto traced = [&](const std::shared_ptr<const render::Scene>& sc) {
        render::PathTracer tracer;
        tracer.setSettings(s);
        tracer.setScene(sc);
        while (tracer.samples() < s.samples) tracer.pass();
        return tracer.beauty();
    };
    auto cycled = [&](const std::shared_ptr<const render::Scene>& sc) {
        render::CyclesRender r;
        r.start(sc, s);
        r.wait();
        CHECK(r.error().empty());
        return r.beauty();
    };
    for (const bool cycles : {false, true}) {
        if (cycles && !render::cyclesAvailable()) continue;
        const render::Image img = cycles ? cycled(scene) : traced(scene);
        const render::Image open = cycles ? cycled(ground) : traced(ground);
        const Vec3 left = meanOf(img, 6, 8, 13, 24), right = meanOf(img, 19, 8, 26, 24);
        const Vec3 floor = meanOf(open, 6, 8, 13, 24);
        std::printf("  %s: through the cut-out half %.3f %.3f %.3f, the ground with no sheet %.3f %.3f %.3f, "
                    "the sheet %.3f %.3f %.3f\n",
                    cycles ? "Cycles" : "the path tracer", left.x, left.y, left.z, floor.x, floor.y, floor.z, right.x,
                    right.y, right.z);
        CHECK(std::fabs(left.y / floor.y - 1.0f) < 0.1f);
        CHECK(std::fabs(left.x - left.y) < 0.1f * left.y);
        CHECK(right.x > 4.0f * right.y);
    }
    fs::remove_all(dir);
}

TEST(foliage_the_viewport_knows_what_lets_light_through) {
    // A tree's leaves 0.4, its bark nothing; grass 0.35; a box without the
    // attribute none at all.
    TreeSettings ts;
    ts.levels = 1;
    auto tree = std::make_shared<Geometry>();
    meshTree(growTree(ts, Vec3(), 1.0f, 3), ts, 0, *tree);
    sim::DisplayMesher mesher;
    sim::DisplayMesh mesh;
    mesher.make(tree, mesh);
    CHECK_EQ(mesh.translucency.size(), mesh.vertexCount());
    std::set<float> seen(mesh.translucency.begin(), mesh.translucency.end());
    CHECK(seen == std::set<float>({0.0f, 0.4f}));
    // Leaf vertices green, bark brown: the 0.4s are the leaves'.
    for (size_t v = 0; v < mesh.vertexCount(); ++v) {
        if (mesh.translucency[v] > 0.0f) CHECK(mesh.colors[3 * v + 1] > mesh.colors[3 * v]);
    }
    sim::DisplayMesher grassMesher;
    sim::DisplayMesh blades;
    grassMesher.make(std::make_shared<Geometry>(growGrassClump(GrassSettings(), 2)), blades);
    CHECK(!blades.translucency.empty() && std::all_of(blades.translucency.begin(), blades.translucency.end(),
                                                      [](float t) { return t == 0.35f; }));
    auto bare = std::make_shared<Geometry>();
    bare->addPoints(3);
    const uint32_t tri[3] = {0, 1, 2};
    bare->addPrimitive(tri, true);
    auto P = bare->positionsForWrite();
    P[1] = Vec3(1.0f, 0.0f, 0.0f);
    P[2] = Vec3(0.0f, 1.0f, 0.0f);
    sim::DisplayMesher bareMesher;
    sim::DisplayMesh none;
    bareMesher.make(bare, none);
    CHECK(none.vertexCount() == 3 && none.translucency.empty());
}

namespace {

/// The area of the faces of `geo` that let light through, and how many.
std::pair<double, size_t> foliageOf(const Geometry& geo) {
    const auto lets = geo.primitives().find("translucency")->read<float>();
    const auto P = geo.positions();
    double area = 0.0;
    size_t n = 0;
    for (size_t i = 0; i < geo.primitiveCount(); ++i) {
        if (!(lets[i] > 0.0f)) continue;
        ++n;
        const auto pts = geo.primitivePoints(i);
        for (size_t k = 1; k + 1 < pts.size(); ++k) {
            area += 0.5 * length(cross(P[pts[k]] - P[pts[0]], P[pts[k + 1]] - P[pts[0]]));
        }
    }
    return {area, n};
}

}  // namespace

TEST(foliage_far_plants_are_thinned_and_drawn_by_how_big_they_look) {
    // A tree: a third of its leaves, each grown so they cover as much; its
    // twigs gone, its trunk and boughs kept; no point left loose. As it is
    // at 1.
    TreeSettings ts;
    ts.levels = 3;
    ts.branches = {10, 5, 4};
    Geometry tree;
    meshTree(growTree(ts, Vec3(), 1.0f, 5), ts, 0, tree);
    CHECK(plantDetail(tree, 1.0f).hash() == tree.hash());
    const auto [area, leaves] = foliageOf(tree);
    const Geometry far = plantDetail(tree, 0.35f);
    const auto [farArea, farLeaves] = foliageOf(far);
    std::printf("  tree: %zu leaves, %.3f m2; a third: %zu leaves, %.3f m2; %zu faces of %zu\n", leaves, area, farLeaves,
                farArea, far.primitiveCount(), tree.primitiveCount());
    CHECK(std::fabs(static_cast<double>(farLeaves) - 0.35 * static_cast<double>(leaves)) <= 1.0);
    CHECK(std::fabs(farArea / area - 1.0) < 0.05);
    const auto level = far.primitives().find("level")->read<int32_t>();
    std::set<int32_t> levels(level.begin(), level.end());
    CHECK(levels == std::set<int32_t>({-1, 0, 1}));
    std::vector<uint8_t> used(far.pointCount(), 0);
    for (size_t i = 0; i < far.primitiveCount(); ++i) {
        for (const uint32_t p : far.primitivePoints(i)) used[p] = 1;
    }
    CHECK(std::all_of(used.begin(), used.end(), [](uint8_t u) { return u != 0; }));
    CHECK(far.vertices().find("uv") && far.primitives().find("material"));

    // Grass: an eighth of its blades, each eight times as wide -- as long.
    const Geometry clump = growGrassClump(GrassSettings(), 4);
    const Geometry thin = plantDetail(clump, 0.125f);
    const auto [bladeArea, bladeFaces] = foliageOf(clump);
    const auto [thinArea, thinFaces] = foliageOf(thin);
    std::printf("  grass: %zu faces, %.4f m2; an eighth: %zu faces, %.4f m2\n", bladeFaces, bladeArea, thinFaces, thinArea);
    CHECK(thinFaces * 8 == bladeFaces);
    CHECK(std::fabs(thinArea / bladeArea - 1.0) < 0.15);
    float tallest = 0.0f, thinTallest = 0.0f;
    for (const Vec3& p : clump.positions()) tallest = std::max(tallest, p.y);
    for (const Vec3& p : thin.positions()) thinTallest = std::max(thinTallest, p.y);
    CHECK(thinTallest <= tallest + 1e-5f);

    // Copies by how big they look: a metre across, 10, 30, 200, 2000, 5 and
    // 100 m away -- in full, a third, a billboard, none, in full, and half
    // the pixels an eighth, the other half a billboard.
    std::vector<float> placements;
    for (const float z : {10.0f, 30.0f, 200.0f, 2000.0f, 5.0f, 100.0f}) {
        placements.insert(placements.end(), {0.0f, 0.0f, -z, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f});
    }
    const auto parts = sim::placementsByDetail(placements, Vec3(0.0f, 0.5f, 0.0f), 0.5f, Vec3(0.0f));
    constexpr size_t n = sim::DisplayInstances::kFloats;
    CHECK(parts[0].size() == 2 * n && parts[0][2] == -10.0f && parts[0][n + 2] == -5.0f);
    CHECK(parts[0][n - 1] == 1.0f && parts[0][2 * n - 1] == 1.0f);
    CHECK(parts[1].size() == n && parts[1][2] == -30.0f && parts[1][n - 1] == 1.0f);
    CHECK(parts[2].size() == n && parts[2][2] == -100.0f && std::fabs(parts[2][n - 1] - 0.5f) < 1e-4f);
    CHECK(parts[3].size() == 2 * n && parts[3][2] == -200.0f && parts[3][n - 1] == 1.0f && parts[3][n + 2] == -100.0f &&
          std::fabs(parts[3][2 * n - 1] - 1.5f) < 1e-4f);
    // Fading: as a copy goes off, its share of the nearer level falls as
    // that of the farther rises -- together all of its pixels.
    for (float z = 8.0f; z < 270.0f; z *= 1.07f) {  // short of where they fade out altogether
        const std::vector<float> one = {0.0f, 0.0f, -z, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
        const auto at = sim::placementsByDetail(one, Vec3(0.0f, 0.5f, 0.0f), 0.5f, Vec3(0.0f));
        float share = 0.0f;
        for (const auto& level : at) {
            for (size_t i = n - 1; i < level.size(); i += n) share += level[i] <= 1.0f ? level[i] : 2.0f - level[i];
        }
        CHECK(std::fabs(share - 1.0f) < 1e-4f);
    }

    // The meadow from 50 m off its edge, as the viewport draws it: so many
    // fewer triangles.
    sim::Network meadow;
    CHECK(sim::Network::example("meadow", meadow));
    sim::GeometryGraph g;
    g.sync(meadow);
    const GeometryPtr all = g.cook(meadow.displayed(), 1);
    CHECK(all != nullptr);
    const sim::DisplayInstances inst = sim::instancesOf(*all);
    const Vec3 eye(0.0f, 25.0f, 45.0f);
    double full = 0.0, drawn = 0.0;
    size_t copies[4] = {0, 0, 0, 0};
    for (size_t k = 0; k < inst.prototypes.size(); ++k) {
        const GeometryPtr& proto = inst.prototypes[k];
        const size_t count = inst.placements[k].size() / n;
        // The billboard two triangles a copy.
        std::array<size_t, sim::kDetailLevels> triangles{0, 0, 0, 2};
        for (size_t level = 0; level < 3; ++level) {
            sim::DisplayMesher mesher;
            sim::DisplayMesh mesh;
            mesher.make(level == 0 ? proto : std::make_shared<Geometry>(plantDetail(*proto, sim::kDetailKeep[level])), mesh);
            triangles[level] = mesh.triangleCount();
        }
        const auto byDetail = sim::placementsByDetail(inst.placements[k], inst.centers[k], inst.radii[k], eye);
        full += static_cast<double>(count * triangles[0]);
        for (size_t level = 0; level < sim::kDetailLevels; ++level) {
            drawn += static_cast<double>(byDetail[level].size() / n * triangles[level]);
            copies[level] += byDetail[level].size() / n;
        }
    }
    std::printf("  the meadow from 50 m: %.1f million triangles in full, %.1f million drawn (%.0f %%); copies in full %zu, "
                "a third %zu, an eighth %zu, billboards %zu (fading ones twice)\n",
                full * 1e-6, drawn * 1e-6, 100.0 * drawn / full, copies[0], copies[1], copies[2], copies[3]);
    CHECK(drawn < 0.6 * full);
}

TEST(foliage_the_viewport_draws_the_copies_the_camera_sees) {
    // A camera at the origin looking down -z, 60 degrees high, 1.5 wide.
    const Mat4 viewProjection = glm::perspective(glm::radians(60.0f), 1.5f, 0.1f, 100.0f) *
                                glm::lookAt(Vec3(0.0f), Vec3(0.0f, 0.0f, -1.0f), Vec3(0.0f, 1.0f, 0.0f));
    const sim::ViewPlanes planes = sim::viewPlanesOf(viewProjection);
    CHECK(sim::ballSeen(planes, Vec3(0.0f, 0.0f, -10.0f), 0.1f));       // ahead
    CHECK(!sim::ballSeen(planes, Vec3(0.0f, 0.0f, 10.0f), 1.0f));       // behind
    CHECK(!sim::ballSeen(planes, Vec3(20.0f, 0.0f, -10.0f), 1.0f));     // well off to the right
    CHECK(sim::ballSeen(planes, Vec3(9.0f, 0.0f, -10.0f), 1.0f));       // its edge just in: x/z = tan(30) * 1.5 = 0.87
    CHECK(!sim::ballSeen(planes, Vec3(0.0f, 0.0f, -150.0f), 10.0f));    // past the far plane
    CHECK(sim::ballSeen(planes, Vec3(0.0f, 0.0f, -105.0f), 10.0f));     // ... reaching back over it

    // Copies of a prototype a metre round its middle half a metre up: those
    // seen first, each part in its order.
    constexpr size_t n = sim::DisplayInstances::kFloats;
    std::vector<float> placements;
    const float xs[] = {30.0f, 0.0f, -40.0f, 2.0f, 0.0f};
    const float zs[] = {-10.0f, -5.0f, -10.0f, -20.0f, 5.0f};
    for (size_t i = 0; i < 5; ++i) {
        placements.insert(placements.end(), {xs[i], 0.0f, zs[i], 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f});
    }
    std::vector<float> sorted;
    const size_t seen = sim::seenFirst(placements, sorted, Vec3(0.0f, 0.5f, 0.0f), 0.5f, planes);
    CHECK_EQ(seen, 2u);
    CHECK_EQ(sorted.size(), placements.size());
    const float order[] = {-5.0f, -20.0f, -10.0f, -10.0f, 5.0f};  // seen: 2 and 4; then 1, 3, 5
    for (size_t i = 0; i < 5; ++i) CHECK_EQ(sorted[i * n + 2], order[i]);
    CHECK_EQ(sorted[2 * n], 30.0f);
    CHECK_EQ(sorted[3 * n], -40.0f);

    // The meadow as one standing in it sees it, at eye height, looking along
    // the path: most of its 120 000 clumps and its trees out of the view.
    sim::Network meadow;
    CHECK(sim::Network::example("meadow", meadow));
    sim::GeometryGraph g;
    g.sync(meadow);
    const GeometryPtr all = g.cook(meadow.displayed(), 1);
    CHECK(all != nullptr);
    const sim::DisplayInstances inst = sim::instancesOf(*all);
    const Vec3 eye(-10.0f, 3.4f, 4.0f);
    const sim::ViewPlanes view = sim::viewPlanesOf(glm::perspective(glm::radians(45.0f), 1.6f, 0.05f, 500.0f) *
                                                   glm::lookAt(eye, eye + Vec3(1.0f, -0.25f, 0.2f), Vec3(0.0f, 1.0f, 0.0f)));
    size_t total = 0, inView = 0;
    for (size_t k = 0; k < inst.prototypes.size(); ++k) {
        std::vector<float> out;
        inView += sim::seenFirst(inst.placements[k], out, inst.centers[k], inst.radii[k], view);
        total += inst.placements[k].size() / n;
    }
    std::printf("  the meadow from the path: %zu copies of %zu in view (%.0f %%)\n", inView, total,
                100.0 * static_cast<double>(inView) / static_cast<double>(total));
    CHECK(total > 100000);
    CHECK(inView > 0 && inView < total / 2);
}

TEST(foliage_the_viewport_lays_pictures_on_as_the_renderers_do) {
    // A tree: its bark and its leaves, by uv -- the leaves cut out.
    TreeSettings ts;
    ts.levels = 1;
    auto tree = std::make_shared<Geometry>();
    meshTree(growTree(ts, Vec3(), 1.0f, 3), ts, 0, *tree);
    sim::DisplayMesher mesher;
    sim::DisplayMesh mesh;
    mesher.make(tree, mesh);
    CHECK_EQ(mesh.pictures.size(), 2u);
    CHECK_EQ(mesh.textures.size(), 5 * mesh.vertexCount());
    bool barkPicture = false, leafPicture = false;
    for (const sim::DisplayPicture& p : mesh.pictures) {
        if (p.color.find("bark") != std::string::npos) barkPicture = p.alpha.empty() && !p.normal.empty();
        if (p.color.find("leaf") != std::string::npos) leafPicture = !p.alpha.empty() && p.alphaChannel;
    }
    CHECK(barkPicture && leafPicture);
    for (size_t v = 0; v < mesh.vertexCount(); ++v) {
        CHECK(mesh.textures[5 * v + 4] >= 0.0f);  // every face pictured
        CHECK(mesh.textures[5 * v + 3] == 0.0f);  // by uv
    }
    // As the renderers have it, a vertex's uv its corner's: the leaves' in
    // the top half of the leaf picture.
    for (size_t v = 0; v < mesh.vertexCount(); ++v) {
        if (mesh.translucency[v] > 0.0f) CHECK(mesh.textures[5 * v + 1] >= 0.5f - 1e-6f);
    }

    // A box of concrete, no uv, no Cd: from three sides, the library's
    // picture three metres across -- and the colour the renderers give
    // concrete, not the viewport's grey.
    auto box = std::make_shared<Geometry>();
    box->addPoints(3);
    auto P = box->positionsForWrite();
    P[1] = Vec3(1.0f, 0.0f, 0.0f);
    P[2] = Vec3(0.0f, 1.0f, 0.0f);
    const uint32_t tri[3] = {0, 1, 2};
    box->addPrimitive(tri, true);
    setPrimitiveString(*box, "material", "concrete");
    sim::DisplayMesher boxMesher;
    sim::DisplayMesh boxMesh;
    boxMesher.make(box, boxMesh);
    CHECK_EQ(boxMesh.pictures.size(), 1u);
    CHECK(boxMesh.textures.size() == 15 && boxMesh.textures[3] == 3.0f && boxMesh.textures[4] == 0.0f);
    CHECK(boxMesh.textures[5] == 1.0f && boxMesh.textures[6] == 0.0f);  // the second corner's place
    const Vec3 concrete = render::presetSurface(MaterialPreset::Concrete).color;
    CHECK(boxMesh.colors[0] == concrete.x && boxMesh.colors[1] == concrete.y && boxMesh.colors[2] == concrete.z);
    // A leaf without uv: no picture -- it is drawn for uv alone.
    setPrimitiveString(*box, "material", "leaf");
    sim::DisplayMesher leafMesher;
    sim::DisplayMesh leafMesh;
    leafMesher.make(box, leafMesh);
    CHECK(leafMesh.pictures.empty() && leafMesh.textures.empty());
}

TEST(foliage_the_wind_bows_plants_from_their_feet) {
    // A tree and the ground merged: the ground (flex 0) stays; the tree's
    // foot stays, every point as far from it as before -- turned, not
    // stretched -- and its top bowed the way of the wind; v how fast.
    TreeSettings ts;
    ts.levels = 2;
    Geometry tree;
    meshTree(growTree(ts, Vec3(), 1.0f, 2), ts, 0, tree);
    Geometry ground;
    ground.addPoints(3);
    {
        auto P = ground.positionsForWrite();
        P[0] = Vec3(-5.0f, 0.0f, -5.0f);
        P[1] = Vec3(5.0f, 0.0f, -5.0f);
        P[2] = Vec3(0.0f, 0.0f, 5.0f);
        const uint32_t tri[3] = {0, 1, 2};
        ground.addPrimitive(tri, true);
        ground.points().create("flex", AttrType::Float);
        ground.primitives().create("tree", AttrType::Int);
    }
    Geometry both = ground;
    both.append(tree);
    WindSettings s;
    s.turbulence = 0.0f;
    s.flutter = 0.0f;
    Geometry blown = both;
    blowPlants(blown, s, 0.7f);
    const auto before = both.positions(), after = blown.positions();
    const auto flex = both.points().find("flex")->read<float>();
    for (size_t p = 0; p < 3; ++p) CHECK(after[p] == before[p]);
    // The foot: the trunk's ring at the ground.
    Vec3 foot(0.0f);
    float least = 1e9f;
    for (size_t p = 3; p < before.size(); ++p) {
        if (flex[p] < least) least = flex[p], foot = before[p];
    }
    float stretched = 0.0f, topMoved = 0.0f;
    Vec3 top = foot;
    size_t topAt = 0;
    for (size_t p = 3; p < before.size(); ++p) {
        stretched = std::max(stretched, std::fabs(length(after[p] - foot) - length(before[p] - foot)));
        if (before[p].y > top.y) top = before[p], topAt = p;
    }
    topMoved = after[topAt].x - before[topAt].x;
    const Vec3 bend = windBend(s, foot, 0.7f, 0);
    std::printf("  the top %.2f m up bowed %.3f m along the wind (bend %.3f rad), stretched at most %.2g m\n", top.y,
                topMoved, length(bend), stretched);
    CHECK(stretched < 1e-4f);
    CHECK(topMoved > 0.5f * top.y * std::sin(0.5f * length(bend)));
    CHECK(std::fabs(after[topAt].z - before[topAt].z) < 0.05f * topMoved);  // along +x, the wind's way
    const AttributeArray* v = blown.points().find("v");
    CHECK(v != nullptr);
    Geometry later = both;
    blowPlants(later, s, 0.7f + 1.0f / 240.0f);
    const Vec3 moved = (later.positions()[topAt] - after[topAt]) * 240.0f;
    CHECK(length(v->read<Vec3>()[topAt] - moved) < 1e-3f + 0.01f * length(moved));

    // Gusts run along the wind at their speed: a plant gust-speed metres
    // on, a second later, bows as this one does now.
    WindSettings steady = s;
    steady.gust = 1.0f;
    for (const float t : {0.0f, 0.4f, 1.3f}) {
        const Vec3 here = windBend(steady, Vec3(2.0f, 0.0f, 3.0f), t, 5);
        const Vec3 there = windBend(steady, Vec3(2.0f + steady.gustSpeed, 0.0f, 3.0f), t + 1.0f, 5);
        CHECK(length(here - there) < 1e-4f);
    }

    // Points standing for clumps: the places shared, the clumps bent ahead
    // into a few shapes -- the same plants frame after frame -- the points
    // standing for them, tilted the rest of the way.
    registerBuiltinNodes();
    Graph g;
    Node* grass = g.create("grass", "grass");
    grass->setInt("variants", 3);
    Node* grid = g.create("grid", "ground");
    grid->setFloat("sizex", 10.0f);
    grid->setFloat("sizez", 10.0f);
    CHECK(grass->setInput(0, grid));
    Node* wind = g.create("plantwind", "wind");
    CHECK(wind->setInput(0, grass));
    CookEngine engine;
    CookContext at1, at2;
    at1.time = 0.5;
    at2.time = 1.5;
    const GeometryPtr meadow = engine.cook(*grass, at1);
    const GeometryPtr a = engine.cook(*wind, at1), b = engine.cook(*wind, at2);
    CHECK(a && b && a->pointCount() == meadow->pointCount());
    CHECK(a->positions().data() == meadow->positions().data());
    CHECK(a->prototypeCount() > 3 && a->prototypeCount() <= 3 * (1 + 8 * 4));
    for (size_t k = 0; k < 3; ++k) CHECK(a->prototypes()[k] == meadow->prototypes()[k]);
    std::set<const Geometry*> shapesA, shapesB;
    for (size_t k = 3; k < a->prototypeCount(); ++k) shapesA.insert(a->prototypes()[k].get());
    for (size_t k = 3; k < b->prototypeCount(); ++k) shapesB.insert(b->prototypes()[k].get());
    size_t again = 0;
    for (const Geometry* shape : shapesB) again += shapesA.count(shape);
    std::printf("  %zu clumps: %zu bent shapes at 0.5 s, %zu at 1.5 s, %zu of them the same plants\n", a->pointCount(),
                shapesA.size(), shapesB.size(), again);
    CHECK(again > 0);
    const auto instance = a->points().find("instance")->read<int32_t>();
    for (const int32_t k : instance) CHECK(k >= 0 && static_cast<size_t>(k) < a->prototypeCount());
    // Each clump's blades bowed the way of the wind, near enough: the bent
    // shape's tips, turned as the point is, go along +x.
    const auto orient = a->points().find("orient")->read<Vec4>();
    size_t along = 0, bent = 0;
    for (size_t p = 0; p < a->pointCount(); ++p) {
        if (instance[p] < 3) continue;
        ++bent;
        const Geometry& shape = *a->prototypes()[static_cast<size_t>(instance[p])];
        const Geometry& was = *meadow->prototypes()[static_cast<size_t>(meadow->points().find("instance")->read<int32_t>()[p])];
        Vec3 shift(0.0f);
        for (size_t q = 0; q < shape.pointCount(); ++q) shift = shift + (shape.positions()[q] - was.positions()[q]);
        if (quatRotate(orient[p], shift).x > 0.0f) ++along;
    }
    CHECK(bent > 0 && along > bent * 9 / 10);

    // The meadow example in the wind: its first frame, and those after --
    // only the points that stand for plants made again.
    sim::Network meadowNet;
    CHECK(sim::Network::example("meadow", meadowNet));
    sim::GeometryGraph graph;
    graph.sync(meadowNet);
    const auto t0 = std::chrono::steady_clock::now();
    const GeometryPtr first = graph.cook(meadowNet.displayed(), 1);
    const auto t1 = std::chrono::steady_clock::now();
    for (int frame = 2; frame <= 6; ++frame) CHECK(graph.cook(meadowNet.displayed(), frame) != nullptr);
    const auto t2 = std::chrono::steady_clock::now();
    const GeometryPtr sixth = graph.cook(meadowNet.displayed(), 6);
    std::printf("  the meadow: its first frame %.0f ms, each after %.0f ms; %zu plants held (of %zu grown)\n",
                std::chrono::duration<double, std::milli>(t1 - t0).count(),
                std::chrono::duration<double, std::milli>(t2 - t1).count() / 5.0, sixth->prototypeCount(), size_t(21));
    CHECK(first && sixth && first->pointCount() == sixth->pointCount());
}

TEST(foliage_an_ecosystem_sorts_its_kinds_by_the_ground_and_the_shade) {
    // The example: what is left after 120 years -- each kind most where the
    // ground suits it, the hazels under the trees in the shade.
    sim::Network net;
    CHECK(sim::Network::example("ecosystem", net));
    int places = -1, wood = -1;
    for (const auto& n : net.nodes()) {
        if (n.name == "places") places = n.id;
        if (n.name == "wood") wood = n.id;
    }
    CHECK(places >= 0 && wood >= 0);
    sim::GeometryGraph g;
    g.sync(net);
    const GeometryPtr ground = g.cook(places, 1);
    const GeometryPtr plants = g.cook(wood, 1);
    CHECK(ground && plants && plants->pointCount() > 0);
    const auto wet = ground->points().find("moisture")->read<float>();
    const auto species = plants->points().find("species")->read<int32_t>();
    const auto id = plants->points().find("id")->read<int32_t>();
    const auto age = plants->points().find("age")->read<float>();
    const auto light = plants->points().find("light")->read<float>();
    double moisture[4] = {0, 0, 0, 0}, ages[4] = {0, 0, 0, 0}, lit[4] = {0, 0, 0, 0};
    size_t count[4] = {0, 0, 0, 0};
    for (size_t i = 0; i < plants->pointCount(); ++i) {
        const int k = species[i];
        CHECK(k >= 0 && k < 4);
        CHECK(plants->findGroup("species" + std::to_string(k + 1))->contains(i));
        moisture[k] += wet[static_cast<size_t>(id[i])];
        ages[k] += age[i];
        lit[k] += light[i];
        ++count[k];
    }
    const char* names[4] = {"birches", "oaks", "spruces", "hazels"};
    std::printf("  %zu places, %zu plants after 120 years:", ground->pointCount(), plants->pointCount());
    for (int k = 0; k < 4; ++k) {
        const double n = static_cast<double>(std::max<size_t>(count[k], 1));
        std::printf("%s %s %zu (ground %.2f wet, %.0f years, light %.2f)", k == 0 ? "" : ",", names[k], count[k],
                    moisture[k] / n, ages[k] / n, lit[k] / n);
    }
    std::printf("\n");
    for (int k = 0; k < 4; ++k) CHECK(count[k] > 0);
    CHECK(moisture[2] / count[2] > moisture[1] / count[1] + 0.1);  // spruces wetter than oaks
    CHECK(lit[3] / count[3] < 0.5 * lit[2] / count[2]);           // hazels in the shade of the trees
}

TEST(foliage_trees_are_pruned_to_their_envelope_and_stand_on_roots) {
    // Pruned: the branches kept to the envelope, the wild tree's reach out
    // of it.
    TreeSettings wild;
    wild.levels = 2;
    wild.height = 6.0f;
    TreeSettings kept = wild;
    kept.prune = 1.0f;
    kept.pruneWidth = 0.3f;
    auto inside = [&](const Tree& t) {
        size_t in = 0, all = 0;
        for (const TreeStem& st : t.stems) {
            if (st.level == 0) continue;
            for (const Vec3& p : st.points) {
                const float up = (p.y - kept.crown * t.height) / ((1.0f - kept.crown) * t.height);
                const float shape = up < 0.5f ? std::pow(std::max(up, 0.0f) / 0.5f, 0.5f) : std::pow(std::max(1.0f - up, 0.0f) / 0.5f, 0.5f);
                in += std::hypot(p.x, p.z) <= kept.pruneWidth * t.height * shape + 0.05f * t.height ? 1 : 0;
                ++all;
            }
        }
        return static_cast<double>(in) / static_cast<double>(std::max<size_t>(all, 1));
    };
    const double wildIn = inside(growTree(wild, Vec3(), 1.0f, 3)), keptIn = inside(growTree(kept, Vec3(), 1.0f, 3));
    std::printf("  branch points in the envelope: wild %.0f %%, pruned %.0f %%\n", 100.0 * wildIn, 100.0 * keptIn);
    CHECK(keptIn > 0.95 && wildIn < 0.8);

    // Roots: out from the foot, down into the ground at their tips, no
    // leaves on them, still in the wind (flex 0).
    TreeSettings rooted = wild;
    rooted.roots = 6;
    rooted.leaves = 5;
    const Tree t = growTree(rooted, Vec3(), 1.0f, 3);
    size_t roots = 0;
    for (size_t i = 0; i < t.stems.size(); ++i) {
        const TreeStem& st = t.stems[i];
        if (!st.root) continue;
        ++roots;
        CHECK(st.points.front().y > 0.0f && st.points.back().y < 0.0f);
        CHECK(std::hypot(st.points.back().x, st.points.back().z) > std::hypot(st.points.front().x, st.points.front().z));
        for (const TreeLeaf& leaf : t.leaves) CHECK(leaf.stem != static_cast<int>(i));
    }
    CHECK_EQ(roots, size_t(6));
    Geometry geo;
    meshTree(t, rooted, 0, geo);
    const auto flex = geo.points().find("flex")->read<float>();
    const auto P = geo.positions();
    size_t below = 0;
    for (size_t p = 0; p < P.size(); ++p) {
        if (P[p].y < -0.05f) {
            ++below;
            CHECK(flex[p] == 0.0f);
        }
    }
    CHECK(below > 0);
}

TEST(foliage_trodden_grass_lies_down_and_gets_up_again) {
    // Clumps on a lawn, a foot set down at the middle at 1 s: those round
    // it bowed away from it, those far off not; before 1 s none; long
    // after, upright again.
    registerBuiltinNodes();
    Graph g;
    Node* grid = g.create("grid", "lawn");
    grid->setFloat("sizex", 3.0f);
    grid->setFloat("sizez", 3.0f);
    Node* grass = g.create("grass", "grass");
    grass->setFloat("density", 30.0f);
    grass->setInt("variants", 2);
    CHECK(grass->setInput(0, grid));
    auto foot = std::make_shared<Geometry>();
    foot->addPoints(1);
    foot->points().create("time", AttrType::Float).write<float>()[0] = 1.0f;
    foot->points().create("pscale", AttrType::Float).write<float>()[0] = 1.5f;  // 0.6 m across
    Node* trample = g.create("planttrample", "trample");
    CHECK(trample->setInput(0, grass));
    CookEngine engine;
    const GeometryPtr lawn = engine.cook(*grass, CookContext{});
    auto at = [&](double time) {
        CookContext c;
        c.time = time;
        const GeometryPtr in[2] = {lawn, foot};
        return trample->cookNode(c, in);
    };
    // How far each clump's blades lean away from the foot: their points'
    // shift from the upright clump, along the way from the foot, at their
    // place.
    auto lean = [&](const GeometryPtr& g2, size_t p) {
        const auto inst = g2->points().find("instance")->read<int32_t>();
        const Geometry& shape = *g2->prototypes()[static_cast<size_t>(inst[p])];
        const Geometry& was = *lawn->prototypes()[static_cast<size_t>(lawn->points().find("instance")->read<int32_t>()[p])];
        Vec3 shift(0.0f);
        for (size_t q = 0; q < shape.pointCount(); ++q) shift = shift + (shape.positions()[q] - was.positions()[q]);
        shift = quatRotate(g2->points().find("orient")->read<Vec4>()[p], shift) / static_cast<float>(shape.pointCount());
        Vec3 away = lawn->positions()[p];
        away.y = 0.0f;
        return length(away) > 1e-4f ? dot(shift, normalize(away)) : 0.0f;
    };
    const GeometryPtr before = at(0.5), during = at(1.2), after = at(40.0);
    float nearLean = 0.0f, farLean = 0.0f, beforeLean = 0.0f, afterLean = 0.0f;
    size_t nearCount = 0, farCount = 0;
    for (size_t p = 0; p < lawn->pointCount(); ++p) {
        const Vec3 q = lawn->positions()[p];
        const float d = std::hypot(q.x, q.z);
        if (d < 0.35f && d > 0.05f) {
            nearLean += lean(during, p);
            beforeLean += std::fabs(lean(before, p));
            afterLean += std::fabs(lean(after, p));
            ++nearCount;
        } else if (d > 1.0f) {
            farLean += std::fabs(lean(during, p));
            ++farCount;
        }
    }
    std::printf("  %zu clumps by the foot lean away %.3f m on average, %zu far off %.4f; before %.4f, long after %.4f\n",
                nearCount, nearLean / nearCount, farCount, farLean / farCount, beforeLean / nearCount, afterLean / nearCount);
    CHECK(nearCount > 3 && farCount > 3);
    CHECK(nearLean / nearCount > 0.03f);
    CHECK(farLean / farCount < 1e-4f && beforeLean / nearCount < 1e-4f && afterLean / nearCount < 1e-3f);
}

TEST(foliage_an_ecosystem_is_the_same_for_the_same_seed) {
    std::vector<Vec3> places;
    std::vector<float> wet;
    for (int x = 0; x < 60; ++x) {
        for (int z = 0; z < 60; ++z) {
            places.push_back(Vec3(static_cast<float>(x), 0.0f, static_cast<float>(z)));
            wet.push_back(static_cast<float>(x) / 59.0f);
        }
    }
    EcosystemSettings s;
    s.species = {Species{}, Species{}};
    s.species[0].moisture = 0.1f;
    s.species[1].moisture = 0.9f;
    s.species[0].tolerance = s.species[1].tolerance = 0.25f;
    s.years = 60;
    const auto a = growEcosystem(places, wet, s), b = growEcosystem(places, wet, s);
    CHECK(a.size() == b.size() && !a.empty());
    for (size_t i = 0; i < a.size() && i < b.size(); ++i) CHECK(a[i].place == b[i].place && a[i].species == b[i].species);
    // Each on its side: the dry kind west, the wet east.
    double x[2] = {0, 0};
    size_t n[2] = {0, 0};
    for (const EcoPlant& p : a) x[p.species] += places[p.place].x, ++n[p.species];
    CHECK(n[0] > 0 && n[1] > 0 && x[0] / n[0] < 20.0 && x[1] / n[1] > 40.0);
    // Grown crowns hardly over each other: shade thins them.
    size_t grown = 0, crowded = 0;
    for (const EcoPlant& p : a) {
        if (p.size < 1.0f) continue;
        ++grown;
        for (const EcoPlant& q : a) {
            if (&q != &p && q.size >= 1.0f && length(places[p.place] - places[q.place]) < 1.0f * s.species[0].crown) ++crowded;
        }
    }
    std::printf("  %zu plants, %zu grown, %zu pairs of grown ones within a crown's radius\n", a.size(), grown, crowded / 2);
    CHECK(grown > 0 && crowded / 2 < grown / 4 + 1);
}
