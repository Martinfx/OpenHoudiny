# MaterialX

MaterialX (Academy Software Foundation) is a common material language: a
material as a graph of nodes (image, multiply, `standard_surface`…) that
Houdini, Maya, Blender and USD-based renderers (Karma, Arnold, RenderMan,
Storm in usdview) all understand the same way. Prototype both **writes and
reads** materials in it:

- **export to USD**: every material is a `Material` built from MaterialX
  shaders (`outputs:mtlx:surface`), plus a `UsdPreviewSurface` for programs
  that do not read MaterialX. Faces are assigned to materials via
  `GeomSubset`, one per material, and photos are copied next to the scene;
- **standalone `.mtlx`**: the geometry's materials as a MaterialX document;
- **reading**: a `.mtlx` document (including from Poly Haven or ambientCG)
  can be used as a texture set: on the Material node, in a wrangle and in the library.

Both the writer and the reader were built from the public specification,
without the MaterialX library.

![The foliage example exported to USD, opened in Blender 4.5 and rendered in Cycles: a linden with leaves cut out by alpha, a spruce with needles, bark with a photo and a normal map, grass as instances](img/materialx-blender.jpg)

*The `foliage` example written with `prototype cook foliage foliage.usda` and
opened in Blender 4.5.3 (File › Import › USD), Cycles. The materials, photos,
leaf alpha, normal maps and UVs all came from the export; the camera and
light are Blender's.*

![Soil, bark, leaf and grass materials from the exported .mtlx document, rendered by the MaterialX library renderer](img/materialx-spheres.jpg)

*The `foliage.mtlx` document rendered by the GLSL renderer of the MaterialX
1.39.5 library on a sphere with UVs: soil (triplanar), bark (by UV, with a
normal map), leaf (alpha cutout, quarters of the leaf image) and grass.*

## 1. Quick start

```bash
./build/prototype cook foliage out/foliage.usda        # geometry with materials + out/foliage_textures/
./build/prototype cook foliage out/foliage.mtlx        # materials only, as MaterialX
./build/prototype sim forest - --frames 24 --export out/forest.usda   # shot: /World/Materials, out/forest.mtlx
```

- **Editor:** File › Export Geometry… with the `.usda` or `.mtlx` extension.
- **Python:** `geo.save("strom.usda")`, `geo.save("strom.mtlx")`.
- **Reading:** on the Material node, put `cesta/materialy.mtlx` in Texture
  (the document's first material) or `cesta/materialy.mtlx#bark` (material
  by name). The folder containing the `.mtlx` is enough too, as Poly Haven delivers it.

## 2. What gets written

A material is whatever the renderers distinguish ([materials.md](materials.md)):
`material`, roughness, metalness, translucency, custom texture, its size
and tint, mapping (UV or triplanar) and normal map strength. On top of that,
whether the geometry has a `Cd` color is distinguished. Each such material is
written once and gets a name based on what it is made of: `bark`, `leaf`,
`concrete`, `glass`, the name of the custom texture (`bricks_02`), and
`plain` without a material. A second material of the same name (say, bark
with a different roughness) is `bark_2`.

Each material is a `standard_surface` graph over the same photos the
renderers map:

| Ours | In MaterialX |
|---|---|
| color | `geompropvalue` `displayColor` (that is `Cd` in USD); without `Cd`, the material color |
| photo by UV | `image` (`colorspace="srgb_texture"`), one photo per uv unit (`primvars:st`) |
| triplanar photo | `triplanarprojection` by `position` (object), or by the `rest` primvar if the geometry has one; coordinates times 1/photo size in meters |
| tint by `Cd` | photo × 1/set average × color (two `multiply` nodes); an untinted set is the photo as is |
| normal map (by UV) | `normalmap` with `scale` = Normal Strength; a DirectX map has green flipped (`multiply` (1, −1, 1), `add` (0, 1, 0)) |
| alpha cutout | `opacity` ← `convert` ← `extract` 3 ← `image` color4; a gray mask is `image` float |
| height | the material's `displacementshader`: `displacement` with `scale` = set depth in meters ← `subtract` 0.5 ← `image` float (by UV) or `triplanarprojection` float (triplanar, the same as the color) |
| translucency of leaves and blades | `thin_walled` and `subsurface` = translucency, `subsurface_color` = color |
| roughness, metal | `specular_roughness`, `metalness`; `base` 1 (the color is albedo) |
| glass | `transmission` 1, `specular_IOR` 1.5, `transmission_color` 0.65 + 0.35 × color (like the renderers) |
| water | `transmission` 1, `specular_IOR` 1.33 |

**Height** is displacement of the surface along the normal. The middle of the
image (0.5) stays on the surface, lighter areas go out, darker ones in, by
the set's depth in total (`depth` in `texture.txt`, 13 mm for bricks). A
renderer that displaces geometry (such as Karma in Houdini) thus really
deepens the joints between bricks. Blender turns the height into a
Displacement node when importing USD. Other renderers skip the height or
turn it into bump. Our Cycles turns it into bump; with **Displacement** on
the Output node it displaces the surface the same way
([cycles.md](cycles.md#displacement-by-height)).

Nodes are named after the material (`bark_picture`, `bark_surface`…) and the
graph ends with a `surfacematerial` node named after the material. The
document has version 1.38 and the `lin_rec709` working space; MaterialX 1.39
upgrades it automatically on read.

**UsdPreviewSurface** sits in USD next to MaterialX for programs that do not
read MaterialX (such as Blender's USD import): `diffuseColor` from a
`UsdUVTexture` by `st` or from `displayColor`, `roughness`, `metallic`, a
normal map (`sourceColorSpace` raw, `scale` and `bias`), alpha as `opacity`
with `opacityThreshold` 0.5, height as `displacement` (`UsdUVTexture` with
`scale` = depth and `bias` = −half the depth), glass and water with `ior`
and transparency.
UsdPreviewSurface cannot multiply by a primvar, so a photo tinted by `Cd` is
the photo as is in it.

## 3. In USD

```
/World/Materials/bark          Material                         (shot; single geometry: /<name>/Materials)
    outputs:mtlx:surface       →  bark_surface        ND_standard_surface_surfaceshader
    outputs:mtlx:displacement  →  bark_displacement   ND_displacement_float (if the set has height)
    outputs:surface            →  bark_preview        UsdPreviewSurface
    outputs:displacement       →  bark_preview        its displacement from UsdUVTexture
    bark_picture, bark_evened, bark_tinted, bark_cd, bark_normal…   MaterialX shaders
    bark_preview_picture, bark_preview_st…                           UsdUVTexture, UsdPrimvarReader
/World/<node>/mesh             Mesh with primvars:st
    /bark, /leaf, /plain       GeomSubset: elementType face, familyName materialBind, material binding
```

- **Shaders** are the graph's nodes; `info:id` is the name of the node
  definition in the MaterialX library (`ND_image_color3`,
  `ND_multiply_vector3FA`…). Inputs with an image are `asset` with
  `colorSpace` metadata.
- **The normal map is expanded in USD** into basic nodes (decoding 0..1 to
  −1..1, scaling x and y, tangent, bitangent N × T and world-space normal,
  normalization). MaterialX 1.39 renamed the `normalmap` definition
  (`ND_normalmap` → `ND_normalmap_float`), and a shader with one of those ids
  would not be found by the other version. The expanded nodes have the same
  ids in both versions. The `.mtlx` document keeps `normalmap` (there the
  definition is looked up by types, not by name).
- **Assignment:** a mesh gets a `GeomSubset` for each material (family
  `materialBind`, `nonOverlapping`); each face is in exactly one. A mesh made
  entirely of one material is bound to it as a whole, without subsets.
  Instance prototypes (trees and grass as instances) have their materials
  too.
- **Changing geometry** (wind, simulation) has the subset indices in the frame
  layers like other values ([usd.md](usd.md#3-per-frame-files-value-clips)).
  A material that a frame does not have gets an empty subset in that frame.
- **UVs** are `texCoord2f[] primvars:st`: faceVarying from vertex `uv`,
  vertex from point `uv`.
- **Photos** are copied to `<name>_textures/` next to the scene
  (`bark_color.jpg`: set name and file name), and the paths are relative, so
  it is enough to move the folder with the scene. Frames of a sequence
  (`pole.0007.usda`) share a single `pole_textures/` folder.
- A shot also writes `<name>.mtlx` with all materials next to the scene.

Pieces from the RBD Solver (`/World/pieces`) keep their `surface` and
`glass` materials from `/World/Looks` ([usd.md](usd.md)).

**Back:** the USD Import node reads materials from a USD scene, the
program's own or foreign, MaterialX or UsdPreviewSurface, into `s@material`
and `s@texture` (`scena.usda#/World/Materials/bark`), including roughness,
metalness, color and glass ([usd-import.md](usd-import.md#materials)).

## 4. Reading `.mtlx`

`textureSet()` reads a MaterialX document as a texture set
([materials.md](materials.md#custom-texture-per-object)):

- `materialy.mtlx`: the document's first `surfacematerial`;
  `materialy.mtlx#bark`: a material by name;
  a folder with no `texture.txt` and no photos that contains `.mtlx` files: the first of them.
- From `standard_surface` (or `UsdPreviewSurface`, `open_pbr_surface`,
  `gltf_pbr`) it walks back along every input of every node to an image:
  `image`, `tiledimage`, `triplanarprojection`, `UsdUVTexture`. It also goes
  through graphs (`nodegraph` and their `output`s) and graph inputs that nodes
  point to via `interfacename`. `base_color` gives the color, `normal` the
  normal map, `opacity` the alpha (the alpha channel if the path goes through
  `extract` 3 or the `a` output), `specular_roughness` the roughness and the
  material's `displacement` the height. The depth is its `scale` (for
  UsdPreviewSurface, the `scale` of the height image); without it, 1% of the
  image size. From a USD scene, depth and size are in scene units and are
  converted to meters.
- File paths are resolved from the document's folder.
- A color multiplied by `geompropvalue` means tinting by `Cd`, and the set
  average is what the document divides the photo by (as the export writes
  it). Without that, the photo is used as is, like foreign sets.
- The size in meters comes from `triplanarprojection` (1 / position
  multiplier), otherwise 2 m. A flipped green channel in the normal map means DirectX.

The program's own export is thus read back as the same set: the same files
(copied), the same average, tint, alpha, size, and height with depth.

## 5. Verification

Verified with libraries that prototype does not need. They existed only in
the verification environment: MaterialX 1.38.10 and 1.39.5 (Python),
`usd-core` 26.08, Blender 4.5.3 LTS.

- **The document** `foliage.mtlx` (soil, bark, leaf, grass) is valid
  according to `validate()` in both MaterialX 1.38.10 and 1.39.5. Every node
  has a definition, and the GLSL generator from both versions produces a
  shader for all four materials. The library renderer renders them (image
  above).
- **Shader ids in USD:** all 23 ids (`ND_…`) from the `foliage`,
  `demolition` and `uv_props` exports exist in the 1.38.10 and 1.39.5
  libraries. A network reassembled from the USD shaders, using only their ids
  and connections (as Hydra does), is a valid document in both versions, and
  the GLSL generator produces both surface and displacement shaders from it.
- **Height:** the `.mtlx` documents of those examples (height by UV and
  triplanar) are valid in both versions. A GLSL shader is generated for every
  `displacement`. USD finds both the MaterialX material displacement
  (`ComputeDisplacementSource("mtlx")` → `ND_displacement_float`)
  and the universal one (`UsdPreviewSurface`). Blender 4.5.3 turns it into a
  Displacement node on import, with Midlevel 0.5 and Scale = set depth.
- **USD** (`usd-core` 26.08):
  - for every material the library finds the MaterialX surface
    (`ComputeSurfaceSource("mtlx")` → `ND_standard_surface_surfaceshader`)
    and the universal `UsdPreviewSurface`;
  - the subsets of every mesh form a valid family (`ValidateFamily`) and each
    one leads to its material (`ComputeBoundMaterial`);
  - of the 28 validators, 27 report 0 findings. The shader validator reports
    every `ND_…` as unknown, because `usd-core` is built without MaterialX
    (Sdr only has `glslfx` and `USD`). That is why the ids were verified
    directly against the MaterialX libraries, see above.
- **The shot** `forest`, 3 frames with wind, 827,295 faces: in every frame
  each face is in exactly one subset (bark 479,054, leaf 336,360, ground
  11,881), the indices come from the frame layers. `ValidateSubsets` from
  `usd-core` 26.08 crashes on subsets whose indices are only in time samples
  (segfault, even on a scene created by USD itself), so the coverage was
  verified directly.
- **Blender 4.5.3** imports `foliage.usda`:
  - `bark`: Principled BSDF with the photo and a Normal Map node;
  - `leaf`: with alpha (Math, Blend);
  - `grass`;
  - `soil`: color from the `displayColor` attribute.
  - The mesh has the `st` UVs and material slots according to the subsets;
    the grass prototypes have their own material.

The tests are in `tests/test_materialx.cpp` (8):
- a document read back exactly as it was written (including the `&` and `"`
  characters); anything that is not MaterialX is rejected;
- a graph like one from Poly Haven (`nodegraph`, outputs, `interfacename`,
  `tiledimage`, offset) leads to the photos, also as a texture set from a
  file, from `#name` and from a folder;
- node definition ids and the expanded normal map without `normalmap`;
- our materials as graphs: bark with tint, normal map, height and a fallback
  UsdPreviewSurface, leaf with alpha and translucency, triplanar concrete (by
  position and by `rest`, height likewise), glass, no material, DirectX
  normal map;
- a written material is read back as the same texture set, including height
  and depth;
- a USD scene with faces assigned by subsets, photos alongside,
  `primvars:st`, MaterialX and UsdPreviewSurface displacement, re-import
  (groups and `uv`) and a whole mesh of one material;
- a shot whose geometry changes materials from frame to frame;
- standalone `.mtlx`, the photo folder of a sequence and geometry without faces.

## 6. In the code

| File | What it does |
|---|---|
| `src/pg/io/MaterialX.h` | A MaterialX document without the library: nodes and inputs, writing (`document`), reading XML with graphs and their inputs (`parse`), the path from the surface to the photos (`surfaceOf`), node definition ids (`nodeDef`), the expanded normal map (`portable`), USD types (`usdType`) |
| `src/pg/render/MaterialGraph.h` | Our material as a `standard_surface` graph and UsdPreviewSurface (`materialGraph`), names (`lookName`) and `MaterialLooks`: the geometry's materials as the scene assigns them, their Materials, the document and the photos |
| `src/pg/render/Scene.cpp` | `primitiveMaterials`: the material of every primitive as the renderers see it (shared by `meshOf` and the export) |
| `src/pg/io/Usda.h` | `primvars:st`, `FaceMaterials` and `MaterialBinder`, GeomSubsets (`facesOf`, `bindMesh`, `bindSubset`), Material from shaders (`materialPrim`) |
| `src/pg/sim/UsdExport.h` | The shot's `/World/Materials`, subsets in the frame layers, `<name>.mtlx` and the photos alongside; `exportGeometry`: `.usda` with materials and `.mtlx` |
| `src/pg/render/Textures.cpp` | `.mtlx` as a texture set (`readMaterialX`) |

## 7. Limitations

- **Our renderers do not displace by height**; they only turn it into bump
  (in Cycles, and only where there is no normal map). In the export it is
  displacement, so elsewhere the surface may look deeper than in our renderers.
- **Cycles procedural patterns and stains** (concrete without a photo, smudges
  on facades) are not in MaterialX. The graph has photos and colors.
- **Triplanar mapping** differs in detail: our renderers blend the
  projections by the fourth power and offset each one slightly, while
  `triplanarprojection` blends in its own way. Roof tiles laid along the roof
  go out as triplanar.
- **Reading** takes the surface photos from the document. Procedural nodes
  (noise, gradients) and roughness and metalness values are not carried by
  the texture set. From a USD scene, USD Import carries them over as the
  `roughness` and `metallic` attributes.
- **Pieces from the RBD Solver** keep their `/World/Looks` materials in USD.
