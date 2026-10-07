# Material textures

Photographs of surfaces that the renderers apply to materials (`s@material`,
see [docs/materials.md](../../docs/materials.md)). Each folder is one set
and is named after its material:

| Folder | What it is | Tile size |
|---|---|---|
| `concrete` | concrete with stains and pores | 3 m |
| `broken_concrete` | fracture surface: aggregate pebbles in cement | 0.8 m |
| `plaster` | light, mottled plaster | 3 m |
| `brick_wall` | brick wall with mortar (own colors, `tint 0`) | 2.4 m |
| `mortar` | photos from `sand`, finer grain | 0.4 m |
| `metal` | brushed sheet metal with scratches | 1 m |
| `asphalt` | small pebbles in tar | 2 m |
| `wood` | wood with visible grain | 1.2 m |
| `roof` | photos from `concrete`, six-meter tile (flat roofs) | 6 m |
| `roof_tiles` | slate roof tiles in rows, laid along the roof (`projection face`) | 1.8 m |
| `paving` | paving of setts about 18 × 14 cm | 1.3 m |
| `bark` | linden bark (with a normal map) | 1 m |
| `leaf` | leaves in quarters: two broad, a narrow one, needles; alpha cut-out and normal map (`alpha 1`), applied by UV | — |
| `grass` | grass blade, applied by UV | — |
| `soil` | moist soil | 2.5 m |
| `lawn` | lawn | 1.5 m |
| `sand` | sand | 1.2 m |

Each folder contains:

- `color.jpg` — color (sRGB), at most 1024 × 1024 (`color.png` with alpha
  for leaves),
- `height.jpg` — height 0–1; Cycles uses it for displacement,
- `normal.jpg` (`brick_wall`, `wood`, `bark`; `normal.png` for `leaf`) — a normal map derived from the height slopes,
  for UV-based mapping,
- `texture.txt` — `size` (how many meters one tile covers), `depth`
  (how many meters lie between the lowest and highest point of the height), `mean`
  (average color, linear), `tint` (1: the surface's `Cd` color replaces the
  photo's color and the photo only lightens and darkens around it; 0: the photo as is),
  `projection face` (applied along a sloped face so that rows stay
  horizontal; otherwise from three sides), `alpha 1` (the color's alpha channel cuts
  out the surface), `pictures` (photos from another folder), `source` and `license`.

The sets are produced by the script [tools/textures/prepare.py](../../tools/textures/prepare.py)
from photographs in the [pbrt-v4-scenes](https://github.com/mmp/pbrt-v4-scenes)
and [BabylonJS/Assets](https://github.com/BabylonJS/Assets) repositories. It downsizes them to
at most 1024 px and reconstructs the height from the normal map by integration in Fourier
space. It determines which way the map's green axis points by checking which direction gives
a plausible surface. Where there is no normal map, it takes the height from the photo's lightness.
Finally it writes `texture.txt`.

## License and authors

The photographs are modified (downsized, height reconstructed from normals or from
lightness; the metal color composed from the grooves of its relief and the stains of its photo) and are
distributed under the **[CC-BY 4.0](https://creativecommons.org/licenses/by/4.0/)** license:

- `concrete`, `plaster`, `brick_wall`, `wood`, `bark`, `soil`, `paving`,
  `roof_tiles`, `metal` (and `roof`): **Amazon Lumberyard Bistro**, © Amazon,
  CC-BY 4.0, <https://developer.nvidia.com/orca/amazon-lumberyard-bistro>;
  taken from [pbrt-v4-scenes](https://github.com/mmp/pbrt-v4-scenes)
  (directory `bistro/textures`: `MASTER_Concrete_Plaster`, `Concrete2`,
  `MASTER_Brick_Small_Red`, `MASTER_Wood_Brown`,
  `Foliage_Linde_Tree_Large_Trunk`, `Pavement_Ground_Wet`,
  `Pavement_Cobblestone_Big_BLENDSHADER`, `MASTER_Roofing_Shingle_Grey`,
  `Banner_Metal`).
- `asphalt`, `broken_concrete`, `lawn`, `sand` (and `mortar`):
  **Babylon.js Assets**, © Babylon.js, CC-BY 4.0,
  <https://github.com/BabylonJS/Assets> (`meshes/PowerPlant/gravel_a.png`,
  `textures/rockyGround_basecolor.png` and `rockyGround_normal.png`,
  `textures/grass.png`, `textures/sand.jpg`).

The rest of the program has its own license; these files are not covered by it.
The exceptions are `leaf` and `grass`: they were drawn by the script
[tools/textures/foliage.py](../../tools/textures/foliage.py) without
photographs and fall under the program's license.

## Custom textures

- **A different library** is set on the Output node with the **Texture Folder** parameter
  (or the `PG_TEXTURES` environment variable): a folder of folders named
  after materials.
- **A single object** gets a texture from the **Material** node (Texture parameter):
  pick the color image of a set from Poly Haven or ambientCG. The other maps are
  found next to it by name (`_diff_`, `_rough_`, `_disp_`; `_Color`,
  `_Roughness`, `_Displacement`).
