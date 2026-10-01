//
// What surfaces are made of (core/Material.h) and the pictures laid on them
// (render/Textures.h): a new string attribute reads "" where nothing was
// written, merged or not; the nodes that make surfaces say what they are --
// Brick Wall, Concrete Fracture, Glass Fracture, Tree, Grass, Rebar -- and
// the Material node any; the pieces of the RBD Solver keep where they were
// (rest), their cut faces broken concrete, their bars steel; the renderers'
// meshes take the materials' roughness, their textures and the windows'
// numbers, and geometry with no colour its materials' own; texture sets are
// found from a folder or from one picture of them, the library's for each
// material; both renderers lay them on -- rows along a sloping face level.
//
#include "pg/core/CookEngine.h"
#include "pg/core/Graph.h"
#include "pg/core/Grass.h"
#include "pg/core/Material.h"
#include "pg/core/Tree.h"
#include "pg/io/Picture.h"
#include "pg/nodes/Nodes.h"
#include "pg/render/Cycles.h"
#include "pg/render/PathTracer.h"
#include "pg/render/Scene.h"
#include "pg/render/Textures.h"
#include "pg/sim/Network.h"
#include "pg/sim/Rigid.h"

#include "test_framework.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <set>

using namespace pg;
namespace fs = std::filesystem;

namespace {

/// A node of `type` set up by `setup`, cooked over `input`.
GeometryPtr cooked(const std::string& type, GeometryPtr input, const std::function<void(Node&)>& setup) {
    registerBuiltinNodes();
    Graph g;
    Node* node = g.create(type, "node");
    setup(*node);
    const GeometryPtr in[1] = {std::move(input)};
    return node->cookNode(CookContext{}, in);
}

/// A box of six faces, 1 m.
GeometryPtr box() {
    registerBuiltinNodes();
    Graph g;
    Node* b = g.create("box", "box");
    b->setInt("divisions", 1);
    CookEngine engine;
    return engine.cook(*b, CookContext{});
}

/// A wrangle's snippet run over `cls` (0 points, 1 primitives, 3 detail) of `input`.
GeometryPtr wrangled(GeometryPtr input, const std::string& snippet, int cls) {
    return cooked("attribwrangle", std::move(input), [&](Node& n) {
        n.setString("snippet", snippet);
        n.setInt("runover", cls);
    });
}

/// What each primitive of `geo` is made of.
std::vector<std::string> materials(const Geometry& geo) {
    std::vector<std::string> out;
    for (size_t p = 0; p < geo.primitiveCount(); ++p) out.push_back(primitiveString(geo, "material", p));
    return out;
}

std::set<std::string> kinds(const Geometry& geo) {
    const auto all = materials(geo);
    return {all.begin(), all.end()};
}

/// A picture of `width` x `height`, `pixel(x, y)` its colour (as shown), to `path`.
void writeTestPicture(const std::string& path, int width, int height, const std::function<Vec3(int, int)>& pixel) {
    io::Picture p;
    p.width = width;
    p.height = height;
    p.rgba.resize(static_cast<size_t>(width) * height * 4);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const Vec3 c = pixel(x, y);
            float* q = &p.rgba[(static_cast<size_t>(y) * width + x) * 4];
            q[0] = c.x;
            q[1] = c.y;
            q[2] = c.z;
            q[3] = 1.0f;
        }
    }
    std::string error;
    CHECK(io::writePicture(path, p, 95, error));
}

/// A square of ground, 4 m, facing up, its colour Cd; `material` its s@material.
std::shared_ptr<Geometry> ground(const Vec3& color, const std::string& material) {
    auto geo = std::make_shared<Geometry>();
    geo->addPoints(4);
    auto P = geo->positionsForWrite();
    P[0] = Vec3(-2.0f, 0.01f, -2.0f);
    P[1] = Vec3(-2.0f, 0.01f, 2.0f);
    P[2] = Vec3(2.0f, 0.01f, 2.0f);
    P[3] = Vec3(2.0f, 0.01f, -2.0f);
    const uint32_t quad[4] = {0, 1, 2, 3};
    geo->addPrimitive(quad, true);
    auto cd = geo->primitives().create("Cd", AttrType::Vec3).write<Vec3>();
    cd[0] = color;
    if (!material.empty()) setPrimitiveString(*geo, "material", material);
    return geo;
}

render::Image cycles(const std::shared_ptr<const render::Scene>& scene, const render::Settings& s) {
    render::CyclesRender r;
    r.start(scene, s);
    r.wait();
    CHECK(r.error().empty());
    return r.beauty();
}

/// How much a picture's green varies over its mean, and the mean.
double variation(const render::Image& img, double& mean) {
    double sum = 0.0, square = 0.0;
    const size_t n = static_cast<size_t>(img.width) * img.height;
    for (size_t p = 0; p < n; ++p) {
        const double g = img.pixels[3 * p + 1];
        sum += g;
        square += g * g;
    }
    mean = sum / static_cast<double>(n);
    return std::sqrt(std::max(square / static_cast<double>(n) - mean * mean, 0.0)) / std::max(mean, 1e-9);
}

}  // namespace

TEST(materials_a_new_string_reads_empty_where_nothing_was_written) {
    // A wrangle over primitives writing one of them: the others "", not
    // what the first written was.
    const GeometryPtr one = wrangled(box(), "if (@primnum == 2) s@material = \"roof\";", 1);
    CHECK_EQ(one->primitiveCount(), 6u);
    for (size_t p = 0; p < 6; ++p) CHECK_EQ(primitiveString(*one, "material", p), p == 2 ? "roof" : "");
    // setprimattrib from a detail wrangle, the same.
    const GeometryPtr two = wrangled(box(), "setprimattrib(0, \"material\", 4, \"window\");", 3);
    for (size_t p = 0; p < 6; ++p) CHECK_EQ(primitiveString(*two, "material", p), p == 4 ? "window" : "");
    // Merged with geometry without it -- before or after -- those read "".
    Geometry a = *one, b = *box();
    Geometry ab = a, ba = b;
    ab.append(b);
    ba.append(a);
    for (size_t p = 0; p < 12; ++p) {
        CHECK_EQ(primitiveString(ab, "material", p), p == 2 ? "roof" : "");
        CHECK_EQ(primitiveString(ba, "material", p), p == 8 ? "roof" : "");
    }
    // Merged with geometry whose first string is another: each its own.
    Geometry c = *wrangled(box(), "s@material = @primnum < 3 ? \"brick\" : \"mortar\";", 1);
    Geometry ac = a;
    ac.append(c);
    CHECK_EQ(primitiveString(ac, "material", 0), "");
    CHECK_EQ(primitiveString(ac, "material", 6), "brick");
    CHECK_EQ(primitiveString(ac, "material", 11), "mortar");
}

TEST(materials_the_nodes_that_make_surfaces_say_what_they_are) {
    registerBuiltinNodes();
    // Brick Wall: bricks, mortar, plaster.
    {
        Graph g;
        Node* b = g.create("box", "box");
        b->setVec3("size", Vec3(2.08f, 1.2f, 0.25f));
        b->setVec3("center", Vec3(0.0f, 0.6f, 0.0f));
        Node* wall = g.create("brickwall", "wall");
        wall->setInput(0, b);
        wall->setFloat("plaster", 0.015f);  // 15 mm of it
        CookEngine engine;
        const auto made = kinds(*engine.cook(*wall, CookContext{}));
        CHECK(made.count("brick") == 1);
        CHECK(made.count("mortar") == 1);
        CHECK(made.count("plaster") == 1);
        CHECK(made.count("") == 0);
    }
    // Concrete Fracture: concrete outside, broken concrete where it cut.
    {
        Graph g;
        Node* b = g.create("box", "box");
        Node* f = g.create("concretefracture", "fracture");
        f->setInput(0, b);
        f->setInt("count", 6);
        CookEngine engine;
        const GeometryPtr pieces = engine.cook(*f, CookContext{});
        const Group* cut = pieces->findGroup("inside");
        CHECK(cut != nullptr);
        size_t broken = 0, whole = 0;
        for (size_t p = 0; p < pieces->primitiveCount(); ++p) {
            const std::string& m = primitiveString(*pieces, "material", p);
            if (cut->contains(p)) {
                CHECK_EQ(m, "broken_concrete");
                ++broken;
            } else {
                CHECK_EQ(m, "concrete");
                ++whole;
            }
        }
        CHECK(broken > 0 && whole > 0);
    }
    // Glass Fracture: glass, cracks and all.
    {
        Graph g;
        Node* b = g.create("box", "pane");
        b->setVec3("size", Vec3(1.0f, 1.0f, 0.01f));
        Node* f = g.create("glassfracture", "glass");
        f->setInput(0, b);
        CookEngine engine;
        CHECK(kinds(*engine.cook(*f, CookContext{})) == std::set<std::string>{"glass"});
    }
    // Rebar: steel.
    {
        Graph g;
        Node* b = g.create("box", "block");
        b->setVec3("size", Vec3(1.0f, 1.0f, 0.2f));
        Node* r = g.create("rebar", "bars");
        r->setInput(0, b);
        CookEngine engine;
        const GeometryPtr bars = engine.cook(*r, CookContext{});
        CHECK(bars->primitiveCount() > 0);
        CHECK(kinds(*bars) == std::set<std::string>{"steel"});
    }
    // A tree: bark and leaves; grass, grass.
    {
        const TreeSettings s;
        Geometry geo;
        meshTree(growTree(s, Vec3(), 1.0f, 3), s, 0, geo);
        meshTree(growTree(s, Vec3(3.0f, 0.0f, 0.0f), 1.0f, 4), s, 1, geo);
        const auto level = geo.primitives().find("level")->read<int32_t>();
        for (size_t p = 0; p < geo.primitiveCount(); ++p) {
            CHECK_EQ(primitiveString(geo, "material", p), level[p] < 0 ? "leaf" : "bark");
        }
        CHECK(kinds(growGrassClump(GrassSettings(), 5)) == std::set<std::string>{"grass"});
    }
}

TEST(materials_the_material_node_says_what_a_group_is_and_lays_a_texture) {
    // A group of three faces brick wall, the rest none.
    const GeometryPtr some = cooked("material", box(), [](Node& n) {
        n.setString("group", "0-2");
        n.setInt("material", static_cast<int>(MaterialPreset::BrickWall));
    });
    for (size_t p = 0; p < 6; ++p) CHECK_EQ(primitiveString(*some, "material", p), p < 3 ? "brick_wall" : "");
    // None takes it away; a group there is not, an error and no change.
    const GeometryPtr none = cooked("material", some, [](Node& n) { n.setInt("material", 0); });
    CHECK(kinds(*none) == std::set<std::string>{""});
    registerBuiltinNodes();
    Graph g;
    Node* b = g.create("box", "box");
    Node* m = g.create("material", "m");
    m->setInput(0, b);
    m->setString("group", "nothing_of_that_name");
    CookEngine engine;
    engine.cook(*m, CookContext{});
    CHECK(m->cookError().find("no primitive group") != std::string::npos);
    // A texture of one's own: its picture, size and tint on the faces.
    const GeometryPtr textured = cooked("material", box(), [](Node& n) {
        n.setString("group", "5");
        n.setInt("material", static_cast<int>(MaterialPreset::Plaster));
        n.setString("texture", "/somewhere/wall_diff_1k.jpg");
        n.setFloat("texture_size", 1.5f);
        n.setBool("texture_tint", true);
    });
    CHECK_EQ(primitiveString(*textured, "texture", 5), "/somewhere/wall_diff_1k.jpg");
    CHECK_EQ(primitiveString(*textured, "texture", 0), "");
    CHECK_NEAR(textured->primitives().find("texture_size")->read<float>()[5], 1.5f, 1e-6f);
    CHECK_EQ(textured->primitives().find("texture_tint")->read<int32_t>()[5], 1);
    // The network's choices are the materials' names, "none" for "".
    const sim::NodeType* type = sim::findNodeType("material");
    CHECK(type != nullptr);
    for (const sim::ParamDef& d : type->params) {
        if (std::string(d.name) != "material") continue;
        CHECK_EQ(d.choices.size(), kMaterialPresets);
        for (size_t i = 1; i < kMaterialPresets; ++i) CHECK_EQ(std::string(d.choices[i]), std::string(kMaterialNames[i]));
        CHECK_EQ(std::string(d.choices[0]), "none");
    }
}

TEST(materials_pieces_keep_where_they_were_their_cuts_broken_concrete) {
    CHECK(sim::rigidAvailable());
    // A plastered box broken in pieces, and a brick one: what the cut faces
    // are when they are drawn.
    registerBuiltinNodes();
    Graph g;
    Node* b = g.create("box", "box");
    b->setInt("divisions", 1);
    b->setVec3("center", Vec3(0.0f, 2.0f, 0.0f));
    Node* paint = g.create("material", "paint");
    paint->setInput(0, b);
    paint->setInt("material", static_cast<int>(MaterialPreset::Plaster));
    Node* f = g.create("voronoifracture", "fracture");
    f->setInput(0, paint);
    f->setInt("count", 6);
    CookEngine engine;
    sim::RigidScene scene;
    scene.pieces = engine.cook(*f, CookContext{});
    scene.solver.glue = 0.0f;
    sim::RigidSolver solver(scene);
    CHECK(solver.error().empty());
    for (int i = 0; i < 10; ++i) solver.step();
    const sim::RigidFrame frame = solver.capture();
    // Posed: moved, the rest where they were.
    const GeometryPtr posed = sim::posedPieces(frame);
    const AttributeArray* rest = posed->points().find("rest");
    CHECK(rest != nullptr && rest->type() == AttrType::Vec3);
    const auto R = rest->read<Vec3>();
    const auto P0 = scene.pieces->positions();
    const auto P = posed->positions();
    float moved = 0.0f;
    for (size_t i = 0; i < P.size(); ++i) {
        CHECK(length(R[i] - P0[i]) < 1e-5f);
        moved = std::max(moved, length(P[i] - P0[i]));
    }
    CHECK(moved > 0.1f);
    // Drawn: the cut faces broken concrete, the rest plaster.
    const GeometryPtr drawn = sim::drawnPieces(frame, Vec3(0.5f), Vec3(0.6f), "inside");
    const Group* cut = drawn->findGroup("inside");
    CHECK(cut != nullptr);
    size_t broken = 0;
    for (size_t p = 0; p < drawn->primitiveCount(); ++p) {
        const std::string& m = primitiveString(*drawn, "material", p);
        CHECK_EQ(m, cut->contains(p) ? "broken_concrete" : "plaster");
        broken += cut->contains(p);
    }
    CHECK(broken > 0);
    // Brick is brick through: its cut faces brick.
    paint->setInt("material", static_cast<int>(MaterialPreset::Brick));
    scene.pieces = engine.cook(*f, CookContext{});
    sim::RigidSolver bricks(scene);
    bricks.step();
    CHECK(kinds(*sim::drawnPieces(bricks.capture(), Vec3(0.5f), Vec3(0.6f), "inside")) == std::set<std::string>{"brick"});
}

TEST(materials_the_renderers_meshes_take_presets_textures_and_windows) {
    using render::Material;
    // Concrete rough, steel metal, a window smooth -- the attribute
    // roughness, where there is one, before the material's.
    auto geo = std::make_shared<Geometry>(*wrangled(
        box(), "s@material = @primnum == 0 ? \"concrete\" : @primnum == 1 ? \"steel\" : @primnum == 2 ? \"window\" : \"\";", 1));
    auto mesh = render::meshOf(*geo);
    auto materialOf = [&](const render::Mesh& m, MaterialPreset preset) -> const Material* {
        for (const Material& x : m.materials) {
            if (x.preset == preset) return &x;
        }
        return nullptr;
    };
    const Material* concrete = materialOf(*mesh, MaterialPreset::Concrete);
    const Material* steel = materialOf(*mesh, MaterialPreset::Steel);
    const Material* window = materialOf(*mesh, MaterialPreset::Window);
    const Material* plain = materialOf(*mesh, MaterialPreset::None);
    CHECK(concrete && steel && window && plain);
    CHECK_NEAR(concrete->roughness, render::presetSurface(MaterialPreset::Concrete).roughness, 0.01f);
    CHECK(steel->metallic > 0.5f);
    CHECK(window->roughness < 0.1f);
    CHECK_NEAR(plain->roughness, 0.5f, 0.01f);
    // The windows' faces each a number of its own, the same however many
    // faces come before it.
    CHECK_EQ(mesh->random.size(), mesh->count());
    auto roughGeo = std::make_shared<Geometry>(*geo);
    roughGeo->primitives().create("roughness", AttrType::Float).write<float>()[0] = 0.2f;
    const auto rough = render::meshOf(*roughGeo);
    CHECK_NEAR(materialOf(*rough, MaterialPreset::Concrete)->roughness, 0.2f, 0.01f);
    // A material of glass is glass.
    const auto glass = render::meshOf(*wrangled(box(), "s@material = \"glass\";", 1));
    CHECK(glass->clear);
    // A texture of its own: carried, with its size and tint.
    const GeometryPtr textured = cooked("material", box(), [](Node& n) {
        n.setString("texture", "/x/y_diff.png");
        n.setFloat("texture_size", 3.0f);
    });
    const auto tm = render::meshOf(*textured);
    CHECK_EQ(tm->materials.size(), 1u);
    CHECK_EQ(tm->materials[0].texture, "/x/y_diff.png");
    CHECK_NEAR(tm->materials[0].textureSize, 3.0f, 1e-6f);
    CHECK_EQ(static_cast<int>(tm->materials[0].textureTint), 0);
    // No colour of its own: each surface its material's -- a lawn green --
    // and the viewport's grey where it is of none; with Cd, Cd.
    const auto lawn = render::meshOf(*wrangled(box(), "s@material = @primnum == 0 ? \"lawn\" : \"\";", 1));
    bool green = false;
    for (size_t t = 0; t < lawn->count(); ++t) {
        const bool isLawn = lawn->materials[lawn->material[t]].preset == MaterialPreset::Lawn;
        green = green || isLawn;
        const Vec3 want = isLawn ? render::presetSurface(MaterialPreset::Lawn).color : Vec3(0.72f, 0.72f, 0.74f);
        CHECK(length(lawn->colors[3 * t] - want) < 1e-4f);
    }
    CHECK(green);
    CHECK(render::presetSurface(MaterialPreset::Lawn).color.y > 1.5f * render::presetSurface(MaterialPreset::Lawn).color.x);
    const auto colored = render::meshOf(*wrangled(wrangled(box(), "s@material = \"lawn\";", 1), "@Cd = {0.2, 0.4, 0.6};", 1));
    for (size_t t = 0; t < colored->count(); ++t) CHECK(length(colored->colors[3 * t] - Vec3(0.2f, 0.4f, 0.6f)) < 1e-4f);
    // Where the points were: carried corner by corner.
    auto moved = std::make_shared<Geometry>(*box());
    auto restOut = moved->points().create("rest", AttrType::Vec3).write<Vec3>();
    for (size_t i = 0; i < restOut.size(); ++i) restOut[i] = moved->positions()[i] + Vec3(10.0f, 0.0f, 0.0f);
    const auto rm = render::meshOf(*moved);
    CHECK_EQ(rm->rest.size(), 3 * rm->count());
    for (size_t t = 0; t < rm->count(); ++t) CHECK(length(rm->rest[3 * t] - (rm->v0[t] + Vec3(10.0f, 0.0f, 0.0f))) < 1e-5f);
}

TEST(materials_texture_sets_are_found_from_a_picture_or_a_folder) {
    const fs::path dir = fs::temp_directory_path() / "pg_test_textures";
    fs::remove_all(dir);
    fs::create_directories(dir / "polyhaven");
    fs::create_directories(dir / "ambient");
    fs::create_directories(dir / "ours" / "stone");
    fs::create_directories(dir / "ours" / "rock");
    auto grey = [](float v) { return [v](int, int) { return Vec3(v, v, v); }; };
    // Poly Haven's names: the colour, roughness, displacement, a normal map
    // -- and another set's picture beside them.
    writeTestPicture((dir / "polyhaven" / "brick_wall_02_diff_1k.png").string(), 8, 8, [](int x, int) {
        return x < 4 ? Vec3(1.0f, 0.0f, 0.0f) : Vec3(0.0f, 0.0f, 1.0f);
    });
    writeTestPicture((dir / "polyhaven" / "brick_wall_02_rough_1k.png").string(), 8, 8, grey(0.7f));
    writeTestPicture((dir / "polyhaven" / "brick_wall_02_disp_1k.png").string(), 8, 8, grey(0.5f));
    writeTestPicture((dir / "polyhaven" / "brick_wall_02_nor_gl_1k.png").string(), 8, 8, grey(0.5f));
    writeTestPicture((dir / "polyhaven" / "other_diff_1k.png").string(), 8, 8, grey(0.2f));
    const render::TextureSet ph = render::textureSet((dir / "polyhaven" / "brick_wall_02_diff_1k.png").string());
    CHECK(ph.valid());
    CHECK(ph.roughness.find("brick_wall_02_rough_1k") != std::string::npos);
    CHECK(ph.height.find("brick_wall_02_disp_1k") != std::string::npos);
    CHECK(!ph.tint);
    // Half red, half blue: the mean half of each (linear).
    CHECK_NEAR(ph.mean.x, 0.5f, 0.02f);
    CHECK_NEAR(ph.mean.y, 0.0f, 0.02f);
    CHECK_NEAR(ph.mean.z, 0.5f, 0.02f);
    // Picked by its roughness: the same set.
    CHECK(render::textureSet((dir / "polyhaven" / "brick_wall_02_rough_1k.png").string()).color == ph.color);
    // ambientCG's names, its folder named.
    writeTestPicture((dir / "ambient" / "Concrete034_1K-JPG_Color.png").string(), 8, 8, grey(0.6f));
    writeTestPicture((dir / "ambient" / "Concrete034_1K-JPG_Displacement.png").string(), 8, 8, grey(0.5f));
    writeTestPicture((dir / "ambient" / "Concrete034_1K-JPG_NormalGL.png").string(), 8, 8, grey(0.5f));
    const render::TextureSet acg = render::textureSet((dir / "ambient").string());
    CHECK(acg.valid() && acg.color.find("_Color") != std::string::npos);
    CHECK(acg.height.find("_Displacement") != std::string::npos);
    CHECK(acg.roughness.empty());
    // Ours: texture.txt -- size, depth, mean, tint -- and another folder's
    // pictures by `pictures`.
    writeTestPicture((dir / "ours" / "stone" / "color.png").string(), 8, 8, grey(0.5f));
    writeTestPicture((dir / "ours" / "stone" / "height.png").string(), 8, 8, grey(0.5f));
    {
        std::ofstream t(dir / "ours" / "stone" / "texture.txt");
        t << "# a test\nsize 1.25\ndepth 0.004\nmean 0.2 0.3 0.4\ntint 0\n";
        std::ofstream r(dir / "ours" / "rock" / "texture.txt");
        r << "pictures ../stone\nsize 5\n";
    }
    const render::TextureSet ours = render::textureSet((dir / "ours" / "stone").string());
    CHECK(ours.valid() && !ours.height.empty());
    CHECK_NEAR(ours.size, 1.25f, 1e-6f);
    CHECK_NEAR(ours.depth, 0.004f, 1e-6f);
    CHECK_NEAR(ours.mean.y, 0.3f, 1e-6f);
    CHECK(!ours.tint);
    const render::TextureSet rock = render::presetTextureSet((dir / "ours").string(), MaterialPreset::Stone);
    CHECK(rock.color == ours.color);
    const render::TextureSet borrowed = render::textureSet((dir / "ours" / "rock").string());
    CHECK(borrowed.color == ours.color);
    CHECK_NEAR(borrowed.size, 5.0f, 1e-6f);
    CHECK(borrowed.tint);
    // Nothing: none.
    CHECK(!render::textureSet((dir / "nothing").string()).valid());
    CHECK(!render::presetTextureSet((dir / "ours").string(), MaterialPreset::Wood).valid());
    // The library that comes with the program: a set for every material
    // but those of a pattern of their own -- a brick wall as it is, roof
    // tiles laid along the roof.
    const std::string library = render::textureLibrary();
    CHECK(!library.empty());
    const std::set<MaterialPreset> patterned = {MaterialPreset::None,  MaterialPreset::Brick, MaterialPreset::Window,
                                                MaterialPreset::Glass, MaterialPreset::Steel, MaterialPreset::Stone,
                                                MaterialPreset::Leaf,  MaterialPreset::Grass};
    for (size_t i = 0; i < kMaterialPresets; ++i) {
        const auto p = static_cast<MaterialPreset>(i);
        const render::TextureSet set = render::presetTextureSet(library, p);
        CHECK_EQ(set.valid(), !patterned.count(p));
        if (!set.valid()) continue;
        CHECK(!set.height.empty());
        CHECK(set.size > 0.2f && set.size < 10.0f);
        CHECK(set.tint == (p != MaterialPreset::BrickWall));
        CHECK(set.alongFace == (p == MaterialPreset::RoofTiles));
    }
    // What a material gets: its own, else its preset's, sized as it says;
    // none with textures off.
    render::Settings s;
    render::Material m;
    m.preset = MaterialPreset::Concrete;
    CHECK(render::textureOf(m, s).valid());
    m.textureSize = 6.0f;
    CHECK_NEAR(render::textureOf(m, s).size, 6.0f, 1e-6f);
    m.texture = ph.color;
    m.textureTint = 1;
    CHECK(render::textureOf(m, s).color == ph.color && render::textureOf(m, s).tint);
    s.textures = false;
    CHECK(!render::textureOf(m, s).valid());
    fs::remove_all(dir);
}

TEST(materials_rows_are_laid_along_a_sloping_face) {
    // A picture of level rows, four to a picture of a metre, laid on roofs of
    // 40 degrees facing each way: along the face, the rows stay level -- the
    // same colour across the roof -- and change up it; from three sides,
    // where the roof faces x, the picture seen from above runs them down
    // the slope. A face lying flat: from three sides either way.
    render::TexturePicture rows;
    rows.width = 16;
    rows.height = 16;
    rows.pixels.resize(256);
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) rows.pixels[static_cast<size_t>(y * 16 + x)] = (y / 2) % 2 ? Vec3(0.9f) : Vec3(0.1f);
    }
    rows.size = 1.0f;
    const float sn = std::sin(0.7f), cs = std::cos(0.7f);
    for (const Vec3& face : {Vec3(sn, cs, 0.0f), Vec3(0.0f, cs, sn), Vec3(-sn, cs, 0.0f), Vec3(0.0f, cs, -sn)}) {
        const Vec3 level = normalize(Vec3(face.z, 0.0f, -face.x));
        const Vec3 up = cross(face, level);
        auto spread = [&](bool along, const Vec3& dir) {
            rows.alongFace = along;
            float lo = 1e9f, hi = -1e9f;
            for (int i = 0; i < 64; ++i) {
                const float g = rows.onSurface(Vec3(3.0f, 2.0f, 1.0f) + dir * (0.03f * static_cast<float>(i)), face).y;
                lo = std::min(lo, g);
                hi = std::max(hi, g);
            }
            return hi - lo;
        };
        CHECK(spread(true, level) < 0.02f);
        CHECK(spread(true, up) > 0.5f);
        if (std::fabs(face.x) > 0.1f) CHECK(spread(false, level) > 0.2f);
    }
    const Vec3 lying(0.0f, 1.0f, 0.0f), at(0.3f, 0.0f, 0.6f);
    rows.alongFace = false;
    const Vec3 three = rows.onSurface(at, lying);
    rows.alongFace = true;
    CHECK(length(rows.onSurface(at, lying) - three) < 1e-6f);
}

TEST(materials_the_path_tracer_lays_a_texture_on) {
    // A square of ground seen from above under an even sky, a texture of
    // stripes laid on it: striped as it is; plain with textures off; tinted,
    // as bright on the whole as without.
    const fs::path dir = fs::temp_directory_path() / "pg_test_texture_stripes";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const std::string file = (dir / "stripes_diff.png").string();
    writeTestPicture(file, 16, 16, [](int x, int) { return (x / 4) % 2 ? Vec3(0.9f) : Vec3(0.1f); });
    auto geo = ground(Vec3(0.5f), "");
    setPrimitiveString(*geo, "texture", file);
    geo->primitives().create("texture_size", AttrType::Float).write<float>()[0] = 1.0f;
    geo->primitives().create("texture_tint", AttrType::Int).write<int32_t>()[0] = 0;
    sim::Camera cam = sim::Camera::lookingAt(Vec3(0.0f, 5.0f, 0.01f), Vec3(0.0f, 0.0f, 0.0f));
    cam.width = 48;
    cam.height = 48;
    sim::Look look;
    look.lightIntensity = 0.0f;
    look.skyIntensity = 1.0f;
    look.skyColor = Vec3(1.0f);
    look.floor = false;
    render::SceneInput in;
    in.geometry = geo;
    in.look = look;
    in.camera = cam;
    render::SceneBuilder builder;
    const auto scene = builder.build(in);
    auto render = [&](bool textures) {
        render::Settings s;
        s.width = 48;
        s.height = 48;
        s.samples = 8;
        s.denoise = false;
        s.textures = textures;
        render::PathTracer tracer;
        tracer.setSettings(s);
        tracer.setScene(scene);
        while (tracer.samples() < s.samples) tracer.pass();
        return tracer.beauty();
    };
    double withMean = 0.0, withoutMean = 0.0;
    const double with = variation(render(true), withMean), without = variation(render(false), withoutMean);
    std::printf("  stripes: varying %.3f (mean %.3f); without textures %.3f (mean %.3f)\n", with, withMean, without,
                withoutMean);
    CHECK(with > 0.3);
    CHECK(without < 0.05);
    // Tinted: the stripes round the colour, as bright on the whole.
    geo->primitives().find("texture_tint")->write<int32_t>()[0] = 1;
    const auto tintedScene = builder.build(in);
    render::Settings s;
    s.width = 48;
    s.height = 48;
    s.samples = 8;
    s.denoise = false;
    render::PathTracer tracer;
    tracer.setSettings(s);
    tracer.setScene(tintedScene);
    while (tracer.samples() < s.samples) tracer.pass();
    double tintedMean = 0.0;
    const double tinted = variation(tracer.beauty(), tintedMean);
    std::printf("  tinted: varying %.3f (mean %.3f)\n", tinted, tintedMean);
    CHECK(tinted > 0.3);
    CHECK(std::fabs(tintedMean / withoutMean - 1.0) < 0.15);
    fs::remove_all(dir);
}

TEST(materials_cycles_draws_the_photographs_and_the_patterns) {
    if (!render::cyclesAvailable()) return;
    // The ground from above under an even sky: a brick wall's photograph
    // laid on it varies -- the mortar lighter than the bricks -- and much
    // less without textures; concrete's about as bright on the whole as
    // its colour without; a pattern (asphalt) varies too.
    sim::Camera cam = sim::Camera::lookingAt(Vec3(0.0f, 4.0f, 0.01f), Vec3(0.0f, 0.0f, 0.0f));
    cam.width = 48;
    cam.height = 48;
    sim::Look look;
    look.lightIntensity = 0.0f;
    look.skyIntensity = 1.0f;
    look.skyColor = Vec3(1.0f);
    look.floor = false;
    auto render = [&](const std::string& material, const Vec3& color, bool textures, double& mean) {
        render::SceneInput in;
        in.geometry = ground(color, material);
        in.look = look;
        in.camera = cam;
        render::SceneBuilder builder;
        const auto scene = builder.build(in);
        render::Settings s;
        s.width = 48;
        s.height = 48;
        s.samples = 16;
        s.denoise = true;
        s.sky = render::Settings::Sky::Look;
        s.textures = textures;
        return variation(cycles(scene, s), mean);
    };
    double brickMean = 0.0, plainMean = 0.0, concreteMean = 0.0, flatMean = 0.0, asphaltMean = 0.0;
    const double brick = render("brick_wall", Vec3(0.4f, 0.2f, 0.15f), true, brickMean);
    const double plain = render("brick_wall", Vec3(0.4f, 0.2f, 0.15f), false, plainMean);
    render("concrete", Vec3(0.5f), true, concreteMean);
    render("", Vec3(0.5f), false, flatMean);
    const double asphalt = render("asphalt", Vec3(0.3f), false, asphaltMean);
    std::printf("  brick wall: varying %.3f, without textures %.3f; concrete %.3f against %.3f; asphalt %.3f\n", brick,
                plain, concreteMean, flatMean, asphalt);
    CHECK(brick > 2.0 * plain && brick > 0.15);
    CHECK(std::fabs(concreteMean / flatMean - 1.0) < 0.2);
    CHECK(asphalt > 0.03);
}

TEST(materials_cycles_lays_rows_along_a_roof) {
    if (!render::cyclesAvailable()) return;
    // A roof of 40 degrees facing +x, seen square on under an even sky, a
    // picture of level rows laid on it: along the roof, the rows level in
    // the picture of it -- each row of pixels about one colour, each column
    // striped; from three sides, the picture from above runs them down the
    // slope, across the rows of pixels.
    const fs::path dir = fs::temp_directory_path() / "pg_test_texture_rows";
    fs::remove_all(dir);
    for (const std::string which : {"along", "three"}) {
        fs::create_directories(dir / which);
        writeTestPicture((dir / which / "color.png").string(), 16, 16,
                         [](int, int y) { return (y / 2) % 2 ? Vec3(0.9f) : Vec3(0.1f); });
        std::ofstream t(dir / which / "texture.txt");
        t << "size 8\ntint 0\n" << (which == "along" ? "projection face\n" : "");
    }
    const float sn = std::sin(0.7f), cs = std::cos(0.7f);
    const Vec3 face(sn, cs, 0.0f), level(0.0f, 0.0f, -1.0f), up = cross(face, level);
    sim::Camera cam = sim::Camera::lookingAt(face * 6.0f, Vec3(0.0f));
    cam.width = 48;
    cam.height = 48;
    sim::Look look;
    look.lightIntensity = 0.0f;
    look.skyIntensity = 1.0f;
    look.skyColor = Vec3(1.0f);
    look.floor = false;
    // How much the green varies along the rows of pixels, and along the
    // columns, on the whole.
    auto spread = [&](const std::string& which, double& alongRows, double& alongColumns) {
        auto geo = std::make_shared<Geometry>();
        geo->addPoints(4);
        auto P = geo->positionsForWrite();
        P[0] = (-level - up) * 10.0f;
        P[1] = (level - up) * 10.0f;
        P[2] = (level + up) * 10.0f;
        P[3] = (-level + up) * 10.0f;
        const uint32_t quad[4] = {0, 1, 2, 3};
        geo->addPrimitive(quad, true);
        setPrimitiveString(*geo, "texture", (dir / which).string());
        geo->primitives().create("texture_tint", AttrType::Int).write<int32_t>()[0] = 0;
        render::SceneInput in;
        in.geometry = geo;
        in.look = look;
        in.camera = cam;
        render::SceneBuilder builder;
        render::Settings s;
        s.width = 48;
        s.height = 48;
        s.samples = 16;
        s.denoise = false;
        s.sky = render::Settings::Sky::Look;
        const render::Image img = cycles(builder.build(in), s);
        auto stdOf = [&](int x0, int y0, int dx, int dy) {
            double sum = 0.0, square = 0.0;
            for (int i = 0; i < 48; ++i) {
                const double g = img.pixels[3 * static_cast<size_t>((y0 + dy * i) * img.width + x0 + dx * i) + 1];
                sum += g;
                square += g * g;
            }
            const double mean = sum / 48.0;
            return std::sqrt(std::max(square / 48.0 - mean * mean, 0.0));
        };
        alongRows = alongColumns = 0.0;
        for (int i = 0; i < 48; ++i) {
            alongRows += stdOf(0, i, 1, 0) / 48.0;
            alongColumns += stdOf(i, 0, 0, 1) / 48.0;
        }
    };
    double alongRows = 0.0, alongColumns = 0.0, threeRows = 0.0, threeColumns = 0.0;
    spread("along", alongRows, alongColumns);
    spread("three", threeRows, threeColumns);
    std::printf("  along the roof: rows %.3f, columns %.3f; from three sides: rows %.3f, columns %.3f\n", alongRows,
                alongColumns, threeRows, threeColumns);
    CHECK(alongColumns > 0.1);
    CHECK(alongRows < 0.25 * alongColumns);
    CHECK(threeRows > 3.0 * alongRows);
    fs::remove_all(dir);
}
