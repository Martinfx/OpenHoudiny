//
// Foliage as the renderers see it: the uv of trees and grass -- round and up
// the bark in whole pictures, square at a stem's foot; each leaf in its
// quarter of the leaf picture; once across and up a blade of grass -- none
// mirrored, and the library's bark, leaves and grass laid on by it; leaves
// cut out by their picture's alpha -- seen through and letting the sun
// through there, in both renderers and both ray engines.
//
#include "pg/core/CookEngine.h"
#include "pg/core/Graph.h"
#include "pg/core/Grass.h"
#include "pg/core/Material.h"
#include "pg/core/Tree.h"
#include "pg/io/Picture.h"
#include "pg/nodes/Nodes.h"
#include "pg/render/Cycles.h"
#include "pg/render/Embree.h"
#include "pg/render/PathTracer.h"
#include "pg/render/Scene.h"
#include "pg/render/Textures.h"

#include "test_framework.h"

#include <algorithm>
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
