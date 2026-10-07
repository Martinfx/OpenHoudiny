# Materials and textures

Every primitive can say **what it is made of**: concrete, plaster, brick,
window, steel, wood, paving, tiled roofs, lawn… It says so through the
string primitive attribute `material`.
The renderers use it to draw the surface: Cycles with a photograph if the
library has one, otherwise with a procedural pattern, and always with that
material's roughness and metallic values. The path tracer lays the same
photographs, takes relief only from the normal map, and uses the roughness
and metallic values. A photo is laid from three sides, or by the
UV texture coordinates if the primitives have them ([below](#by-uv-and-normal-map)).

![Material swatches in Cycles](img/materials.jpg)

*Swatches (Cycles) without `Cd` colors: each material in its own color. Front
row: concrete, broken concrete, plaster, brick wall, mortar, metal. Second:
asphalt, paving, wood, bark, soil, lawn. Third: sand, flat roof, hipped
tiled roof, brick, window, steel. Back: stone, leaf, grass, glass, no
material.*

![A street corner from library photos](img/materials-library.jpg)

*Library photos in one scene (Cycles), again without `Cd`: asphalt, a paved
sidewalk, lawn, sand with chunks of broken concrete, a metal dumpster and a
plastered house with a hipped tiled roof. The rows of tiles run horizontally
on every side of the roof.*

## 1. Quick start

```bash
# Street: plaster, windows with rooms behind the glass, wooden doors, roofs, asphalt.
./build/prototype sim street ulice.png --renderer cycles
# Demolition: a concrete and plaster tower, broken concrete on the falling pieces.
./build/prototype sim demolition odstrel.png --renderer cycles
# Without photographs (patterns and colors only):
./build/prototype sim street ulice.png --renderer cycles --set output.render_textures=0
# By UV: a crate, a column and a sphere with photos and normal maps.
./build/prototype sim uv_props uv.png --renderer cycles
```

In a wrangle, one line is enough:

```c
s@material = "plaster";                 // Primitive Wrangle
if (@group_glass) s@material = "window";
if (@group_roof) s@material = "roof_tiles";
```

## 2. Materials

| Name | What Cycles draws | Roughness / metal |
|---|---|---|
| `concrete` | **photo** of stained concrete, with water streaks running down at the top | 0.85 / 0 |
| `broken_concrete` | **photo** of a fracture: aggregate pebbles in cement, deep relief | 0.95 / 0 |
| `brick` | the face of a single brick: stains, darker specks, a rough fired surface | 0.85 / 0 |
| `brick_wall` | **photo** of a brick wall including the mortar (for a wall that is a single primitive) | 0.85 / 0 |
| `mortar` | **photo** of sand, finer (mortar grains) | 0.95 / 0 |
| `plaster` | **photo** of plaster, streaks below the windows, stains across the whole facade | 0.8 / 0 |
| `window` | glass with a room behind it: each window darker or lighter in its own way (curtains, lights), warmer or cooler, reflecting the sky | 0.04 / 0 |
| `glass` | glass (transparent, like Glass Fracture) | 0 |
| `steel` | rebar steel, rusty in places (rust is rough and non-metallic) | 0.45 / 0.8 |
| `metal` | **photo** of sheet metal: brushed grooves, scratches, stains | 0.3 / 1 |
| `asphalt` | **photo** of asphalt: small pebbles in tar | 0.9 / 0 |
| `wood` | **photo** of wood with its grain | 0.65 / 0 |
| `stone` | stone: grains of several colors, stains, uneven | 0.75 / 0 |
| `roof` | flat roof: **photo** of concrete in a six-meter tile, water stains | 0.8 / 0 |
| `roof_tiles` | pitched roof: **photo** of slate tiles in rows, laid along the roof (rows horizontal, whichever way the primitive faces) | 0.7 / 0 |
| `paving` | **photo** of paving made of setts about 18 × 14 cm, sand in the joints | 0.8 / 0 |
| `bark` | **photo** of bark (vertical furrows, deep relief) | 0.9 / 0 |
| `leaf` | leaf: some yellower, some darker, in clusters within the crown | 0.5 / 0 |
| `grass` | grass (blades from the Grass node): drier in patches several meters across | 0.6 / 0 |
| `lawn` | **photo** of lawn: for ground that is a single primitive | 0.65 / 0 |
| `soil` | **photo** of soil | 0.95 / 0 |
| `sand` | **photo** of sand | 0.95 / 0 |

Without photos (Textures off), Cycles draws each material with a pattern:
paving and tiles in rows, broken concrete with pebbles, lawn with blades
and sand with ripples.

An unknown name or `""` means no material: the surface has the `Cd` color
and the "detail" from [cycles.md](cycles.md) (§3). The `roughness`
and `metallic` attributes, if the geometry has them, take precedence over
the material's roughness and metallic values.

**The color stays yours.** Both the photo and the pattern are drawn around
the `Cd` color: the average color of the surface is still `Cd`, and the photo
only lightens and darkens it. A facade thus keeps the color the asset gave
it, and each brick of Brick Wall keeps its own shade. The exception is the
brick wall (`brick_wall`). The mortar is lighter than the bricks, so a bright
`Cd` would blow it out to white; it is therefore drawn in the photo's own colors.

**Without `Cd`, the material's color.** When the geometry has no `Cd` at all
(neither on points nor on primitives), each primitive gets the color of its
material: lawn green, asphalt dark gray, sand beige, tiles slate gray. For
photos, this is roughly their own color. So setting `material` is enough
for the surface to look right, in the renderers and in the viewport.
Primitives without a material stay gray.

### Nodes that set the material themselves

| Node | Material |
|---|---|
| **Brick Wall** | `brick`, `mortar`, `plaster` |
| **Concrete Fracture** | `concrete`, `broken_concrete` on fracture faces |
| **Wood Fracture** | `wood` on primitives that have no other material |
| **RBD Solver** (runtime fracturing) | `broken_concrete` on the fracture faces of fragments where the piece was `concrete`; otherwise the piece's material |
| **Glass Fracture** | `glass` |
| **Rebar** | `steel` (including the bar tubes that RBD Solver draws) |
| **Tree** | `bark`, `leaf` |
| **Grass** | `grass` |
| **Building** (asset) | `plaster`, `window`, doors `wood`, roof `roof` |

**The Material node** sets the material on a group of primitives (Group: a
group name, numbers and ranges `0-9 12`, `*`; empty means all). The None
option removes the material.

### Pieces in flight

RBD Solver draws pieces where they currently are. If the photo or pattern
were computed from that position, it would "flow" across the piece. Each
point therefore carries `rest`, i.e. where it was before it moved, and the
texture is drawn from `rest`. A piece thus carries its pattern with it, even
as it rotates.

Fracture faces (the `inside` group) get `broken_concrete` when drawn.
The cut cap inherits the material of the adjacent primitive (plaster, paint,
window), but there is no plaster inside a wall. The exception is materials
that are the same all the way through: brick, stone, wood, metal and glass
stay themselves.

## 3. Photographs (textures)

The library that ships with the program is in
[examples/textures](../examples/textures/README.md): `concrete`,
`broken_concrete`, `plaster`, `brick_wall`, `mortar`, `metal`, `asphalt`,
`wood`, `roof`, `roof_tiles`, `paving`, `bark`, `soil`, `lawn` and `sand`.
Each set contains `color.jpg`, `height.jpg` and `texture.txt` (tile size in
meters, relief depth, average color, `tint` and optionally
`projection`). The photos come from Bistro by Amazon Lumberyard (via
pbrt-v4-scenes) and from BabylonJS/Assets, and are distributed under the
CC-BY 4.0 license; the authors are credited in the library's README.

**How a photo is laid.** Without UV ([below](#by-uv-and-normal-map)) the
photo is projected from three sides at once (triplanar). Each axis contributes as much as the
primitive faces in that direction, raised to the fourth power. A wall thus gets the photo from the front, a floor
from above, and a slanted primitive a blend of both. The projection uses `rest` and the normal in
`rest`, so a piece in flight carries its pattern. Each of the three projections is offset
so that tile seams do not meet at the same place on the edges. On facades,
large stains and streaks are added on top, so the tile repetition is not visible.

**Along the primitive** are laid the sets whose rows must stay horizontal: tiles
(`roof_tiles`, line `projection face` in `texture.txt`). From three sides,
two projections would blend on a 45° roof and the rows would double. On a
hip facing the x axis they would also run down the slope. Along the primitive, the photo is
laid so that the u axis runs horizontally across the primitive and the v axis runs uphill. The rows thus
lie horizontally on every side of the roof. Nearly horizontal primitives (less than
about 15°) are laid from three sides like the rest. Your own set can do the same:
just write `projection face` in its `texture.txt`.

**Height** produces relief in Cycles (Bump): joints between bricks, furrows in bark,
pores in concrete. With **Displacement** on the Output node, Cycles actually moves the
surface by the height: the outline of a brick sphere is jagged and the bricks shade the mortar
([cycles.md](cycles.md#displacement-by-height)). **Roughness** is taken
from a map if the set has one, otherwise from the material, and is slightly
higher in hollows.

### Custom texture per object

The **Material** node has a Texture section:

| Parameter | What it does |
|---|---|
| **Texture** | color image (`.jpg`, `.png`, `.exr`) from a set, e.g. from Poly Haven or ambientCG; the other maps are found next to it by name: `_diff_`/`_rough_`/`_disp_` (Poly Haven), `_Color`/`_Roughness`/`_Displacement` (ambientCG); a folder with `texture.txt` and a MaterialX document also work (`materialy.mtlx`, `materialy.mtlx#bark`, or a folder containing it, [materialx.md](materialx.md#4-reading-mtlx)) |
| **Texture Size** | how many meters one tile covers (0: from `texture.txt`, otherwise 2 m) |
| **Tint by Color** | on: the `Cd` color instead of the photo's color (the photo lightens and darkens around it); off: the photo as it is |

| **Projection** | how the photo is laid: **Auto** a custom texture by UV if the primitives have it, otherwise from three sides; **UV** by UV, library photos too; **Three Sides** from three sides |
| **Normal Strength** | how strongly the set's normal map bends the light (with UV): 0 not at all, 1 as the map has it |

It writes `s@texture`, `f@texture_size`, `i@texture_tint`,
`i@texture_projection` (0 auto, 1 UV, 2 three sides) and
`f@texture_normal`, so the same can be done in a wrangle. A relative path
is read from the network's folder.

`s@texture` can also be a material from a USD scene: `scena.usda#/World/Looks/Wood`.
Its MaterialX or UsdPreviewSurface network provides the set's images. The **USD
Import** node therefore writes materials from the file by itself
([usd-import.md](usd-import.md#materials)).

### By UV and normal map

![The uv_props example in Cycles: a wooden crate with six sides, each with one photo, a brick column with the photo wrapped once around it and a brick sphere from pole to pole; the brick joints have relief from the normal map](img/uv-props.jpg)

**UV** are texture coordinates: a vector attribute `uv` on primitive corners
(u, v, 0), as Houdini has it, or on points. Imports bring it in
(USD `primvars:st`, Alembic `uv`, OBJ `vt`) and the
**UV Project** node creates it ([geometry.md](geometry.md)):

| Projection | uv |
|---|---|
| **Planar** | along the Axis, as seen from its positive side: from above, x to the right and −z up; Scale meters per photo |
| **Box** | each primitive along the axis it faces most, as seen from outside: six sides of a box, each with its own photo, none mirrored |
| **Cylindrical** | u once around the axis (from the front, left to right), v along it, Scale meters per photo |
| **Spherical** | u once around, v from the bottom pole (0) to the top pole (1) |

On a cylinder and a sphere, each primitive stays whole: a primitive across the seam has u above 1 on one
side, and a corner at the pole takes the u of the rest of its primitive. Center is where the projection
comes from, Group which primitives.

**Laying by UV.** Where a material is laid by UV (Projection UV, or
Auto with a custom texture), one photo covers one uv unit. Texture
Size is not used; the size comes from the UV (Scale of the UV Project node). The library
photos are made for three-sided laying in meters, so Auto
lays them from three sides even where UV exists. The exceptions are bark, leaves and grass
(`bark`, `leaf`, `grass`): their images are made for the UV that the
Tree and Grass nodes give them ([trees.md](trees.md)), so Auto lays them by
UV where the primitives have it. Primitives without UV are laid from three sides,
whatever Projection says. The photo goes with the primitive wherever the UV moves it.

**A normal map** says which way the surface faces at each pixel: x, y, z
in red, green and blue from 0 to 1. It is found next to the photo by name:
`_nor_gl_` and `_nor_dx_` (Poly Haven), `_NormalGL` and `_NormalDX`
(ambientCG), `normal.jpg` in the folder with `texture.txt`. In DirectX, green points
down the image and the renderers flip it (`_dx_`, `_NormalDX`, line
`normal dx` in `texture.txt`). Where both exist, OpenGL is used. The normal map applies
with UV laying and bends the normal in tangent space: the tangent is the direction in which
u increases, the second axis the direction in which v increases. Without a normal map, relief
in Cycles comes from height as before. From three sides, the normal map is not used; relief
comes from height (Cycles).

- **Cycles:** the mesh gets the `ATTR_STD_UV` attribute, the photos are read via
  the UV coordinates and the normal map via a Normal Map node in tangent space,
  which Cycles computes with the MikkTSpace method.
- **Path tracer:** computes tangents the same way as MikkTSpace. It sums corners at the same
  place with the same normal and uv, each weighted by the angle the
  triangle has there. Mirrored uv are kept separate, and it bends the normal the same way as the
  Normal Map in Cycles, strength included. Where the bent normal points away from the eye, it turns it
  toward the primitive just far enough for the eye's reflection to get above the surface. Diffuse
  light at grazing angles to the unbent normal is attenuated as in Cycles (GGX
  microfacet shadowing after Conty Estevez et al. 2019). Without this, a sphere
  with a normal map would have sharp seams.

The library has normal maps for the brick wall (`brick_wall`), wood (`wood`)
and bark (`bark`). They were computed from the height slopes by `tools/textures/prepare.py
--normals`.

**Alpha cutout.** A set with alpha cuts out the surface where alpha is 0, if it is
laid by UV. That is the edge of a leaf, the gaps between needles. Alpha is found
as follows: line `alpha 1` in `texture.txt` (the alpha channel of `color.png`), an
`opacity` image in the folder, for third-party sets `_opacity`, `_alpha` or `_mask` next to the
photo (gray is used), otherwise the alpha channel of the color photo itself, if it omits any
pixels. The set's average color (`mean`) is computed only from what is
there.

- **Cycles:** the shader is mixed with a Transparent BSDF by alpha. Rays
  and shadows pass through (Cycles transparent shadows), up to 64 layers deep.
- **Path tracer:** a ray that hits a cut-out primitive passes through it
  with probability 1 − alpha and continues (up to 64 layers). A shadow
  ray is attenuated by alpha, so a leaf's shadow has a soft edge as
  in Cycles. Meshes with a cutout go into Embree as "transparent", which a shadow
  ray passes through primitive by primitive, like glass.

The library has leaf and grass images drawn by the script
`tools/textures/foliage.py` (no photographs, no third-party license): `leaf`
is, in quarters, two broad leaves, a narrow leaf and a sprig of needles, with alpha
and a normal map of the veins; `grass` is a blade with a midrib and stripes.
The Tree and Grass nodes give them UV ([trees.md](trees.md)).

Alpha verification (`tests/test_foliage.cpp`): a red board one meter above the ground
with its left half at alpha 0. From above, the ground is visible through the left half, lit
by the sun the same as without the board (path tracer 1.044 vs. 1.057, Cycles
1.073 vs. 1.082); the right half is red. In both engines (our BVH
and Embree), a shadow ray passes fully through the left half and not at all through the right.

Verification (`tests/test_uv.cpp`): a four-color photo laid by UV puts
each quarter in its place in both renderers. A plane with the sun 30°
above the horizon and a normal map tilted 30° toward the sun is 2.90× brighter
in the path tracer and 2.95× in Cycles (Lambert 1.73× plus the specular; the normal
lies halfway between the sun and the eye). Tilted away from the sun it is dark. A
DirectX map with the same pixels bends the opposite way. A sphere with a map tilted in both u and v
under a sun from the side differs from Cycles in each quarter and in the middle
by at most 5 %.

### In the viewport

The viewport lays photos the same way as the renderers: by UV, or from three sides
(by `rest`, otherwise by the point position; it lays even sets laid along a slanted primitive
from three sides). It tints the color with `Cd` around the set's average, or leaves it
as it is. The normal map bends the normal in tangent space, which the viewport
computes from screen-space derivatives of position and UV (Schüler). Leaf alpha cuts out
the primitive and its shadow too. So that thin needles do not disappear in the smaller copies of the image (mipmaps),
alpha is boosted in them according to the copy's level (Golus).

The images are in two texture arrays, 512 × 512 pixels per layer, at most 16
sets at a time. The viewport draws any further sets with color only. Height and relief
(bump) are not in the viewport.

![Viewport: a linden, a spruce and grass from the foliage example with leaf and bark images; the crate, column and sphere from uv_props with wood and brick photos laid by UV](img/viewport-textures.jpg)

### On the Output node

| Parameter | What it does |
|---|---|
| **Textures** | photographs on or off (off: patterns and colors only) |
| **Texture Folder** | a different library: a folder of folders named after the materials (empty: the one that ships with the program; also the `PG_TEXTURES` variable) |
| **Surface Detail** | how strong the patterns and stains over the photos are; 0 turns them off. A primitive with `f@surface_detail` gets only that many times as much (0 none, like materials from USD Import that are not presets) |

From the command line: `--set output.render_textures=0`,
`--set output.render_texture_folder=/cesta/k/texturam`.

### To other applications

Exporting to USD writes the materials of the displayed geometry as MaterialX (and
UsdPreviewSurface), assigns primitives to them and copies the photos next to the scene.
`.mtlx` writes only the materials. This is described in [materialx.md](materialx.md).

## 4. Before and after

![Demolition without materials and with them](img/materials-demolition.jpg)

*The same demolition frame in Cycles. Left without materials, right with them:
windows with rooms behind the glass, stained roofs and plaster.*

![The tower up close](img/materials-close.jpg)

*The tower as it starts to collapse: plaster and concrete floor slabs from photographs, the windows
of the surrounding houses reflect the sky.*

## 5. How it works

- `src/pg/core/Material.h`: material names (`MaterialPreset`,
  `kMaterialNames`). A new string attribute has `""` at the start of its table,
  so primitives nobody wrote anything to have no material. This holds for
  wrangles, `setPrimitiveString` and merging geometry (Merge).
- `src/pg/nodes/Surface.cpp`: the Material and UV Project nodes.
- `src/pg/io/Obj.cpp`: `vt` to corner `uv` and back.
- `src/pg/sim/Rigid.cpp`: `posedPieces` writes `rest`, and `drawnPieces` gives
  fracture faces `broken_concrete` and bar tubes `steel`.
- `src/pg/render/Scene.cpp`: `meshOf` reads `material`, `texture*`, `rest`
  and `uv`, computes tangents from UV (`cornerTangents`) and gives windows a number based on the
  position of their first primitive in `rest`. The number is
  the same frame after frame, even as pieces disappear. It gives geometry without `Cd` the
  materials' colors (`presetSurface`).
- `src/pg/render/MaterialGraph.cpp`: materials as MaterialX graphs
  ([materialx.md](materialx.md)).
- `src/pg/render/Textures.cpp`: finding sets (the library, `texture.txt`,
  Poly Haven and ambientCG file names, normal maps, MaterialX
  documents), average color,
  triplanar lookup for the path tracer and the bent normal (`bentNormal`).
- `src/pg/render/PathTracer.cpp`: `facingNormal` and `bumpShadowing`, as
  Cycles has them.
- `src/pg/render/Cycles.cpp`: `patternOf` (procedural patterns at three
  scales), `laidOn` and `sampled` (photos from three sides or along the primitive,
  `alongFace`), `sampledByUv` and `normalMapped` (by UV, the Normal Map node),
  `weathered` (stains and streaks over the photos). Meshes carry the attributes `pg_rest`,
  `pg_rest_normal`, `pg_random` and UV.
- `tools/textures/prepare.py`: building the library from pbrt-v4-scenes photos
  and BabylonJS/Assets. It integrates height from the normal map, and it tells which way the
  green axis points by which direction of the green axis yields a real surface (with the other one, the slopes would
  not belong to any surface). For metal, it composes the color from the grooves of its
  relief, because its photo has only ten shades of gray.
