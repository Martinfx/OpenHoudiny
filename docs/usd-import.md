# Reading USD

A studio hands a shot around in USD: matchmove delivers the camera, the layout department
the set and the assets, another department caches from Houdini or Maya. Prototype
**reads** these files and composes them into a scene the same way the USD library does:
- the **matchmove camera** goes into Output as the shot camera, frame by frame;
- **geometry** (the set, models, caches) goes into the network as a node, which then
  provides colliders for a simulation, shapes for fracture or anything else.

The reader does not need the USD library. It reads all three forms of a layer:
- `.usda` (text),
- `.usdc` (binary "crate"),
- `.usdz` (package).

The binary format is not described by any specification other than the OpenUSD source code, so
its layout was worked out from that code. The reader's code is our own. The results are
verified against the `usd-core` library (§6).

## 1. Quick start

```bash
./build/prototype usd examples/usd/shot.usda                 # what the file contains: tree, cameras, geometry
./build/prototype sim matchmove out/mm.png --every 24         # fire in a set from USD, through a camera from USD
./build/prototype sim usd_looks looks.png --renderer cycles   # props with materials from USD (MaterialX, UsdPreviewSurface)
./build/prototype sim usd_subdivision s.png --renderer cycles   # subdivision surfaces from USD with creased edges and corners
PYTHONPATH=build/python python3 examples/usd/make_plate.py    # the shot plate; then the fire is in the filmed courtyard
```

In the editor:
- **Shift+A › Geometry › USD Import:** geometry from a file.
- **Shift+A › Render › USD Camera:** a camera from a file. It connects to the
  Camera input of the Output node.

From Python:

```python
import pg
stage = pg.UsdStage("examples/usd/shot.usda")
stage.prims()                        # prim by prim, as USD composes them
cam = stage.camera(time=1010)        # world matrix (metres, Y up), focal length, film
set_ = stage.geometry(prims=["/Set"])  # pg.Geometry: points, primitives, attributes
```

## 2. Nodes

### USD Import (geometry)

The scene's geometry at the given frame, in world coordinates:

| From USD | To geometry |
|---|---|
| **Mesh** | polygons; `leftHanded` and mirroring transforms reverse the corner order, holes (`holeIndices`) are dropped |
| subdivision surface (`subdivisionScheme` `catmullClark` or `loop`) | a smooth surface: faces subdivided with Catmull-Clark as in OpenSubdiv, creased edges and points as given in the file, normals from the surface (see Subdivision surfaces below) |
| normals (`normals`, `primvars:normals`) | `N`: on points, corners or primitives according to `interpolation`; transformed and normalized. Corner `N` (`faceVarying`) gives hard edges in the viewport and in the renderers |
| `primvars:st` | `uv` (a vector, z = 0), including indices (`primvars:st:indices`) |
| `primvars:displayColor`, `displayOpacity` | `Cd`, `Alpha` |
| other primvars (numbers, vectors) | an attribute of the same name; `constant` and `uniform` on primitives, `vertex` on points, `faceVarying` on corners |
| `velocities` | `v`, transformed |
| face **GeomSubset** | a primitive group named after the subset (`roof`, `glass`) |
| **Points** | loose points: `pscale` from `widths` (half, scaled by the transform), `id` from `ids` |
| **BasisCurves** | open polylines (control points; `periodic` ones closed), `pscale` from `widths` |
| **PointInstancer** | instances (see Instances below): geometry prototypes and one point per instance with `instance`, `orient`, `pscale`, `id`, `v` and the instance primvars |
| **Volume** (`OpenVDBAsset` fields) | volumes: a grid from the VDB file (`filePath` at the given time, `fieldName`) named after the field (`density`; a vector as `vel.x`, `vel.y`, `vel.z`). It is placed according to the field and volume transforms, in meters with Y up; vectors are rotated and rescaled, level set distances rescaled. A volume rotated off the axes is resampled onto cubic voxels |
| **Cube, Sphere, Cylinder, Cone, Capsule, Plane** | polygons according to the dimensions and axis |
| prim path | a string primitive attribute `path` (`/Set/beam`) |
| material (`material:binding`, also on a GeomSubset) | `material`, `texture`, `roughness`, `metallic`, `glass`, `Cd` (see Materials below) |

A primvar of the same name can have a different interpolation in each prim, for example `st`
on points in one mesh and on corners in another. The values are then merged on corners,
just as after a Merge in Houdini: each corner gets the value its prim gave it,
whether it was on the corner, on the point or on the primitive. Renderers, the viewport
and export read corners first, and on the other prims they would find
zeros there. Loose points (Points) keep their values on points. Normals are
merged on corners too: renderers and the viewport take the corner normal before
the point normal, so hard edges from Blender (`faceVarying`) stay hard.
Glass is shaded from the normals as well: a smooth glass sphere or bottle stays
smooth, while glass without normals is drawn face by face. Only velocities stay
where the prim had them, because renderers take them from points.

Parameters:
- **File:** `.usd`, `.usda`, `.usdc` or `.usdz`. A relative path is resolved from the
  network's folder. A file that changes is reloaded.
- **Prims:** which prims to read, together with everything below them. These are paths separated by
  spaces. An empty field means the whole scene.
- **Frame Offset:** a time offset (§3).
- **Render, Proxy, Guide:** which *purpose* to read. The default is `default`
  and `render`, the same as a renderer.
- **Metres, Y Up:** conversion of units and of the up axis (§4).
- **Subsets as Groups, Path Attribute:** groups from subsets and the `path` attribute.
- **Materials:** materials bound to the geometry (below). When off: as before,
  only `displayColor`.
- **Subdivision:** how many times subdivision surfaces are subdivided (below), default 2.
  At 0 the control mesh is kept.

Invisible prims (`visibility = invisible` on them or above them) are not
read. When the geometry in the file moves (time samples, value clips,
an animated transform or visibility), the node cooks again on every
frame. Otherwise it cooks only once.

### Subdivision surfaces

A mesh with `subdivisionScheme = "catmullClark"` is a smooth surface in USD:
the control mesh is only a cage and the renderer smooths it. That is how it comes out of Maya,
Houdini, and Blender with the subdivision modifier. USD Import smooths it
itself, the same way as OpenSubdiv, which Blender, Houdini and Hydra use for this.
Ten thousand quads of control mesh thus become, with Subdivision 2,
160 thousand quads of smooth surface.

- **How many times:** the **Subdivision** parameter. Each step turns a face
  with n corners into n quads, and a quad into four. If the result would exceed
  4 million primitives, it subdivides fewer times and the node reports it in a warning. At 0
  the control mesh is kept, only shaded smooth. A mesh of 250 thousand
  quads is subdivided twice into 4 million primitives in about 7 s.
- **Creased edges:** `creaseIndices`, `creaseLengths` and `creaseSharpnesses`,
  with one sharpness per edge chain, or with a sharpness for each edge separately.
  A sharpness of 10 or more keeps the edge sharp forever. A smaller one keeps it sharp for as many steps as
  the sharpness, and then the edge rounds off: a sharpness of 2.5 holds for two steps and in the third
  by half.
- **Corner points:** `cornerIndices` and `cornerSharpnesses`, with the same
  sharpness semantics.
- **Boundary:** `interpolateBoundary`. The default `edgeAndCorner` pins a corner
  that has only one face (a grid corner stays in place). `edgeOnly`
  rounds it.
- **Normals** from the file are not used by a subdivision surface, as USD specifies. It gets
  corner normals from the smoothed surface: smooth, breaking only across edges
  with a sharpness of at least 1.
- **What remains:** sharpness that remains after subdivision stays on the geometry.
  On corners as `creaseweight` (the sharpness of the edge from that corner to the next one),
  on points as `cornerweight`. The Subdivide node continues subdividing according to them,
  so USD Import with Subdivision 1 followed by Subdivide 1 gives the same result as
  Subdivision 2.

A mesh with `subdivisionScheme = "none"` or `"bilinear"` stays polygons.
So does a mesh that has no scheme at all. According to the USD specification it would be
a subdivision surface, but scripts and simple exporters do not write the scheme even
for hard-surface models. Blender and Houdini always write it.

The `usd_subdivision` example reads `examples/usd/subdivision.usda` (created by
the `make_subdivision.py` script with the USD library). It contains three coarse meshes:

- a cube with one point sharp forever: round, pointed in just
  one place;
- a bar of soap, a box with edges of sharpness 1.5: sharp for one step, half sharp for the second,
  then rounded;
- a twelve-sided can: the lid rim sharp forever, the bottom with sharpness 1,
  so it rounds off only slightly.

```bash
./build/prototype sim usd_subdivision s.png --renderer cycles
./build/prototype sim usd_subdivision s0.png --renderer cycles --set subdivision.subdivision=0   # control meshes
```

![The usd_subdivision example in Cycles. Top: the control meshes from the file (Subdivision 0): a cube, a box and a twelve-sided prism. Bottom: the same meshes smoothed (Subdivision 3): a round drop with one sharp tip, a bar of soap with soft edges and a can with a sharp lid rim](img/usd-subdivision.jpg)

After three steps the example's bodies match Blender (OpenSubdiv) to within 4e-7.

### Instances (PointInstancer)

Scattered copies from other applications (vegetation, rocks, debris) come in as
the program's instances, just like grass and trees from its own nodes. Each
prototype is read once. Each instance is a point that places the prototype
(attributes `instance`, `orient`, `pscale`). Renderers draw them as
instances, and the Unpack node turns them into copies.

- **Placement** is computed the same way as in USD (`ComputeInstanceTransformsAtTime`):
  first the scale (`scales`), then the rotation (`orientationsf`, otherwise
  `orientations`), the position (`positions`) and finally the instancer's
  transform. The prototype root's own transform is included; whatever is
  above it (e.g. a `Prototypes` scope) is not.
- **Between samples:** when `velocities` has a sample at the time where `positions` has its
  last sample before the given time, the position is moved from it by the velocity
  and `accelerations`, and the rotation is turned by `angularVelocities`. Otherwise the
  values are interpolated between samples.
- **Hidden:** instances from `invisibleIds` and `inactiveIds` are skipped.
- **Attributes:** `ids` become `id`, `velocities` become `v`, instance primvars
  (`primvars:tint` from our own export, `displayColor`…) become point attributes.
- **Nesting:** an instancer inside another instancer's prototype produces nested
  instances.
- **Stretching:** an instance that the transform stretches, shears or mirrors
  (non-uniform `scales`, non-uniform scale above the instancer) cannot be expressed by a rotation
  and a single scale. It therefore gets a prototype of its own:
  a copy stretched the way the instance places it. Placement stays exact;
  it just takes more memory.
- **Rotation precision:** the quaternion is taken as unit length. USD rotates
  by the quaternion from `orientations` (half) exactly as written, and its
  matrix differs from a pure rotation by up to 6·10⁻⁴. For a point one meter from the center of
  the prototype, that is at most a millimeter.

```bash
./build/prototype usd examples/usd/looks.usda
# instances: 40 of 2 prototypes, from 1 PointInstancers
```

### Materials

USD Import also reads materials bound to meshes, shapes and their GeomSubsets.
It resolves bindings the same way USD does for rendering (`ComputeBoundMaterial`):
- first bindings with the `full` purpose (`material:binding:full`), and only when
  there is none on the prim or above it, bindings for all purposes
  (`material:binding`);
- the prim's own binding applies, otherwise the binding of the nearest ancestor;
- an ancestor binding with `bindMaterialAs = "strongerThanDescendants"` overrides
  the bindings below it (the highest such binding wins);
- primitives in GeomSubsets with `familyName = "materialBind"` get the subset's
  material, the other primitives get the mesh's material.

Each primitive gets attributes that the renderers, the viewport and export read:

| Attribute | What it contains |
|---|---|
| `s@material` | the material name (of the Material prim); `bark_2` from our own export as `bark`. When it is a preset name (`concrete`, `glass`…), the renderers treat it as that preset |
| `s@texture` | the material's images, if its color comes from an image: `file.usda#/path/to/material`. From this, the renderers, viewport and export read the texture set: color, normal map, roughness, height and opacity |
| `i@texture_tint` | 1 when the image color is multiplied by `displayColor` (geompropvalue, UsdPrimvarReader) |
| `i@texture_projection`, `f@texture_size` | 1 by uv, 2 from three sides (triplanar), and how many meters one image covers |
| `f@roughness`, `f@metallic` | the material's values; whatever it does not specify gets its shader's default (UsdPreviewSurface roughness 0.5, standard_surface 0.2, OpenPBR 0.3) |
| `i@glass` | 1 for a material that transmits light: `transmission` at least 0.5, or UsdPreviewSurface with `opacity` below 0.5 and no image |
| `f@surface_detail` | 0 for a material that is not a preset: Cycles does not add the stains, bumps or weathering it gives the program's own primitives (**Surface Detail** on the Output node). Presets and primitives without a material have 1. A material written by this program has the value in its `pg:surface_detail` attribute |
| `Cd` | the material color when it is a value (times the `base` weight); overrides `displayColor` only on primitives with that material |

Primitives without a material keep the `f@roughness` and `f@metallic` of their preset,
so they do not change in the render. A color that the material takes from an image or
from `displayColor` leaves `Cd` as it is.

**What is read.** The surface from the `outputs:mtlx:surface` output (MaterialX), otherwise
from `outputs:surface`. Shaders:
- UsdPreviewSurface with UsdUVTexture and UsdPrimvarReader;
- standard_surface, open_pbr_surface and gltf_pbr from MaterialX;
- MaterialX nodes between them (image, tiledimage, triplanarprojection,
  normalmap, multiply, convert, extract, geompropvalue…), recognised by
  their definition names (`ND_image_color3`).

The network is read through NodeGraph outputs and through the interface inputs of NodeGraphs
and of the material itself. From the `displacement` output, the height and its depth
(`scale`) are read too, converted from scene units to meters. Cycles uses it for bump,
and with **Displacement** on the Output node it actually displaces the surface
([cycles.md](cycles.md#displacement-by-height)). Image paths
are resolved as in USD, relative to the layer that wrote them. Inside a
`.usdz`, an image is `package.usdz[textures/a.png]` and is read directly
from the package.

```bash
./build/prototype usd scene.usdz
# material Bricks: 6 primitives, pictures /…/scene.usdz#/root/_materials/Bricks
# material RedMetal: 512 primitives
```

The `usd_looks` example shows all the kinds at once. The file
`examples/usd/looks.usda` was created by the `make_looks.py` script with the USD library.
Each prop in it has its own material, and scattered pebbles in front come
as a PointInstancer (two stone shapes with their own material):
- a wooden crate: `standard_surface` from MaterialX with a color image
  and a normal map;
- a brick sphere: UsdPreviewSurface with UsdUVTexture and a normal map;
- a red metal bead: values only;
- a glass sphere: `opacity` 0.05, with point normals, so it is smooth
  (glass without normals is drawn face by face);
- a floor made of two GeomSubsets: blue plastic from OpenPBR and paving
  named after a preset (`paving`), and therefore with the preset's photos.

Materials that are not presets have `f@surface_detail` 0, so Cycles does not add to them
the stains and bumps it otherwise gives primitives without an image.
They look the way the program wrote them. The paving is a preset, so it gets them.

```bash
./build/prototype sim usd_looks looks.png --renderer cycles
```

![The usd_looks example in Cycles: a wooden crate with a normal map on a blue plastic floor, a brick sphere, a shiny red metal bead and a smooth glass sphere on paving, with pebbles from a PointInstancer in front. Each material came from USD in a different form: MaterialX, UsdPreviewSurface, OpenPBR](img/usd-looks.jpg)

### USD Camera

A camera from a file. Connected to the Camera input of the Output node, it is the camera
used for rendering, and the one the viewport shows when looking through the camera (0).

- **Position and rotation:** from the camera's world matrix on every frame. A USD camera
  looks down its −z, just like Camera. Angles are chosen closest to the previous
  frame, so the rotation does not jump from 180° to −180°.
- **Lens:** the left-to-right field of view corresponds to the horizontal aperture
  and focal length (`horizontalAperture`, `focalLength`), like "fit horizontal"
  in Maya and Houdini.
- **Width:** image width in pixels.
- **Height:** image height. A value of 0 means the height is derived from the film
  aspect ratio, i.e. Width × vertical / horizontal aperture. Super 35
  (24.89 × 14 mm) at a width of 1280 gives 720.
- **Prim:** the path to the camera. An empty field means the first camera in the scene.
- **Plate**, **Plate Frame:** the shot image behind the CG; its frames follow the shot's
  time codes, `….1001.…` on the frame that reads time code 1001
  ([plate.md](plate.md)).

What the prototype's drawing cannot do, the node reports as a warning:
- it draws an orthographic camera in perspective;
- it ignores the film offset (`horizontalApertureOffset`).

## 3. Time

Prototype frames start at 1; USD times are usually plate frame numbers (1001…).
- **Frame 1** reads the scene's `startTimeCode`. When the scene does not specify one, it reads time code 1.
- **Subsequent frames** advance by the scene's `timeCodesPerSecond` divided by the Output frame rate.
  At 24 and 24 one frame is one time code; when Output is at 12 fps, it is two.
- **Frame Offset** shifts reading by that many frames.

USD interpolates between samples, and so does the reader:
- numbers and vectors linearly, quaternions along the shorter arc (slerp);
- arrays only when both have the same number of elements. Otherwise the earlier value is held.

So even motion blur subframes come out as in USD.

## 4. Units and axes

Prototype computes in meters with Y up. A USD scene can have anything:
- **Units:** `metersPerUnit` says how many meters one unit is. When the scene does not
  specify it, 0.01 applies, i.e. centimeters, as is USD's default.
- **Up axis:** `upAxis` is Y or Z (Maya, 3ds Max). A Z-up scene
  is rotated by −90° around X.

Both are taken from the **root layer**, as in USD. Layers it
loads do not have their own units; the pipeline must keep them the same everywhere. The
**Metres, Y Up** parameter turns the conversion off, and the geometry stays in the file's units.

## 5. What is composed

The scene is composed as in the USD library (Pcp), including the strength order of opinions
(LIVRPS):

| | |
|---|---|
| **sublayers** | layers below the root, with time offset and scale; when a layer has a different `timeCodesPerSecond`, time is rescaled |
| **references** | to another file (to the default prim or a named prim), within layers (internal), with time offset |
| **payloads** | like references; always loaded |
| **variant sets** | the selected variant: the strongest opinion anywhere in the index (the shot's selection overrides the asset's default), variants within variants |
| **inherits, specializes** | classes (`class`) as well as ordinary prims; whatever is specialized is weakest in the entire index, even if it came from an asset in a reference |
| **def / over / class, active** | what is defined, what only adds to it, abstract classes; a class that a prim inherits from does not make the prim a class; an inactive prim has no children |
| **value clips** | values from per-frame layers (including our own export, [usd.md](usd.md) §3) – see below |
| **list edits** | `prepend`, `append`, `delete`, `add`, `reorder` and explicit lists, composed starting from the weakest layer |

A reference to a prim deeper in the tree (`@asset.usd@</Tree/crown>`) also brings
what that prim receives from its ancestors, such as their variants and references.

**Value clips** are read as in USD:
- **Strength:** clips sit right after the layer that named them. An opinion from a stronger
  layer (e.g. an override from lighting) overrides them; they override an opinion from a weaker layer
  (e.g. a default value in the topology).
- **Prims below a prim** with clips take values from the corresponding prims in the clips,
  in addition to their own clips if they have any.
- **Clip set fields** (`assetPaths`, `active`, `times`, `primPath`, manifest) are
  composed across layers: each one from the strongest layer that has it. The times in
  `active` and `times` go through the offset and scale of the layer that wrote them.
- **Templates:** `templateAssetPath` (`fx/sim.###.usd`, also `###.###` for
  subframes) with `templateStartTime`, `templateEndTime`, `templateStride`
  and `templateActiveOffset`; a missing file is skipped.
- **Time:** `times` can go backwards and can jump (two entries at the same time),
  e.g. for a loop. Values are interpolated between samples even across a clip boundary.
- **Manifest** says which attributes the clips provide. When it is missing, they are the ones
  that have samples in some clip. A clip without samples of an attribute gives the manifest's default
  value, otherwise nothing; with `interpolateMissingClipValues` the value is
  interpolated from neighboring clips.
A file that cannot be read is a warning (`prototype usd` prints it), not
an error: the rest of the scene is still composed.

Not supported:
- relocates;
- implied inherits across references;
- instancing as such: instanceable prims are expanded like ordinary ones;
- the session layer.

## 6. Verification

The reader was compared against Pixar's `usd-core` 26.08 library. It is only
in the test environment; prototype does not need it. The Python tests are skipped
without it.

- **Formats:**
  - the same scene as `.usda` and `.usdc` gives the same values, time samples,
    list edits, variants, dictionaries and relationships;
  - crate versions 0.4.0 to 0.12.0 (which the library writes when the scene has splines)
    are read the same way, including compressed arrays (integers, value tables,
    halfs, 64-bit numbers);
  - `.usdz` is read including references to files inside the package.
- **Transforms:**
  - six random scenes with operations of all kinds: translate, scale, rotation
    around one axis and around three axes in six orders, `orient`, `transform`,
    inversion (`!invert!`), `!resetXformStack!`;
  - with time samples, in both formats;
  - world matrices differ from `ComputeLocalToWorldTransform` by at most
    6e-7 relative, i.e. at the level of float precision.
- **Composition:**
  - a shot from five files: a sublayer with offset, scale and a different FPS,
    a reference with offset, a variant selection in the shot, a class, a specialize to a prim
    inside a reference, an internal reference, a payload, an instanceable prim,
    an inactive prim, value clips;
  - all 25 prims, their types and all authored values at 8 times (including between
    samples) and the world matrices match the library;
  - the same holds for our own water and rain export (per-frame value clips)
    on frames and between them;
  - specializes in an asset behind a reference and the specifier of a prim that inherits a class
    come out as in the library.
- **Value clips:**
  - 1300 random shots: clips named in a sublayer between a stronger
    and a weaker layer, with time offset and scale, 1–4 clips, `times` with
    jumps and reversals, a manifest with and without default values, templates,
    `interpolateMissingClipValues`, an ancestor's clips on a descendant;
  - the value at every quarter frame, the time samples and "might be time-varying" match
    the library in 1295 shots;
  - the remaining 5 hit undefined behavior in the library itself: when a clip
    sample cannot be mapped back to scene time through `times`, USD reads an
    empty `std::optional`. The reader skips such a sample. If it instead
    takes 0 (what this build of the library reads in that situation), all
    1300 match;
  - 2000 further random shots whose `times` cover all samples
    all match.
- **Geometry:**
  - world-space points after unit and axis conversion match the library's points
    transformed by its matrix (deviation below 1e-4);
  - `leftHanded`, holes, `faceVarying` uv with indices, `uniform` colors,
    subsets, purpose and visibility come out the way the renderer reads them;
  - uv on the points of one mesh and on the corners of another, and color on primitives
    and on points, are merged on corners, each corner with the value of its own prim.
- **Camera:** the position, view direction and horizontal field of view of USD Camera match
  the library's camera.
- **Volume:**
  - a Blender 4.5 export: a fireball VDB as an object translated
    and rotated by 28.6°, a Z-up scene. The density centroid matches
    Blender's transform to within 2·10⁻⁵ m, and the amount of smoke is preserved
    (0.40099 versus 0.40097);
  - our own gas export (`--export shot.usda`) is read frame by frame
    as the same grids that are in the VDB files.
- **PointInstancer:**
  - 6 random scenes in centimeters with Z up;
  - instances rotated, scaled and translated, including between samples (velocities,
    accelerations, angular velocities);
  - hidden via `invisibleIds` and via `inactiveIds`, some stretched;
  - a prototype with its own transform and a prototype with an instancer inside.

  Every point of every instance matches `ComputeInstanceTransformsAtTime` to within
  10⁻⁶ m when the rotation is in floats (`orientationsf`). With the rotation
  in halfs the difference is up to 3 mm, for the reason described above.
- **Materials:**
  - bindings: 30 random scenes in `.usdc`. The bindings are on groups, inside
    groups, on meshes and on subsets, some stronger than descendants,
    some only for the `full` purpose. The material of every primitive matches
    `UsdShade.ComputeBoundMaterial` and `GetMaterialBindSubsets`, as do
    its roughness, metalness and color;
  - exports from Blender 4.5:
    - `.usda` with UsdPreviewSurface;
    - `.usdc` with a MaterialX network (OpenPBR);
    - `.usdz` with an image in the package.

    The image, color, roughness, metalness and glass (from `transmission` in the
    OpenPBR network) are read as Blender wrote them;
  - our own export (`Export Geometry` to `.usda`) comes back with the same
    presets, glass, roughness and images: copies next to the scene, the same
    mean colors, placed the same way (by uv, or from three sides).
- **Speed:** a `.usdc` with 200 meshes (22 MB, a million points) opens in
  0.07 s, and the geometry is read from it in 0.12 s.
- **Sample files:** the shot in `examples/usd` reports 0 findings in all 28
  USD validators. So does the materials example `looks.usda`, except for
  `MissingShaderIdInRegistry` on the MaterialX nodes (`ND_…`): `usd-core` from pip
  has no MaterialX plugin, so it does not know the definitions of its nodes.

Tests:
- **`tests/test_usd_read.cpp` (24):**
  - text with values of all kinds, and an error with a line number;
  - crate against the text of the same scene from `tests/data/usd`, as written by USD;
  - crate version 0.4.0 and `.usdz`;
  - composition, specializes and specifier;
  - value clips: interpolation across a boundary, strength relative to the layers above and below
    them, fields from two layers, templates with offset, a loop by a jump (values
    from the library);
  - transforms, geometry import, the USD Camera and USD Import nodes;
  - a primvar on the points of one prim and on the corners of another is merged on corners;
    loose points keep their color;
  - corner normals out to USD as `faceVarying` and back on corners;
  - reading back our own export;
  - PointInstancer: prototypes, placement, hidden and inactive instances,
    a stretched instance, tint and change over time; reading back our own
    instances;
  - Volume: a VDB field in a scene in centimeters with Z up, a vector
    and a volume rotated off the axes;
  - subdivision surfaces: seven meshes subdivided by Blender (OpenSubdiv), through
    USD with creased edges, corner points and an `edgeOnly` boundary. The points match
    to within 4e-7. A Subdivide after the import continues as if subdivision had gone on.
    Sharpness per chain and per edge give the same result; `none`, `bilinear` and a mesh without
    a scheme stay polygons; instance prototypes are smoothed too;
  - 500 corrupted files rejected without a crash (also under ASan).
- **`tests/test_usd_materials.cpp` (6):**
  - UsdPreviewSurface with images, values and glass;
  - MaterialX through a NodeGraph and interface, OpenPBR, glTF and height;
  - binding rules (ancestor, stronger ancestor, `full` purpose, subsets, a binding
    to something that is not a material);
  - our own export and back;
  - `.usdz` with an image in the package;
  - the USD Import node with the Materials parameter.
- **`tests/python/test_usd.py`:**
  - the sample shot;
  - with the `usd-core` library: random transforms, composition, geometry and value
    clips against it, including 80 random shots with clips;
  - material bindings against `UsdShade`;
  - PointInstancers against `ComputeInstanceTransformsAtTime`.
  - a subdivision surface written by the library with a creased front face and a sharp corner.

## 7. In the code

| File | What it does |
|---|---|
| `src/pg/usd/Layer.h` | Layer: values, prim and property specs, list edits, variants; paths; reading a file, including from a package (`readLayer`, `resolveAsset`) |
| `src/pg/usd/Text.cpp` | `.usda` parser |
| `src/pg/usd/Crate.cpp` | `.usdc` reader: LZ4, USD integer coding, token, path, field and spec tables, values of all types |
| `src/pg/usd/Stage.h` | Scene composition: prim indices, opinion strength, values over time, interpolation, value clips, cache of open stages (`openCached`) |
| `src/pg/usd/Geom.h` | Transforms from xformOps, units and axis, geometry (`importGeometry`), camera (`cameraAt`), time (`timeCodeAt`) |
| `src/pg/usd/Shade.h` | Material bindings (`boundMaterial`, `boundSubsets`) and the material network as MaterialX nodes (`materialNodes`, `materialSurface`) |
| `src/pg/render/Textures.cpp` | `textureSet("x.usda#/path")`: material images from the scene |
| `src/pg/nodes/Usd.cpp` | USD Import node (`usdimport`) |
| `src/pg/nodes/Topology.cpp` | Catmull-Clark with creased edges as in OpenSubdiv (`subdivideGeometry`, `subdivisionNormals`), which USD Import uses to subdivide subdivision surfaces |
| `src/pg/sim/Camera.cpp` | `cameraFromUsd`: a USD camera as the shot camera |
| `src/pg/sim/Network.cpp` | The `usd_import` and `usd_camera` nodes; the camera from the file on every frame |
| `src/python/PyUsd.cpp`, `pg.UsdStage` | Reading from Python |
| `tools/prototype/Commands.cpp` | `prototype usd` |
| `examples/usd/make_shot.py` | How the sample shot files were created (with the USD library) |
| `examples/usd/make_looks.py` | How the materials example `looks.usda` was created (with the USD library) |
| `examples/usd/make_subdivision.py` | How the subdivision surface example `subdivision.usda` was created (with the USD library) |
| `tests/data/subdivision/make_subdivision.py` | How the points that Blender (OpenSubdiv) produced from the test meshes were created |

## 8. Limitations

- **Materials:** what the renderers support is read. That is: color image,
  normal map, roughness (value and image), height, opacity,
  metalness, color as a value, and glass. Not read:
  - procedural patterns and material blending;
  - emission, coat and subsurface;
  - `UsdTransform2d` (uv translation and rotation);
  - bindings through collections (`material:binding:collection:*`);
  - a material in a `.mtlx` file attached as a layer.

  Glass from UsdPreviewSurface is recognised only by `opacity`. Blender does not write glass into
  UsdPreviewSurface, but does in a MaterialX network. A material named
  after a preset (`wood`, `glass`…) gets the preset's properties.
  If it has no image of its own, it also gets the preset's photos from the library.
- **NURBS** are not read. `prototype usd` lists them as skipped.
  Prototypes under a PointInstancer are not standalone geometry: they do not stand
  where they are in the file, but where the instancer places them.
- **Volume:** only `OpenVDBAsset` fields are read, not `Field3DAsset`.
  `fieldIndex` is ignored: the first grid of the given name is used.
- **Subdivision surfaces:**
  - `loop` is subdivided with Catmull-Clark. The shape comes out similarly smooth, but made of
    quads.
  - `uv` and other corner values are blended linearly between the corners of a control
    face. By default (`faceVaryingLinearInterpolation`
    `cornersPlus1`) USD smooths them in the interior, so a texture on a coarse mesh can
    shift slightly.
  - Holes (`holeIndices`) are dropped before subdivision. Their edges then become a
    boundary, so the surface near a hole smooths slightly differently than in OpenSubdiv.
  - `interpolateBoundary = none` is treated as `edgeOnly`.
    `triangleSubdivisionRule` is not read.
  - A mesh without `subdivisionScheme` stays polygons (Subdivision surfaces above).
- **Splines** (curve animation, `x.spline`, USD 25 and later) are not read:
  an attribute that has only a spline has no value. Time samples and the rest of the
  file are still read.
- **Collisions from geometry:** an Object with a shape from USD Import takes the shape from frame 1,
  even when the geometry in the file moves. The camera does move frame by frame.
- **Objects with color from the file:** an Object is drawn in the single color of its
  parameter, not with `Cd` from USD.
- **Camera:** film offset, lens distortion and orthographic projection are
  not drawn.
- **Plate:** the shot image behind the CG, holdouts and shadow catchers are described in
  [plate.md](plate.md); the plate must be free of lens distortion.
