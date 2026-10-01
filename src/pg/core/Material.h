#pragma once

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

}  // namespace pg
