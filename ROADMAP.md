# Roadmap

> Document status: **v3** · Last updated: 2026-10-04
>
> A living document. Version 1 (2026-09-21) planned a headless geometry
> library, a GUI only in the third phase and simulations after version 1.0.
> The goal has changed: **a prototype of professional Houdini-like
> software** — the whole path from geometry through simulations to the
> image and export. Version 1 is in the git history
> (`git show a8058b2:ROADMAP.md`); its measured criteria still apply and the
> prototype verifies them ([§3](#3-what-is-done)). Version 3 spelled out what
> is missing for use in a VFX studio: step 3 is pipeline integration (USD,
> farm, EXR), step 4 destruction for production, step 5 scale.
>
> Related: [ARCHITECTURE.md](ARCHITECTURE.md) — data model, cook engine
> and what the prototype verifies of them.

---

## 1. Goal

Node-based procedural software for geometry and visual effects, modeled on
Houdini:

| Layer | What it means |
|---|---|
| Geometry | Nodes like SOPs: attributes on points, vertices, primitives and the whole geometry, groups, volumes; lazy cooking of only what has changed |
| Language | Wrangle: custom computation over every element, with variables, conditions, loops and geometry queries |
| Proceduralism | Expressions and references in parameters, subnets and digital assets, loops |
| Simulation | Smoke and fire, water (FLIP), rain and wind; rigid bodies and destruction; cloth, ropes and soft bodies (XPBD) |
| Image | Viewport, camera, shot rendering to images and video |
| Pipeline | Command line, disk cache, export (PLY, OBJ, OpenVDB, USD, Alembic), reading USD, Alembic and OpenVDB, Python API |

The inspiration from Houdini is conceptual. The implementation is **clean
room**: no HDK, no reverse engineering, no copying of documentation.

### What it is not (yet)

- **A production tool.** Simulations run on the CPU, and so does Cycles; the
  editor is built on Dear ImGui and much of what a studio needs is missing
  ([§5](#5-later)). The prototype is meant to show that the architecture
  holds together, and to measure what everything costs.
- **Full parity with Houdini.** The value is not in the number of nodes but
  in the fact that a few well-designed nodes over the right data model cover
  most tasks.
- **Compositing, audio, rigging and character animation** — different contexts.

---

## 2. Architectural invariants

These rules apply in every phase. Violating any of them is a reason
to reject a PR — not because they are sacred, but because they **cannot be retrofitted**.
Each of them is discussed in [ARCHITECTURE.md §2](ARCHITECTURE.md#2-invariants).

1. **Copy-on-write at the level of individual attribute arrays.**
   A node that changes `P` shares all other arrays via refcount. It never copies the whole geometry.
2. **Struct-of-arrays.** An attribute is a contiguous typed array. No `struct Point { ... }`.
3. **One geometry container for everything.** Polygons, curves, volumes, packed prims, instances.
   Separate geometry types = loss of compositionality.
4. **Lazy pull evaluation.** Data is computed only on demand, and only what is dirty.
5. **Determinism.** Same scene → bit-identical output, regardless of the number of threads and the platform.
6. **Headless-first.** Every feature must be usable without the GUI. The GUI is a client of the library, not the other way round.
7. **No global mutable data.** Cook must be callable from multiple threads and reentrant.
8. **Versioned node type.** Every node has a version and a migration path from the first commit.

---

## 3. What is done

| Area | Status | Documentation |
|---|---|---|
| Core | COW attributes, cook engine with versions and cache, time dependency, deterministic parallelism, per-element language (interpreter); vectors, matrices and quaternions from the GLM library | [ARCHITECTURE.md](ARCHITECTURE.md) |
| Shader graph | Nodes from text, four targets (GLSL, GLSL ES, Vulkan, HLSL), editor with preview | [docs/shader-graph.md](docs/shader-graph.md) |
| Simulation | Smoke and fire, water (FLIP), rain and wind; from nodes, deterministic on any number of threads | [docs/pyro.md](docs/pyro.md) |
| Destruction | Voronoi Fracture, rigid bodies on top of Jolt, glued pieces as a single body, charges, crushing, debris, dust driven by displaced air; a high-rise demolition as a video | [docs/destruction.md](docs/destruction.md) |
| Viewport editing | Points, edges and primitives selected with the mouse (click, box, lasso, brush; visible only, or hidden too); a handle moves, rotates and scales them (Edit with a soft radius), group and delete from the selection, attribute brush (pins, cloth tearing) — all as network nodes | [docs/editing.md](docs/editing.md) |
| Geometry in the editor | 31 SOP nodes (including PolyExtrude, Subdivide, Clip, Fuse, Dissolve, Connectivity, Attribute Transfer, Voronoi Fracture, Convert Volume, Liquid Surface), For-Each loops, display flag, attribute spreadsheet; cooking on its own thread with interruption; geometry as the shape of simulations and simulations back as geometry | [docs/geometry.md](docs/geometry.md) |
| Trees | Tree node: trunk with a fork, three levels of branches, seven crown shapes, leaves and needles, a forest on points, `flex` for wind, a skeleton for custom leaves; deterministic on any number of threads | [docs/trees.md](docs/trees.md) |
| Vegetation | Instances: points that stand in for prototypes (GPU instancing, USD PointInstancer, Unpack); Grass node (grass clumps), trees and shrubs as variants; Scatter with density, mask, slope and spacing; a meadow by a forest in the wind | [docs/vegetation.md](docs/vegetation.md) |
| Render | Blender's Cycles as a library (meshes and instances, Principled BSDF, glass and water, smoke and fire, physical sky as in Blender, AgX color transform, surface detail, depth of field, motion blur including gas and rotating objects, Open Image Denoise), the default in the Render tab, `--renderer cycles`; a custom CPU path tracer via Intel Embree 4 and NanoVDB with the same motion blur as the second option (`--renderer path`); AgX, ACES 1.0 and ACES 2.0 views as in OpenColorIO, plus views from OpenColorIO configs (`config.ocio`); PNG and EXR with passes in Rec. 709, ACEScg or ACES2065-1; both over the shot plate with holdouts and shadow catchers | [docs/cycles.md](docs/cycles.md), [docs/pathtracer.md](docs/pathtracer.md), [docs/color.md](docs/color.md), [docs/plate.md](docs/plate.md) |
| Proceduralism | Wrangle like VEX, expressions in parameters (`$F`, `ch()`), digital assets with a library and versions, `prototype cook` | [docs/wrangle.md](docs/wrangle.md), [docs/assets.md](docs/assets.md) |
| Animation | Keys on any parameter, moving colliders whose motion both gas and water pick up | [docs/animation.md](docs/animation.md) |
| Cache and export | Frames to disk and back; PLY, OBJ, OpenVDB; the whole shot to USD (geometry, moving bodies, debris and grains as pebbles, water surface, rain, dust and steam, camera, lights; whatever changes goes into a file per frame) and to Alembic; reading Alembic and OpenVDB (smoke from other programs played back as gas, a level set as a collider) | [docs/cache.md](docs/cache.md), [docs/usd.md](docs/usd.md), [docs/alembic.md](docs/alembic.md), [docs/vdb.md](docs/vdb.md) |
| Image | Shot camera, rendering to PNG, sequences and video | [docs/render.md](docs/render.md) |

### What measurement changed

- **M1 · COW:** 50 nodes over 2 M points allocate 50 arrays instead of 250
  (`bench/bench_main.cpp`, block 1). Verification on 20 M points remains.
- **M2 · Cook engine:** an edit in the middle of a 100-node chain costs 48 %
  of the cold cook time, a recook without changes 0.025 ms, tests clean under TSan.
- **M5 · Language — criterion revised 2026-09-21.** The original wording ("< 150 ms
  on 10 M points on 16 cores" and "50× over the interpreter") rested on an
  estimate that the interpreter would manage on the order of 1 Mpoint/s. In
  reality it is **38 Mpoints/s**, so even the interpreter would meet the
  absolute target and the multiple was unreachable. New
  wording: an arithmetic-bound snippet on 10 M points under 120 ms on 4 cores
  and at least 8× faster than the interpreter (block 3 in `bench/bench_main.cpp`).
  This is exactly why the prototype is built before the plan, not after it.

---

## 4. Next steps

Each step ends with a demo and a "done when" criterion. The next step starts
only when the previous one is done, documented and tested (ASan, UBSan, TSan, libc++).

### Step 1 — Procedural core in full ✅

- ✅ **Wrangle v2** ([docs/wrangle.md](docs/wrangle.md)): local
  variables, `if`/`else`, `for`, `while`, user-defined functions; running over
  points, primitives, vertices and the whole geometry; reading other elements
  and finding neighbors (`point()`, `nearpoints()`); creating and deleting
  points and primitives; integer attributes; arrays as query results; sliders
  from `ch()`.
- ✅ **Expressions in parameters** ([docs/animation.md §6](docs/animation.md#6-expressions)):
  `$F`, `$T`, maths and references `ch("../uzel/parametr")`, with tracking of
  dependencies and time; an fx button in the editor, `--set` on the command line.
- ✅ **Digital assets** ([docs/assets.md](docs/assets.md)): selected nodes
  as a single node (Make Asset) with the parameters it chooses (promote);
  a `.pgasset` library (shipped with the program, `$PROTOTYPE_ASSETS`, user
  folder), diving in and back out (I/U), every change is a new version that
  all instances follow; the network carries the definitions of its assets
  with it; nesting, cycles rejected. A subnet without a library does not exist
  yet — an asset stands in for it.
- ✅ **Loops and new nodes** ([docs/geometry.md](docs/geometry.md#for-each-loops)):
  For-Each Begin/End by pieces, primitives, points, by count and with
  feedback (the body cooked in its own graph, bit-identical on 1 and 4
  threads); PolyExtrude with inset, Subdivide (Catmull-Clark), Clip with cap
  on the cut, Fuse, Connectivity, Attribute Transfer. No Boolean yet — plane
  cuts (Clip) cover what Voronoi Fracture needs in step 2.
- ✅ **Background cooking:** displayed geometry is cooked on its own thread
  (`pg/sim/Cooker.h`), the editor does not wait for it; a new request
  interrupts the cook in progress (`CookContext::interrupt` — wrangles, loops
  and assets give up midway) and nothing interrupted is stored in the cache.

**Done when:** a procedural building from a few sliders (a digital asset)
works both in the editor and from the command line (`prototype cook`), and
the same values give bit-identical geometry on 1 and 4 threads. ✅ The
**Building** asset
([docs/assets.md §5](docs/assets.md#5-example-a-building-from-sliders)) has
ten sliders; the **street** example builds a street from it;
`prototype cook street - --hash --threads 1` and `--threads 4` print the same
hash, and a test guards it.

### Step 2 — Destruction ✅

- ✅ **Voronoi Fracture** ([docs/destruction.md §1](docs/destruction.md#1-voronoi-fracture)):
  cells of points (given, or random inside) as successive plane cuts
  with caps (Clip), pieces closed and together exactly the original body;
  `piece` on primitives and points, cut faces in the `inside` group; in
  parallel and bit-identical on 1 and 4 threads.
- ✅ **RBD Solver** ([docs/destruction.md §3](docs/destruction.md#3-rbd-solver))
  on top of Jolt Physics 5.6 (MIT, `CROSS_PLATFORM_DETERMINISTIC`, single thread):
  pieces as convex hulls with density; glued pieces are a single body
  (compound), which an impact stronger than `glue` (kPa times the joint area)
  breaks, and the force carries on to further joints; charges, crushing into
  dust, debris; keyed objects as kinematic colliders; a floor.
- ✅ **Fragments further on:** the solver in Output draws itself (`Cd`
  colors, a cut color); the RBD Pieces node returns them as geometry with
  `v`; the Collider output gives pieces as moving mesh colliders to water,
  gas and rain; the Dust output is a dust source that spreads with air
  displaced by the collapse; shadows of geometry and pieces; frames with
  piece positions and debris go to the cache (format 3).

**Done when:** a building collapses and raises dust — all as a video,
deterministically. ✅ The **demolition** example: the demolition of a
fourteen-storey high-rise in a city block — charges on the ground floor, the
tower slumps into its own footprint, the floors are crushed and a dust cloud
rolls through the streets; `prototype sim demolition
out.mp4` produces the video and the frames are bit-identical on every run
(tested on 1 and 4 threads and between two solvers).

### Step 3 — Studio integration (pipeline)

A studio would use the prototype as it uses Houdini: as an FX tool in the
middle of the pipeline, which receives models and the camera from other
departments and delivers simulations that the lighting department renders
(Karma, Arnold, RenderMan, Cycles) and compositing assembles. Without data
exchange with other programs, nobody would use it, however well it simulates.

- ✅ **USD — writing** (`.usda`, without the library; [docs/usd.md](docs/usd.md)):
  the whole scene —
  displayed geometry, pieces as moving bodies (shape once, then only
  position and rotation), debris as points, the water surface as a closed mesh
  with velocity and foam, rain, dust as volumes (VDB alongside),
  the camera with focal length per the USD convention, sun and sky; time
  samples only where something changes. Whatever is large and different in
  every frame goes into a file per frame (value clips), so a shot of any
  length does not have to fit in memory.
- ✅ **USD — reading** (without the library; [docs/usd-import.md](docs/usd-import.md)):
  `.usda`, `.usdc` (versions 0.4.0 to 0.10.0) and `.usdz`, the scene composed
  as in USD — sublayers, references and payloads with time offset, variants,
  classes, value clips; the USD Camera node gives Output a matchmove camera
  frame by frame, USD Import brings in a set and models as geometry in meters
  with Y up, including materials: MaterialX and UsdPreviewSurface networks
  bound as in USD, into `s@material` and `s@texture` (images, roughness,
  metalness, color, glass), PointInstancers as instances, volumes (Volume)
  from VDB; `prototype usd`, `pg.UsdStage`.
  Verified against the USD library; example
  [examples/sim/matchmove.pgsim](examples/sim/matchmove.pgsim).
- ✅ **Alembic** (without the library; [docs/alembic.md](docs/alembic.md)): the
  whole shot as a single `.abc` archive — displayed geometry, pieces as
  moving bodies (shape once, then Xform), debris, grains, rebar, cloth, water,
  rain, camera; gas as VDB alongside — and the Alembic Import and Alembic
  Camera nodes. Verified with Blender 4.5 (Alembic 1.8.3) in both directions;
  example [examples/sim/alembic_shot.pgsim](examples/sim/alembic_shot.pgsim).
- ✅ **OpenVDB — reading** (without the library; [docs/vdb.md](docs/vdb.md)): zip,
  Blosc (LZ4, zlib, BloscLZ), half, inactive values, tiles, instances,
  streams, rotated grids; VDB Gas plays back smoke and fire from other
  programs as the shot's gas, VDB Import outputs volumes or level-set polygons.
  Verified on files from OpenVDB 10 and 13; examples
  [examples/sim/vdb_fireball.pgsim](examples/sim/vdb_fireball.pgsim)
  and [examples/sim/vdb_rock.pgsim](examples/sim/vdb_rock.pgsim).
- ✅ **`v` and a stable `id`** on all particles (debris, water, rain): renderers
  use them to compute motion blur and instancing. They are carried by frames
  (cache format 4), the Liquid Points, Rain Points and RBD Pieces (`grit`)
  nodes, and debris, water and rain in USD.
- ✅ **Water surface as a mesh** (Liquid Surface, like Particle Fluid Surface
  in Houdini) and volume to polygons (Convert Volume): surface nets, a closed
  mesh with velocity from the solver grid (cache format 5) and foam
  ([docs/geometry.md](docs/geometry.md#water-surface-liquid-surface-and-convert-volume)).
- ✅ **Python API** (`import pg`, [docs/python.md](docs/python.md)): building
  networks, parameters, expressions and keys, cooking and simulating from a
  script; attributes, topology, volumes and frame data as numpy arrays without
  copying; cache, USD, rendering via `prototype`; `as_code()` writes a network
  as Python. The scene from step 2 built purely from Python:
  [examples/python/demolition.py](examples/python/demolition.py).
- ✅ **Farm**: a frame range (`--start`, `--end`) for rendering and for export
  from cache. A simulation interrupted midway can be finished from a checkpoint
  (`--checkpoint K`, `--resume`, in Python `save_state` / `load_state`)
  bit-identically. The editor has a background bake with progress, cancel
  and resume, and a preview at half resolution
  ([docs/cache.md](docs/cache.md#3-background-bake-checkpoints-and-preview)).
- ✅ **EXR**: preview render to linear EXR with depth, motion vectors
  and masks — for previs and compositing ([docs/render.md](docs/render.md#4-exr-for-compositing)).
- ✅ **Plate** ([docs/plate.md](docs/plate.md)): the shot's image (a PNG,
  JPEG or EXR sequence, read without libraries and verified against libjpeg,
  Pillow and OpenEXR) behind CG when you look through the shot camera; objects
  and the floor as a holdout or a shadow catcher that takes on the CG's
  shadows and the light of fire; CG with alpha and a `catcher` pass to EXR.
  Where CG changes nothing, the plate comes out of the render pixel for pixel
  as it went in. In the viewport, in Cycles (transparent film, its shadow
  catchers) and in the path tracer.

**Done when:** the scene from step 2 can be built and computed purely
from Python; the result opens in Blender and in usdview as USD (pieces,
debris, dust, camera, light) and the Cycles render matches our preview; a
simulation interrupted midway can be finished from cache bit-identically. The
last of these is **met**. A bake killed at frame 43 resumed from the
checkpoint at frame 40 and all 60 frames came out byte for byte identical to
an uninterrupted run. A bake cancelled in the editor at frame 50 and resumed
gave 150 identical frames. Tests verify this for gas, water, rain and the
demolition with dust.

### Step 4 — Destruction for production

- **Material-based fracturing:** ✅ concrete — the **Concrete Fracture** node
  ([docs/destruction.md §2](docs/destruction.md#2-concrete-fracture)):
  unequal pieces, smallest around the impact point, chipped corners as
  separate fragments, rough fracture surfaces (vector noise, the same on both
  sides of the crack, pieces still fit exactly) and beneath them a flat cut in
  the `proxy` attribute: the RBD Solver simulates the proxy and draws the
  detail. On top of that, `spread` and `rings` on the glue (how far an impact
  breaks — Houdini's *Propagate Rate* and *Iterations*), clusters glued to the
  foundation stand where they were built, and the **concrete_wall** example:
  a wrecking ball punches through a concrete wall on a plinth. ✅ Wood — the
  **Wood Fracture** node
  ([docs/destruction.md §2](docs/destruction.md#wood-wood-fracture)):
  Voronoi in a space compressed along the grain gives long splinters and
  slats, cross-grain fractures frayed into splinters, ridged along the grain;
  the grain direction travels with the pieces (`grain`), so they break along
  it at runtime too. The **wood_beam** example: a steel ball punches through a
  wooden beam.
- ✅ **Bricks:** the **Brick Wall** node ([docs/destruction.md §2](docs/destruction.md#bricks-brick-wall))
  lays a wall of bricks in a bond (stretcher, English, Flemish, stack), each
  brick with its mortar and plaster as one piece, with straight reveals at
  openings; mortar is the glue, so the wall cracks at the joints, and split
  bricks are two halves of one chunk. The **brick_wall** example: a wrecking
  ball punches through the brick wall of a house next to a window; the
  **concrete_column** example: the demolition of a reinforced concrete column
  that leaves a bare reinforcement cage.
- ✅ **Glass:** the **Glass Fracture** node ([docs/destruction.md §2](docs/destruction.md#glass-glass-fracture))
  breaks panes with radial cracks from the point of impact and arcs around it
  (a spider web: shards in the middle, long shards further out, branching);
  the pane stays whole until a joint cracks — then the whole web appears.
  Glass makes a tenth of the dust and glass debris. The renderer draws it
  transparent (two layers of faces, Fresnel on both sides of the pane,
  reflections of sky and sun, green shard edges); it goes to USD with a glass
  material and the cracks visible from the moment it breaks.
  The **glass_window** example: a ball flies through a window, in slow motion.
- ✅ **Rebar:** the **Rebar** node ([docs/destruction.md §2](docs/destruction.md#reinforcement-rebar))
  lays a mesh into a wall and a reinforcement cage with stirrups into a beam,
  oriented the way the block lies; the RBD Solver threads the bars (including
  drawn, bent lines with `width`) through the pieces and joins pieces that the
  glue no longer holds with plastic constraints: a bar holds what the steel or
  its anchorage in concrete can bear (`rebar_strength`, `bond`), then yields
  and stays bent, pulls out of short ends and snaps when stretched by
  `stretch`. It is drawn as steel tubes bent between pieces with stubs of
  snapped bars, and goes to USD as curves with width. Both concrete examples
  have rebar: the beam bends over the block and hangs on it, pieces hang from
  the wall around the hole.
- ✅ **Constraint network as geometry:** the **RBD Constraints** node
  ([docs/destruction.md §3](docs/destruction.md#constraint-network-rbd-constraints))
  turns the glue into one point per body and one line per joint with `strength`
  (a multiple of Glue), `area` and a color by strength. The network, edited
  with ordinary nodes — weakened, deleted, drawn in between pieces that do not
  touch — goes into Constraints of the RBD Solver and acts as the glue. RBD
  Pieces returns the frame's network with `broken` and `time` (state of the
  joints in the frame and the cache). The **constraint_network** example: the
  wall cracks along the line the shot wants. Remaining: rebar and joints
  (*Hard*, *Cone Twist*, soft constraints) as network primitives.
- ✅ **Secondary fracturing:** a piece falls apart only on impact — the **RBD
  Cluster** node ([docs/destruction.md §2](docs/destruction.md#chunks-and-secondary-fracturing-rbd-cluster))
  groups fine pieces into chunks with stronger glue inside (`cluster`,
  `clusterglue`, k-means++ and Lloyd over the piece centroids): an object breaks
  into chunks and a chunk shatters only when it lands hard. The
  **concrete_drop** example: a beam breaks over a block and its halves fall
  apart into chunks when they land.
- ✅ **Runtime fracturing:** ([docs/destruction.md §3](docs/destruction.md#breaking-during-the-simulation))
  a piece hit harder than its cross-section can withstand (`fracture`
  of the RBD Solver, `f@fracture` of the piece) breaks where the blow landed:
  into fragments, smallest around the blow, with rough fractures (wood along
  the grain, with splinters), which fly on as the piece was flying, with dust
  and debris. Whatever hit it carries on, slowed only by what the piece could
  bear. Fragments keep breaking down to a given depth and size. A fracture is
  an event (body, location, seed, count, time): frames and the cache
  (format 16) carry only those, and the fragments are rebuilt from them bit for
  bit; USD has the fragments as bodies visible from the moment of fracture.
  The **shatter_blocks** example (a ball passes through three intact concrete
  blocks) and **wood_beam**.
- ✅ **Fragments as particles:** debris ([docs/destruction.md §3](docs/destruction.md#debris-as-particles))
  flies out from the edge of the face where a joint cracked, in its plane. Air
  slows it (small bits more) and spins it, it hits pieces, colliders and the
  floor, bounces and comes to rest on stairs, on ledges and on pieces, and
  rides along with the piece it lies on until that piece starts moving, tilts
  or disappears. Dust trails behind pieces that break off (`trail`). The
  orientation of each bit (`orient`) goes into frames, the cache (version 9),
  RBD Pieces, Python and USD, and Copy to Points orients pebbles by it. The
  **debris_stairs** example: an undercut column topples down the stairs and
  the debris stays on the steps. ✅ USD carries debris as a
  `PointInstancer` with pebbles of the same shapes the renderers draw
  ([docs/usd.md](docs/usd.md)), and debris wired into the Grit input of the
  Grain Solver becomes grains that collide with each other and pile up (the
  **shatter_grit** example, [docs/grains.md](docs/grains.md#concrete-grit-as-grains)).
- ✅ **Guided simulation:** Guide of the RBD Solver ([docs/destruction.md §3](docs/destruction.md#guided-simulation-guide))
  is an animation of the pieces, the same points moved and rotated (a keyed
  Transform around a Pivot, a wrangle driven by `@Time`). At every step the
  solver leads each glued body into the pose that best places its points onto
  the Guide points: exactly with `guide_strength` 1, lagging behind with less.
  It cancels gravity; pieces still collide. It lets them go after
  `guide_until`, when the glue cracks (`guide_let_go`), or when something stops
  them further than `guide_reach`. The `guide` attribute says how strongly
  Guide leads each piece. Only the pose of each piece is taken from Guide in
  every frame (a few bytes per piece). The **guided_fall** example: a
  demolished chimney falls by the keys exactly into the street between two
  houses and breaks apart freely on the road. Remaining: guiding by force or
  a spring constraint instead of velocity, and a Guide that deforms the pieces.
- ✅ **Rigid bodies on multiple threads**, deterministically ([docs/destruction.md §3](docs/destruction.md#how-it-works)):
  Jolt on its own thread pool, impacts collected from its threads and sorted
  by substep, body and body part, debris on threads, a ray with a fixed
  order for equally close faces; frames on 1 and 4 threads bit for bit
  identical (tests, `prototype sim --threads`, `pgbench_rigid`). Voronoi
  Fracture cuts a cell only from nearby parts of the body using nearby points,
  bit for bit identically: a tower of 5,628 cells in 1.0 s instead of 13.5 s.
  The glue finds faces by sweeping instead of all against all.
- ✅ **Bodies at rest freeze** (Freeze at Rest, [docs/destruction.md §3](docs/destruction.md#how-it-works)):
  a body that has not moved for half a second and lies on something that does
  not move is static in Jolt and costs nothing. It is woken by an impact with
  enough momentum, by a body that reaches it within a substep (swept boxes in
  Jolt's broad phase), by water, gas, a charge or a keyed object; together
  with it, whatever lies on it. The settled tower of 5,628 pieces steps in
  1.9 ms per frame instead of 27 ms (before cheaper contacts 2.1 instead of
  56 ms), and frames remain bit-identical on 1 and 4 threads. The `rest`
  parameter (on by default).
- ✅ **Cheaper contacts** ([docs/destruction.md §3](docs/destruction.md#how-it-works)):
  Jolt looks for speculative contacts 5 mm ahead instead of 2 cm and keeps the
  contacts of a pair that moved by less than 5 mm and 5° (instead of 1 mm
  and 2°). The large tower falls 30 % faster; the pile and all the examples
  look the same. Along the way, cloth that tore the stretched edge of a hole
  again at every substep was fixed: the tarp is 23 % faster, bit-identical.

**Done when:** the demolition from step 2 has concrete, glass and rebar, dust
trails and secondary fracturing, and ten times more pieces in the same time
per frame. Concrete, rebar, glass, secondary fracturing, dust trails and
threads are in. The speed target is not met. The tower from the example (593
pieces) steps during the fall in 2.1 ms per frame on 4 threads (3.2 ms on
one), ten times more pieces (5,628) in 19.5 ms (44 ms on one), i.e. nine
times longer. Debris that has settled costs almost nothing any more, because
it freezes: the settled large tower 1.9 ms per frame, 360 frames on average
27 ms instead of 61 ms before freezing and cheaper contacts. During the fall
the step is almost entirely in Jolt, in collisions between pairs of convex
hulls, and grows with the number of bodies that are moving. The tower pieces
are not small (0.4–0.75 m, on average 11 points in the hull), so there is
nothing to simplify. Ten times as many in the same time needs a GPU solver
(step 5).

### Step 5 — Scale

- ✅ **Sparse grid for smoke and fire** ([docs/pyro.md §4](docs/pyro.md#sparse-grid-compute-only-where-there-is-gas)):
  fields in tiles of 8 × 8 × 8 cells, only where there is gas and around it,
  as far as it can travel in a step; pressure via multigrid only on those,
  p = 0 outside; frames and the cache (version 10) only with tiles that
  contain gas. With all tiles active it computes bit-identically to the dense
  grid. Cells occupied by pieces are searched only within their bounding
  boxes (previously 40 % of dust time). Resolution up to 1024.
- ✅ **Sparse water** ([docs/pyro.md §5](docs/pyro.md#a-large-run)): water
  grids in tiles of 8 × 8 × 8 only around particles, free-surface pressure on
  those; frames and the cache (versions 13 and 14) only with tiles near the
  surface, tiles deep in the water just as a number; surface nets only around
  them. With all tiles active it is bit-identical to dense water. Resolution
  up to 1024. The `flood_crates_hd` example: 512 × 128 × 256 cells and 17.6
  million particles, 30 s to 2.5 minutes per frame on 4 cores, 2.2 to 5.7 GB
  of memory.
- ✅ **Upres** ([docs/pyro.md §4](docs/pyro.md#upres-coarse-simulation-fine-image)):
  the Pyro Upres node carries the solver's gas on a sparse grid two to four
  times finer, sources and combustion at the fine resolution, vortices from
  curl noise advected with the flow according to the vorticity of the coarse
  simulation. The `campfire_upres` example: solver at 64 with upres ×3 in
  177 ms and 167 MB per frame versus 358 ms and 334 MB for the solver at 192.
- ✅ **Viewport for large caches** ([docs/cache.md](docs/cache.md#large-caches-in-the-viewport)):
  frames in memory within a budget (Cache Size), beyond it spilled to disk and
  read back; the cache from disk read ahead of the playhead on its own
  thread, the timeline never waits; memory and disk bands on the timeline;
  proxy grids for gas and water during playback (at most ~4 million cells),
  full ones after stopping. `flood_crates_hd` (24 MB per frame) plays back from
  disk at ~15 frames/s; 150 frames of the flood with a 64 MB cache play through
  completely.
- **GPU** for the solvers.
- **Packed primitives, instances and out-of-core** — millions of pieces and
  data larger than memory.

**Done when:** the demolition dust has 100 million voxels and computes on a
single machine overnight. **Met:** the dust of the `demolition` example with
`--resolution 576` has a domain of 576 × 312 × 576 = **103.5 million voxels**
(16 cm cells) and 180 frames compute in **19 minutes** on 4 cores (6.2 s
per frame including rigid bodies, `pgbench_pyro 576 --frames 180`), in at
most 3.4 GB of memory, 5 GB with rendering. At most 23 % of the domain is
computed (24 million voxels, at the densest moment); the rest is still air.
A dense grid would need over 10 GB for the fields alone.

![Demolition dust at 103.5 million voxels: frames 60, 90, 120 and 150](docs/img/demolition-576.jpg)

---

## 5. Later

Ordered by value / cost ratio:

1. **XPBD solver** — ✅ cloth, ropes and soft bodies with pressure (similar to
   Vellum): pinned points carried by animation, collisions with objects, RBD
   pieces and itself, air, wind and gas flow, deterministic on any number of
   threads, tearing (points split, ropes come apart, balloons
   burst) and two-way coupling with RBD pieces ([cloth.md](docs/cloth.md)).
   ✅ Primitives and edges collide with objects and with each other (a tree of
   faces with normal cones, cloth hangs over a bar thinner than the point
   spacing), soft bodies hold their shape (shape matching) and with plasticity
   stay dented (the **soft_bodies** example). Remaining: continuous collision
   detection (CCD) with time of impact, clustered shape matching. ✅ Granular
   materials: Grain Solver, sand and gravel with friction and cohesion,
   pouring, two-way coupling with RBD pieces, RBD Solver debris as grains
   ([grains.md](docs/grains.md)).
2. **Coupling between solvers** — ✅ debris in water and in gas: water buoys
   it up and carries it, gas flow carries debris, two-way with water and gas,
   which flow around the pieces ([destruction.md](docs/destruction.md#eleventh-example-a-flood-in-a-courtyard)).
   ✅ Water and fire: water and raindrops put out fire, cool the gas, soak
   the fuel and the sources and make steam; rain fills the water it falls
   into; FLIP volume correction ([quench.md](docs/quench.md)). ✅ Steam is its
   own gas field (white, rises, thins out; in all three renderers, in the
   cache, VDB and USD) and flames evaporate water and drops (the **fire_hose**
   example). Remaining: steam condensation, quenching of the fine upres fields.
3. **Rendering for the final image** — ✅ Blender's Cycles as a library
   ([cycles.md](docs/cycles.md)) and a custom CPU path tracer
   ([pathtracer.md](docs/pathtracer.md)): surfaces, glass and water, depth
   of field, AOVs to EXR; smoke, fire and dust with multiple scattering,
   smoke shadows and light from flames; neural-network denoising with Intel
   Open Image Denoise; materials by what the surface is made of
   (`s@material`: concrete, plaster, brick, window, steel, paving, roof
   tiles, lawn…), surface photographs from a library (fifteen sets) and custom
   textures projected from three sides or along the face by the piece's
   position before it moved, or by UV (UV Project node, imports) with normal
   maps ([materials.md](docs/materials.md)); debris as angular fragments of
   stone and glass, rain as streaks of water and wet surfaces where it rains
   ([pathtracer.md](docs/pathtracer.md#grit-rain-and-wet-surfaces)); in both
   renderers motion blur by point `v`, gas velocity, motion and rotation of
   objects, and camera motion
   ([cycles.md](docs/cycles.md#motion-blur)); CG over the shot plate,
   holdouts and shadow catchers in both renderers, to EXR with alpha
   and a `catcher` pass ([plate.md](docs/plate.md#in-the-final-render-cycles-and-the-path-tracer)).
   ✅ ACES: ACES 1.0 and 2.0 views as in the OpenColorIO configs
   (at most a hundredth of a step out of 255 from OpenColorIO 2.6), EXR in
   ACEScg and ACES2065-1 with chromaticities, and reading them ([color.md](docs/color.md)).
   ✅ OpenColorIO configs without the library: displays, views, looks,
   view transforms, `.spi1d`, `.spi3d`, `.spimtx` and `.cube` LUTs,
   built-in ACES outputs for SDR; the ACES and Blender configs match
   OpenColorIO 2.6, the plate round-trips exactly
   ([color.md](docs/color.md#3-opencolorio-configs)).
   ✅ Motion blur of gas (velocity in frames, from VDB and to VDB) and of scene
   objects in Cycles and in the path tracer, which blurs everything else too.
   ✅ UV and normal maps: UV Project node, `vt` from OBJ, photos by UV
   and normal maps (OpenGL and DirectX) in both renderers, tangents as in
   MikkTSpace ([materials.md](docs/materials.md#by-uv-and-normal-map)).
   Remaining: Cycles on the GPU, HDR displays and OCIO grading transforms. Until then, studios render demanding shots via USD
   with their own renderers.
4. **JIT for wrangle** (LLVM ORC or Warp) — once the interpreter becomes the
   bottleneck (criterion M5 above).
5. **Data exchange** — ✅ Alembic (writing and reading, [alembic.md](docs/alembic.md))
   and reading OpenVDB ([vdb.md](docs/vdb.md)), gas velocity (`vel`) in
   frames, VDB and USD, writing VDB with Blosc or zip compression, materials
   as MaterialX in USD and in `.mtlx` (height as displacement) and reading
   `.mtlx` as texture sets
   ([materialx.md](docs/materialx.md)).
6. **Build per the VFX Reference Platform** — Rocky Linux and libraries in the
   versions studio pipelines expect.
7. **Geometry editing in the viewport** — ✅ points, edges and primitives
   selected with the mouse, visible only (a bounding-volume tree over the
   polygons); a handle on the selection drives the Edit node (drags compose
   exactly, soft radius), Ctrl+G group, Delete Blast, a brush paints an
   attribute with dabs as locations (Attribute Paint); markers in the renderer
   with a depth test ([editing.md](docs/editing.md)); Tab inserts any node on
   the selection (PolyExtrude with a handle on Distance, wrangle), N shows
   element numbers; box, lasso and brush selection (S), H also selects hidden
   elements and shows them translucent; handles for geometry nodes (Transform
   and Edit at the pivot, Clip); soft selection (O) with a preview of the
   motion falloff on the geometry, radius via keys and the wheel during a
   drag, distance straight or along the surface, five falloff shapes;
   sculpt (U, Sculpt node): Push / Pull, Smooth with borders that hold their
   line, Grab, Flatten, dabs as locations, incremental stroke computation
   bit-identical to computing from scratch (mouse move on a million points
   12 ms), the selection tree only recomputes its bounds (refit); the viewport
   draws displayed geometry indexed and, when points move, uploads only vertex
   positions and normals (a million points 62 ms instead of seconds, 24 MB on
   the GPU instead of 215 MB).
   ✅ Dissolve: edges and primitives selected in the viewport merge polygons
   (Ctrl+X), collinear points on the side disappear.
   ✅ Symmetry (M): Edit also moves the mirror images (points on the plane
   stay on it), Sculpt and Attribute Paint write a dab's mirror image along
   with it, applied at the same time and attenuated where they overlap.
   ✅ Dyntopo in Sculpt (Ctrl+D): the mesh is refined under every dab (the
   longest edges halved, with a smooth transition to the coarser mesh) and
   short edges are collapsed to a point without breaking the surface, the
   border or the corners; attributes and groups travel with the points,
   detail by brush radius or in meters, incrementally bit-identical to
   computing from scratch.
   ✅ Vertex mode (5): primitive vertices as dots slightly inside the
   polygons, selection by click, box, lasso and brush, conversion between
   modes; patterns like `5v2` as in Houdini; Group, Edit and Blast with the
   Vertices class (Blast removes vertices from polygons).
8. **Vegetation** — ✅ Tree node: a tree grows like a plant following the
   Weber and Penn model — a trunk (also split into leader branches), up to
   three levels of branches around the parent at the golden angle, bent by
   weight, turned towards the light and wandering, seven crown shapes,
   leaves, narrow leaves and needles; one tree on each input point (a forest,
   each one different by `id`); a mesh with a `flex` attribute for wind via a
   wrangle, or a skeleton with `orient` for custom leaves via
   Copy to Points ([trees.md](docs/trees.md)). A forest of 34 trees (3.5 million
   points) in 0.9 s; in the wind the viewport uploads only positions.
   ✅ Instances: a point stands in for a geometry prototype (`instance`,
   `orient`, `pscale`, `tint`) — Merge, Transform and Unpack handle them, the
   viewport draws them via GPU instancing, USD gets a PointInstancer (also per
   frame), OBJ and PLY get copies; the Grass node grows clumps of grass and
   scatters them over terrain as instances; Tree with an Instances output
   (variants) for forests and shrubs; Scatter with density per m², a mask from
   an attribute, slope and spacing; Copy to Points with instances and pieces by
   attribute. The meadow example: 122,577 clumps (1.9 million blades), 84 trees
   and 65 shrubs in 148 ms, a frame in the wind 31–39 ms ([vegetation.md](docs/vegetation.md)).
   ✅ UVs on bark, leaves and blades; bark with a normal map by UV,
   leaves from a library image with an alpha cutout and grass from a blade
   image in Cycles and the path tracer (the **foliage** example, [trees.md](docs/trees.md)).
   ✅ Translucency of leaves and blades against the sun, in the viewport too.
   ✅ Levels of detail in the viewport: distant trees and grass with fewer
   but larger leaves and blades (a meadow from 50 m: 44 % of the triangles).
   ✅ The viewport lays photos and leaf images as the renderers do (UV, three
   sides, normal maps, alpha cutout), distant plants as billboards,
   and levels of detail blend.
   ✅ Plant Wind node: bending from the base by `flex` without stretching,
   gusts, leaf flutter, `v` for motion blur; instances pre-bent into several
   shapes.
   ✅ Pruning by an envelope, and roots (Tree), trampling (Plant Trample),
   an ecosystem of three species over years (Ecosystem, the **ecosystem**
   example).
   ✅ Wind as springs (Plant Wind, Dynamics): each stem a damped
   oscillator solved exactly in steps of 1/120 s, branches carried and
   whipped by the stem they grow from, states stored between frames (the same
   result in any frame order and thread count); a forest 0.29 s per frame.
   Along the way, trees from two Tree nodes after a Merge that bent around
   another tree's base were fixed.
   ✅ Ecosystem in 3D (Light By Height): crowns as ellipsoids of foliage,
   overcast sky light attenuated by the crowns above a plant (Beer–Lambert),
   growth and decline by light, seedlings by light near the ground;
   a fourth species, a shrub, as understorey (the **ecosystem** example: hazels
   under the trees with light 0.31, spruce seedlings waiting in the shade).
   ✅ Branches avoid obstacles (Tree, Obstacles input): Clearance
   distance, turning along the surface, otherwise the end of the stem; a tree
   under a roof slips out and grows above it (the **tree_obstacles** example).
   Remaining: branches of neighboring trees avoiding each other, herbaceous
   plants as another layer of the ecosystem.

---

## 6. Risks

| # | Risk | Impact | Mitigation |
|---|---|---|---|
| R1 | ~~"Houdini" trademark~~ | — | **Resolved 2026-09-27:** the project is called Prototype |
| R2 | Scope outgrows capacity | Nothing gets finished | Each step ends with a demo and a "done when" criterion; the next only after it |
| R3 | Non-determinism discovered late | Cache and render diverge, tests fail | Bit-identical results on 1 and 4 threads are in the tests of every solver |
| R4 | Dense grid performance | Scenes stay small | A small `Grid` interface, replaced by sparse grids (step 5) |
| R5 | Dependencies (Jolt, Python) | Build on FreeBSD, offline | Every dependency optional, the core stays pure C++20; the build is also verified with libc++ |
| R6 | The project stays a "one man show" | Death by burnout | Documentation and examples for every step, tests as the specification |

---

## 7. Name, license, clean room

- [x] **Name: Prototype** (2026-09-27). The working name was confusingly
      similar to a registered SideFX trademark. The namespace in the code
      remains the neutral `pg`; the file formats do not change.
- [ ] License **Apache 2.0** (patent grant, ASWF standard). Rule out GPL —
      studios must be allowed to write proprietary nodes.
- [ ] `CONTRIBUTING.md`, `CODE_OF_CONDUCT.md`, DCO.
- [ ] Put the clean-room policy for contributors in writing.
