#pragma once

#include "pg/core/Types.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace pg {

/// What a surface is made of. A primitive's string attribute `material`
/// names it; a renderer draws each its own way -- with the pictures of it
/// that come with the program where there are some (examples/textures:
/// render/Textures.h), else a pattern of it, on the colour Cd, rough or
/// smooth, metal or not (render/Cycles.cpp). Brick Wall, Concrete Fracture,
/// Glass Fracture, Tree, Grass and the RBD Solver's bars set theirs; the
/// Material node any. Brick is a brick's own face; Brick Wall a wall of
/// them, bricks and mortar in one picture -- for a wall that is one face.
/// Roof a flat one (concrete, tar); Roof Tiles a pitched one, slates in
/// rows. The later ones last: a Material node keeps its choice by number.
enum class MaterialPreset : uint8_t {
    None,
    Concrete,
    BrokenConcrete,
    Brick,
    BrickWall,
    Mortar,
    Plaster,
    Window,
    Glass,
    Steel,
    Metal,
    Asphalt,
    Wood,
    Stone,
    Roof,
    Bark,
    Leaf,
    Grass,
    Soil,
    Paving,
    RoofTiles,
    Lawn,
    Sand,
};
inline constexpr size_t kMaterialPresets = 23;

/// The names `material` has for them, in their order: "" for none.
inline constexpr std::array<std::string_view, kMaterialPresets> kMaterialNames = {
    "",      "concrete", "broken_concrete", "brick", "brick_wall", "mortar", "plaster", "window", "glass", "steel",
    "metal", "asphalt",  "wood",            "stone", "roof",       "bark",   "leaf",    "grass",  "soil",
    "paving", "roof_tiles", "lawn", "sand"};

/// The preset `name` stands for: None for "" and for a name there is none of.
inline MaterialPreset materialPreset(std::string_view name) {
    for (size_t i = 1; i < kMaterialPresets; ++i) {
        if (kMaterialNames[i] == name) return static_cast<MaterialPreset>(i);
    }
    return MaterialPreset::None;
}

inline std::string_view materialName(MaterialPreset p) {
    const auto i = static_cast<size_t>(p);
    return i < kMaterialPresets ? kMaterialNames[i] : std::string_view();
}

/// Whether the library's pictures of `p` are made to be laid on by uv --
/// bark round and up a stem, a leaf on its blade, grass up a blade --
/// where the faces have it (the Tree and Grass nodes' do), rather than from
/// three sides.
inline bool laidByUv(MaterialPreset p) {
    return p == MaterialPreset::Bark || p == MaterialPreset::Leaf || p == MaterialPreset::Grass;
}

/// How rough and how metal a surface of `preset` is -- what both renderers
/// make of it; the attributes roughness and metallic, where there are any,
/// before it -- and its colour where the geometry has no Cd (linear light;
/// for a set of photographs, about their own).
struct PresetSurface {
    float roughness = 0.5f;
    float metallic = 0.0f;
    Vec3 color{0.72f, 0.72f, 0.74f};
};

inline PresetSurface presetSurface(MaterialPreset preset) {
    switch (preset) {
        case MaterialPreset::None: return {0.5f, 0.0f, Vec3(0.72f, 0.72f, 0.74f)};
        case MaterialPreset::Concrete: return {0.85f, 0.0f, Vec3(0.31f, 0.3f, 0.28f)};
        case MaterialPreset::BrokenConcrete: return {0.95f, 0.0f, Vec3(0.36f, 0.34f, 0.31f)};
        case MaterialPreset::Brick: return {0.85f, 0.0f, Vec3(0.3f, 0.1f, 0.06f)};
        case MaterialPreset::BrickWall: return {0.85f, 0.0f, Vec3(0.25f, 0.08f, 0.05f)};
        case MaterialPreset::Mortar: return {0.95f, 0.0f, Vec3(0.42f, 0.4f, 0.36f)};
        case MaterialPreset::Plaster: return {0.8f, 0.0f, Vec3(0.6f, 0.58f, 0.54f)};
        case MaterialPreset::Window: return {0.04f, 0.0f, Vec3(0.04f, 0.05f, 0.06f)};
        case MaterialPreset::Glass: return {0.0f, 0.0f, Vec3(0.9f, 0.95f, 0.95f)};
        case MaterialPreset::Steel: return {0.45f, 0.8f, Vec3(0.4f, 0.4f, 0.42f)};
        case MaterialPreset::Metal: return {0.3f, 1.0f, Vec3(0.55f, 0.56f, 0.57f)};
        case MaterialPreset::Asphalt: return {0.9f, 0.0f, Vec3(0.06f, 0.06f, 0.065f)};
        case MaterialPreset::Wood: return {0.65f, 0.0f, Vec3(0.23f, 0.14f, 0.08f)};
        case MaterialPreset::Stone: return {0.75f, 0.0f, Vec3(0.3f, 0.29f, 0.27f)};
        case MaterialPreset::Roof: return {0.8f, 0.0f, Vec3(0.2f, 0.19f, 0.18f)};
        case MaterialPreset::Bark: return {0.9f, 0.0f, Vec3(0.15f, 0.1f, 0.06f)};
        case MaterialPreset::Leaf: return {0.5f, 0.0f, Vec3(0.08f, 0.17f, 0.03f)};
        case MaterialPreset::Grass: return {0.6f, 0.0f, Vec3(0.1f, 0.22f, 0.04f)};
        case MaterialPreset::Soil: return {0.95f, 0.0f, Vec3(0.09f, 0.065f, 0.045f)};
        case MaterialPreset::Paving: return {0.8f, 0.0f, Vec3(0.16f, 0.15f, 0.14f)};
        case MaterialPreset::RoofTiles: return {0.7f, 0.0f, Vec3(0.06f, 0.065f, 0.07f)};
        case MaterialPreset::Lawn: return {0.65f, 0.0f, Vec3(0.14f, 0.24f, 0.05f)};
        case MaterialPreset::Sand: return {0.95f, 0.0f, Vec3(0.45f, 0.36f, 0.22f)};
    }
    return {};
}

}  // namespace pg
