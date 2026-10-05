#pragma once
//
// UsdShade, as far as a renderer of our own needs it.
//
// Which material a prim is bound to, as USD resolves it for rendering: the
// bindings of the purpose full -- the prim's own, else the nearest above it
// with one, unless one above says its binding is strongerThanDescendants
// (bindMaterialAs), the topmost such then --; none of them anywhere above,
// the bindings of all purposes so. The faces of a mesh's GeomSubsets of
// the family materialBind go by theirs.
//
// What a material's surface is: the shader its MaterialX surface output
// (outputs:mtlx:surface) is connected to -- else its universal one
// (outputs:surface) -- and the shaders behind it, as MaterialX nodes
// (io/MaterialX.h): UsdPreviewSurface with UsdUVTexture and
// UsdPrimvarReader, or MaterialX's own (ND_standard_surface_surfaceshader,
// ND_image_color3, ND_normalmap...) by their node definitions' names;
// through node graphs' outputs and the interface inputs of node graphs and
// of the material; its displacement too. Each node is named by its prim's
// path; files are resolved as USD resolves an asset: from the layer that
// names it (inside a .usdz, "x.usdz[inner/path]", as io::readPicture reads
// them).
//
#include "pg/io/MaterialX.h"
#include "pg/usd/Stage.h"

#include <cstdint>
#include <string>
#include <vector>

namespace pg::usd {

/// The path of the Material `prim` is bound to -- a GeomSubset: its faces'
/// -- for rendering; "" for none.
std::string boundMaterial(const Stage& stage, const Stage::Prim& prim);

/// A mesh's faces that go by a GeomSubset of the family materialBind: each
/// subset's faces -- indices of the mesh's, as the file has them -- and its
/// material (its own binding, else as the mesh's).
struct BoundFaces {
    std::string subset, material;
    std::vector<uint32_t> faces;
};
std::vector<BoundFaces> boundSubsets(const Stage& stage, const Stage::Prim& mesh, double time);

/// The surface of `material` as MaterialX nodes: a surfacematerial first,
/// its surfaceshader and displacementshader the shaders the material's
/// outputs are connected to, then every node behind them. Empty when it
/// has no surface output connected to a shader.
std::vector<io::mtlx::Node> materialNodes(const Stage& stage, const Stage::Prim& material, double time);

/// What `material` shows (io::mtlx::surfaceOf of its nodes); not found
/// when it has no surface.
io::mtlx::Surface materialSurface(const Stage& stage, const Stage::Prim& material, double time);

/// MaterialX's category and output type of a shader's info:id:
/// "ND_standard_surface_surfaceshader" is standard_surface of
/// surfaceshader, "ND_multiply_color3FA" multiply of color3; USD's own
/// (UsdPreviewSurface, UsdUVTexture, UsdPrimvarReader_float3) as they are.
void shaderKind(const std::string& id, std::string& category, std::string& type);

}  // namespace pg::usd
