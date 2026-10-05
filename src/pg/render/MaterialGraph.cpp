#include "pg/render/MaterialGraph.h"

#include "pg/render/PathTracer.h"
#include "pg/render/Textures.h"

#include <algorithm>
#include <climits>
#include <filesystem>

namespace pg::render {

namespace fs = std::filesystem;
namespace mtlx = io::mtlx;

namespace {

mtlx::Input value(const char* name, const char* type, std::string v, std::string colorspace = {}) {
    return {name, type, std::move(v), {}, std::move(colorspace), {}};
}

mtlx::Input from(const char* name, const char* type, const std::string& node, std::string output = {}) {
    return {name, type, {}, node, {}, std::move(output)};
}

/// The nodes of a graph, each named after the material: bark_picture.
struct Nodes {
    std::string prefix;
    std::vector<mtlx::Node> list;

    std::string add(const char* category, const char* suffix, const char* type, std::vector<mtlx::Input> inputs) {
        list.push_back({category, prefix + "_" + suffix, type, std::move(inputs)});
        return list.back().name;
    }
};

/// The colour of a material's surfaces where they have none of their own:
/// the viewport's grey where it is of no material, or glass or water.
Vec3 ownColor(const Material& m) {
    return presetSurface(m.kind == Material::Kind::Surface ? m.preset : MaterialPreset::None).color;
}

/// Whether `geo` has texture coordinates as the renderers take them: uv of
/// the corners, else of the points.
bool hasUv(const Geometry& geo) {
    for (const AttributeArray* a : {geo.vertices().find("uv"), geo.points().find("uv")}) {
        if (a && (a->type() == AttrType::Vec2 || a->type() == AttrType::Vec3)) return true;
    }
    return false;
}

bool hasColor(const Geometry& geo) {
    return geo.vertices().find("Cd") || geo.points().find("Cd") || geo.primitives().find("Cd") || geo.detail().find("Cd");
}

}  // namespace

std::string lookName(const Material& m) {
    switch (m.kind) {
        case Material::Kind::Glass: return "glass";
        case Material::Kind::Water: return "water";
        case Material::Kind::Rain: return "rain";
        case Material::Kind::Surface: break;
    }
    if (!m.texture.empty()) {
        // Its own texture's: its folder, or its picture without the extension.
        const fs::path p(m.texture);
        const std::string own = p.has_extension() ? p.stem().string() : p.filename().string();
        if (!own.empty()) return io::usda::identifier(own);
    }
    if (m.preset != MaterialPreset::None) return std::string(materialName(m.preset));
    return "plain";
}

MaterialGraph materialGraph(const Material& m, const std::string& name, const GraphSource& source,
                            const std::function<std::string(const std::string& file)>& picture) {
    MaterialGraph g;
    g.mtlx.name = name;
    Nodes n{name, {}};
    std::vector<mtlx::Node> preview;
    auto previewNode = [&](const char* category, const char* suffix, std::vector<mtlx::Input> inputs) {
        preview.push_back({category, name + "_" + suffix, "", std::move(inputs)});
        return preview.back().name;
    };
    auto file = [&](const std::string& f) { return picture ? picture(f) : f; };
    const Vec3 own = ownColor(m);
    // What the renderers lay on it -- none on glass or water.
    Settings settings;
    const TextureSet set = textureOf(m, settings);
    std::vector<mtlx::Input> surface;
    std::vector<mtlx::Input> look;  // the UsdPreviewSurface's
    std::string displacement;       // its displacementshader, where it has a height
    // Where the pictures laid from three sides are taken: the position over
    // the metres a picture covers.
    std::string scaled;
    // The colour of its surfaces: theirs, else its material's.
    const std::string color = source.colored ? n.add("geompropvalue", "cd", "color3",
                                                     {value("geomprop", "string", "displayColor"),
                                                      value("default", "color3", mtlx::numbers(own))})
                                             : std::string();
    std::string st;  // the preview's texture coordinates
    auto previewSt = [&]() {
        if (st.empty()) st = previewNode("UsdPrimvarReader_float2", "preview_st", {value("varname", "string", "st")});
        return st;
    };
    auto previewPicture = [&](const char* suffix, const std::string& f, const char* space, std::vector<mtlx::Input> more) {
        std::vector<mtlx::Input> in = {value("file", "filename", file(f)), from("st", "vector2", previewSt()),
                                       value("sourceColorSpace", "token", space), value("wrapS", "token", "repeat"),
                                       value("wrapT", "token", "repeat")};
        for (mtlx::Input& i : more) in.push_back(std::move(i));
        return previewNode("UsdUVTexture", suffix, std::move(in));
    };

    switch (m.kind) {
        case Material::Kind::Surface: {
            // Its colour: the picture over its mean times the colour, or the
            // picture as it is; without one, the colour.
            std::string base = color, previewColor;
            if (set.valid()) {
                const std::string f = file(set.color);
                std::string shown;
                if (m.byUv) {
                    shown = n.add("image", "picture", "color3", {value("file", "filename", f, "srgb_texture")});
                } else {
                    const std::string at = source.rest ? n.add("geompropvalue", "rest", "vector3",
                                                               {value("geomprop", "string", "rest")})
                                                       : n.add("position", "position", "vector3",
                                                               {value("space", "string", "object")});
                    scaled = n.add("multiply", "scaled", "vector3",
                                   {from("in1", "vector3", at),
                                    value("in2", "float", mtlx::number(1.0f / std::max(set.size, 1e-3f)))});
                    shown = n.add("triplanarprojection", "picture", "color3",
                                  {value("filex", "filename", f, "srgb_texture"), value("filey", "filename", f, "srgb_texture"),
                                   value("filez", "filename", f, "srgb_texture"), from("position", "vector3", scaled)});
                }
                const Vec3 evened = Vec3(1.0f) / glm::max(set.mean, Vec3(1e-4f));
                if (!set.tint) {
                    base = shown;
                } else if (!color.empty()) {
                    const std::string even = n.add("multiply", "evened", "color3",
                                                   {from("in1", "color3", shown), value("in2", "color3", mtlx::numbers(evened))});
                    base = n.add("multiply", "tinted", "color3", {from("in1", "color3", even), from("in2", "color3", color)});
                } else {
                    base = n.add("multiply", "tinted", "color3",
                                 {from("in1", "color3", shown), value("in2", "color3", mtlx::numbers(own * evened))});
                }
                if (m.byUv) {
                    // The preview cannot multiply by a primvar: tinted by its
                    // material's colour, else as it is.
                    const Vec3 k = set.tint && color.empty() ? own * evened : Vec3(1.0f);
                    previewColor = previewPicture("preview_picture", set.color, "sRGB",
                                                  {value("scale", "vector4", mtlx::numbers(k) + ", 1")});
                }
            }
            surface.push_back(value("base", "float", "1"));
            surface.push_back(base.empty() ? value("base_color", "color3", mtlx::numbers(own)) : from("base_color", "color3", base));
            surface.push_back(value("specular_roughness", "float", mtlx::number(m.roughness)));
            surface.push_back(value("metalness", "float", mtlx::number(m.metallic)));
            if (m.translucency > 0.0f) {
                // A thin sheet letting light through: a leaf, a blade.
                surface.push_back(value("thin_walled", "boolean", "true"));
                surface.push_back(value("subsurface", "float", mtlx::number(m.translucency)));
                surface.push_back(base.empty() ? value("subsurface_color", "color3", mtlx::numbers(own))
                                               : from("subsurface_color", "color3", base));
            }
            // Its normal map, laid on by uv.
            if (set.valid() && m.byUv && !set.normal.empty() && m.normalStrength > 0.0f) {
                const std::string f = file(set.normal);
                std::string map = n.add("image", "normal_picture", "vector3", {value("file", "filename", f)});
                if (set.normalDirectX) {
                    // Its green the other way: up the picture as OpenGL has it.
                    map = n.add("multiply", "normal_flipped", "vector3",
                                {from("in1", "vector3", map), value("in2", "vector3", "1, -1, 1")});
                    map = n.add("add", "normal_opengl", "vector3", {from("in1", "vector3", map), value("in2", "vector3", "0, 1, 0")});
                }
                const std::string bent = n.add("normalmap", "normal", "vector3",
                                               {from("in", "vector3", map), value("scale", "float", mtlx::number(m.normalStrength))});
                surface.push_back(from("normal", "vector3", bent));
                const float s = m.normalStrength, g = set.normalDirectX ? -1.0f : 1.0f;
                const std::string normal = previewPicture(
                    "preview_normal", set.normal, "raw",
                    {value("scale", "vector4", mtlx::numbers(Vec3(2.0f * s, 2.0f * s * g, 2.0f)) + ", 1"),
                     value("bias", "vector4", mtlx::numbers(Vec3(-s, -s * g, -1.0f)) + ", 0")});
                look.push_back(from("normal", "normal3f", normal, "rgb"));
            }
            // Cut out where its alpha has none.
            std::string previewAlpha, previewAlphaOutput;
            if (set.valid() && m.byUv && !set.alpha.empty()) {
                const std::string f = file(set.alpha);
                std::string alpha;
                if (set.alphaChannel) {
                    const std::string rgba = n.add("image", "alpha_picture", "color4", {value("file", "filename", f)});
                    alpha = n.add("extract", "alpha", "float", {from("in", "color4", rgba), value("index", "integer", "3")});
                } else {
                    alpha = n.add("image", "alpha", "float", {value("file", "filename", f)});
                }
                surface.push_back(from("opacity", "color3", n.add("convert", "opacity", "color3", {from("in", "float", alpha)})));
                if (set.alphaChannel && set.alpha == set.color && !previewColor.empty()) {
                    previewAlpha = previewColor;
                    previewAlphaOutput = "a";
                } else {
                    previewAlpha = previewPicture("preview_alpha", set.alpha, "raw", {});
                    previewAlphaOutput = set.alphaChannel ? "a" : "r";
                }
            }
            // How high: its height picture, laid on as its colour is, about
            // its middle -- half of it out, half in -- as deep as the set
            // says from its lowest to its highest: the material's
            // displacement, what the renderers bump by.
            if (set.valid() && !set.height.empty() && set.depth > 0.0f) {
                const std::string f = file(set.height);
                const std::string high =
                    m.byUv ? n.add("image", "height_picture", "float", {value("file", "filename", f)})
                           : n.add("triplanarprojection", "height_picture", "float",
                                   {value("filex", "filename", f), value("filey", "filename", f),
                                    value("filez", "filename", f), from("position", "vector3", scaled)});
                const std::string middle =
                    n.add("subtract", "height", "float", {from("in1", "float", high), value("in2", "float", "0.5")});
                displacement = n.add("displacement", "displacement", "displacementshader",
                                     {from("displacement", "float", middle), value("scale", "float", mtlx::number(set.depth))});
                if (m.byUv) {
                    const float d = set.depth;
                    const std::string height =
                        previewPicture("preview_height", set.height, "raw",
                                       {value("scale", "vector4", mtlx::numbers(Vec3(d)) + ", 1"),
                                        value("bias", "vector4", mtlx::numbers(Vec3(-0.5f * d)) + ", 0")});
                    look.push_back(from("displacement", "float", height, "r"));
                }
            }
            if (!previewColor.empty()) {
                look.push_back(from("diffuseColor", "color3", previewColor, "rgb"));
            } else if (!color.empty()) {
                look.push_back(from("diffuseColor", "color3",
                                    previewNode("UsdPrimvarReader_float3", "preview_cd",
                                                {value("varname", "string", "displayColor"),
                                                 value("fallback", "vector3", mtlx::numbers(own))}),
                                    "result"));
            } else {
                look.push_back(value("diffuseColor", "color3", mtlx::numbers(own)));
            }
            look.push_back(value("roughness", "float", mtlx::number(m.roughness)));
            look.push_back(value("metallic", "float", mtlx::number(m.metallic)));
            if (!previewAlpha.empty()) {
                look.push_back(from("opacity", "float", previewAlpha, previewAlphaOutput));
                look.push_back(value("opacityThreshold", "float", "0.5"));
            }
            break;
        }
        case Material::Kind::Glass: {
            // Clear, bending light, tinted a little by the colour at each
            // face it passes, as the renderers have it.
            surface.push_back(value("base", "float", "0"));
            if (!color.empty()) {
                surface.push_back(from("transmission_color", "color3",
                                       n.add("mix", "tint", "color3",
                                             {from("fg", "color3", color), value("bg", "color3", "1, 1, 1"),
                                              value("mix", "float", "0.35")})));
            } else {
                surface.push_back(value("transmission_color", "color3", mtlx::numbers(Vec3(0.65f) + own * 0.35f)));
            }
            surface.push_back(value("transmission", "float", "1"));
            surface.push_back(value("specular_roughness", "float", "0"));
            surface.push_back(value("specular_IOR", "float", mtlx::number(m.ior)));
            look.push_back(value("diffuseColor", "color3", mtlx::numbers(Vec3(0.65f) + own * 0.35f)));
            look.push_back(value("ior", "float", mtlx::number(m.ior)));
            look.push_back(value("opacity", "float", "0.1"));
            look.push_back(value("roughness", "float", "0.02"));
            break;
        }
        case Material::Kind::Water:
        case Material::Kind::Rain: {
            surface.push_back(value("base", "float", "0"));
            surface.push_back(value("transmission", "float", "1"));
            surface.push_back(value("specular_roughness", "float", "0"));
            surface.push_back(value("specular_IOR", "float", mtlx::number(m.ior)));
            look.push_back(value("diffuseColor", "color3", "0.8, 0.9, 1"));
            look.push_back(value("ior", "float", mtlx::number(m.ior)));
            look.push_back(value("opacity", "float", "0.35"));
            look.push_back(value("roughness", "float", "0.02"));
            break;
        }
    }
    const std::string shader = n.add("standard_surface", "surface", "surfaceshader", std::move(surface));
    std::vector<mtlx::Input> outputs = {from("surfaceshader", "surfaceshader", shader)};
    if (!displacement.empty()) outputs.push_back(from("displacementshader", "displacementshader", displacement));
    n.list.push_back({"surfacematerial", name, "material", std::move(outputs)});
    previewNode("UsdPreviewSurface", "preview", std::move(look));
    g.mtlx.nodes = std::move(n.list);
    g.preview = std::move(preview);
    return g;
}

MaterialLooks::MaterialLooks(std::string scope, std::string pictures)
    : scope_(std::move(scope)), prefix_(std::move(pictures)) {}

std::string MaterialLooks::pictureName(const std::string& file) {
    for (const Picture& p : pictures_) {
        if (p.from == file) return prefix_ + p.name;
    }
    // Its set's name and its own: bark_color.jpg.
    const fs::path path(file);
    const std::string set = path.parent_path().filename().string();
    const std::string stem = io::usda::identifier((set.empty() ? std::string() : set + "_") + path.stem().string());
    std::string extension = path.extension().string();
    for (char& c : extension) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    std::string name = stem + extension;
    for (int k = 2; std::any_of(pictures_.begin(), pictures_.end(), [&](const Picture& p) { return p.name == name; }); ++k) {
        name = stem + "_" + std::to_string(k) + extension;
    }
    pictures_.push_back({file, name});
    return prefix_ + name;
}

void MaterialLooks::materialsOf(const Geometry& geo, Found& found) const {
    primitiveMaterials(geo, false, hasUv(geo), found.materials, found.ofPrim);
    found.source.colored = hasColor(geo);
    const AttributeArray* rest = geo.points().find("rest");
    found.source.rest = rest && rest->type() == AttrType::Vec3;
}

const MaterialLooks::Look* MaterialLooks::lookOf(const Material& m, const GraphSource& source) const {
    for (const Look& l : looks_) {
        if (l.material == m && l.source.colored == source.colored && l.source.rest == source.rest) return &l;
    }
    return nullptr;
}

io::usda::FaceMaterials MaterialLooks::facesOf(const Found& found) const {
    io::usda::FaceMaterials out;
    std::vector<uint32_t> which(found.materials.size(), UINT32_MAX);
    for (size_t k = 0; k < found.materials.size(); ++k) {
        const Look* l = lookOf(found.materials[k], found.source);
        if (!l) continue;
        which[k] = static_cast<uint32_t>(out.names.size());
        out.names.push_back(l->name);
        out.targets.push_back("<" + scope_ + "/" + l->name + ">");
    }
    out.ofPrim.resize(found.ofPrim.size(), UINT32_MAX);
    for (size_t p = 0; p < found.ofPrim.size(); ++p) {
        if (found.ofPrim[p] < which.size()) out.ofPrim[p] = which[found.ofPrim[p]];
    }
    return out;
}

io::usda::FaceMaterials MaterialLooks::bind(const Geometry& geo) {
    Found found;
    materialsOf(geo, found);
    for (const Material& m : found.materials) {
        if (lookOf(m, found.source)) continue;
        // Named as it is called; the second of a name _2, and so on.
        const std::string called = lookName(m);
        std::string name = called;
        for (int n = 2; std::any_of(looks_.begin(), looks_.end(), [&](const Look& l) { return l.name == name; }); ++n) {
            name = called + "_" + std::to_string(n);
        }
        Look look{m, found.source, name, {}};
        look.graph = materialGraph(m, name, found.source, [this](const std::string& f) { return pictureName(f); });
        looks_.push_back(std::move(look));
    }
    return facesOf(found);
}

io::usda::FaceMaterials MaterialLooks::bound(const Geometry& geo) const {
    Found found;
    materialsOf(geo, found);
    return facesOf(found);
}

void MaterialLooks::addTo(io::usda::Prim& scope) const {
    for (const Look& l : looks_) {
        std::vector<mtlx::Node> nodes = l.graph.mtlx.nodes;
        nodes.insert(nodes.end(), l.graph.preview.begin(), l.graph.preview.end());
        io::usda::Prim& material = scope.children.emplace_back(io::usda::materialPrim(l.name, scope_ + "/" + l.name, nodes));
        // How much of what Cycles adds to the program's own surfaces it
        // takes (Material::detail): for USD Import, which gives a material
        // made elsewhere none.
        if (l.material.kind == Material::Kind::Surface) {
            material.set("float", "pg:surface_detail", io::usda::number(l.material.detail)).custom = true;
        }
    }
}

std::string MaterialLooks::document() const {
    std::vector<mtlx::Material> all;
    for (const Look& l : looks_) all.push_back(l.graph.mtlx);
    return mtlx::document(all);
}

bool MaterialLooks::copyPictures(const std::string& folder, std::string& error) const {
    if (pictures_.empty()) return true;
    std::error_code ec;
    fs::create_directories(folder, ec);
    for (const Picture& p : pictures_) {
        const fs::path to = fs::path(folder) / p.name;
        fs::copy_file(p.from, to, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            error = "cannot copy " + p.from + " to " + to.string() + ": " + ec.message();
            return false;
        }
    }
    return true;
}

}  // namespace pg::render
