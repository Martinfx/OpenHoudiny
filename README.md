# Prototype

A prototype of professional node-based software for procedural geometry
and visual effects, modeled on Houdini: geometry built from nodes with
attributes and wrangles, smoke, fire, water and rain simulations, animation,
shot rendering to images and video, and export to other tools. Run without
a command, `prototype` opens the editor; with a command (`sim`, `render`,
`gen`, …) it works without a window.

- **[ROADMAP.md](ROADMAP.md)** — goal, what is done, next steps, risks
- **[ARCHITECTURE.md](ARCHITECTURE.md)** — data model, cook engine, invariants
- **[docs/shader-graph.md](docs/shader-graph.md)** — shader node editor for
  OpenGL, OpenGL ES, Vulkan and Direct3D: how it works and how to extend it
- **[docs/pyro.md](docs/pyro.md)** — smoke, fire, water and rain simulations
  built from nodes (like Pyro and FLIP in Houdini): an editor with a node
  network, objects and a gizmo in the viewport, flow on a 3D grid, multigrid,
  particle-based water, rain in the wind, volume rendering, a water surface
  with reflections and refraction, and a shot camera
- **[docs/geometry.md](docs/geometry.md)** — geometry in the same network
  (nodes like SOPs in Houdini): display flag, viewport, a thumbnail in every
  node, attribute spreadsheet, geometry as the shape of colliders and
  sources, simulations back as points and volumes
- **[docs/editing.md](docs/editing.md)** — editing geometry in the viewport
  as in Houdini: points, edges, primitives and vertices (corners) selected
  with the mouse (click, box, lasso, brush; visible only, or hidden too); a
  handle moves, rotates and scales them (Edit node; soft selection with a
  preview, distance also measured along the surface), group, delete and
  dissolve edges from the selection (Group, Blast, Dissolve), symmetry for
  edits and brushes (M), a brush paints an attribute — cloth pins and
  tearing (Attribute Paint) — and shapes geometry like clay: inflate,
  push in, smooth, grab, flatten (Sculpt, incrementally even on a million
  points; with dyntopo the mesh refines itself under the brush)
- **[docs/trees.md](docs/trees.md)** — trees that grow the way plants do
  (Tree node modeled on Weber and Penn): a trunk that can fork into leader
  branches, three levels of branches placed around the parent at the golden
  angle, seven crown shapes (spruce, oak, birch, poplar, acacia, willow,
  linden), leaves and needles; a forest on points, every tree different;
  branches that grow over a wall or a roof; wind via the Plant Wind node
  driven by `flex`, also as springy branches with inertia (Dynamics); a
  skeleton for custom leaves
- **[docs/vegetation.md](docs/vegetation.md)** — vegetation as instances:
  grass from clumps of blades (Grass node) across terrain following painted
  density and slope, shrubs and trees as variants that points stand in for;
  the viewport draws them with GPU instancing, USD gets a PointInstancer, OBJ
  gets copies; wind rotates `orient`; a meadow by a forest with 1.9 million
  blades in 148 ms
- **[docs/cycles.md](docs/cycles.md)** — rendering with Blender's Cycles:
  a Render tab next to the Viewport (first image immediately, frame by
  frame during playback) and the command line `--renderer cycles`; the
  scene converted to Cycles including smoke and fire, Principled BSDF, glass
  and water, a physical sky and sun as in Blender with adjustable clouds, or
  a sky from an image (HDRI), AgX color transform, surface detail, surfaces
  displaced by texture height (Displacement), Open Image Denoise denoising;
  debris as angular fragments of stone and glass, raindrops as streaks of
  water and wet ground where it rains; motion blur while the shutter is open
  (pieces, debris, cloth, water, objects, smoke and fire, camera), the same
  in the path tracer
- **[docs/materials.md](docs/materials.md)** — materials and textures:
  primitives say what they are made of (`s@material`: concrete, concrete
  fracture, plaster, brick, window, steel, wood, bark, paving, roof tiles,
  lawn…), generators set them themselves, the Material node sets them on
  anything; Cycles draws photographs from a library (Bistro and Babylon.js,
  CC-BY 4.0) or procedural patterns, the path tracer draws photographs with
  relief from normal maps; roof tiles along the roof, courses horizontal;
  geometry without `Cd` in the colors of its materials; custom textures
  from Poly Haven and ambientCG; projected from three sides, or by UV
  (UV Project node, imports) with normal maps; the texture travels with a
  piece in flight
- **[docs/pathtracer.md](docs/pathtracer.md)** — a custom CPU path tracer,
  the second option of the Render tab and `--renderer path`; bounced
  light, soft sun, translucent grass and leaves, glass and water, debris,
  rain and wet surfaces, depth of field, neural-network denoising (Intel
  Open Image Denoise); settings in the Output node, PNG and EXR
- **[docs/wrangle.md](docs/wrangle.md)** — wrangle, a language for
  computations over geometry like VEX: variables, loops, functions, arrays;
  runs over points, primitives or the whole geometry; neighbors, additional
  inputs, creating and deleting geometry; sliders from `ch()`
- **[docs/assets.md](docs/assets.md)** — digital assets: selected nodes
  as a single node with its own parameters and version, a `.pgasset`
  library, diving in and back out, the network carries its assets with it
- **[docs/animation.md](docs/animation.md)** — keyframes on any
  parameter, expressions in parameters (`$F`, `ch("../box1/sizex")`), moving
  colliders whose motion both gas and water pick up
- **[docs/cache.md](docs/cache.md)** — simulation cache on disk and export:
  points to PLY, volumes to OpenVDB, polygons to OBJ, frame by frame for
  Houdini, Blender and renderers; background bake with progress, cancel
  and resume from a checkpoint, preview on coarser grids (how fine is set
  by the Output node), wedge (parameter variants), step profile and large
  caches in the viewport (spilled to disk, read ahead, proxy grids)
- **[docs/destruction.md](docs/destruction.md)** — destruction: Voronoi,
  Concrete, Wood and Glass Fracture (concrete, wood into splinters along the
  grain, glass), rigid bodies on top of Jolt Physics, glued pieces as a
  single body that impacts break apart, pieces that break at runtime where
  the blow landed, charges, crushing into dust, debris as particles that
  hit pieces and stay lying on them, dust trailing flying pieces, air
  pushed out by the collapse driving dust into the streets, a fall directed
  by animation (Guide), debris in water and in gas (wood floats and the
  current carries it, debris is carried by the blast wave); a high-rise
  demolition in a city and a wall collapse seen from ground level as
  videos, a flooded courtyard with floating crates
- **[docs/cloth.md](docs/cloth.md)** — cloth, ropes and soft bodies (XPBD,
  similar to Vellum): a tablecloth over a table, a flag in the wind, a ball
  holding its volume, pinned points carried by animation, point, edge and
  primitive collisions with objects, RBD pieces and itself, wind and gas
  flow; tearing and two-way coupling with rigid bodies (a tarp catches
  crates, a concrete block punches through it); soft bodies that hold their
  shape, clay that stays dented
- **[docs/grains.md](docs/grains.md)** — sand, gravel and soil (similar to
  Vellum Grains): grains with friction and cohesion, piles as steep as
  friction holds, wet sand stands; pouring as a stream, collisions with
  objects, two-way coupling with RBD pieces, wind and gas flow; debris from
  RBD Solver fractures as grains that pile up on and around the wreckage
- **[docs/quench.md](docs/quench.md)** — water and fire: Liquid Solver water
  and raindrops put out the fire they reach (they cool it, soak the fuel
  and the sources), and the heat they take turns into white steam — its own
  gas field that rises and thins out; flames evaporate water; a campfire in
  a downpour goes out, a bucket of water puts it out within a few frames, a
  hose gradually; a downpour fills the pool it falls into
- **[docs/python.md](docs/python.md)** — Python API (`import pg`): networks,
  parameters, geometry as numpy arrays without copying, frame-by-frame
  simulation, cache, USD and rendering from a script; a network as Python
  code (`as_code()`)
- **[docs/usd.md](docs/usd.md)** — the whole shot to USD for Houdini, Blender
  and renderers: geometry, pieces as moving bodies, debris, water surface,
  rain, dust as VDB, camera, sun and sky; whatever changes every frame goes
  into a file per frame (value clips)
- **[docs/usd-import.md](docs/usd-import.md)** — reading USD without the
  library (`.usda`, `.usdc`, `.usdz`): the scene composed as in USD
  (sublayers, references, payloads, variants, classes, value clips), a
  matchmove camera as the shot camera, set and models as geometry in the
  network including materials (MaterialX, UsdPreviewSurface), subdivision
  surfaces smoothed; verified against the USD library
- **[docs/alembic.md](docs/alembic.md)** — Alembic without the library: the
  whole shot as a single `.abc` archive (geometry, pieces as moving bodies,
  debris, grains, water, rain, cloth, camera; gas as VDB alongside) and
  reading geometry and cameras from Blender, Maya or Houdini as network
  nodes; verified with Blender
- **[docs/materialx.md](docs/materialx.md)** — materials as MaterialX
  without the library: in USD export (MaterialX and UsdPreviewSurface
  shaders, height as displacement, primitives assigned via GeomSubsets,
  photos alongside the scene), as `.mtlx`, and reading `.mtlx` (including
  from Poly Haven) as texture sets; verified with the MaterialX 1.38 and
  1.39 libraries, `usd-core` and Blender
- **[docs/vdb.md](docs/vdb.md)** — reading OpenVDB without the library:
  smoke and fire from Houdini, Blender or EmberGen played back as the shot's
  gas (VDB Gas), a level set as a collider or source shape (VDB Import);
  zip, Blosc, half, tiles, verified on files from OpenVDB 10 and 13
- **[docs/color.md](docs/color.md)** — color: AgX, ACES 1.0 and ACES 2.0
  views as in OpenColorIO configs, views from a studio's, ACES or Blender
  OpenColorIO configs (`config.ocio`), EXR in ACEScg and ACES2065-1;
  verified against OpenColorIO 2.6
- **[docs/render.md](docs/render.md)** — images and video: PNG, sequences,
  `.avi` video with no dependencies and `.mp4`/`.webm`/`.gif` via ffmpeg,
  rendering in the background of the editor with progress; EXR in linear
  light with depth, motion vectors and masks for compositing
- **[docs/plate.md](docs/plate.md)** — the shot's image (plate) behind CG
  through the shot camera: PNG, JPEG and EXR sequences read without
  libraries, holdout and shadow catcher, CG with alpha and a `catcher` pass
  to EXR; in the viewport, in Cycles and in the path tracer
- **[docs/gpu.md](docs/gpu.md)** — compute on the GPU through Vulkan, the
  foundation for moving the solvers to NVIDIA, AMD and Intel cards: the
  devices, memory on the device, kernels compiled with the program, and
  `prototype gpu`, which measures a card against the CPU with results the
  same to the bit; the Pyro Solver's GPU switch steps the gas on the card

> **Name.** The project is called **Prototype**; the working name was too
> similar to a SideFX trademark. The namespace in the code remains the
> neutral `pg`, and the file formats (`.pgsim`, `.pgsg`, `.pgnodes`) do not
> change.

## What it is

Prototype **is not a product**. It demonstrates the whole path that
professional procedural software takes — from geometry through
simulations to the image and export — and measures what it costs. The core
rests on four claims that the prototype verifies by measurement while they
are still cheap to disprove:

1. **Copy-on-write on attribute arrays** keeps memory in check across long
   node chains.
2. **Lazy pull evaluation with versioning** makes interactive editing
   possible.
3. **Deterministic chunking** gives a bit-identical result regardless of
   the number of threads.
4. **A per-element language** bound to slots is a usable and measurable
   baseline for a future JIT.

The prototype outright disproved one of the criteria of the original
roadmap — see [ROADMAP.md §3](ROADMAP.md#3-what-is-done), the note on M5.

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
ctest --test-dir build --output-on-failure
```

The whole program computes vectors, matrices and quaternions with the
**GLM** library (OpenGL Mathematics, MIT, header-only). A system version
1.0 or newer is used (`pkg install glm` on FreeBSD,
`sudo apt install libglm-dev` on Debian 13 and Ubuntu 25.04); otherwise
CMake downloads it.
Older packages (Ubuntu 24.04 has 0.9.9) are skipped.

The path tracer's rays are traced by the **Intel Embree 4** library
(Apache 2.0): `pkg install embree` on FreeBSD,
`sudo apt install libembree-dev` on Debian 13 and Ubuntu 24.04. When it is not on the system,
CMake downloads it and builds it once with only what the path tracer needs
(about 11 minutes on four cores). Without it
(`-DPG_EMBREE=OFF`) the path tracer uses its own BVH: the same image, but
slower ([docs/pathtracer.md](docs/pathtracer.md)).

The path tracer renders smoke, fire and dust via **NanoVDB** (part of
OpenVDB, Apache 2.0, header-only, nothing is compiled). The system version
is used if this compiler can compile it. Otherwise CMake downloads it with
OpenVDB 13.0 (35 MB in a few seconds). Ubuntu 24.04 has the old version
10.0.1 with a different interface, which is skipped. NanoVDB from OpenVDB
13.1 with libc++ 18 (clang 18 on FreeBSD 14) is skipped too, because it
needs `std::atomic_ref`, which that libc++ lacks. Without NanoVDB
(`-DPG_NANOVDB=OFF`) the path tracer does not render gas; the viewport does.

The noise a render leaves behind is removed by **Intel Open Image Denoise 2**
(Apache 2.0): `pkg install oidn` on FreeBSD. When it is not on the system,
CMake downloads it and builds it once from source: version 2.3.3, static,
CPU only, in about a minute. For that it needs ISPC 1.21 or newer and TBB
(`sudo apt install ispc libtbb-dev` on Debian and Ubuntu). Without it
(`-DPG_OIDN=OFF`) a custom filter does the denoising.

The final image is rendered by Blender's **Cycles** (Apache 2.0). CMake
downloads it from GitHub (tag v4.5.0) and builds it once, CPU only
(about 2 minutes on four cores). It needs **OpenImageIO** and TBB: `pkg
install openimageio pugixml onetbb` on FreeBSD, `sudo apt install
libopenimageio-dev libpugixml-dev libtbb-dev` on Debian and Ubuntu. Without
them (or with `-DPG_CYCLES=OFF`) the custom path tracer renders
([docs/cycles.md](docs/cycles.md)).

Compute on the GPU goes through **Vulkan** ([docs/gpu.md](docs/gpu.md)).
Building it takes the Vulkan headers and glslangValidator: `pkg install
vulkan-headers glslang` on FreeBSD, `sudo apt install libvulkan-dev
glslang-tools` on Debian and Ubuntu. Nothing is linked against Vulkan: the
loader is opened when a device is first asked for, so a machine without it
computes on the CPU. Without them (or with `-DPG_WITH_VULKAN=OFF`) it is
left out.

The default build includes the editor: at configure time it downloads Dear
ImGui and GLFW if they are not on the system (`sudo apt install libglfw3-dev`).
When it finds the Python development files (`python3-dev`), it downloads
pybind11 and also builds the `pg` module into `build/python`
([docs/python.md](docs/python.md)); a different Python is selected with
`-DPython3_EXECUTABLE=…`. Without the editor and without Python the build has
no external dependencies apart from Jolt, GLM, Embree, NanoVDB, Open Image Denoise and Cycles (with OpenImageIO); C++20 and the standard
library are enough:

```bash
cmake -S . -B build -DPG_BUILD_GUI=OFF -DPG_BUILD_PYTHON=OFF
```

With sanitizers:

```bash
cmake -S . -B build-asan -DPG_SANITIZE=ON -DPG_BUILD_GUI=OFF        && cmake --build build-asan && ./build-asan/pgtests
cmake -S . -B build-tsan -DPG_SANITIZE_THREAD=ON -DPG_BUILD_GUI=OFF && cmake --build build-tsan && ./build-tsan/pgtests
```

## Running

```bash
./build/pgtests            # 715 tests: 105 core, geometry and viewport editing, 42 wrangle language and expressions, 7 digital assets, 88 rigid bodies and destruction (concrete, rebar, glass, bricks, constraint network, debris, runtime fracture, guided simulation, debris in water and gas), 55 gas (sparse grid, upres, quenching, motion blur), 27 water and rain, 31 cloth, soft bodies and grains, 19 particles, animation and determinism, 39 simulation network and geometry in it, 42 shader graph, materials, UV and normal maps, 56 render, ACES colours and OpenColorIO configs, motion blur, EXR, images and video, 57 cache, export and checkpoints, 39 USD (writing and reading, materials, instances, volumes), 11 Alembic, 24 VDB (reading, writing with compression), 8 MaterialX, 40 trees and vegetation, 6 GPU compute (the gas stepped on the GPU with every force, water and solids, division and square roots, the same to the bit; skipped without a Vulkan device)
ctest --test-dir build -R python                   # 57 tests of the pg module (Python); against the USD, Pillow and OpenEXR libraries, if present
./build/pgeditortests      # editor UI without a window and without OpenGL: font, Escape and menus, node menu, rows, tabs, node names in the network, room for thumbnails, unsaved changes, file overwrite, autosave
ctest --test-dir build -R editor_                  # the editor itself, scripts under xvfb: Quit with changes, Save As, crash and recovery, Simulate Again
./build/prototype gpu      # the Vulkan devices; the best GPU measured against the CPU (docs/gpu.md)
PYTHONPATH=build/python python3 examples/python/fracture_stats.py
./build/pgbench            # measurements of the claims above
./build/pgbench_rigid      # rigid bodies: the demolition tower and ten times more pieces, 1 and all threads
./build/pgbench_pyro 96 576 # demolition dust at several resolutions: phase timings, memory, how much of the domain the dust occupies (--dense: dense)
./build/pgbench_pyro 64 --example campfire --upres 3 # campfire with upres: time and memory of the fine grid
./build/pgdemo out.obj --frames 24
./build/prototype                                  # editor: empty scene, Shift+A adds fire, water, rain
./build/prototype --example campfire               # simulation example: campfire
./build/prototype sim campfire fire.mp4            # the whole shot to video (.avi even without ffmpeg)
./build/prototype examples/shaders/fire.pgsg       # editor on a shader network, with the graph
./build/prototype gen examples/shaders/marble.pgsg --target all -o out/
./build/prototype sim campfire fire.png            # simulation without a window, to PNG
./build/prototype sim explosion out/boom.png --every 2 --set charge.fuel=80
./build/prototype sim lakeside shot.png            # shot through the camera: fire, water, rain, wind
./build/prototype --example rock_garden            # geometry in the network: rocks from copies of a sphere, rain
./build/prototype --example spiral_stairs          # Detail Wrangle builds a staircase, water runs down it
./build/prototype sim liquid_points points.png     # water particles as points coloured by a wrangle
./build/prototype --example wake                   # animation: a sphere moves through a pool, wave and wake
./build/prototype sim campfire_vdb - --cache cache/fire                          # simulate once, to disk
./build/prototype sim campfire_vdb fire.png --from-cache cache/fire --every 10   # render from cache
./build/prototype sim campfire_vdb - --from-cache cache/fire --export-node volumes --export 'out/fire.$F4.vdb'
./build/prototype sim vdb_fireball fireball.png     # fireball from a VDB file: played back, not simulated
./build/prototype sim vdb_rock rock.png --every 15  # a stream of water hits a boulder from a VDB level set
./build/prototype sim demolition - --export demolition.abc   # the whole demolition as a single Alembic archive
./build/prototype sim alembic_shot shot.png --every 24       # fire in a set from Blender, through a camera from Alembic
./build/prototype sim liquid_points - --export 'out/water.$F4.ply'                # particles to PLY
./build/prototype --example street                 # a street from three Building digital assets
./build/prototype --example meadow                 # a meadow by a forest: grass, shrubs and trees as instances
./build/prototype --example river_flight           # a flight over a meadow along a river up to the mountains
./build/prototype sim foliage f.png --renderer cycles   # linden and spruce in grass: leaves with alpha cutout, bark by UV
./build/prototype sim ecosystem e.png              # a forest that grew by itself: birches, oaks and spruces over 120 years, hazels in the shade beneath them
./build/prototype sim tree_obstacles t.png         # trees by a wall and under a pergola: branches avoid the obstacles
./build/prototype cook street street.obj --set tower.floors=12   # geometry without a window, to OBJ
./build/prototype cook street - --hash --threads 1 # geometry hash: the same on 1 and 4 threads
./build/prototype sim matchmove mm.png --every 24  # fire in a set from USD, through a matchmove camera (USD)
./build/prototype sim usd_looks l.png --renderer cycles   # props with materials from USD: MaterialX, UsdPreviewSurface, OpenPBR
./build/prototype sim usd_subdivision s.png --renderer cycles   # subdivision surfaces from USD smoothed as in OpenSubdiv, with creased edges
./build/prototype sim displacement d.png --renderer cycles   # bricks, bark and paving actually displaced by height (Displacement)
PYTHONPATH=build/python python3 examples/usd/make_plate.py   # shot plate: then it burns in a filmed courtyard
./build/prototype usd examples/usd/shot.usda       # what a USD file contains: layers, tree, cameras, geometry
./build/prototype help                             # commands: list, gen, check, render, sim, cook, usd
```

`pgdemo` builds the graph `grid → pointwrangle → groupbox → blast → transform`,
cooks it, prints an attribute table in the style of the geometry spreadsheet
and writes an OBJ that can be opened in Blender or anywhere else.

## Language snippet example

```c
@P.y = noise(@P * 0.45 + vec3(@Time, 0.0, 0.0)) * 2.0 - 1.0;
@height = @P.y;
@Cd = vec3(fit(@P.y, -1.0, 1.0, 0.1, 1.0), 0.4, 0.8);
int near[] = nearpoints(0, @P, ch("radius"));
foreach (int pt; near) {
    if (pt > @ptnum) addprim(0, "polyline", @ptnum, pt);  // lines to nearby points
}
```

The type of a created attribute is inferred from the right-hand side:
`@height` is created as a `float`, `@Cd` as a `vector`. The node marks itself
as time-dependent because the snippet reads `@Time`, and `ch("radius")`
adds a slider to it. The language is described in
[docs/wrangle.md](docs/wrangle.md).

## Status

Done and tested: COW geometry with volumes, cook engine, time dependency,
LRU cache, deterministic parallelism, 28 node types (generators,
primitives, scatter, copy to points, OBJ, extrude, subdivide, clip, volume
to polygons…), 65 core tests (clean under ASan,
UBSan and ThreadSanitizer). **Wrangle** is a language like VEX: types,
variables, loops, functions, arrays, strings, matrices; it runs over points,
primitives, vertices or once over the whole geometry, reads neighbors and
additional inputs, creates and deletes geometry, and the result does not
depend on the number of threads.

Alongside geometry there is a second kind of network: a **shader graph**
with a node library in text files, a generator for four languages (GLSL 330,
GLSL ES 300, Vulkan GLSL 450 → SPIR-V, HLSL) and an editor with a live
preview, including animated effects (fire, smoke). In CTest every built-in
node is compiled for all targets via glslangValidator and spirv-val.

**Smoke, fire, water and rain simulation** (`src/pg/sim`) is assembled from
nodes like Pyro and FLIP in Houdini: scene objects (sphere, box, cylinder,
cone, torus and models from OBJ files, each translated, rotated and
stretched), sources of the same shapes (fuel, smoke, heat, flicker,
motion, time window), forces (turbulence, wind, vortex, attractor, drag), a
solver, look and output. The solver computes gas flow on a 3D grid: a
staggered MAC grid, MacCormack advection, combustion with expansion,
vorticity confinement and pressure via a multigrid that knows about the floor
and the colliders. The grid is sparse as in Sparse Pyro: only tiles of
8 × 8 × 8 cells that contain gas are computed, so the demolition dust at
103.5 million voxels takes 19 minutes. The Pyro Upres node carries the gas of
a coarse simulation on a grid two to four times finer and adds vortices that
the coarse grid cannot hold (curl noise advected with the flow): a campfire
computed at resolution 64 has the detail of a simulation at 192 in half the
time. Water is carried by particles (FLIP) and the grid preserves its volume:
pressure with a free surface (ghost fluid, walls partially covered by
bodies) is solved by a conjugate gradient method with multigrid. Water falls,
splashes, flows around bodies and fills tanks; in the image it reflects the
sky and objects, refracts light and takes on its color with depth. Rain
falls from a cloud, the wind slants it in gusts (fronts that travel with the
wind), it splashes off objects and makes rings on water; the floor is wet.
Everything is deterministic on any number of threads. The camera node
defines the shot: the editor looks through it (with a frame guide) and both
rendering and `prototype sim` use its view at its resolution. The editor has
the same layout for simulation and for shaders: its own node canvas with
zoom and, in every node, a thumbnail of what it does (geometry, object
shape, solver frame, camera shot; for shaders a sample of the node's
output), a parameter panel, a viewport (gas on the floor
with shadows, fire glow, objects, guides) and a timeline over the frame
cache, with simulation on its own thread, undo/redo and twenty-six
examples. The viewport works like a 3D application: click selects, the
gizmo translates, rotates and scales (W, E, R, snapping, local and world
axes) and Shift+A adds an object, a smoke or water source, rain, a force or
a camera, wired straight into the network.

**Geometry** lives in the same network as simulation, like SOPs in Houdini:
box, sphere, cylinder, grid, OBJ file, scatter, copy to points, transform,
merge, wrangle, PolyExtrude, Subdivide (Catmull-Clark with creased edges like
OpenSubdiv), Clip with cap on the cut,
Fuse, Connectivity, Attribute Transfer, Convert Volume (volume to
polygons) and more. **For-Each loops** run
part of the network for each piece, primitive or point, or repeatedly on its
own result. The geometry core computes it incrementally — dragging a slider
recooks only the nodes downstream of it. The node with the *display flag* is
shown in the viewport (polygons in `Cd` colors, points, lines, volumes) and
the attribute spreadsheet shows points, vertices, primitives, detail and
volumes. Geometry can be the shape of a collider or of a smoke or water
source, and simulations come back as geometry: water particles, raindrops
and gas grids as points and volumes for further nodes, and water also as a
closed surface with velocity and foam (Liquid Surface), from which the
renderer renders it.

**Digital assets**: selected geometry nodes become a single node
(Edit › Make Asset) with the parameters the asset chooses (right-click a
parameter name › Promote). Double-click (I) dives inside, U goes back; every
change inside is a new version, which all instances follow immediately. The
library reads `.pgasset` files from the program, from `$PROTOTYPE_ASSETS`
and from the user folder; a network saves the definitions of the assets it
uses at its end, so it opens anywhere. The **Building** asset builds a
building from ten sliders (floors, dimensions, windows, balconies, color)
and the **street** example builds a street from it; `prototype cook` cooks
geometry without a window to OBJ, PLY or VDB
and prints its hash — the same on 1 and 4 threads.

**Destruction**: **Voronoi Fracture** cuts a closed body into pieces
(plane cuts with caps; the pieces together are exactly the original body),
**Concrete Fracture** breaks it like concrete — unequal pieces, smallest
around the impact point, chipped corners, rough fracture surfaces that still
fit together exactly, and beneath them a flat cut (`proxy`) for simulation —
**Wood Fracture** splits it like wood into long splinters and slats along
the grain, with cross-grain fractures frayed into splinters,
**RBD Cluster** groups pieces into chunks with stronger glue inside, which
break apart only on a hard landing (secondary fracturing), **Rebar** lays a
mesh into a wall and a reinforcement cage of steel bars into a beam, from
which pieces hang even after the glue cracks — the bars bend, pull out and
snap —, **Glass Fracture** breaks a pane of glass with radial cracks and
rings around the point of impact (it stays whole until it cracks, and the
renderer draws it transparent with reflections),
**Brick Wall** lays a wall of bricks in a bond with mortar, plaster and
openings (it breaks apart at the joints, some bricks split in half), **RBD
Constraints** turns the glue into geometry — a constraint network, one point
per piece and one line per joint — which can be weakened, deleted or drawn
in and wired back into the solver, and
**RBD Solver** on top of [Jolt Physics](https://github.com/jrouwe/JoltPhysics)
turns them into rigid bodies: convex hulls with mass; pieces glued where
they touch along a face are a single body until an impact stronger than the
glue (`glue` in kPa) breaks it; a piece hit harder than its cross-section
can withstand (`fracture`) breaks at runtime where the blow landed, and
whatever hit it carries on. Charges (`release`, `kick`, `vanish`) break the
glue at a given time, pieces with `crush` are crushed into dust under
falling floors, impacts shed debris and displaced air drives dust into the
streets. Debris are particles: they fly out from the edge of the face where
a joint cracked, the air slows and spins them, they hit pieces and
colliders, come to rest on a step or on a piece and ride along with it;
dust trails behind pieces that break off (`trail`), and RBD
Pieces outputs debris as points with `orient` for Copy to Points. Wired into
the Grain Solver, debris become grains that pile up, and they go to USD as a
PointInstancer with pebbles. The fall can be directed:
animation of the pieces (a keyed Transform around a Pivot) wired into the
**Guide** input of the RBD Solver leads the pieces where the shot wants them
and lets them go when the glue cracks, when time runs out, or when something
stops them further than `guide_reach`.
Keyed objects are kinematic colliders, pieces go as moving colliders into
water, gas and rain, dust into the Pyro Solver, and the RBD Pieces node
returns them as geometry with velocity `v`. Jolt runs on all threads
and deterministically: the same frames on every run and on any number of
threads, frames to cache. The demolition tower (593 pieces) steps in 3 ms
per frame, ten times more pieces in 31 ms (4 threads); Voronoi Fracture cuts
each cell only from nearby parts of the body, a tower of 5,628 cells in 1 s.
The **demolition** example: the demolition of a fourteen-storey high-rise in
a city block in golden light; the **wall_collapse** example: the facade of
a brick house blows out into the street and the pieces tumble towards the
camera just above the asphalt, in dust against the sun; the
**concrete_wall** example: a wrecking ball punches through a concrete wall
on a plinth (`rings` keeps the damage around the ball, the rest of the wall
stands) and chunks with rough fractures, fragments and dust pour out of the
hole; the **concrete_drop** example: a concrete beam breaks over a block and
its halves fall apart into chunks when they land; the **brick_wall** example:
a wrecking ball punches through the brick wall of a house next to a window,
the hole is stepped along the courses and the window stays intact; the
**concrete_column** example: a charge halfway up a reinforced concrete
column exposes the reinforcement cage and pieces of concrete hang on it; the
**constraint_network** example: a constraint network weakened along a line —
a ball breaks off a corner of the wall and the wall cracks exactly along it;
the **debris_stairs** example: an undercut concrete column topples down the
stairs and the debris stays lying on the steps; the **guided_fall** example:
a demolished chimney falls via Guide exactly into the street between two
houses and breaks apart on the road; the
**house_collapse** example: a family house built like a real one (walls of
blocks in a For-Each loop, ceilings, a roof with tiles, windows with glass,
gutters, a fence) collapses into the garden and a dust cloud creeps down a
street with trees and neighboring houses, photorealistically in Cycles ([destruction.md](docs/destruction.md#twelfth-example-the-collapse-of-a-family-house));
the **wood_beam** example: a steel ball punches through a wooden beam, which
splits into long splinters; the **shatter_blocks** example: a ball passes
through three intact concrete blocks and each breaks where it was struck.

**Animation**: every numeric parameter can have keyframes (Smooth, Linear,
Step) — a diamond next to the parameter, keys on the timeline, K in the
viewport, the gizmo writes keys. The network is evaluated frame by frame and
at every step the solvers pick up that frame's sources, forces and
colliders. Moving colliders pass their velocity and rotation to both gas and
water: a sphere in a pool makes a wave and a wake, a paddle swirls smoke, a
flying torch leaves a trail.

**Cache and export**: simulation frames go to disk and back (editor:
Simulation › Save/Load Cache, `prototype sim --cache` and `--from-cache`) —
they are played back, rendered and exported without recomputing; zeros
are not written, 150 frames of the campfire take 63 MB. **Bake to Disk**
computes the shot at full resolution in a separate process; the editor stays
free, plays frames from disk as they arrive and shows progress and an
estimate of the time remaining. Every 10 frames the whole simulation state is
saved (checkpoint), so an interrupted bake resumes where it left off,
bit-identical to an uninterrupted one. **Preview
Resolution** meanwhile computes gas and water at half resolution for tuning.
**Wedge** computes a parameter at several values, each into its own folder,
and **Profile** shows where the step time goes. The geometry of any node can
be exported frame by frame: points with attributes to PLY, volumes to
OpenVDB (a custom writer without the library, files verified by reading in
OpenVDB 10), polygons to OBJ. The whole shot goes to **USD** as a single
`.usda` scene (`--export
shot.usda`, in the editor File › Export USD Scene…): geometry, pieces as
moving bodies (shape once, then only position and rotation), debris, water
surface, rain, dust as VDB alongside, camera, sun and sky,
materials as MaterialX with photos alongside. Whatever is
large and different in every frame goes into a file per frame, written as
soon as the frame arrives, and the scene composes it (USD value clips) — a
shot of any length does not have to fit in memory. Verified with Pixar's
library, all validators without findings ([docs/usd.md](docs/usd.md)).

**Reading USD**: a shot from other departments comes into the network
without the USD library, from text `.usda`, binary `.usdc` (all versions from
0.4.0) and `.usdz` packages, composed as in USD: sublayers with time offset,
references and payloads, variants (the shot's selection overrides the
asset's default), classes, value clips. The **USD Camera** node gives Output
a matchmove camera, frame by frame, with the lens fitted to the film
back. **USD Import** brings in a set, models or caches as geometry
(normals, uv, colors, primvars, subsets as groups, PointInstancers as
instances, volumes from VDB) in meters with Y up, even when the file came
from Maya in centimeters with Z up. Subdivision surfaces
(`subdivisionScheme = catmullClark`) are smoothed as in OpenSubdiv, with
creased edges and corners from the file. It reads materials too: MaterialX
and UsdPreviewSurface networks bound as in USD, with images, roughness,
metalness, color and glass, including from Blender's export. Transforms,
composition, geometry and value clips match the USD library
([docs/usd-import.md](docs/usd-import.md)).

**Alembic**: the whole shot goes into a single `.abc` archive (`--export
shot.abc`, in the editor File › Export Alembic…): displayed geometry, pieces
as moving bodies, debris, grains, rebar, cloth, water surface, rain and
camera; gas as VDB alongside. The **Alembic Import** and **Alembic Camera**
nodes read geometry and cameras from Blender, Maya or Houdini. The Ogawa
container and the schemas are custom, without the library; Blender reads
the prototype's archives and the demolition pieces stand in them where they
stood in the simulation ([docs/alembic.md](docs/alembic.md)).

**Reading OpenVDB**: **VDB Gas** plays back smoke and fire from Houdini,
Blender or EmberGen as the shot's gas, one file per frame: Volume Look draws
and renders it like Pyro Solver gas. **VDB Import** outputs grids as volumes,
or level-set polygons as a collider or source shape. The reader handles zip,
Blosc, half floats, tiles, instances and rotated grids, and reads, voxel by
voxel, what OpenVDB 10 and 13 wrote ([docs/vdb.md](docs/vdb.md)).

**ACES**: the View of the Output node converts light to an image like
Blender's AgX, or like ACES 1.0 or ACES 2.0 as the OpenColorIO configs for
ACES show them; it differs from OpenColorIO 2.6 by at most a hundredth of a
step out of 255. The view can also come from a studio's, ACES or Blender
OpenColorIO config (`config.ocio`): prototype reads it itself, without the
library, with its displays, views, looks and LUTs, and the image matches
OpenColorIO to within 10⁻⁵. EXR is written in linear Rec. 709, ACEScg or
ACES2065-1 with the chromaticities attribute, and EXR in ACES is converted on
read ([docs/color.md](docs/color.md)).

**Plate**: the shot's image, a PNG, JPEG or EXR sequence, goes behind CG
when you look through the shot camera, in the editor and in the render. The
readers are custom, without libraries: JPEG bit for bit like libjpeg, PNG and
EXR with all compressions except DWA value for value like Pillow and
OpenEXR. Objects and the floor can be real things from the shot. **Holdout**
hides the CG behind it. **Shadow
catcher** additionally takes on the shadows of smoke and the light of fire.
Where CG changes nothing, the plate comes out of the render pixel for pixel
as it went in. CG with alpha and a `catcher` pass go to EXR for compositing
([docs/plate.md](docs/plate.md)).

**Images and video**: the shot goes to PNG, to a numbered sequence or to
video — `.avi` (Motion JPEG, a custom JPEG encoder and container) with no
dependencies at all, `.mp4`, `.mov`, `.mkv`, `.webm` and `.gif` via ffmpeg.
The editor renders frame by frame in the background with a progress window,
waits for the simulation and finally offers to open the file; the `sim` and
`render` commands draw without a window via EGL, and when EGL is not
available, via a hidden window. For compositing a frame goes to **EXR**
(a custom writer, RLE): the image in linear light over white, depth `Z`,
motion vectors `forward.u/v` (pieces by their velocity, everything by the
camera) and masks for the floor, geometry, pieces, objects, water and smoke
([docs/render.md](docs/render.md#4-exr-for-compositing)).

**Python**: the `pg` module builds a network, sets parameters, cooks
geometry and simulates from a script; attributes, topology, volumes, and the
particles and bodies of frames are numpy arrays over the core's memory,
without copying. A shot goes from a script to cache, to USD and to an image,
and `net.as_code()` prints the network as Python that builds it again —
that is how the scene from step 2 was built purely
from Python ([docs/python.md](docs/python.md)).

Deliberately missing (for now): materials and instances from USD, Alembic, VDB reading, JIT, packed primitives,
Python inside the network (Python SOP), cloth simulation — the order is in
[ROADMAP.md §4](ROADMAP.md#4-next-steps). Details in
[ARCHITECTURE.md §9](ARCHITECTURE.md#9-what-the-prototype-can-actually-do).

## License

The planned license is Apache 2.0 (see [ROADMAP.md §7](ROADMAP.md#7-name-license-clean-room)).
The implementation is clean room — no HDK, no reverse engineering.
