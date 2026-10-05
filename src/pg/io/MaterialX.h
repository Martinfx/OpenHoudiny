#pragma once
//
// MaterialX documents (.mtlx): materials as graphs of nodes -- what Houdini,
// Maya, Blender (4.x import) and the USD renderers share. Written and read
// without the library, of the standard nodes (MaterialX 1.38 and on):
//
//   <materialx version="1.38" colorspace="lin_rec709">
//     <image name="brick_color" type="color3">
//       <input name="file" type="filename" value="color.jpg" colorspace="srgb_texture" />
//     </image>
//     <standard_surface name="brick_surface" type="surfaceshader">
//       <input name="base_color" type="color3" nodename="brick_color" />
//     </standard_surface>
//     <surfacematerial name="brick" type="material">
//       <input name="surfaceshader" type="surfaceshader" nodename="brick_surface" />
//     </surfacematerial>
//   </materialx>
//
// A node is a category (image, multiply, standard_surface...), a name, a
// type, and inputs: each a value, or the output of another node by its
// name. The same graph goes into USD as UsdShade shaders (ND_<category>_
// <type> ids), sim/UsdExport.h.
//
#include "pg/core/Types.h"

#include <string>
#include <vector>

namespace pg::io::mtlx {

struct Input {
    std::string name, type;
    std::string value;       ///< as MaterialX writes it: "0.5", "0.2, 0.3, 0.4", a file
    std::string nodename;    ///< or the node it comes from
    std::string colorspace;  ///< a file's: "srgb_texture", "lin_rec709"; "" for none
    /// Which output of `nodename`: "" its only one (out). A UsdPreviewSurface
    /// graph's textures have several -- rgb, a, r.
    std::string output;
};

struct Node {
    std::string category, name, type;
    std::vector<Input> inputs;
    /// The input `name`, or null.
    const Input* input(const std::string& name) const;
};

/// A material: its nodes, the last a surfacematerial named as the material.
struct Material {
    std::string name;
    std::vector<Node> nodes;
};

/// Values as MaterialX writes them: 0.5; 0.2, 0.3, 0.4.
std::string number(float x);
std::string numbers(const Vec3& v);

/// The document of `materials`, as MaterialX writes one.
std::string document(const std::vector<Material>& materials);

/// The id of the definition of node `n` in MaterialX 1.38's standard
/// library -- what USD names a MaterialX shader by (info:id):
/// ND_image_color3, ND_multiply_vector3FA (times a float),
/// ND_convert_float_color3, ND_normalmap. `n`'s inputs' types say which.
std::string nodeDef(const Node& n);

/// `nodes` with each normalmap spelled out as the arithmetic it stands for
/// -- its tangent-space vector from 0..1 to -1..1, x and y scaled, along
/// the surface's tangent, bitangent (N x T) and normal in world space,
/// normalized -- of nodes whose definitions MaterialX 1.38 and 1.39 name
/// alike: 1.39 renamed normalmap's (ND_normalmap_float), so that a USD
/// shader of either id is lost on the other. The spelled-out one keeps the
/// normalmap's name, so what took its output takes the sum's.
std::vector<Node> portable(const std::vector<Node>& nodes);

/// The type USD gives a value of MaterialX type `type`: color3f for color3,
/// float3 for vector3, asset for filename, token for a shader; any other --
/// a USD type already (normal3f, token) -- as it is.
std::string usdType(const std::string& type);

/// The nodes of a document, at its top level and in its node graphs
/// (prefixed by none: names are unique in a document as written by
/// MaterialX); false, with why, for text that is not one.
bool parse(const std::string& text, std::vector<Node>& nodes, std::string& error);

/// What a surface material of `nodes` shows, followed back from its
/// standard_surface (or UsdPreviewSurface, open_pbr_surface, gltf_pbr): the
/// files of its base colour,
/// its normal map, its opacity, its roughness and its height (its
/// displacement, as deep as its scale says) -- through what passes a
/// picture on: image, tiledimage, triplanarprojection, normalmap, multiply,
/// extract... -- and its roughness and metalness where they are values. The
/// first surfacematerial, or the one named `name`.
struct Surface {
    std::string color, normal, opacity, roughnessFile, height;
    float roughness = -1.0f, metalness = -1.0f;  ///< -1: not given
    float size = -1.0f;            ///< metres a picture, laid from three sides as many as its position is scaled by; -1 not said
    /// Metres from the lowest of its height to the highest: the scale of
    /// its displacement (a UsdPreviewSurface's: its height picture's); -1
    /// not said.
    float depth = -1.0f;
    bool opacityFromAlpha = false; ///< the opacity the alpha of its picture (extract 3, a texture's a)
    bool tinted = false;           ///< the colour times a geometric property's (displayColor): tinted by Cd
    /// What the colour's picture is multiplied by first, a colour of three
    /// values -- one over its mean, as MaterialGraph.h evens it -- or -1.
    Vec3 scale{-1.0f, -1.0f, -1.0f};
    bool normalDirectX = false;    ///< the normal map's green turned over on its way (times -1)
    /// What it says as values -- the shader's own defaults where it says
    /// none, as a renderer of it takes them: the base colour, times its
    /// weight (-1 where something else gives it: a picture, a property);
    /// how rough, how metal, how opaque (a colour's mean), how much light
    /// goes through it, its index of refraction.
    struct Values {
        Vec3 color{-1.0f, -1.0f, -1.0f};
        float roughness = 0.5f, metalness = 0.0f, opacity = 1.0f, transmission = 0.0f, ior = 1.5f;
    } values;
    std::string shader;            ///< its category: standard_surface, UsdPreviewSurface, open_pbr_surface, gltf_pbr
    bool found = false;
};
Surface surfaceOf(const std::vector<Node>& nodes, const std::string& name = {});

}  // namespace pg::io::mtlx
