//
// What surfaces are made of (core/Material.h) and the pictures laid on them
// (render/Textures.h): a new string attribute reads "" where nothing was
// written, merged or not; the nodes that make surfaces say what they are --
// Brick Wall, Concrete Fracture, Glass Fracture, Tree, Grass, Rebar -- and
// the Material node any; the pieces of the RBD Solver keep where they were
// (rest), their cut faces broken concrete, their bars steel; the renderers'
// meshes take the materials' roughness, their textures and the windows'
// numbers; texture sets are found from a folder or from one picture of
// them, the library's for each material; both renderers lay them on.
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
    // The library that comes with the program: concrete, plaster, a brick
    // wall, wood, bark, soil and roofs -- a brick wall as it is.
    const std::string library = render::textureLibrary();
    CHECK(!library.empty());
    for (const MaterialPreset p : {MaterialPreset::Concrete, MaterialPreset::Plaster, MaterialPreset::BrickWall,
                                   MaterialPreset::Wood, MaterialPreset::Bark, MaterialPreset::Soil, MaterialPreset::Roof}) {
        const render::TextureSet set = render::presetTextureSet(library, p);
        CHECK(set.valid());
        CHECK(!set.height.empty());
        CHECK(set.tint == (p != MaterialPreset::BrickWall));
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
