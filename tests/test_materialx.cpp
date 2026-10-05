//
// Materials out to the other programs and back, as MaterialX: documents
// written and read back as they were; node graphs, their outputs and their
// own inputs followed back to the pictures; the ids of the nodes' definitions
// the same in MaterialX 1.38 and 1.39 (a normal map spelled out); the
// renderers' materials as standard_surface graphs -- pictures by uv or from
// three sides, tinted by Cd, normal maps (DirectX's turned over), leaves cut
// out by their alpha and letting light through, glass -- and a
// UsdPreviewSurface of each; a document read as a texture set; USD stages
// whose faces are bound to their materials, a GeomSubset each, in a stage of
// one geometry and in a shot, frame by frame; the materials alone as .mtlx;
// the pictures beside them.
//
#include "pg/core/Material.h"
#include "pg/io/MaterialX.h"
#include "pg/io/Picture.h"
#include "pg/io/Usda.h"
#include "pg/render/MaterialGraph.h"
#include "pg/render/Scene.h"
#include "pg/render/Textures.h"
#include "pg/sim/UsdExport.h"
#include "pg/usd/Geom.h"
#include "pg/usd/Stage.h"

#include "test_framework.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>

using namespace pg;
namespace fs = std::filesystem;
namespace mtlx = io::mtlx;

namespace {

/// A folder of its own under the system's temporary one, gone afterwards.
struct TempFolder {
    fs::path path;
    explicit TempFolder(const std::string& name) {
        std::random_device rd;
        path = fs::temp_directory_path() / ("pg_test_" + name + "_" + std::to_string(rd()));
        fs::create_directories(path);
    }
    ~TempFolder() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    std::string operator/(const std::string& name) const { return (path / name).string(); }
};

std::string fileText(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void writeFile(const std::string& path, const std::string& text) {
    fs::create_directories(fs::path(path).parent_path());
    std::ofstream(path, std::ios::binary) << text;
}

/// Quads side by side along x, a metre each, uv once across each corner to
/// corner; quad k of the material `materials[k]` ("glass": glass).
std::shared_ptr<Geometry> quads(const std::vector<std::string>& materials, float x0 = 0.0f) {
    auto g = std::make_shared<Geometry>();
    const size_t n = materials.size();
    g->addPoints(2 * (n + 1));
    auto P = g->positionsForWrite();
    for (size_t i = 0; i <= n; ++i) {
        P[2 * i] = Vec3(x0 + static_cast<float>(i), 0.0f, 0.0f);
        P[2 * i + 1] = Vec3(x0 + static_cast<float>(i), 1.0f, 0.0f);
    }
    for (size_t k = 0; k < n; ++k) {
        const auto a = static_cast<uint32_t>(2 * k);
        const uint32_t ring[4] = {a, a + 2, a + 3, a + 1};
        g->addPrimitive(ring, true);
    }
    auto uv = g->vertices().create("uv", AttrType::Vec3).write<Vec3>();
    for (size_t k = 0; k < n; ++k) {
        uv[4 * k] = Vec3(0.0f, 0.0f, 0.0f);
        uv[4 * k + 1] = Vec3(1.0f, 0.0f, 0.0f);
        uv[4 * k + 2] = Vec3(1.0f, 1.0f, 0.0f);
        uv[4 * k + 3] = Vec3(0.0f, 1.0f, 0.0f);
    }
    for (size_t k = 0; k < n; ++k) {
        std::vector<uint8_t> mask(n, 0);
        mask[k] = 1;
        setPrimitiveString(*g, "material", materials[k], mask);
    }
    return g;
}

const mtlx::Node* nodeNamed(const std::vector<mtlx::Node>& nodes, const std::string& name) {
    for (const mtlx::Node& n : nodes) {
        if (n.name == name) return &n;
    }
    return nullptr;
}

const mtlx::Node* nodeOf(const std::vector<mtlx::Node>& nodes, const std::string& category) {
    for (const mtlx::Node& n : nodes) {
        if (n.category == category) return &n;
    }
    return nullptr;
}

/// The node an input of `n` comes from, or null.
const mtlx::Node* behind(const std::vector<mtlx::Node>& nodes, const mtlx::Node* n, const std::string& input) {
    const mtlx::Input* in = n ? n->input(input) : nullptr;
    return in && !in->nodename.empty() ? nodeNamed(nodes, in->nodename) : nullptr;
}

std::string valueOf(const mtlx::Node* n, const std::string& input) {
    const mtlx::Input* in = n ? n->input(input) : nullptr;
    return in ? in->value : std::string("<none>");
}

render::Material preset(MaterialPreset p, bool byUv) {
    render::Material m;
    m.preset = p;
    m.roughness = render::presetSurface(p).roughness;
    m.metallic = render::presetSurface(p).metallic;
    m.byUv = byUv;
    return m;
}

/// The connection of a property of a prim of a stage, as a path: "" for none.
std::string connection(const usd::Stage& s, const std::string& prim, const std::string& property) {
    const usd::Stage::Prim* p = s.find(prim);
    const usd::Property* a = p ? s.property(*p, property) : nullptr;
    if (!a) return {};
    const std::vector<usd::ListItem> items = a->targets.items();
    return items.empty() ? std::string() : items.front().text;
}

std::string token(const usd::Stage& s, const std::string& prim, const std::string& property, double time = 0.0) {
    const usd::Stage::Prim* p = s.find(prim);
    if (!p) return "<no prim>";
    const usd::Value v = s.value(*p, property, time);
    return v.strings.empty() ? std::string() : v.strings.front();
}

std::vector<int> indices(const usd::Stage& s, const std::string& prim, double time) {
    const usd::Stage::Prim* p = s.find(prim);
    std::vector<int> out;
    if (!p) return out;
    for (const double x : s.value(*p, "indices", time).numbers) out.push_back(static_cast<int>(x));
    return out;
}

}  // namespace

TEST(materialx_documents_read_back_as_they_were_written) {
    mtlx::Material brick{"brick",
                         {{"image", "brick_color", "color3", {{"file", "filename", "a & \"b\".jpg", {}, "srgb_texture", {}}}},
                          {"standard_surface",
                           "brick_surface",
                           "surfaceshader",
                           {{"base_color", "color3", {}, "brick_color", {}, {}},
                            {"specular_roughness", "float", "0.8", {}, {}, {}},
                            {"metalness", "float", "0.25", {}, {}, {}}}},
                          {"surfacematerial", "brick", "material", {{"surfaceshader", "surfaceshader", {}, "brick_surface", {}, {}}}}}};
    const std::string text = mtlx::document({brick});
    CHECK(text.rfind("<?xml version=\"1.0\"?>\n<materialx version=\"1.38\" colorspace=\"lin_rec709\">\n", 0) == 0);
    CHECK(text.find("value=\"a &amp; &quot;b&quot;.jpg\"") != std::string::npos);
    std::vector<mtlx::Node> nodes;
    std::string error;
    CHECK(mtlx::parse(text, nodes, error));
    CHECK_EQ(nodes.size(), size_t(3));
    for (size_t k = 0; k < std::min(nodes.size(), brick.nodes.size()); ++k) {
        const mtlx::Node &a = nodes[k], &b = brick.nodes[k];
        CHECK_EQ(a.category, b.category);
        CHECK_EQ(a.name, b.name);
        CHECK_EQ(a.type, b.type);
        CHECK_EQ(a.inputs.size(), b.inputs.size());
        for (size_t i = 0; i < std::min(a.inputs.size(), b.inputs.size()); ++i) {
            CHECK_EQ(a.inputs[i].name, b.inputs[i].name);
            CHECK_EQ(a.inputs[i].type, b.inputs[i].type);
            CHECK_EQ(a.inputs[i].value, b.inputs[i].value);
            CHECK_EQ(a.inputs[i].nodename, b.inputs[i].nodename);
            CHECK_EQ(a.inputs[i].colorspace, b.inputs[i].colorspace);
        }
    }
    const mtlx::Surface s = mtlx::surfaceOf(nodes);
    CHECK(s.found);
    CHECK_EQ(s.color, std::string("a & \"b\".jpg"));
    CHECK_EQ(s.roughness, 0.8f);
    CHECK_EQ(s.metalness, 0.25f);
    CHECK(!s.tinted);
    // Not MaterialX: said so.
    CHECK(!mtlx::parse("<html></html>", nodes, error));
    CHECK(!mtlx::parse("<materialx><image name=\"x\">", nodes, error));
}

TEST(materialx_node_graphs_and_their_own_inputs_are_followed_to_the_pictures) {
    // As Poly Haven gives a material away: a node graph of tiled images, its
    // outputs taken by the surface and the displacement, a file named by
    // the graph's own input.
    TempFolder dir("mtlx_graph");
    const std::string doc = R"(<?xml version="1.0"?>
<materialx version="1.38" fileprefix="./">
  <standard_surface name="Rock_surface" type="surfaceshader">
    <input name="base_color" type="color3" nodegraph="NG_Rock" output="out_color" />
    <input name="normal" type="vector3" nodegraph="NG_Rock" output="out_normal" />
    <input name="specular_roughness" type="float" nodegraph="NG_Rock" output="out_rough" />
  </standard_surface>
  <displacement name="Rock_displacement" type="displacementshader">
    <input name="displacement" type="float" nodegraph="NG_Rock" output="out_height" />
  </displacement>
  <surfacematerial name="Rock" type="material">
    <input name="surfaceshader" type="surfaceshader" nodename="Rock_surface" />
    <input name="displacementshader" type="displacementshader" nodename="Rock_displacement" />
  </surfacematerial>
  <nodegraph name="NG_Rock">
    <input name="color_file" type="filename" value="textures/rock_diff_1k.png" colorspace="srgb_texture" />
    <tiledimage name="color" type="color3">
      <input name="file" type="filename" interfacename="color_file" />
      <input name="uvtiling" type="vector2" value="1.0, 1.0" />
    </tiledimage>
    <tiledimage name="normal_image" type="vector3">
      <input name="file" type="filename" value="textures/rock_nor_gl_1k.png" />
    </tiledimage>
    <normalmap name="normal" type="vector3">
      <input name="in" type="vector3" nodename="normal_image" />
    </normalmap>
    <tiledimage name="rough" type="float">
      <input name="file" type="filename" value="textures/rock_rough_1k.png" />
    </tiledimage>
    <tiledimage name="height" type="float">
      <input name="file" type="filename" value="textures/rock_disp_1k.png" />
    </tiledimage>
    <output name="out_color" type="color3" nodename="color" />
    <output name="out_normal" type="vector3" nodename="normal" />
    <output name="out_rough" type="float" nodename="rough" />
    <output name="out_height" type="float" nodename="height" />
  </nodegraph>
</materialx>
)";
    std::vector<mtlx::Node> nodes;
    std::string error;
    CHECK(mtlx::parse(doc, nodes, error));
    const mtlx::Surface s = mtlx::surfaceOf(nodes, "Rock");
    CHECK(s.found);
    CHECK_EQ(s.color, std::string("textures/rock_diff_1k.png"));
    CHECK_EQ(s.normal, std::string("textures/rock_nor_gl_1k.png"));
    CHECK_EQ(s.roughnessFile, std::string("textures/rock_rough_1k.png"));
    CHECK_EQ(s.height, std::string("textures/rock_disp_1k.png"));
    CHECK(s.opacity.empty());
    CHECK(!mtlx::surfaceOf(nodes, "Moss").found);
    // ... a texture set, its files from the document's folder; the folder
    // it is in a set too.
    writeFile(dir / "rock/rock.mtlx", doc);
    io::Picture grey;
    grey.width = grey.height = 2;
    grey.rgba.assign(16, 0.5f);
    for (const char* f : {"rock_diff_1k.png", "rock_nor_gl_1k.png", "rock_rough_1k.png", "rock_disp_1k.png"}) {
        fs::create_directories(dir.path / "rock/textures");
        CHECK(io::writePicture((dir.path / "rock/textures" / f).string(), grey, 95, error));
    }
    for (const std::string& where : {dir / "rock/rock.mtlx", dir / "rock/rock.mtlx#Rock", dir / "rock"}) {
        const render::TextureSet set = render::textureSet(where);
        CHECK(set.valid());
        CHECK_EQ(fs::path(set.color), (dir.path / "rock/textures/rock_diff_1k.png").lexically_normal());
        CHECK_EQ(fs::path(set.normal), (dir.path / "rock/textures/rock_nor_gl_1k.png").lexically_normal());
        CHECK_EQ(fs::path(set.height), (dir.path / "rock/textures/rock_disp_1k.png").lexically_normal());
        CHECK(!set.normalDirectX);
        CHECK(!set.tint);  // someone else's photograph: as it is
        CHECK(set.alpha.empty());
        CHECK_EQ(set.size, 2.0f);
    }
    CHECK(!render::textureSet(dir / "rock/rock.mtlx#Moss").valid());
}

TEST(materialx_node_ids_are_those_both_versions_define) {
    auto node = [](const char* category, const char* type, std::vector<mtlx::Input> inputs) {
        return mtlx::Node{category, "n", type, std::move(inputs)};
    };
    CHECK_EQ(mtlx::nodeDef(node("image", "color3", {})), std::string("ND_image_color3"));
    CHECK_EQ(mtlx::nodeDef(node("standard_surface", "surfaceshader", {})), std::string("ND_standard_surface_surfaceshader"));
    CHECK_EQ(mtlx::nodeDef(node("multiply", "vector3", {{"in2", "float", "2", {}, {}, {}}})), std::string("ND_multiply_vector3FA"));
    CHECK_EQ(mtlx::nodeDef(node("multiply", "color3", {{"in2", "color3", "1, 1, 1", {}, {}, {}}})), std::string("ND_multiply_color3"));
    CHECK_EQ(mtlx::nodeDef(node("convert", "color3", {{"in", "float", {}, "a", {}, {}}})), std::string("ND_convert_float_color3"));
    CHECK_EQ(mtlx::nodeDef(node("extract", "float", {{"in", "color4", {}, "a", {}, {}}})), std::string("ND_extract_color4"));
    CHECK_EQ(mtlx::usdType("color3"), std::string("color3f"));
    CHECK_EQ(mtlx::usdType("vector3"), std::string("float3"));
    CHECK_EQ(mtlx::usdType("filename"), std::string("asset"));
    CHECK_EQ(mtlx::usdType("surfaceshader"), std::string("token"));
    // A normal map spelled out: its name the normalized sum of the tangent,
    // the bitangent and the normal, as far as its picture says; no
    // normalmap left.
    const std::vector<mtlx::Node> graph = {
        {"image", "bump", "vector3", {{"file", "filename", "n.png", {}, {}, {}}}},
        {"normalmap", "bent", "vector3", {{"in", "vector3", {}, "bump", {}, {}}, {"scale", "float", "0.5", {}, {}, {}}}},
        {"standard_surface", "s", "surfaceshader", {{"normal", "vector3", {}, "bent", {}, {}}}}};
    const std::vector<mtlx::Node> spelled = mtlx::portable(graph);
    CHECK(!nodeOf(spelled, "normalmap"));
    const mtlx::Node* bent = nodeNamed(spelled, "bent");
    CHECK(bent && bent->category == "normalize");
    CHECK_EQ(valueOf(nodeNamed(spelled, "bent_scaled"), "in2"), std::string("0.5, 0.5, 1"));
    CHECK_EQ(valueOf(nodeNamed(spelled, "bent_tangent"), "space"), std::string("world"));
    CHECK_EQ(valueOf(nodeNamed(spelled, "bent_doubled"), "in2"), std::string("2"));
    CHECK_EQ(behind(spelled, nodeNamed(spelled, "bent_doubled"), "in1"), nodeNamed(spelled, "bump"));
    CHECK_EQ(behind(spelled, nodeNamed(spelled, "s"), "normal"), bent);
    for (const mtlx::Node& n : spelled) CHECK(mtlx::nodeDef(n) != "ND_normalmap");
}

TEST(the_renderers_materials_become_standard_surface_graphs) {
    using render::GraphSource;
    const std::string library = render::textureLibrary();
    CHECK(!library.empty());
    auto plain = [](const std::string& f) { return f; };
    // Bark by uv, tinted by Cd: the picture over its mean times displayColor;
    // its normal map; as rough as bark.
    {
        const render::Material bark = preset(MaterialPreset::Bark, true);
        const render::TextureSet set = render::presetTextureSet(library, MaterialPreset::Bark);
        GraphSource source;
        source.colored = true;
        const render::MaterialGraph g = render::materialGraph(bark, "bark", source, plain);
        const auto& nodes = g.mtlx.nodes;
        CHECK_EQ(nodes.back().category, std::string("surfacematerial"));
        CHECK_EQ(nodes.back().name, std::string("bark"));
        const mtlx::Node* surface = behind(nodes, &nodes.back(), "surfaceshader");
        CHECK(surface && surface->category == "standard_surface");
        CHECK_EQ(valueOf(surface, "base"), std::string("1"));
        CHECK_EQ(valueOf(surface, "specular_roughness"), mtlx::number(bark.roughness));
        const mtlx::Node* tinted = behind(nodes, surface, "base_color");
        CHECK(tinted && tinted->category == "multiply");
        const mtlx::Node* cd = behind(nodes, tinted, "in2");
        CHECK(cd && cd->category == "geompropvalue" && valueOf(cd, "geomprop") == "displayColor");
        const mtlx::Node* evened = behind(nodes, tinted, "in1");
        CHECK_EQ(valueOf(evened, "in2"), mtlx::numbers(Vec3(1.0f) / set.mean));
        const mtlx::Node* picture = behind(nodes, evened, "in1");
        CHECK(picture && picture->category == "image" && picture->type == "color3");
        CHECK_EQ(valueOf(picture, "file"), set.color);
        CHECK_EQ(picture->input("file")->colorspace, std::string("srgb_texture"));
        const mtlx::Node* normal = behind(nodes, surface, "normal");
        CHECK(normal && normal->category == "normalmap");
        CHECK_EQ(valueOf(normal, "scale"), std::string("1"));
        CHECK_EQ(valueOf(behind(nodes, normal, "in"), "file"), set.normal);
        CHECK(behind(nodes, normal, "in")->input("file")->colorspace.empty());  // values, not light
        // Its height: the material's displacement -- the picture about its
        // middle, as deep as the set says from its lowest to its highest.
        CHECK(!set.height.empty());
        const mtlx::Node* displacement = behind(nodes, &nodes.back(), "displacementshader");
        CHECK(displacement && displacement->category == "displacement" && displacement->type == "displacementshader");
        if (displacement) CHECK_EQ(mtlx::nodeDef(*displacement), std::string("ND_displacement_float"));
        CHECK_EQ(valueOf(displacement, "scale"), mtlx::number(set.depth));
        const mtlx::Node* middle = behind(nodes, displacement, "displacement");
        CHECK(middle && middle->category == "subtract" && valueOf(middle, "in2") == "0.5");
        const mtlx::Node* height = behind(nodes, middle, "in1");
        CHECK(height && height->category == "image" && height->type == "float");
        CHECK_EQ(valueOf(height, "file"), set.height);
        // ... and as a UsdPreviewSurface: its picture by st, as it is.
        const mtlx::Node* look = nodeOf(g.preview, "UsdPreviewSurface");
        CHECK(look);
        const mtlx::Node* texture = behind(g.preview, look, "diffuseColor");
        CHECK(texture && texture->category == "UsdUVTexture" && look->input("diffuseColor")->output == "rgb");
        CHECK_EQ(valueOf(texture, "file"), set.color);
        CHECK_EQ(valueOf(behind(g.preview, texture, "st"), "varname"), std::string("st"));
        CHECK(behind(g.preview, look, "normal"));
        // ... its height by st, about its middle, as deep.
        const mtlx::Node* high = behind(g.preview, look, "displacement");
        CHECK(high && high->category == "UsdUVTexture" && look->input("displacement")->output == "r");
        CHECK_EQ(valueOf(high, "file"), set.height);
        CHECK_EQ(valueOf(high, "scale"), mtlx::numbers(Vec3(set.depth)) + ", 1");
        CHECK_EQ(valueOf(high, "bias"), mtlx::numbers(Vec3(-0.5f * set.depth)) + ", 0");
    }
    // A leaf: cut out by its picture's alpha, a thin sheet that lets light
    // through; the preview's opacity the alpha of its picture.
    {
        render::Material leaf = preset(MaterialPreset::Leaf, true);
        leaf.translucency = 0.4f;
        const render::MaterialGraph g = render::materialGraph(leaf, "leaf", GraphSource(), plain);
        const auto& nodes = g.mtlx.nodes;
        const mtlx::Node* surface = nodeOf(nodes, "standard_surface");
        CHECK_EQ(valueOf(surface, "thin_walled"), std::string("true"));
        CHECK_EQ(valueOf(surface, "subsurface"), std::string("0.4"));
        const mtlx::Node* opacity = behind(nodes, surface, "opacity");
        CHECK(opacity && opacity->category == "convert" && opacity->type == "color3");
        const mtlx::Node* alpha = behind(nodes, opacity, "in");
        CHECK(alpha && alpha->category == "extract" && valueOf(alpha, "index") == "3");
        CHECK_EQ(behind(nodes, alpha, "in")->type, std::string("color4"));
        // Without Cd: the leaf's own colour times the picture over its mean.
        const mtlx::Node* tinted = behind(nodes, surface, "base_color");
        CHECK(tinted && !behind(nodes, tinted, "in2"));
        const mtlx::Node* look = nodeOf(g.preview, "UsdPreviewSurface");
        CHECK_EQ(look->input("opacity")->output, std::string("a"));
        CHECK_EQ(valueOf(look, "opacityThreshold"), std::string("0.5"));
    }
    // Concrete from three sides: its position, scaled by one over the metres
    // a picture covers; by rest where the points have it.
    {
        const render::Material concrete = preset(MaterialPreset::Concrete, false);
        const render::TextureSet set = render::presetTextureSet(library, MaterialPreset::Concrete);
        for (const bool rest : {false, true}) {
            GraphSource source;
            source.rest = rest;
            const render::MaterialGraph g = render::materialGraph(concrete, "concrete", source, plain);
            const auto& nodes = g.mtlx.nodes;
            const mtlx::Node* picture = nodeOf(nodes, "triplanarprojection");
            CHECK(picture);
            CHECK_EQ(valueOf(picture, "filex"), set.color);
            CHECK_EQ(valueOf(picture, "filez"), set.color);
            const mtlx::Node* scaled = behind(nodes, picture, "position");
            CHECK_EQ(valueOf(scaled, "in2"), mtlx::number(1.0f / set.size));
            const mtlx::Node* at = behind(nodes, scaled, "in1");
            CHECK(at && at->category == (rest ? "geompropvalue" : "position"));
            CHECK(!nodeOf(nodes, "normalmap"));  // a normal map only by uv
            // Its height from three sides as its colour, by the same position.
            const mtlx::Node* middle = behind(nodes, behind(nodes, &nodes.back(), "displacementshader"), "displacement");
            const mtlx::Node* height = behind(nodes, middle, "in1");
            CHECK(height && height->category == "triplanarprojection" && height->type == "float");
            CHECK_EQ(valueOf(height, "filey"), set.height);
            CHECK(behind(nodes, height, "position") == scaled);
            // The preview: its colour without the picture, nor its height.
            CHECK(!nodeOf(g.preview, "UsdUVTexture"));
            CHECK(!nodeOf(g.preview, "UsdPreviewSurface")->input("displacement"));
        }
    }
    // Glass: clear, bending light as glass does, tinted a little.
    {
        render::Material glass;
        glass.kind = render::Material::Kind::Glass;
        glass.roughness = 0.0f;
        glass.ior = 1.5f;
        CHECK_EQ(render::lookName(glass), std::string("glass"));
        const render::MaterialGraph g = render::materialGraph(glass, "glass", GraphSource(), plain);
        const mtlx::Node* surface = nodeOf(g.mtlx.nodes, "standard_surface");
        CHECK_EQ(valueOf(surface, "transmission"), std::string("1"));
        CHECK_EQ(valueOf(surface, "specular_IOR"), std::string("1.5"));
        CHECK(!nodeOf(g.mtlx.nodes, "image"));
        CHECK(!g.mtlx.nodes.back().input("displacementshader"));
    }
    // A surface of no material: plain, of the viewport's grey.
    {
        const render::Material none = render::Material::surface(0.5f);
        CHECK_EQ(render::lookName(none), std::string("plain"));
        const render::MaterialGraph g = render::materialGraph(none, "plain", GraphSource(), plain);
        CHECK_EQ(valueOf(nodeOf(g.mtlx.nodes, "standard_surface"), "base_color"), mtlx::numbers(Vec3(0.72f, 0.72f, 0.74f)));
    }
    // A texture of one's own whose normal map is DirectX's: its green turned
    // over on its way to the normal map.
    {
        TempFolder dir("mtlx_dx");
        io::Picture flat;
        flat.width = flat.height = 2;
        flat.rgba = {0.5f, 0.5f, 1.0f, 1.0f, 0.5f, 0.5f, 1.0f, 1.0f, 0.5f, 0.5f, 1.0f, 1.0f, 0.5f, 0.5f, 1.0f, 1.0f};
        std::string error;
        fs::create_directories(dir.path / "tiles");
        CHECK(io::writePicture(dir / "tiles/color.png", flat, 95, error));
        CHECK(io::writePicture(dir / "tiles/normal.png", flat, 95, error));
        writeFile(dir / "tiles/texture.txt", "size 3\nnormal dx\n");
        render::Material own = render::Material::surface(0.6f);
        own.texture = dir / "tiles";
        own.byUv = true;
        CHECK_EQ(render::lookName(own), std::string("tiles"));
        const render::MaterialGraph g = render::materialGraph(own, "tiles", GraphSource(), plain);
        const auto& nodes = g.mtlx.nodes;
        const mtlx::Node* normal = behind(nodes, nodeOf(nodes, "standard_surface"), "normal");
        const mtlx::Node* opengl = behind(nodes, normal, "in");
        CHECK(opengl && opengl->category == "add");
        const mtlx::Node* flipped = behind(nodes, opengl, "in1");
        CHECK_EQ(valueOf(flipped, "in2"), std::string("1, -1, 1"));
        CHECK_EQ(valueOf(behind(nodes, flipped, "in1"), "file"), (dir.path / "tiles/normal.png").string());
    }
}

TEST(a_written_material_reads_back_as_its_texture_set) {
    TempFolder dir("mtlx_back");
    const std::string library = render::textureLibrary();
    // Bark and leaves by uv, tinted by Cd; concrete from three sides.
    auto geo = quads({"bark", "leaf", "concrete"});
    geo->detail().create("Cd", AttrType::Vec3).write<Vec3>()[0] = Vec3(0.5f, 0.4f, 0.3f);
    std::vector<uint8_t> concrete = {0, 0, 1};
    // Concrete laid from three sides even where it has uv.
    auto& how = geo->primitives().create("texture_projection", AttrType::Int);
    how.write<int32_t>()[2] = 2;
    render::MaterialLooks looks("/World/Materials", "./pictures/");
    const io::usda::FaceMaterials bound = looks.bind(*geo);
    CHECK_EQ(bound.names.size(), size_t(3));
    CHECK_EQ(looks.size(), size_t(3));
    std::string error;
    CHECK(looks.copyPictures(dir / "pictures", error));
    writeFile(dir / "looks.mtlx", looks.document());
    for (const MaterialPreset p : {MaterialPreset::Bark, MaterialPreset::Leaf, MaterialPreset::Concrete}) {
        const std::string name(materialName(p));
        const render::TextureSet original = render::presetTextureSet(library, p);
        const render::TextureSet back = render::textureSet(dir / ("looks.mtlx#" + name));
        CHECK(back.valid());
        // The copy beside the document, named by its set and its own name.
        CHECK_EQ(fs::path(back.color).filename().string(), name + "_" + fs::path(original.color).filename().string());
        CHECK(fs::exists(back.color));
        CHECK_EQ(fileText(back.color), fileText(original.color));
        CHECK_EQ(back.tint, original.tint);
        // Its mean as the document evens it: the library's, not the copy's.
        if (back.tint) CHECK(glm::length(back.mean - original.mean) < 1e-5f);
        if (p == MaterialPreset::Concrete) {
            CHECK(std::fabs(back.size - original.size) < 1e-4f);
            CHECK(back.normal.empty());
        } else {
            CHECK_EQ(back.normal.empty(), original.normal.empty());
        }
        CHECK_EQ(back.alpha.empty(), original.alpha.empty());
        CHECK_EQ(back.alphaChannel, original.alphaChannel);
        // Its height: a copy, as deep as it was.
        CHECK_EQ(back.height.empty(), original.height.empty());
        if (!back.height.empty() && !original.height.empty()) {
            CHECK_EQ(fileText(back.height), fileText(original.height));
            CHECK_NEAR(back.depth, original.depth, 1e-7);
        }
    }
}

TEST(a_usd_stage_binds_the_faces_to_their_materials) {
    TempFolder dir("mtlx_usd");
    // Bark, a leaf and a pane of glass, side by side.
    auto geo = quads({"bark", "leaf", "glass"});
    std::string error;
    CHECK(sim::exportGeometry(*geo, dir / "thing.usda", error));
    const std::string text = fileText(dir / "thing.usda");
    CHECK(text.find("def Scope \"Materials\"") != std::string::npos);
    CHECK(text.find("ND_normalmap") == std::string::npos);  // spelled out
    const auto stage = usd::Stage::open(dir / "thing.usda", error);
    CHECK(stage);
    if (!stage) return;
    const usd::Stage& s = *stage;
    CHECK(s.warnings().empty());
    // A GeomSubset a material, a face each, bound to it.
    const char* names[] = {"bark", "leaf", "glass"};
    for (int k = 0; k < 3; ++k) {
        const std::string subset = std::string("/thing/mesh/") + names[k];
        const usd::Stage::Prim* p = s.find(subset);
        CHECK(p && p->type == "GeomSubset");
        if (!p) continue;
        CHECK_EQ(token(s, subset, "elementType"), std::string("face"));
        CHECK_EQ(token(s, subset, "familyName"), std::string("materialBind"));
        CHECK(indices(s, subset, 0.0) == std::vector<int>{k});
        const std::vector<std::string> bound = s.targets(*p, "material:binding");
        CHECK(bound.size() == 1 && bound[0] == std::string("/thing/Materials/") + names[k]);
        const std::string material = std::string("/thing/Materials/") + names[k];
        CHECK(s.find(material) && s.find(material)->type == "Material");
        // MaterialX for what reads it; a UsdPreviewSurface for the rest.
        const std::string surface = connection(s, material, "outputs:mtlx:surface");
        CHECK_EQ(surface, material + "/" + names[k] + "_surface.outputs:out");
        CHECK_EQ(token(s, material + "/" + names[k] + "_surface", "info:id"), std::string("ND_standard_surface_surfaceshader"));
        CHECK_EQ(connection(s, material, "outputs:surface"), material + "/" + names[k] + "_preview.outputs:surface");
        CHECK_EQ(token(s, material + "/" + names[k] + "_preview", "info:id"), std::string("UsdPreviewSurface"));
    }
    CHECK_EQ(token(s, "/thing/mesh", "subsetFamily:materialBind:familyType"), std::string("nonOverlapping"));
    // The bark's height: MaterialX's displacement and the preview's; the
    // glass has none.
    CHECK_EQ(connection(s, "/thing/Materials/bark", "outputs:mtlx:displacement"),
             std::string("/thing/Materials/bark/bark_displacement.outputs:out"));
    CHECK_EQ(token(s, "/thing/Materials/bark/bark_displacement", "info:id"), std::string("ND_displacement_float"));
    CHECK_EQ(connection(s, "/thing/Materials/bark", "outputs:displacement"),
             std::string("/thing/Materials/bark/bark_preview.outputs:displacement"));
    CHECK(connection(s, "/thing/Materials/glass", "outputs:mtlx:displacement").empty());
    // The bark's picture: copied beside the stage, its colour space said.
    const std::string file = token(s, "/thing/Materials/bark/bark_picture", "inputs:file");
    CHECK_EQ(file, std::string("./thing_textures/bark_color.jpg"));
    CHECK(fs::exists(dir.path / "thing_textures/bark_color.jpg"));
    CHECK(fs::exists(dir.path / "thing_textures/leaf_color.png"));
    const usd::Property* fileProperty = s.property(*s.find("/thing/Materials/bark/bark_picture"), "inputs:file");
    CHECK(fileProperty && fileProperty->meta("colorSpace") && fileProperty->meta("colorSpace")->text() == "srgb_texture");
    // The uv as st, a corner each.
    const usd::Stage::Prim* mesh = s.find("/thing/mesh");
    const usd::Property* st = s.property(*mesh, "primvars:st");
    CHECK(st && st->typeName == "texCoord2f[]");
    CHECK(st && st->meta("interpolation") && st->meta("interpolation")->text() == "faceVarying");
    CHECK_EQ(s.value(*mesh, "primvars:st", 0.0).numbers.size(), size_t(2 * 12));
    // Read back, the subsets are groups and the uv is there.
    const std::shared_ptr<Geometry> back = usd::importGeometry(s, 0.0, usd::ImportOptions());
    CHECK(back && back->primitiveCount() == 3);
    for (const char* name : names) CHECK(back && back->findGroup(name));
    CHECK(back && back->vertices().find("uv"));
    // All of one material: the mesh bound whole.
    auto bark = quads({"bark", "bark"});
    CHECK(sim::exportGeometry(*bark, dir / "trunk.usda", error));
    const auto trunk = usd::Stage::open(dir / "trunk.usda", error);
    CHECK(trunk && trunk->find("/trunk/mesh") && !trunk->find("/trunk/mesh/bark"));
    if (trunk) {
        const std::vector<std::string> whole = trunk->targets(*trunk->find("/trunk/mesh"), "material:binding");
        CHECK(whole.size() == 1 && whole[0] == "/trunk/Materials/bark");
    }
}

TEST(a_shot_binds_its_geometry_to_materials_frame_by_frame) {
    TempFolder dir("mtlx_shot");
    std::string error;
    {
        sim::UsdExport usd(dir / "shot.usda", "trees");
        sim::Frame frame;
        // Frame 1 bark and a leaf; frame 2 concrete besides; frame 3 a leaf alone.
        frame.number = 1;
        CHECK(usd.add(frame, quads({"bark", "leaf"}), nullptr, sim::Look(), error));
        frame.number = 2;
        CHECK(usd.add(frame, quads({"bark", "leaf", "concrete"}), nullptr, sim::Look(), error));
        frame.number = 3;
        CHECK(usd.add(frame, quads({"leaf"}), nullptr, sim::Look(), error));
        CHECK(usd.finish(error));
    }
    const auto stage = usd::Stage::open(dir / "shot.usda", error);
    CHECK(stage);
    if (!stage) return;
    const usd::Stage& s = *stage;
    CHECK(s.find("/World/Materials/bark") && s.find("/World/Materials/leaf") && s.find("/World/Materials/concrete"));
    const std::string mesh = "/World/trees/mesh";
    CHECK(indices(s, mesh + "/bark", 1) == std::vector<int>{0});
    CHECK(indices(s, mesh + "/leaf", 1) == std::vector<int>{1});
    CHECK(indices(s, mesh + "/concrete", 1).empty());
    CHECK(indices(s, mesh + "/concrete", 2) == std::vector<int>{2});
    CHECK(indices(s, mesh + "/bark", 3).empty());
    CHECK(indices(s, mesh + "/leaf", 3) == std::vector<int>{0});
    for (const char* name : {"bark", "leaf", "concrete"}) {
        const usd::Stage::Prim* p = s.find(mesh + "/" + name);
        CHECK(p && p->type == "GeomSubset");
        if (p) CHECK(s.targets(*p, "material:binding") == std::vector<std::string>{std::string("/World/Materials/") + name});
    }
    // The materials as MaterialX of their own, their pictures beside them.
    std::vector<mtlx::Node> nodes;
    CHECK(mtlx::parse(fileText(dir / "shot.mtlx"), nodes, error));
    for (const char* name : {"bark", "leaf", "concrete"}) CHECK(mtlx::surfaceOf(nodes, name).found);
    CHECK(fs::exists(dir.path / "shot_textures/concrete_color.jpg"));
}

TEST(geometry_goes_to_materialx_alone_as_mtlx) {
    TempFolder dir("mtlx_alone");
    std::string error;
    CHECK(sim::exportGeometry(*quads({"bark", "leaf"}), dir / "field.0007.mtlx", error));
    std::vector<mtlx::Node> nodes;
    CHECK(mtlx::parse(fileText(dir / "field.0007.mtlx"), nodes, error));
    CHECK(mtlx::surfaceOf(nodes, "bark").found && mtlx::surfaceOf(nodes, "leaf").found);
    // The pictures of a sequence's frames in one folder: of its name without
    // the frame.
    CHECK(fs::exists(dir.path / "field_textures/bark_color.jpg"));
    CHECK(std::find(sim::exportExtensions(), sim::exportExtensions() + 6, std::string(".mtlx")) != sim::exportExtensions() + 6);
    // Nothing made of a material: said so.
    Geometry points;
    points.addPoints(3);
    CHECK(!sim::exportGeometry(points, dir / "dots.mtlx", error));
    CHECK(!error.empty());
}
