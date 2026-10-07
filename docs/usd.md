# Export to USD

USD (Universal Scene Description, from Pixar) is the format in which VFX
studios exchange entire scenes: models, animation, cameras, lights and
volumes. It is read by Houdini (Solaris), Blender, Maya, Omniverse, usdview
and renderers such as Karma, Arnold, RenderMan and others. Prototype writes
**the whole shot as a single `.usda` scene** into it:
- the displayed geometry,
- pieces from the RBD Solver as moving bodies,
- debris and grains as pebbles (PointInstancer),
- the water surface as a closed mesh with velocity and foam,
- rain drops and splash droplets,
- dust and steam as VDB files alongside,
- the camera, sun, sky and floor,
- materials of the displayed geometry as MaterialX with photos alongside
  ([materialx.md](materialx.md)).

Whatever is large and different in every frame (water, rain, debris,
changing geometry) goes into a **file per frame** next to the scene, and the
scene takes its values from there (USD *value clips*, §3). A shot of any
length therefore does not have to fit in memory, and the scene itself stays
small.

The writer was built from the public specification, without the USD library
(clean room).

## 1. Quick start

```bash
./build/prototype sim demolition - --export out/demolition.usda        # simulates and writes
./build/prototype sim rain_pond - --export out/pond.usda               # water and rain: out/pond_frames/
./build/prototype sim wall_collapse - --from-cache cache/wall --export out/wall.usda   # from cache
./build/prototype cook street out/street.usda                          # geometry only, no simulation
```

- **File without `$F`:** `--export` with the `.usda` extension writes the whole shot as one scene;
  with `--start` and `--end`, only part of it (scene times start at `--start`).
- **File with `$F4`:** `--export 'geo.$F4.usda'` writes each frame to its own
  file, geometry only, as with `.ply` and `.obj`.
- **Editor:** the **File › Export USD Scene…** menu item writes all frames that
  are in the cache — in the background, with a progress window; **Stop** leaves a valid scene from the
  frames written up to that point. **File › Export Geometry…** with the `.usda` extension writes the geometry
  of one frame with its materials; with `.mtlx`, only the materials ([materialx.md](materialx.md)).

Where to open the scene:

- **Blender:** File › Import › Universal Scene Description.
- **Houdini:** in Solaris a Sublayer or Reference node, or File › Import.
- **Inspection:** `usdview demolition.usda`, or `usdcat` and `usdchecker` from the `usd-core` package.

## 2. What is in the scene

```
/World                     Xform, default prim
  /Looks/surface           Material: UsdPreviewSurface, color from displayColor
  /Looks/water, /rain      Material: water (transparent, smooth, ior 1.33), rain
  /Looks/glass             Material: glass (clear, smooth, ior 1.5), if any pieces have it
  /Materials/bark …        Material: MaterialX + UsdPreviewSurface, materials of the displayed geometry
  /<node>                  displayed geometry: mesh, curves, points
      /mesh/bark …           GeomSubset: the faces of one material
      /instances             PointInstancer: grass, trees as instances
          /Prototypes/proto_0 …  … prototypes, once
  /pieces/body_0000 …      RBD Solver bodies: Xform (translate, orient)
      /mesh                  … over the body's shape around its center
          /inside            GeomSubset: faces cut by the fracture
          /glass             GeomSubset: glass faces, with the glass material
      /cracks                … glass cracks: invisible until the frame the pane cracks
  /grit                    PointInstancer: debris             ┐
      /Prototypes/proto_0 …  … 12 stone fragments, 6 glass     │
                               shards, once in the scene      │
  /grains                  PointInstancer: grains, same shapes│
  /cloth/mesh, /curves     Mesh and BasisCurves: cloth, ropes │ values from
  /water                   Mesh: water surface                │ per-frame
  /rain/drops              Points: drops                      │ files
  /rain/droplets           Points: splash droplets            ┘
  /gas                     Volume: fields density, temperature, flame (and steam, vel)
      /density …             OpenVDBAsset → <name>_gas/<name>_gas.0001.vdb …
  /camera                  Camera
  /sun                     DistantLight
  /sky                     DomeLight
  /ground                  Mesh: floor
```

On disk:

```
pond.usda                      the scene
pond_frames/pond.0001.usda …   values for one frame (a layer, "value clip")
pond_frames/pond.manifest.usda which attributes the layers provide
pond_gas/pond_gas.0001.vdb …   dust, if any
pond_textures/bark_color.jpg … material photos, if any
pond.mtlx                      materials as a MaterialX document, if any
```

| Prim | What it contains |
|---|---|
| geometry | The displayed node, named after it (`street`, `city`). Closed polygons are a Mesh without subdivision, open lines linear BasisCurves and loose points Points. Points have a width from `pscale`, `v` as `velocities` and `id` as `ids`; mesh `v` are also `velocities`. Vertex normals `N` are `normals` with `faceVarying` interpolation (sharp edges); otherwise point `N` with `vertex` interpolation. Other point attributes (floats, integers, vectors, e.g. `foam`) go out as primvars (`primvars:foam`). Instances (points standing in for prototypes — grass from Grass, trees with Output Instances) are a PointInstancer `instances`: prototypes in a `Prototypes` scope under it, `protoIndices`, `positions`, `orientations`, `scales`, `primvars:tint`, `ids` ([vegetation.md](vegetation.md)). Geometry that does not change is in the scene once; when it changes, it is in the files of the frames in which it changed — instance prototypes stay in the scene, the frames hold only how the instances are placed. |
| color | `Cd` of a vertex, point, primitive or the whole geometry as `displayColor`. It is written once for the whole object, once per face, or once per vertex, depending on how the color varies. |
| materials | Every material of the displayed geometry and its prototypes (what it is made of, roughness, photos, mapping, height) as a `Material` in `/World/Materials`: a MaterialX `standard_surface` graph over the photos, the height as its `displacement`, and a `UsdPreviewSurface`. Faces are assigned by `GeomSubset`s, one per material; for changing geometry the indices are in the frame layers. UVs are `primvars:st`; photos are copied to `<name>_textures/` ([materialx.md](materialx.md)). The material attribute `pg:surface_detail` says how many stains and bumps Cycles adds to it (`f@surface_detail`): USD Import reads it, so the program's materials come back as they were. |
| bodies | A body's shape (colors as in the preview) is written **once**, offset to the body's center. Each frame then has only `translate` and `orient`. A shattered body has `visibility = invisible` from that frame on. A piece that broke during the simulation ([breaking during the simulation](destruction.md#breaking-during-the-simulation)) disappears the same way, and its fragments are extra bodies: `body_NNNN` after the existing ones, with the fragment's shape, invisible until the frame of the break (`visibility` `invisible`, from the break `inherited`), earlier in the piece's place. |
| debris | A PointInstancer with pebbles: the prototypes are the same fragments the renderers draw — twelve stone shapes (a box flattened and stretched, with broken corners and edges) and six glass shards, flat, with unit distance from the center to the farthest corner. They are in the scene once (`Prototypes/proto_0` to `proto_17`); the frame layers hold only which fragment is where: `protoIndices` (shape by fragment number, shards from 12), `positions`, `orientations` (`quath[]`, rotation from the simulation), `scales` (half the fragment size), `velocities`, `ids` and per-fragment `primvars:displayColor` (cut color one shade darker and each fragment with its own shade, as in the preview; glass in the glass color). A fragment gets its number when it is ejected and keeps it while it is in the scene, so the renderer tracks it from frame to frame and motion-blurs it. The instancer has the `surface` material, the shards the `glass` material if there is glass in the scene. Caches older than format 4 have no numbers or velocities (the shape then follows the order), older than format 9 no rotation (then random by number). Until the first fragment flies out, the debris is invisible. |
| cloth | Cloth from the Cloth Solver where its points are, with normals (`normals`) and velocities (`velocities`) for motion blur. Faces form a Mesh, ropes BasisCurves. The color comes from the geometry's `Cd`, otherwise from the Color parameter. Every frame has the whole mesh in its layer, including topology. Torn cloth has, from the frame it tore, more points (torn-off copies) and faces reconnected to them ([cloth.md](cloth.md)). Attributes read only by the solver (`pin`, `mass`, `tear`) are not written. |
| grains | Grain Solver grains as a PointInstancer `/World/grains` with the same twelve pebbles as the debris: the size (`scales`) is the grain radius, plus `positions`, `orientations`, `velocities`, `ids`, `protoIndices` and per-grain color (`primvars:displayColor`), in every frame layer ([grains.md](grains.md)). Debris that became grains (the Grit input) is only here. |
| water | The water surface as a closed quad mesh, the same as from the Liquid Surface node ([geometry.md](geometry.md#water-surface-liquid-surface-and-convert-volume)): smooth normals, `velocities` from the water velocity (cache from format 5), `primvars:foam` (0 to 1) for white foam, ripples from rain on the surface. Closed also at the bottom and walls so that the renderer refracts light through it. Material `water`: color from Water Look, transparency 0.35, roughness 0.02, index of refraction 1.33. When Water Look hides the surface (Surface off), the water is not written. |
| rain | Drops and splash droplets as two Points prims with a number (`ids`) and velocity (`velocities`); a drop is 2 mm wide, a droplet 1 mm. A renderer with motion blur turns them into streaks according to their velocity, as the preview draws them. Material `rain`: color and transparency from the rain Look. |
| dust | Each frame writes one VDB file into the `<name>_gas/` folder next to the scene: grids `density`, `temperature`, `flame` and `steam` if the gas contains steam ([quench.md](quench.md)), and a vector grid `vel`, the gas velocity on blocks of 2 × 2 × 2 cells. The `vel` field has `fieldDataType` float3 and `vectorDataRoleHint` Vector, as the OpenVDBAsset schema marks a vector field. Paths are relative, so the folder can be moved together with the scene. |
| camera | Position and rotation as in the Camera node (`translate`, `rotateXYZ`: degrees around x, then y, then z), focal length, aperture and `exposure` in EV. |
| lights | The sun shines from the direction set in Look, and the sky has the color and intensity from Look. Intensity is relative, as in Look, not in physical units. |
| floor | A square around the scene in the ground color from Output, if the floor is enabled. |

## 3. Per-frame files (value clips)

A `.usda` scene with thousands of frames of water would be gigabytes in size
and would have to be built in memory as a whole. So everything that is large
and different in every frame goes into the **layer for that frame**
(`pond_frames/pond.0007.usda`), and the scene takes values from it through a
USD mechanism called *value clips* — the same as an export from Houdini with
the "file per frame" option.

- **A frame layer** contains only the time samples in that frame, under the
  prim they belong to (`over "World" { over "water" { point3f[] points.timeSamples = { 7: [...] } } }`).
  It is written as soon as the frame arrives: the export holds only one frame in memory.
- **The scene**, on the prim `/World/water` (and `/World/rain`, `/World/grit`,
  changing geometry), states which layers it takes values from and from which frame:

  ```
  def Mesh "water" (
      clips = {
          dictionary default = {
              double2[] active = [(1, 0), (2, 1), …]
              asset[] assetPaths = [@./pond_frames/pond.0001.usda@, …]
              asset manifestAssetPath = @./pond_frames/pond.manifest.usda@
              string primPath = "/World/water"
              double2[] times = [(1, 1), (2, 2), …]
          }
      }
  )
  ```

  Attributes provided by the layers are only declared by the scene (type and,
  for primvars, `interpolation`), without a value. What does not change (drop
  width, water color, `subdivisionScheme`, material) is held by the scene itself.
- **The manifest** (`pond.manifest.usda`) declares every attribute the
  layers provide. A layer that has no sample for it means "no value in this
  frame" — nothing is carried over from another frame.
- **Changing geometry** has a layer only in frames where it changed; in
  between, the last one applies. Until the displayed geometry changes, the
  export holds back the first frame's layer: if it never changes, it is
  written once into the scene, as before. Topology (faces) is in the layer of
  every frame in which the geometry changed, even if it stayed the same.
- **Frames where something is missing** (debris before the explosion, water
  hidden in Look) have `visibility = invisible` on the prim.
- **Between frames** (motion blur, frames 7.25 and 7.75) USD interpolates the
  values as with ordinary time samples. When the point count changes (water,
  rain), it holds the frame's value and the renderer moves the points by
  `velocities`.

The per-frame files sit next to the scene and the paths are relative: it is
enough to move the `pond_frames/` folder together with `pond.usda`.

## 4. Conventions

- **Units and time:**
  - the Y axis points up, the unit is the meter (`metersPerUnit = 1`);
  - the time code is the frame number (1 … N); `timeCodesPerSecond` comes from Output:
    30, not the 29.999998 that `1 / (1 / 30.0f)` gives.
- **Camera according to the USD specification:** focal length and aperture are given in tenths of a scene unit.
  - With meters, 38 mm = `0.38`.
  - The film is 24 mm high (like `Camera::fovY`); the width follows the image aspect ratio.
  - Blender follows this convention. A program that reads the focal length directly in mm will see a focal length a hundred times shorter.
- **Bodies as transforms, not a mesh in every frame:**
  - The file is an order of magnitude smaller.
  - The renderer motion-blurs correctly: between samples a body rotates around its own center, not around the scene origin.
  - USD writes quaternions with the real part first: `(w, x, y, z)`.
- **Time samples only where the value changes:**
  - A static city is written once.
  - A body that does not move has one position.
  - A camera that stays still has one position.
- **Material:** the displayed geometry has MaterialX materials ([materialx.md](materialx.md)).
  Everything else (pieces, debris, cloth, floor) has a single `UsdPreviewSurface` that takes its color from `displayColor`
  (`UsdPrimvarReader_float3`). Cut faces are a `GeomSubset` with the
  `materialBind` family, so a custom material (concrete, brick) can be assigned to them in Houdini or Blender.
  Glass faces (primitives with `glass`) have their own `glass` subset with the material
  `/World/Looks/glass`; glass cracks are a `cracks` mesh next to `mesh`, invisible
  while the pane is intact, and glass debris uses the shard prototypes (`protoIndices`
  from 12) with the glass material.

## 5. Verification

We verified the scenes with Pixar's `usd-core` 26.08 library. It exists only
in the verification environment; prototype does not need it.

- **USD validators:** for `wall_collapse`, `demolition`, `rain_pond`, `liquid_points`, `glass_window` and `debris_stairs`
  all 28 validators report 0 findings (schemas, stage metadata, GeomSubset families, material
  binding, shaders).
- **Per-frame files:** the library composes the values from the layers as they are in the frames:
  the `rain_pond` water surface has 37,173 points at frame 24 with normals, velocities and foam,
  the drops have their numbers; between frames the extent is interpolated, the points are held. The
  `wall_collapse` debris is invisible until the first fragment flies out, and then has its own fragments in every frame.
  The `debris_stairs` debris has as many `primvars:orient` (`quatf[]`, per vertex) in every frame
  as it has points, and all are unit quaternions.
- **Bodies:** body points transformed to world space by the library (`ComputeLocalToWorldTransform`)
  match the pieces returned by `posedPieces`.
  - The median deviation is 1e-6 m, i.e. float precision.
  - The maximum is 0.13 mm, for bodies whose point is shared with a neighboring piece: the preview
    moves it with the first of them, USD gives each body its own copy.
- **Camera:** the axes match the Rz·Ry·Rx rotation of the Camera node, the focal length is 22 mm and the aperture 42.67 × 24 mm.
- **Dust:** paths to the VDB files resolve relative to the scene.
- **Materials:** how the library composes them and what the validators report is described in
  [materialx.md](materialx.md#5-verification).

| Shot | Frames | Bodies | `.usda` | Frame layers | VDB | Export from cache | Opening |
|---|---|---|---|---|---|---|---|
| `wall_collapse` | 120 | 274 | 4.8 MB | 105 files, 12 MB | 148 MB | 14 s | 0.1 s |
| `demolition` | 180 | 710 (421 shattered) | 16 MB | 150 files, 9.4 MB | 215 MB | 27 s | 0.3 s |
| `rain_pond` | 24 | — | 8 kB | 24 files, 116 MB | — | 1.5 s | 0.02 s |

The tests are in `tests/test_usd.cpp` (12):
- values as USD writes them (numbers, quaternion `(w, x, y, z)`, strings, prim names);
- a scene written prim by prim with time samples, exact text;
- geometry to `.usda` by extension;
- bodies translated and rotated exactly like `posedPieces`;
- debris as a PointInstancer with pebble prototypes: numbers, velocities, rotations (`quath[]`), sizes and shapes from the frame in the frame layer, written as soon as the frame arrived;
- dust as VDB files next to the scene;
- focal length in tenths of a unit and the sun where Look puts it;
- what does not change written once, moving geometry in the layers of the frames where it
  changed (the first frame only once it is clear that it moves), and a geometry name that
  does not collide with another prim;
- water and rain in every frame's layer, with materials in the scene and a manifest;
- glass with the glass material and cracks that appear when the pane breaks;
- cloth in every frame's layer, torn as it is;
- gravel grains as a PointInstancer with pebble prototypes in every frame's layer.

That USD Import does not take PointInstancer prototypes as scene geometry (so
debris and grains do not show up as a heap of pebbles at the origin when the
program's own export is loaded) is verified by `tests/test_usd_read.cpp` ([usd-import.md](usd-import.md)).

## 6. In the code

| File | What it does |
|---|---|
| `src/pg/io/Usda.h` | USDA writer: values as text, prims (`def` and `over`), stage, time samples, `clips`. Geometry as Mesh, BasisCurves and Points (`geometryPrim`, `geometryStage`), instances as a PointInstancer (`instancerPrim`, `addPrototypes`), its per-frame attributes (`fields`) and primvars from point attributes (`pointPrimvars`). |
| `src/pg/io/Export.cpp` | `.usda` in `writeGeometry`: geometry as a standalone scene |
| `src/pg/sim/UsdExport.h` | A simulation shot: frames arrive one at a time (`add`) and the layer of each is written immediately; the scene and manifest are written at the end (`finish`). `exportGeometry`: geometry as a scene with materials, or `.mtlx` |
| `src/pg/render/MaterialGraph.h` | Materials as MaterialX graphs and their assignment to faces ([materialx.md](materialx.md)) |
| `src/pg/sim/WaterMesh.h` | The frame's water surface as a mesh (`waterMesh`), the same as from the Liquid Surface node |
| `src/pg/nodes/Volumes.cpp` | Volume to polygon mesh (`volumeToMesh`, surface nets) and the Convert Volume node |
| `tools/prototype/Commands.cpp` | `prototype sim --export ….usda` |
| `tools/prototype/SimWorkspace.cpp` | File › Export USD Scene… |

## 7. Limitations

- **Text `.usda` only.** Binary `.usdc` is not written, so large scenes
  are bigger and load more slowly than from Houdini.
- **Text is large.** A `rain_pond` frame (35 thousand surface points, 5 thousand
  drops) is 4.8 MB in its layer; a binary `.usdc` with the same content is 1.4 MB.
- **Water particles as such** (for custom meshing in Houdini) do not go into
  the scene by themselves: it is enough to display Liquid Points and they go out as changing geometry.
- **A farm chunk** (`--start`, `--end`) writes its own scene with only its
  frames. Frame layers are separate files, but nothing assembles the scene of
  the whole shot from the chunks yet.
- **Objects from the Object node** (spheres, boxes) are not exported. They are
  simulation obstacles, not what gets rendered. To see them, display them as geometry.
- **Reading** (a matchmove camera, sets and models from other programs, loading
  back the program's own export) is described in [usd-import.md](usd-import.md).
- **Lights have no physical units.** The intensity of the sun and sky needs to
  be adjusted in the renderer.
