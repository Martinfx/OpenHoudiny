//
// Materials from USD (usd/Shade.h, usd::importGeometry): which material a
// face is bound to, as USD resolves it; UsdPreviewSurface and MaterialX
// networks -- through node graphs, their interfaces, a package -- as the
// program's material, texture, roughness, metallic, glass and Cd; their
// pictures as a texture set the renderers read; and what the program
// itself writes read back as it was.
//
#include "pg/core/Geometry.h"
#include "pg/core/Material.h"
#include "pg/io/Picture.h"
#include "pg/render/Scene.h"
#include "pg/render/Textures.h"
#include "pg/sim/GeometryGraph.h"
#include "pg/sim/Network.h"
#include "pg/sim/UsdExport.h"
#include "pg/usd/Geom.h"
#include "pg/usd/Shade.h"
#include "pg/usd/Stage.h"

#include "test_framework.h"

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

using namespace pg;
namespace fs = std::filesystem;

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

void writeFile(const std::string& path, const std::string& text) {
    fs::create_directories(fs::path(path).parent_path());
    std::ofstream(path, std::ios::binary) << text;
}

/// A picture of one colour, as a screen shows it, 4 x 4.
std::vector<uint8_t> pngOf(const Vec3& shown) {
    io::Picture p;
    p.width = p.height = 4;
    for (int i = 0; i < 16; ++i) p.rgba.insert(p.rgba.end(), {shown.x, shown.y, shown.z, 1.0f});
    const fs::path file = fs::temp_directory_path() / ("pg_test_png_" + std::to_string(std::random_device()()) + ".png");
    std::string error;
    io::writePicture(file.string(), p, 95, error);
    std::ifstream in(file, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::error_code ec;
    fs::remove(file, ec);
    return bytes;
}

void writePng(const std::string& path, const Vec3& shown) {
    fs::create_directories(fs::path(path).parent_path());
    const std::vector<uint8_t> bytes = pngOf(shown);
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

/// A package as USD makes one: a zip of files stored as they are, the first
/// its layer.
void writeUsdz(const std::string& path, const std::vector<std::pair<std::string, std::vector<uint8_t>>>& files) {
    auto crc32 = [](const std::vector<uint8_t>& data) {
        uint32_t c = 0xFFFFFFFFu;
        for (const uint8_t b : data) {
            c ^= b;
            for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
        }
        return ~c;
    };
    std::vector<uint8_t> zip, directory;
    auto put = [](std::vector<uint8_t>& out, uint32_t v, int bytes) {
        for (int k = 0; k < bytes; ++k) out.push_back(static_cast<uint8_t>(v >> (8 * k)));
    };
    for (const auto& [name, data] : files) {
        const uint32_t at = static_cast<uint32_t>(zip.size());
        const uint32_t crc = crc32(data);
        // Its data at a multiple of 64 bytes, as USD lays a package out.
        const size_t start = zip.size() + 30 + name.size();
        const size_t pad = (64 - start % 64) % 64;
        put(zip, 0x04034b50u, 4);
        put(zip, 20, 2);
        put(zip, 0, 2);
        put(zip, 0, 2);
        put(zip, 0, 4);
        put(zip, crc, 4);
        put(zip, static_cast<uint32_t>(data.size()), 4);
        put(zip, static_cast<uint32_t>(data.size()), 4);
        put(zip, static_cast<uint32_t>(name.size()), 2);
        put(zip, static_cast<uint32_t>(pad), 2);
        zip.insert(zip.end(), name.begin(), name.end());
        zip.insert(zip.end(), pad, 0);
        zip.insert(zip.end(), data.begin(), data.end());
        put(directory, 0x02014b50u, 4);
        put(directory, 20, 2);
        put(directory, 20, 2);
        put(directory, 0, 2);
        put(directory, 0, 2);
        put(directory, 0, 4);
        put(directory, crc, 4);
        put(directory, static_cast<uint32_t>(data.size()), 4);
        put(directory, static_cast<uint32_t>(data.size()), 4);
        put(directory, static_cast<uint32_t>(name.size()), 2);
        put(directory, 0, 2);
        put(directory, 0, 2);
        put(directory, 0, 2);
        put(directory, 0, 2);
        put(directory, 0, 4);
        put(directory, at, 4);
        directory.insert(directory.end(), name.begin(), name.end());
    }
    const uint32_t directoryAt = static_cast<uint32_t>(zip.size());
    zip.insert(zip.end(), directory.begin(), directory.end());
    put(zip, 0x06054b50u, 4);
    put(zip, 0, 2);
    put(zip, 0, 2);
    put(zip, static_cast<uint32_t>(files.size()), 2);
    put(zip, static_cast<uint32_t>(files.size()), 2);
    put(zip, static_cast<uint32_t>(directory.size()), 4);
    put(zip, directoryAt, 4);
    put(zip, 0, 2);
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(zip.data()), static_cast<std::streamsize>(zip.size()));
}

/// A quad a metre across at x, with st, bound to `material` ("" none).
std::string quad(const std::string& name, float x, const std::string& material, const std::string& more = "") {
    const std::string a = std::to_string(x), b = std::to_string(x + 1.0f);
    std::string text = "    def Mesh \"" + name + "\" (\n        prepend apiSchemas = [\"MaterialBindingAPI\"]\n    )\n    {\n";
    text += "        int[] faceVertexCounts = [4]\n        int[] faceVertexIndices = [0, 1, 2, 3]\n";
    text += "        point3f[] points = [(" + a + ", 0, 0), (" + b + ", 0, 0), (" + b + ", 1, 0), (" + a + ", 1, 0)]\n";
    text += "        texCoord2f[] primvars:st = [(0, 0), (1, 0), (1, 1), (0, 1)] (\n            interpolation = \"vertex\"\n        )\n";
    text += more;
    if (!material.empty()) text += "        rel material:binding = <" + material + ">\n";
    text += "    }\n";
    return text;
}

std::string stringOf(const Geometry& g, const char* name, size_t prim) {
    const AttributeArray* a = g.primitives().find(name);
    return a && a->type() == AttrType::String ? a->stringValue(a->read<int32_t>()[prim]) : std::string("<none>");
}

float floatOf(const Geometry& g, const char* name, size_t prim) {
    const AttributeArray* a = g.primitives().find(name);
    return a && a->type() == AttrType::Float ? a->read<float>()[prim] : -100.0f;
}

int intOf(const Geometry& g, const char* name, size_t prim) {
    const AttributeArray* a = g.primitives().find(name);
    return a && a->type() == AttrType::Int ? a->read<int32_t>()[prim] : -100;
}

/// The colour a primitive shows: its corners', else its points', else its own.
Vec3 colourOf(const Geometry& g, size_t prim) {
    if (const AttributeArray* v = g.vertices().find("Cd")) return v->read<Vec3>()[g.primitiveVertexStart(prim)];
    if (const AttributeArray* p = g.points().find("Cd")) return p->read<Vec3>()[g.primitivePoints(prim)[0]];
    if (const AttributeArray* p = g.primitives().find("Cd")) return p->read<Vec3>()[prim];
    return Vec3(-1.0f);
}

bool near(const Vec3& a, const Vec3& b, float within = 1e-5f) { return length(a - b) <= within; }

std::string normal(const std::string& path) { return fs::path(path).lexically_normal().generic_string(); }

std::shared_ptr<Geometry> import(const std::string& file, bool materials = true) {
    std::string error;
    const auto stage = usd::Stage::open(file, error);
    if (!stage) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return std::make_shared<Geometry>();
    }
    usd::ImportOptions o;
    o.materials = materials;
    std::vector<std::string> notes;
    return usd::importGeometry(*stage, 0.0, o, &notes);
}

const char* kHeader = "#usda 1.0\n(\n    defaultPrim = \"World\"\n    metersPerUnit = 1\n    upAxis = \"Y\"\n)\n\n";

}  // namespace

TEST(usd_materials_of_preview_surfaces_become_the_programs) {
    TempFolder dir("usdmat_preview");
    writePng(dir / "maps/wood_diffuse.png", Vec3(0.6f, 0.4f, 0.2f));
    writePng(dir / "maps/wood_normal.png", Vec3(0.5f, 0.5f, 1.0f));
    writePng(dir / "maps/wood_rough.png", Vec3(0.7f));
    const std::string file = dir / "set.usda";
    writeFile(file, std::string(kHeader) + "def Xform \"World\"\n{\n" + quad("Box", 0, "/World/Looks/Painted") +
                        quad("Floor", 2, "/World/Looks/Wood") + quad("Pane", 4, "/World/Looks/Glass") +
                        quad("Bare", 6, "", "        color3f[] primvars:displayColor = [(0.1, 0.8, 0.2)]\n") + R"(
    def Scope "Looks"
    {
        def Material "Painted"
        {
            token outputs:surface.connect = </World/Looks/Painted/Surface.outputs:surface>
            def Shader "Surface"
            {
                uniform token info:id = "UsdPreviewSurface"
                color3f inputs:diffuseColor = (0.8, 0.1, 0.1)
                float inputs:roughness = 0.3
                float inputs:metallic = 1
                token outputs:surface
            }
        }
        def Material "Wood"
        {
            token outputs:surface.connect = </World/Looks/Wood/Surface.outputs:surface>
            def Shader "Surface"
            {
                uniform token info:id = "UsdPreviewSurface"
                color3f inputs:diffuseColor.connect = </World/Looks/Wood/Diffuse.outputs:rgb>
                normal3f inputs:normal.connect = </World/Looks/Wood/Normal.outputs:rgb>
                float inputs:roughness.connect = </World/Looks/Wood/Rough.outputs:r>
                token outputs:surface
            }
            def Shader "Reader"
            {
                uniform token info:id = "UsdPrimvarReader_float2"
                token inputs:varname = "st"
                float2 outputs:result
            }
            def Shader "Diffuse"
            {
                uniform token info:id = "UsdUVTexture"
                asset inputs:file = @maps/wood_diffuse.png@ (
                    colorSpace = "sRGB"
                )
                float2 inputs:st.connect = </World/Looks/Wood/Reader.outputs:result>
                float3 outputs:rgb
            }
            def Shader "Normal"
            {
                uniform token info:id = "UsdUVTexture"
                asset inputs:file = @./maps/wood_normal.png@
                float4 inputs:scale = (2, 2, 2, 1)
                float4 inputs:bias = (-1, -1, -1, 0)
                float2 inputs:st.connect = </World/Looks/Wood/Reader.outputs:result>
                float3 outputs:rgb
            }
            def Shader "Rough"
            {
                uniform token info:id = "UsdUVTexture"
                asset inputs:file = @maps/wood_rough.png@
                float outputs:r
            }
        }
        def Material "Glass"
        {
            token outputs:surface.connect = </World/Looks/Glass/Surface.outputs:surface>
            def Shader "Surface"
            {
                uniform token info:id = "UsdPreviewSurface"
                float inputs:opacity = 0.1
                float inputs:ior = 1.5
                token outputs:surface
            }
        }
    }
}
)");
    const auto geo = import(file);
    CHECK_EQ(geo->primitiveCount(), 4u);
    if (geo->primitiveCount() != 4) return;
    // Each face its material's name; the bare one none.
    CHECK_EQ(stringOf(*geo, "material", 0), std::string("Painted"));
    CHECK_EQ(stringOf(*geo, "material", 1), std::string("Wood"));
    CHECK_EQ(stringOf(*geo, "material", 2), std::string("Glass"));
    CHECK_EQ(stringOf(*geo, "material", 3), std::string());
    // Its pictures: the stage's file and the material's path.
    CHECK_EQ(stringOf(*geo, "texture", 0), std::string());
    CHECK_EQ(stringOf(*geo, "texture", 1), file + "#/World/Looks/Wood");
    CHECK_EQ(intOf(*geo, "texture_tint", 1), 0);
    CHECK_EQ(intOf(*geo, "texture_projection", 1), 1);
    // Its values; where it gives none -- a picture's roughness, a face of
    // no material -- the shader's, the preset's.
    CHECK_NEAR(floatOf(*geo, "roughness", 0), 0.3f, 1e-6);
    CHECK_NEAR(floatOf(*geo, "metallic", 0), 1.0f, 1e-6);
    CHECK_NEAR(floatOf(*geo, "roughness", 1), 0.5f, 1e-6);
    CHECK_NEAR(floatOf(*geo, "metallic", 1), 0.0f, 1e-6);
    CHECK_NEAR(floatOf(*geo, "roughness", 3), presetSurface(MaterialPreset::None).roughness, 1e-6);
    // Light goes through the glass.
    CHECK_EQ(intOf(*geo, "glass", 2), 1);
    CHECK_EQ(intOf(*geo, "glass", 0), 0);
    // A colour that is a value is the face's; displayColor stays where no
    // material says otherwise.
    CHECK(near(colourOf(*geo, 0), Vec3(0.8f, 0.1f, 0.1f)));
    CHECK(near(colourOf(*geo, 3), Vec3(0.1f, 0.8f, 0.2f)));
    // The pictures as a set: colour, normal and roughness, from the layer's folder.
    const render::TextureSet set = render::textureSet(stringOf(*geo, "texture", 1));
    CHECK(set.valid());
    CHECK_EQ(normal(set.color), normal(dir / "maps/wood_diffuse.png"));
    CHECK_EQ(normal(set.normal), normal(dir / "maps/wood_normal.png"));
    CHECK_EQ(normal(set.roughness), normal(dir / "maps/wood_rough.png"));
    CHECK(!set.tint);
    // What the renderers make of it.
    std::vector<render::Material> materials;
    std::vector<uint16_t> ofPrim;
    render::primitiveMaterials(*geo, false, true, materials, ofPrim);
    const render::Material& painted = materials[ofPrim[0]];
    CHECK(painted.kind == render::Material::Kind::Surface);
    CHECK_NEAR(painted.roughness, 77.0f / 255.0f, 1e-6);
    CHECK_NEAR(painted.metallic, 1.0f, 1e-6);
    const render::Material& wood = materials[ofPrim[1]];
    CHECK_EQ(wood.texture, file + "#/World/Looks/Wood");
    CHECK(wood.byUv && wood.textureTint == 0);
    CHECK(materials[ofPrim[2]].kind == render::Material::Kind::Glass);
    // Off: the materials are not read.
    const auto plain = import(file, false);
    CHECK(!plain->primitives().find("material") && !plain->primitives().find("texture"));
}

TEST(usd_materials_of_materialx_go_through_node_graphs_and_interfaces) {
    TempFolder dir("usdmat_mtlx");
    writePng(dir / "maps/brick_color.png", Vec3(0.5f, 0.2f, 0.1f));
    writePng(dir / "maps/brick_normal.png", Vec3(0.5f, 0.5f, 1.0f));
    writePng(dir / "maps/brick_height.png", Vec3(0.4f));
    const std::string file = dir / "set.usda";
    writeFile(file, std::string(kHeader) + "def Xform \"World\"\n{\n" + quad("Wall", 0, "/World/Looks/Brick") +
                        quad("Toy", 2, "/World/Looks/Plastic") + quad("Ball", 4, "/World/Looks/Gltf") +
                        quad("Pane", 6, "/World/Looks/Clear") + R"(
    def Scope "Looks"
    {
        def Material "Brick"
        {
            asset inputs:color_file = @maps/brick_color.png@
            token outputs:mtlx:surface.connect = </World/Looks/Brick/Surface.outputs:out>
            token outputs:mtlx:displacement.connect = </World/Looks/Brick/Displace.outputs:out>
            token outputs:surface.connect = </World/Looks/Brick/Preview.outputs:surface>
            def Shader "Surface"
            {
                uniform token info:id = "ND_standard_surface_surfaceshader"
                color3f inputs:base_color.connect = </World/Looks/Brick/Graph.outputs:base_color>
                float inputs:specular_roughness = 0.7
                float3 inputs:normal.connect = </World/Looks/Brick/Graph.outputs:normal>
                token outputs:out
            }
            def Shader "Preview"
            {
                uniform token info:id = "UsdPreviewSurface"
                color3f inputs:diffuseColor = (1, 0, 1)
                token outputs:surface
            }
            def Shader "Displace"
            {
                uniform token info:id = "ND_displacement_float"
                float inputs:displacement.connect = </World/Looks/Brick/Height.outputs:out>
                token outputs:out
            }
            def Shader "Height"
            {
                uniform token info:id = "ND_image_float"
                asset inputs:file = @maps/brick_height.png@
                float outputs:out
            }
            def NodeGraph "Graph"
            {
                asset inputs:file.connect = </World/Looks/Brick.inputs:color_file>
                color3f outputs:base_color.connect = </World/Looks/Brick/Graph/Image.outputs:out>
                float3 outputs:normal.connect = </World/Looks/Brick/Graph/Normal.outputs:out>
                def Shader "Image"
                {
                    uniform token info:id = "ND_image_color3"
                    asset inputs:file.connect = </World/Looks/Brick/Graph.inputs:file>
                    color3f outputs:out
                }
                def Shader "NormalImage"
                {
                    uniform token info:id = "ND_image_vector3"
                    asset inputs:file = @maps/brick_normal.png@
                    float3 outputs:out
                }
                def Shader "Normal"
                {
                    uniform token info:id = "ND_normalmap"
                    float3 inputs:in.connect = </World/Looks/Brick/Graph/NormalImage.outputs:out>
                    float3 outputs:out
                }
            }
        }
        def Material "Plastic"
        {
            token outputs:mtlx:surface.connect = </World/Looks/Plastic/Surface.outputs:out>
            def Shader "Surface"
            {
                uniform token info:id = "ND_open_pbr_surface_surfaceshader"
                color3f inputs:base_color = (0.2, 0.4, 0.9)
                float inputs:base_weight = 0.5
                float inputs:base_metalness = 0.25
                token outputs:out
            }
        }
        def Material "Gltf"
        {
            token outputs:mtlx:surface.connect = </World/Looks/Gltf/Surface.outputs:out>
            def Shader "Surface"
            {
                uniform token info:id = "ND_gltf_pbr_surfaceshader"
                color3f inputs:base_color = (0.5, 0.5, 0.5)
                float inputs:roughness = 0.2
                float inputs:metallic = 0
                token outputs:out
            }
        }
        def Material "Clear"
        {
            token outputs:mtlx:surface.connect = </World/Looks/Clear/Surface.outputs:out>
            def Shader "Surface"
            {
                uniform token info:id = "ND_standard_surface_surfaceshader"
                float inputs:transmission = 1
                token outputs:out
            }
        }
    }
}
)");
    const auto geo = import(file);
    CHECK_EQ(geo->primitiveCount(), 4u);
    if (geo->primitiveCount() != 4) return;
    // The MaterialX network before the preview surface: its pictures,
    // through the node graph, its interface and the material's.
    CHECK_EQ(stringOf(*geo, "texture", 0), file + "#/World/Looks/Brick");
    const render::TextureSet set = render::textureSet(stringOf(*geo, "texture", 0));
    CHECK_EQ(normal(set.color), normal(dir / "maps/brick_color.png"));
    CHECK_EQ(normal(set.normal), normal(dir / "maps/brick_normal.png"));
    CHECK_EQ(normal(set.height), normal(dir / "maps/brick_height.png"));
    CHECK_NEAR(floatOf(*geo, "roughness", 0), 0.7f, 1e-6);
    // OpenPBR: its colour times its weight, its metalness, its roughness
    // as it is when it says none.
    CHECK(near(colourOf(*geo, 1), Vec3(0.1f, 0.2f, 0.45f)));
    CHECK_NEAR(floatOf(*geo, "metallic", 1), 0.25f, 1e-6);
    CHECK_NEAR(floatOf(*geo, "roughness", 1), 0.3f, 1e-6);
    // glTF's PBR.
    CHECK(near(colourOf(*geo, 2), Vec3(0.5f)));
    CHECK_NEAR(floatOf(*geo, "roughness", 2), 0.2f, 1e-6);
    // What light goes through is glass.
    CHECK_EQ(intOf(*geo, "glass", 3), 1);
    // The ids of MaterialX's node definitions as MaterialX's categories.
    std::string category, type;
    usd::shaderKind("ND_standard_surface_surfaceshader", category, type);
    CHECK(category == "standard_surface" && type == "surfaceshader");
    usd::shaderKind("ND_open_pbr_surface_surfaceshader", category, type);
    CHECK(category == "open_pbr_surface" && type == "surfaceshader");
    usd::shaderKind("ND_multiply_color3FA", category, type);
    CHECK(category == "multiply" && type == "color3");
    usd::shaderKind("ND_convert_float_color3", category, type);
    CHECK(category == "convert" && type == "color3");
    usd::shaderKind("ND_normalmap", category, type);
    CHECK(category == "normalmap" && type == "vector3");
    usd::shaderKind("UsdPrimvarReader_float3", category, type);
    CHECK(category == "UsdPrimvarReader_float3" && type == "float3");
}

TEST(usd_materials_are_bound_as_usd_resolves_bindings) {
    TempFolder dir("usdmat_bind");
    std::string looks = "    def Scope \"Looks\"\n    {\n";
    for (const char* m : {"A", "B", "C", "D", "E"}) {
        looks += std::string("        def Material \"") + m + "\"\n        {\n            token outputs:surface.connect = </World/Looks/" +
                 m + "/S.outputs:surface>\n            def Shader \"S\"\n            {\n                uniform token info:id = "
                     "\"UsdPreviewSurface\"\n                token outputs:surface\n            }\n        }\n";
    }
    looks += "    }\n";
    const std::string file = dir / "bind.usda";
    writeFile(file, std::string(kHeader) + R"(def Xform "World" (
    prepend apiSchemas = ["MaterialBindingAPI"]
)
{
    rel material:binding = </World/Looks/A>
    def Xform "Group" (
        prepend apiSchemas = ["MaterialBindingAPI"]
    )
    {
        rel material:binding = </World/Looks/B>
)" + quad("Inherits", 0, "") + R"(    }
)" + quad("Own", 2, "/World/Looks/C") + R"(
    def Xform "Strong" (
        prepend apiSchemas = ["MaterialBindingAPI"]
    )
    {
        rel material:binding = </World/Looks/D> (
            bindMaterialAs = "strongerThanDescendants"
        )
)" + quad("Overruled", 4, "/World/Looks/C") + R"(    }
)" + quad("Full", 6, "/World/Looks/A", "        rel material:binding:full = </World/Looks/E>\n") + R"(
    def Mesh "Faces" (
        prepend apiSchemas = ["MaterialBindingAPI"]
    )
    {
        int[] faceVertexCounts = [3, 3, 3]
        int[] faceVertexIndices = [0, 1, 2, 0, 2, 3, 0, 3, 4]
        point3f[] points = [(8, 0, 0), (9, 0, 0), (9, 1, 0), (8, 1, 0), (8, 2, 0)]
        rel material:binding = </World/Looks/C>
        uniform token subsetFamily:materialBind:familyType = "nonOverlapping"
        def GeomSubset "left" (
            prepend apiSchemas = ["MaterialBindingAPI"]
        )
        {
            uniform token elementType = "face"
            uniform token familyName = "materialBind"
            int[] indices = [0]
            rel material:binding = </World/Looks/B>
        }
        def GeomSubset "right"
        {
            uniform token elementType = "face"
            uniform token familyName = "materialBind"
            int[] indices = [1]
        }
        def GeomSubset "other" (
            prepend apiSchemas = ["MaterialBindingAPI"]
        )
        {
            uniform token elementType = "face"
            uniform token familyName = "somethingElse"
            int[] indices = [2]
            rel material:binding = </World/Looks/E>
        }
    }
)" + quad("Wrong", 10, "/World/Group") + looks + "}\n");
    std::string error;
    const auto stage = usd::Stage::open(file, error);
    CHECK(stage != nullptr);
    if (!stage) return;
    auto bound = [&](const char* path) {
        const usd::Stage::Prim* p = stage->find(path);
        return p ? usd::boundMaterial(*stage, *p) : std::string("<no prim>");
    };
    // The nearest binding: an ancestor's, its own.
    CHECK_EQ(bound("/World/Group/Inherits"), std::string("/World/Looks/B"));
    CHECK_EQ(bound("/World/Own"), std::string("/World/Looks/C"));
    // An ancestor's stronger than its descendants'.
    CHECK_EQ(bound("/World/Strong/Overruled"), std::string("/World/Looks/D"));
    // For rendering: the purpose full first.
    CHECK_EQ(bound("/World/Full"), std::string("/World/Looks/E"));
    // A binding to what is not a material binds nothing.
    CHECK_EQ(bound("/World/Wrong"), std::string());
    // The faces of a mesh's subsets of the family materialBind by theirs;
    // the others by the mesh's.
    const std::vector<usd::BoundFaces> subsets = usd::boundSubsets(*stage, *stage->find("/World/Faces"), 0.0);
    CHECK_EQ(subsets.size(), 2u);
    const auto geo = import(file);
    std::vector<std::string> names;
    for (size_t p = 0; p < geo->primitiveCount(); ++p) names.push_back(stringOf(*geo, "material", p));
    CHECK(names == (std::vector<std::string>{"B", "C", "D", "E", "B", "C", "C", ""}));
}

TEST(usd_materials_the_program_writes_come_back_as_they_were) {
    TempFolder dir("usdmat_back");
    // Bark laid by uv, concrete from three sides, a picture of one's own,
    // glass, and a face of no material.
    const std::vector<std::string> made = {"bark", "concrete", "", "glass", ""};
    auto geo = std::make_shared<Geometry>();
    geo->addPoints(2 * (made.size() + 1));
    auto P = geo->positionsForWrite();
    for (size_t i = 0; i <= made.size(); ++i) {
        P[2 * i] = Vec3(static_cast<float>(i), 0.0f, 0.0f);
        P[2 * i + 1] = Vec3(static_cast<float>(i), 1.0f, 0.0f);
    }
    for (size_t k = 0; k < made.size(); ++k) {
        const auto a = static_cast<uint32_t>(2 * k);
        const uint32_t ring[4] = {a, a + 2, a + 3, a + 1};
        geo->addPrimitive(ring, true);
    }
    auto uv = geo->vertices().create("uv", AttrType::Vec3).write<Vec3>();
    for (size_t k = 0; k < made.size(); ++k) {
        uv[4 * k] = Vec3(0.0f, 0.0f, 0.0f);
        uv[4 * k + 1] = Vec3(1.0f, 0.0f, 0.0f);
        uv[4 * k + 2] = Vec3(1.0f, 1.0f, 0.0f);
        uv[4 * k + 3] = Vec3(0.0f, 1.0f, 0.0f);
    }
    for (size_t k = 0; k < made.size(); ++k) {
        std::vector<uint8_t> mask(made.size(), 0);
        mask[k] = 1;
        setPrimitiveString(*geo, "material", made[k], mask);
    }
    writePng(dir / "own/tiles_color.png", Vec3(0.3f, 0.5f, 0.7f));
    std::vector<uint8_t> own(made.size(), 0);
    own[2] = 1;
    setPrimitiveString(*geo, "texture", dir / "own/tiles_color.png", own);
    std::string error;
    CHECK(sim::exportGeometry(*geo, dir / "thing.usda", error));
    const auto back = import(dir / "thing.usda");
    CHECK_EQ(back->primitiveCount(), made.size());
    if (back->primitiveCount() != made.size()) return;
    // The presets by their names; a face of none by its look's, which is
    // no preset.
    CHECK_EQ(stringOf(*back, "material", 0), std::string("bark"));
    CHECK_EQ(stringOf(*back, "material", 1), std::string("concrete"));
    CHECK_EQ(stringOf(*back, "material", 3), std::string("glass"));
    CHECK(materialPreset(stringOf(*back, "material", 4)) == MaterialPreset::None);
    // What the renderers make of each: the same kind, preset, roughness and
    // way of laying the pictures on; the pictures the copies beside the
    // stage, of the same colour.
    std::vector<render::Material> was, is;
    std::vector<uint16_t> wasOf, isOf;
    render::primitiveMaterials(*geo, false, true, was, wasOf);
    render::primitiveMaterials(*back, false, true, is, isOf);
    int compared = 0;
    for (size_t p = 0; p < made.size(); ++p) {
        const render::Material& a = was[wasOf[p]];
        const render::Material& b = is[isOf[p]];
        CHECK(a.kind == b.kind);
        if (a.kind != render::Material::Kind::Surface) continue;
        CHECK(a.preset == b.preset);
        CHECK_NEAR(a.roughness, b.roughness, 1e-6);
        CHECK_NEAR(a.metallic, b.metallic, 1e-6);
        const render::TextureSet sa = !a.texture.empty() ? render::textureSet(a.texture)
                                                         : render::presetTextureSet(render::textureLibrary(), a.preset);
        const render::TextureSet sb = render::textureSet(b.texture);
        CHECK(sa.valid() == sb.valid());
        if (!sa.valid() || !sb.valid()) continue;
        CHECK(b.texture.find(dir / "thing.usda#/thing/Materials/") == 0);
        CHECK(fs::path(sb.color).parent_path().filename() == "thing_textures");
        CHECK(near(sa.mean, sb.mean, 0.02f));
        CHECK(a.byUv == b.byUv);
        CHECK_EQ(!sa.normal.empty(), !sb.normal.empty());
        ++compared;
    }
    // Bark, concrete and the picture of one's own; the glass as glass.
    CHECK_EQ(compared, 3);
    CHECK(is[isOf[3]].kind == render::Material::Kind::Glass);
}

TEST(usd_materials_in_a_package_read_their_pictures_from_it) {
    TempFolder dir("usdmat_usdz");
    const std::string layer = std::string(kHeader) + "def Xform \"World\"\n{\n" + quad("Tile", 0, "/World/Looks/Tile") + R"(
    def Scope "Looks"
    {
        def Material "Tile"
        {
            token outputs:surface.connect = </World/Looks/Tile/Surface.outputs:surface>
            def Shader "Surface"
            {
                uniform token info:id = "UsdPreviewSurface"
                color3f inputs:diffuseColor.connect = </World/Looks/Tile/Picture.outputs:rgb>
                token outputs:surface
            }
            def Shader "Picture"
            {
                uniform token info:id = "UsdUVTexture"
                asset inputs:file = @maps/tile.png@
                float3 outputs:rgb
            }
        }
    }
}
)";
    const Vec3 shown(0.8f, 0.4f, 0.2f);
    writeUsdz(dir / "tile.usdz", {{"tile.usda", std::vector<uint8_t>(layer.begin(), layer.end())}, {"maps/tile.png", pngOf(shown)}});
    const auto geo = import(dir / "tile.usdz");
    CHECK_EQ(geo->primitiveCount(), 1u);
    if (geo->primitiveCount() != 1) return;
    CHECK_EQ(stringOf(*geo, "texture", 0), (dir / "tile.usdz") + "#/World/Looks/Tile");
    // The picture in the package, as USD names it, read from there.
    const render::TextureSet set = render::textureSet(stringOf(*geo, "texture", 0));
    CHECK_EQ(set.color, normal(dir / "tile.usdz") + "[maps/tile.png]");
    io::Picture picture;
    std::string error;
    CHECK(io::readPicture(set.color, picture, error));
    CHECK(picture.width == 4 && std::abs(picture.pixel(1, 1)[0] - shown.x) < 0.01f);
    CHECK(near(set.mean, Vec3(io::srgbToLinear(shown.x), io::srgbToLinear(shown.y), io::srgbToLinear(shown.z)), 0.01f));
}

TEST(usd_materials_through_the_usd_import_node) {
    TempFolder dir("usdmat_node");
    const std::string file = dir / "one.usda";
    writeFile(file, std::string(kHeader) + "def Xform \"World\"\n{\n" + quad("Box", 0, "/World/Looks/Red") + R"(
    def Material "Unused"
    {
    }
    def Scope "Looks"
    {
        def Material "Red"
        {
            token outputs:surface.connect = </World/Looks/Red/S.outputs:surface>
            def Shader "S"
            {
                uniform token info:id = "UsdPreviewSurface"
                color3f inputs:diffuseColor = (0.9, 0.05, 0.05)
                token outputs:surface
            }
        }
    }
}
)");
    sim::Network net;
    const int n = net.add("usd_import");
    CHECK(net.setText(n, "file", file));
    sim::GeometryGraph graph;
    graph.sync(net);
    GeometryPtr g = graph.cook(n, 1, 1.0f / 24.0f);
    CHECK_EQ(stringOf(*g, "material", 0), std::string("Red"));
    CHECK(near(colourOf(*g, 0), Vec3(0.9f, 0.05f, 0.05f)));
    // Materials off: as before there were any.
    net.setParam(n, "materials", "0");
    graph.sync(net);
    g = graph.cook(n, 1, 1.0f / 24.0f);
    CHECK(!g->primitives().find("material") && !g->primitives().find("Cd"));
}
