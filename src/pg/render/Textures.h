#pragma once
//
// Pictures of surfaces: textures. A set is the pictures of one -- its colour,
// how high it is (a height or displacement map) and how rough, where there
// are these -- and how many metres one picture covers. Both renderers lay a
// set on a surface from three sides at once, as much from each as the
// surface faces that way, by where the surface was before it moved (the
// point attribute rest): it goes with a piece that flies. A set whose rows
// must stay level -- a roof's slates -- is laid along each face instead,
// where the face slopes: across it level, up it as it rises, whichever way
// it faces. Cycles takes its height for bumps too.
//
// The sets that come with the program are in examples/textures, a folder
// for each material (core/Material.h) that has one -- concrete and its
// break, plaster, a brick wall, mortar, metal, asphalt, wood, roofs, paving,
// bark, soil, a lawn, sand -- made by tools/textures/prepare.py from the
// photographs of pbrt-v4-scenes and BabylonJS/Assets (CC-BY 4.0,
// examples/textures/README.md): color.jpg, height.jpg and texture.txt. Any
// other set -- what Poly Haven or ambientCG give away -- is found from a
// picture of it: the pictures beside it whose names differ in what they are
// (_diff_ and _rough_ and _disp_ and _nor_gl_, _Color and _Roughness and
// _Displacement and _NormalGL...).
//
// A MaterialX document (.mtlx) is a set too -- materials.mtlx#bark one
// material of it, else its first; a folder with one in it, as Poly Haven
// and ambientCG give them away: the pictures its surface shows
// (io/MaterialX.h), tinted by Cd where its colour is tinted by a geometric
// property, as render/MaterialGraph.h writes them.
//
// Where the geometry has texture coordinates (uv on its corners) and the
// material asks for them (Material::byUv), a set is laid on by them instead,
// a picture a unit of uv -- and its normal map, if it has one, bends the
// light: in the tangent space of the uv (Mesh::tangents), its green up the
// picture as OpenGL has it, or down it for one of DirectX's (_nor_dx_,
// _NormalDX). A set with an alpha cuts the surface out where it has none
// (Material::cutout): the leaves of the library's leaf picture.
//
#include "pg/core/Material.h"
#include "pg/core/Types.h"

#include <memory>
#include <string>
#include <vector>

namespace pg::render {

struct Material;
struct Settings;

struct TextureSet {
    std::string color;      ///< the picture of its colour (sRGB); "" for no set
    std::string height;     ///< how high (0 to 1), or ""
    std::string roughness;  ///< how rough (0 to 1), or ""
    /// Its normal map -- the way the surface faces at each pixel, x y z in
    /// red green blue from 0 to 1 -- or "".
    std::string normal;
    /// The normal map's green up the picture's rows, as DirectX has it --
    /// not down them, as OpenGL and Blender do.
    bool normalDirectX = false;
    /// How much of the surface there is (0 none, 1 all of it): the picture
    /// whose alpha says so -- the colour's own (texture.txt: alpha 1) -- or
    /// whose grey does (opacity.png beside texture.txt; someone else's
    /// _opacity_, _alpha_, _mask_, else the alpha of their colour where it
    /// leaves some out); "" for a surface whole everywhere. Cut
    /// out where it is laid on by uv: a leaf's edge.
    std::string alpha;
    bool alphaChannel = false;  ///< `alpha` read from the picture's alpha, not its grey
    float size = 2.0f;      ///< metres one picture covers
    float depth = 0.01f;    ///< metres from the lowest of the height to the highest
    Vec3 mean{0.5f, 0.5f, 0.5f};  ///< the average of its colour, linear light
    /// The colour Cd in place of the picture's own: the picture lighter and
    /// darker round it, its colour divided by `mean`. Else the picture as
    /// it is -- a brick wall, its mortar lighter than its bricks.
    bool tint = true;
    /// Laid along each face where it slopes (texture.txt: projection face),
    /// not from three sides: rows that must stay level.
    bool alongFace = false;
    /// Made to be laid on by uv alone (texture.txt: projection uv) -- a
    /// leaf, a blade of grass: none where a surface has no uv.
    bool onlyByUv = false;

    bool valid() const { return !color.empty(); }
    bool operator==(const TextureSet&) const = default;
};

/// The set `where` names -- a folder (its texture.txt, pictures named as
/// above, or a MaterialX document), one picture of a set, a MaterialX
/// document (#material), or a Material of a USD stage
/// ("shot.usda#/World/Materials/bark": its MaterialX or UsdPreviewSurface
/// network, usd/Shade.h) -- read once and remembered; none for what is not
/// one.
TextureSet textureSet(const std::string& where);

/// The folder of the sets that come with the program: $PG_TEXTURES when it
/// is one, else examples/textures of the sources it was built from; "" if
/// neither is there.
std::string textureLibrary();

/// The set of the material `preset` in `library`: its folder there, named as
/// the material is (kMaterialNames); none where there is none.
TextureSet presetTextureSet(const std::string& library, MaterialPreset preset);

/// What a renderer lays on surfaces of `m` with `settings`: its own texture
/// (the Material node's), else its material's from the library -- either as
/// big as `m` says, if it does; none with textures off.
TextureSet textureOf(const Material& m, const Settings& settings);

/// A set's colour as the path tracer looks it up: linear light, tiling.
struct TexturePicture {
    int width = 0, height = 0;
    std::vector<Vec3> pixels;  ///< the top row first
    float size = 2.0f;         ///< metres one picture covers
    Vec3 mean{0.5f, 0.5f, 0.5f};
    bool tint = true;          ///< TextureSet::tint
    bool alongFace = false;    ///< TextureSet::alongFace

    /// The picture at (u, v) pictures from its corner, between pixels.
    Vec3 at(float u, float v) const;
    /// Laid on at `rest`, the surface facing `face` there (a unit long):
    /// from three sides, or along the face.
    Vec3 onSurface(const Vec3& rest, const Vec3& face) const;
    /// A surface of colour `color` -- Cd times its copy's `tint` -- with it
    /// laid on: tinted, the colour times the picture over its mean; else
    /// the picture times the tint. No more than 0.95. By `uv` where there is
    /// one: a picture a unit of it.
    Vec3 shade(const Vec3& color, const Vec3& tint, const Vec3& rest, const Vec3& face, const Vec2* uv = nullptr) const;
};

/// The picture of `set`, read once and remembered; null when it cannot be read.
std::shared_ptr<const TexturePicture> texturePicture(const TextureSet& set);

/// What the path tracer lays on a material: its picture, and its normal map
/// where it is laid on by uv (Material::byUv).
struct SurfacePictures {
    std::shared_ptr<const TexturePicture> color, normal;
    std::shared_ptr<const TexturePicture> alpha;  ///< where it is cut out (Material::cutout)
    bool directX = false;  ///< TextureSet::normalDirectX
};

/// The normal map of `set`, its values as they are (not light), read once
/// and remembered; null for none, or one that cannot be read.
std::shared_ptr<const TexturePicture> normalPicture(const TextureSet& set);

/// How much of the surface there is, by `set`'s alpha (TextureSet::alpha),
/// in each pixel's red; read once and remembered; null for none.
std::shared_ptr<const TexturePicture> alphaPicture(const TextureSet& set);

/// The unit normal `n` bent as the normal map `map` says at `uv`, as strongly
/// as `strength`: in the space of `tangent` (across n, the way u goes) and
/// n x tangent times `handed` (the way v goes); `directX` its green the
/// other way. Never further than just above the surface the ray met --
/// `face` its normal on n's side.
Vec3 bentNormal(const TexturePicture& map, bool directX, const Vec2& uv, const Vec3& n, const Vec3& tangent, float handed,
                float strength, const Vec3& face);

}  // namespace pg::render
