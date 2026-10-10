# Architecture

> Status: **design v1 + validation prototype** · Last updated: 2026-09-21
>
> This document describes the target architecture. Part of it is already built and measured
> (see [§9](#9-what-the-prototype-can-actually-do)); the rest is design. Each chapter
> states what holds today and what is planned.

Related documents: [ROADMAP.md](ROADMAP.md) — the goal, what is done, and next steps.

---

## 1. Shape of the system

```
┌─────────────────────────────────────────────────────────────┐
│  Editor (Dear ImGui)  nodes · viewport · parameters · table │   done
├─────────────────────────────────────────────────────────────┤
│  CLI / headless cook               Python API (pybind11)    │   done
├─────────────────────────────────────────────────────────────┤
│  Nodes: geometry · simulation · I/O · digital assets        │   done · steps 1–2
├─────────────────────────────────────────────────────────────┤
│  Language: parser → interpreter (later JIT → SIMD kernels)  │   step 1 · §5
├─────────────────────────────────────────────────────────────┤
│  CORE    cook engine · geometry · attributes · parallelism  │   done
├─────────────────────────────────────────────────────────────┤
│  External: Jolt, GLM, Embree, NanoVDB, OIDN                 │   done
│            USD · OpenSubdiv                                 │   steps 2–3 · §5
└─────────────────────────────────────────────────────────────┘
```

The key property of this layering: **the GUI is a client of the library, not the other way round.**
The steps on the right refer to [ROADMAP.md §4](ROADMAP.md#4-next-steps).
Every capability must be reachable through a library call before a button is
made for it. A render farm has no viewport, and tests have no mouse.

---

## 2. Invariants

Eight rules that hold across the whole system. They are not preferences —
they are the set of things that **cannot be retrofitted**, because they permeate
every data structure and every node.

| # | Invariant | What happens if it is violated |
|---|---|---|
| **I1** | Copy-on-write per attribute array | Memory grows as O(nodes × attributes). Long chains become unusable. |
| **I2** | Struct-of-arrays | Loss of vectorization, 3–10× slower per-element operations. |
| **I3** | One geometry container for everything | Loss of composability — nodes can no longer be connected arbitrarily. |
| **I4** | Lazy pull evaluation, dirty only | Every parameter change recooks the whole graph. The end of interactivity. |
| **I5** | Determinism independent of thread count | Frames differ between farm machines. Discovered months after deployment. |
| **I6** | Headless-first | Nothing can run on the farm or be tested in CI. |
| **I7** | No global mutable state | Cook cannot be called in parallel or reentrantly. |
| **I8** | Versioned node type + migrations | The first breaking change invalidates all existing scenes. |

Violating any of them is grounds for rejecting a PR.

---

## 3. Data model

### 3.1 Attribute array

An attribute is **one contiguous typed array** (`AttributeArray`). Not an array of structs.

```cpp
class AttributeArray {
    std::shared_ptr<Buffer>      buffer_;   // element data
    std::shared_ptr<StringTable> strings_;  // String only, also COW
    AttrType type_;
    size_t   count_;
};
```

All of the COW happens in one place:

```cpp
std::byte* AttributeArray::rawWrite() {
    if (!buffer_)                       buffer_ = make(byteSize());
    else if (buffer_.use_count() > 1)   buffer_ = make(*buffer_);  // clone
    return buffer_->bytes.data();
}
```

Reading (`rawRead`) never clones or allocates. Consequence: a node that changes
only `P` **shares all other arrays** with the previous node's output.

**Element types:** `int32`, `float32`, `vec2/3/4`, `string`.
String attributes store an `int32` index into a shared table — elements stay
fixed-width, so the SoA layout and all bulk operations (`gather`, `append`,
`resize`, hash) work unchanged.

> **Plan:** array attributes (variable-length per-element arrays) and `mat3/mat4`.
> Design: a separate offsets array + one flat data array, so that elements
> stay fixed-width.

### 3.2 Four attribute classes

`detail` (1 value for the whole geometry) · `point` · `vertex` (corner of a primitive) ·
`primitive`

The point vs. vertex split is what allows UV seams and hard edges
in normals without having to duplicate points.

### 3.3 Geometry

```cpp
class Geometry {
    AttributeSet detail_, points_, vertices_, primitives_;
    std::map<std::string, Group> groups_;   // masks, also COW
    std::shared_ptr<Topology> topo_;        // also COW
};
```

The topology is flat and index-based:

```
primitive p  ──▶ primStart[p], primCount[p]  ──▶  vertexPoint[v]  ──▶  point
```

**Copying a `Geometry` copies not a single data element.** Attributes, topology
and groups are all `shared_ptr` and are cloned only on write. That is why every node
starts its cook like this:

```cpp
auto geo = editableCopy(input);   // O(number of attributes), not O(number of elements)
```

`GeometryPtr` is `shared_ptr<const Geometry>` — whatever comes out of a node is immutable.

> **Plan:** packed primitives (a primitive as a reference to geometry + a transform)
> and delayed load. Without them a scene with 10,000 instances cannot be built. They belong to M14,
> and the data model accounts for them — that is why topology is separate from attributes.

### 3.4 Geometry hash

`Geometry::hash()` walks the attributes **in sorted name order** (hence
`std::map`, not `unordered_map`), then the topology and groups. It is the foundation of the golden
regression suite: two results with the same hash are bitwise identical in content.

For string attributes, **the strings themselves** are hashed, not the indices — so the hash does not depend
on the order in which the table happened to be filled.

> **Known shortcoming:** byte-wise FNV-1a is slow (~1 GB/s) and serial.
> Production needs xxHash3 or similar, in parallel over chunks with tree-style
> combining.

---

## 4. Cook engine

### 4.1 Versions instead of a dirty flag

Dirtiness is tracked not by a boolean but by a **version counter**:

```
parameter change on node N
    ├─▶ N.version++
    └─▶ for every downstream node (BFS, with a seen set): version++
```

The cache key is `(node, version, frame)`. A stale entry is simply never found
again. **No invalidation pass is needed** — so there is no way to
forget to write one. That is the main reason for this design.

Setting a parameter to the value it already has does not bump the version.

### 4.2 Pull evaluation

```cpp
GeometryPtr cookRecursive(Node& n, const CookContext& ctx, const TimeDepMap& td) {
    key = { &n, n.version(), timeDependent(n) ? ctx.frame : kAnyFrame };
    if (auto hit = cache_.find(key)) return hit;       // ← nothing is computed

    inputs = [cookRecursive recursively for each input]
    out = n.cookNode(ctx, inputs);
    cache_.insert(key, out);
    return out;
}
```

Nodes whose version has not changed **are not called at all**.

### 4.3 Time as a dependency dimension

A node is time-dependent if it is time-dependent itself **or** anything upstream of it is.
Time-independent nodes have one cache entry for all frames; time-dependent
nodes have one entry per frame.

The time-dependency map is computed **once, up front, serially** — computing it
lazily inside the recursion would break as soon as branches start cooking in parallel.

The `pointwrangle` node reports time dependency according to whether its snippet
reads `@Time` or `@Frame` — derived from the parsed program, not from a checkbox
the user will forget.

### 4.4 Cache

LRU with a hard memory cap (default 2 GB). An entry larger than the entire budget
is returned anyway (otherwise a large result could never be delivered). When the cache is full,
results needed by only one frame (time-dependent) are evicted first,
oldest first; time-independent ones only after them. Otherwise an animated node below heavy
geometry (a million points of fractured concrete) would evict that geometry on every frame,
and every frame would cook it again — eighty times slower.

> **Known shortcoming:** an entry is charged `Geometry::memoryUsage()`, which also counts
> buffers shared with another cache entry. It is therefore an **upper bound**. Exact
> accounting requires a buffer registry with deduplication by identity — this belongs
> to M14 (out-of-core), where it will start to matter.

### 4.5 Branch parallelism

Independent inputs of a multi-input node are cooked concurrently. The cache is protected
by a mutex.

> **Known shortcoming:** a node reachable through two branches may be cooked
> twice (same result, wasted work). Removing this requires an
> "in progress" registry with waiting — simple, but not done yet.
> The test that counts exact cook counts on a diamond graph therefore
> turns branch parallelism off.

### 4.6 Interruption

`CookContext::interrupt` is a flag set by whoever requested the cook: "I no
longer want it". The engine reads it before and after every node. When it is set,
the engine stops cooking, returns null and **stores nothing created after the flag was set in the
cache** — an interrupted result may be half-finished. Long-running nodes
check the flag themselves: a wrangle every 1024 interpreter
steps, a For-Each loop between iterations, and an asset passes the flag to its
inner graph. This lets the editor cook the displayed geometry on its own
thread (`pg/sim/Cooker.h`), with each new request (a slider drag)
interrupting the one in progress.

---

## 5. Per-element language

The pipeline we are aiming for:

```
source → lexer → parser → type checking and binding → custom IR → LLVM ORC JIT → SIMD kernel
                                                                      ↑
                                                the JIT plugs in here (ROADMAP §5)
```

**Today**, in place of the last two steps there is a typed-tree interpreter
(`src/pg/lang`; the language is described in [docs/wrangle.md](docs/wrangle.md)). The seam
where the JIT will replace the interpreter is `lang::Program::run()`:

1. **Parse once** (`Parse.cpp`) — the whole program: variables, `if`, `for`,
   `foreach`, `while`, user functions, arrays, strings. The parser already knows the names
   and argument counts of the built-in functions, so a typo in a name is an error
   with line and column right while typing.
2. **Type checking and binding on every run** (`Check.cpp`) — attribute types
   come from the geometry the program runs over, and that can
   change between cooks. The check turns them into a typed tree: every `@name` is bound
   to a pointer into an attribute array, every call to a specific overload,
   and every conversion (int → float, float → vector) is an explicit node. At run time
   nothing is looked up by name and nothing is decided by type.
   The parameters `ch("x")` and `$F` are the same for the whole run, so they are
   substituted here as constants.
3. **Run** (`Eval.cpp`, `Run.cpp`) — evaluation is templated on the result
   type (`ev<float>`, `ev<Vec3>` …), and local variables are slots
   in per-type arrays. A program that touches only its own element runs in parallel over
   deterministic chunks. A program that creates or deletes
   geometry, or writes other elements or strings, runs **in order**: changes are
   queued and applied after the run, in the order the program
   requested them. The result therefore never depends on the thread count — test
   `lang_results_do_not_depend_on_the_thread_count`.

The JIT will replace step 3 with a call to a compiled kernel. Steps 1 and 2 will remain.

**Why a custom language and not embedded Python/Lua:** per-element code must run
ten million times per cook. That rules out any language with a GIL, with dynamic
allocation per value, or without static types.

---

## 6. Parallelism and determinism

The rule that makes results reproducible:

> Work is split into a number of chunks derived **only from the element count**, never
> from the thread count.

It follows that:

- `parallelFor` — chunks write to disjoint ranges; order does not matter.
- `parallelReduce` — one partial result per chunk, **combined serially
  in chunk order**. Floating-point addition is not associative, so this is the only
  way to get a bitwise identical result on both 1 and 32 threads.

A naive `#pragma omp parallel for reduction(+:x)` over floats violates this invariant
and silently produces results that depend on the thread count.

For the same reason the point generator is **hashed from the index**, not a sequential RNG:
point *i* depends only on *i* and the seed.

Thread pool: a thread waiting for a batch to finish **also executes pending
tasks**. That is why a nested `parallelFor` inside a branch cooked in parallel cannot
deadlock.

> **Known shortcoming:** the pool has a single shared queue under a mutex, without
> work stealing and per-thread deques. With fine-grained tasks this means contention.
> Production will use **TBB** (the VFX Reference Platform standard); this pool exists
> so that the prototype has no external dependencies.

---

## 7. Choice of dependencies

The deciding criterion is the license: **Apache 2.0 / BSD / MIT yes, GPL no** —
studios must be allowed to write proprietary nodes.

| Area | Choice | License |
|---|---|---|
| Sparse volumes | **NanoVDB** (done: gas in the path tracer, [docs/pathtracer.md](docs/pathtracer.md)); OpenVDB for reading `.vdb` | Apache 2.0 (since OpenVDB 12) |
| Subdivision | OpenSubdiv | Apache 2.0 |
| Booleans | **Manifold** | Apache 2.0 |
| BVH, raycast | **Embree 4** (done: path tracer rays, [docs/pathtracer.md](docs/pathtracer.md)) | Apache 2.0 |
| Render denoising | **Open Image Denoise 2** (done: [docs/pathtracer.md](docs/pathtracer.md)) | Apache 2.0 |
| Final render | **Cycles** from Blender as a library (done: [docs/cycles.md](docs/cycles.md)) | Apache 2.0 |
| Scene, viewport, render | **OpenUSD + Hydra** | Apache 2.0 (mod.) |
| Images, color | OpenImageIO, OpenColorIO, OpenEXR | BSD / Apache 2.0 |
| Materials | MaterialX, OSL | Apache 2.0 / BSD |
| Threading | Intel TBB | Apache 2.0 |
| Vectors, matrices, quaternions | **GLM** (done: `Vec3` is `glm::vec3`, `Mat4` `glm::mat4`, `Quat` `glm::quat`) | MIT |
| Rigid body | **Jolt Physics** (step 2, done: [docs/destruction.md](docs/destruction.md)) | MIT |
| Python bindings | **pybind11** (step 3, done: [docs/python.md](docs/python.md)) | BSD |
| JIT | LLVM ORC | Apache 2.0 + LLVM ex. |
| GUI | Qt 6 | LGPL (link dynamically) |

**CGAL will not be used** — it is GPL/commercial. Hence Manifold for booleans.

**GLM** does all the linear algebra: vectors, matrices (GLM's column-major
convention, `B * A` is first A, then B), quaternions, the preview camera
(`glm::lookAt`, `glm::perspective`), Euler angles (`glm::eulerAngleZYX`)
and inverses. Every file sees it the same way through the `pgmath` target:
- `GLM_FORCE_CTOR_INIT`: a vector without values is zero, a matrix is identity.
- `GLM_FORCE_XYZW_ONLY`: a vector is just `x`, `y`, `z`, `w`: three floats in
  a row, the way attributes store them.

Only the program's own conventions remain custom, not the math:
- `normalize` leaves a zero vector zero,
- `quatUpTo` is the shortest rotation of +y onto the normal,
- `Rotation::toEuler` picks the angles closest to the previous ones,
- matrices in the wrangle language have VEX's rows, which in memory are the same values
  as GLM's columns, and without values they are zero.

The switch to GLM was verified with fingerprints of all 44 examples. The first six frames
of every simulation (at preview resolution) came out bit-for-bit identical. The cooked
geometry of trees, grass and the street moved only by rounding, at most
0.023 mm. Topology stayed the same, and color changed for only 40
of the meadow's 26.9 million points, by one step out of 255.

**OpenUSD + Hydra** deserves emphasis: it provides a viewport (Storm), a connection to
any production renderer through render delegates, and above all immediate
interoperability with studios. It is the single largest saving of work in the entire
project.

---

## 8. Source layout

```
src/pg/core/     Types      vectors, matrices and quaternions (GLM), transforms, attribute types
                 Attribute  AttributeArray (COW), AttributeSet
                 Geometry   container, topology, groups, volumes, hash
                 Parallel   deterministic chunking, thread pool
                 Node       node, parameters, versioning, registry
                 Graph      node ownership
                 CookEngine pull evaluation, LRU cache
                 Spatial    k-d tree of points (deterministic order), neighbors across edges
                            (Adjacency: two passes into arrays, no vector per point)
                 Selection  element patterns: numbers, ranges, groups, edges (p3-4), * and ^
                 Soft       soft selection: share of motion for points around the selection, distance
                            straight or along the surface (across edges), falloff shapes
                 Sculpt     brush dabs (push/pull, smooth, grab, flatten) on points:
                            a grid that moves with the points, smoothing with boundaries; Sculptor
                            computes a stroke incrementally, bit-identical to computing from scratch
                 Pick       what is under the mouse, in a rectangle, lasso, brush stroke: BVH over
                            polygons, visible only or hidden too; refit when points
                            only move
                 Tree       a tree as it grows (Weber and Penn): trunk with a fork, branch levels
                            around the parent by the golden angle, crown shape, leaves; random numbers
                            for each part separately; mesh (tubes, leaves, flex) or skeleton
                 Grass      a grass tuft: blades from one root, tapering, leaning
                            and bent, color from root to tip, dry blades, flex
                 Instances  points that stand in for geometry prototypes (instance, orient,
                            pscale, tint): placement, copies on points (copiesOnPoints),
                            Unpack, bounds; quaternions; prototypes hold Geometry (COW)
src/pg/lang/     Parse      lexer and parser to AST, checking names and argument counts
                 Check      type checking against geometry: attribute bindings, overloads
                 Eval       typed-tree interpreter
                 Run        running over elements: in parallel in pieces, or in order with deferred
                            geometry changes; parameter expressions (Expression)
                 Builtins   math, noise, matrices, strings, arrays; BuiltinsGeo: geometry
src/pg/nodes/    Generators grid, line, pointcloud
                 Primitives box, sphere, tube (closed, faces pointing outward)
                 Modifiers  transform, merge, switch, null,
                            attribcreate, groupbox, blast
                 Edit       groupcreate, edit (with soft radius), attribpaint, sculpt:
                            what selection, handle and brush do in the viewport
                 Surface    file (OBJ), scatter (count and density, rules: share from an attribute,
                            slope, spacing; scatterPoints), normal, copytopoints (also instances
                            and pieces by attribute), unpack, color
                 Trees      tree: a tree from Tree.h, or one on each input point (forest),
                            trees in parallel and merged in point order; instance: variants
                 Grass      grass: tufts from Grass.h over a surface as instances (variants)
                 Wrangle    pointwrangle and attribwrangle nodes (points, primitives, vertices, detail)
                 Topology   connectivity, fuse, polyextrude, subdivide (Catmull-Clark),
                            clip with cut capping, attribtransfer; new points as weights of old ones
                 Rebuild    new elements from weights of old ones (Blends): attributes, groups, rebuild
                 Fracture   voronoifracture: point cells as cuts with caps
                 Concrete   concretefracture: uneven cells, rough fracture surfaces identical on both
                            sides of a crack, chipped corners, straight cut in the proxy attribute
                 Cluster    rbdcluster: pieces into clusters (k-means++, Lloyd), stronger glue inside
                 Rebar      rebar: bars in a block oriented by its largest face and rotating
                            stirrups — mesh in a wall, cage with stirrups in a beam; width is diameter
                 Glass      glassfracture: a pane as glass — radial and concentric cracks
                            (sectors, branching), cells cut by planes perpendicular to the pane;
                            glass 1 faces, 2 cracks, Cd glass color
                 Bricks     brickwall: a wall of bricks in a bond (stretcher, English, Flemish,
                            stack), courses along segments inside the input (openings), each brick
                            with its mortar and plaster one piece; broken bricks as two halves
                            of one cluster (cluster, clusterglue)
src/pg/io/       Obj        reading and writing OBJ (points, polygons, lines)
                 Ply        points with attributes and polygons to PLY and back (ASCII and binary)
                 Vdb        volumes to OpenVDB without the library: sparse 5-4-3 tree, file version 224
                 Export     geometry by extension (.ply, .obj, .vdb), sequences ($F4)
                 Jpeg       baseline JPEG: YCbCr 4:2:0, standard tables, AAN DCT
                 Picture    images in without libraries: PNG (Png), baseline
                            and progressive JPEG like libjpeg (JpegDecode), OpenEXR with all
                            compressions except DWA (ExrRead); deflate (Inflate); frame
                            sequences (####, $F4, %04d); writing PNG, JPEG and EXR
                 Video      video frame by frame: AVI with Motion JPEG on its own, .mp4/.webm/.gif piped to ffmpeg
src/pg/usd/      Layer      USD layer: values, prim and property specs, list edits, variants;
                            a file, and a file inside a .usdz package
                 Text       .usda parser
                 Crate      .usdc reader: LZ4, USD integer encoding, tables, values of all types
                 Stage      scene composition (prim indexes, LIVRPS opinion strength), time samples,
                            interpolation, value clips
                 Geom       transforms from xformOps, units and up axis, geometry and cameras from the scene
src/pg/shader/   Types      shader graph types and their conversions
                 NodeLibrary node definitions from text (builtin.pgnodes)
                 ShaderGraph node instances, connections, the .pgsg format
                 Generator  graph → statements, types, dead code
                 Target     GLSL 330, GLSL ES 300, Vulkan, HLSL; registry
src/pg/sim/      Grid       dense 3D grid of values, trilinear sampling
                 SparseGrid sparse grid: 8 × 8 × 8 tiles, only the active ones stored;
                            MAC faces with the first layer of the next tile
                 Poisson    pressure equation: geometric multigrid with walls and obstacles,
                            on active tiles (p = 0 outside them)
                 Shape      shapes placed in the world: position, rotation, size; inside test,
                            distance, ray intersection
                 Mesh       models from OBJ: distance field (SDF), rays, file cache
                 Scene      what is simulated: domain, sources, forces, obstacles, objects
                 Pyro       smoke and fire simulation, sparse: only tiles with gas and around them
                 Liquid     water: FLIP -- particles carry the water, the grid holds its volume
                 FreeSurface liquid pressure with a free surface: CG with multigrid, ghost fluid
                 Rain       rain: drops from a cloud in the wind, splashes, ripples on the surface
                 Rigid      rigid bodies on Jolt: bodies and contacts of pieces (rigidLayout), glue
                            as a constraint network (rigidGlue, rigidNetwork), rebar, debris as
                            particles (they collide, come to rest, ride with pieces), dust and its trails,
                            Guide as the pose of each piece (rigidGuide), toward which the solver
                            drives glued bodies; Jolt on threads, impacts sorted
                 Cloth      cloth, ropes and soft bodies (XPBD, small steps): length,
                            shear and bend constraints, balloon volume, pinned points, collisions
                            with objects, RBD pieces (two-way) and itself, air,
                            tearing by splitting points; constraints in colors
                            on threads, bitwise identical on 1 and 4 threads
                 Camera     shot camera: position, rotation, lens, resolution
                 Shared     what the solvers share: parallel loops, noise, gusty wind, forces on the MAC grid
                 World      all of the network's solvers at one frame rate, frame by frame;
                            checkpoint (saveState/loadState: gas, water, rain and cloth are loaded,
                            bodies are recomputed) and preview on coarser grids
                 State      solver state to bytes and back; reading checks every length
                 Network    network of simulation and geometry nodes, .pgsim format, keyframes,
                            translation to World + Look (frame by frame when something is animated)
                 GeometryGraph  the network's geometry nodes as a core graph: synchronization,
                            incremental cooking, simulations back as points and volumes,
                            constraint network from pieces (RBD Constraints)
                 Cooker     geometry cooked on its own thread: request (network, frame, nodes),
                            interrupting the one in progress, asset levels with instance inputs
                 ForEach    For-Each loops: pieces, primitives, points, count, feedback;
                            the loop body as its own network in its own GeometryGraph
                 Asset      digital assets: a versioned library of definitions, instance type
                            from a definition, instance cooked in its own GeometryGraph,
                            asset from selected nodes (collapseToAsset)
                 Display    geometry for the viewport: colored triangles, dots, lines;
                            DisplayMesher: indexed polygons; when points move, only
                            vertex positions and normals
                 Frame      frame: gas in half precision, water surface in bytes, drops
                 Cache      frames on disk: folder, cache.txt, .pgframe with zero runs; network hash
src/pg/render/   Embree     rays via Intel Embree 4: mesh as an Embree scene, placements as
                            instances, mesh without a transform directly; SAH without spatial splits
                            (deterministic), robust edges; shadows through glass and water
                 Bvh        custom bounding-box hierarchy (SAH, bins) for builds without
                            Embree, subtrees built in parallel and deterministically; ray
                            traversal, nearer child first; always for objects (spheres, boxes…)
                 Scene      scene for rays: meshes of the displayed geometry, bodies and water,
                            instances as placements of one mesh, objects exactly; sun, sky
                            and floor from the Look; materials from attributes
                 Denoise    denoising via Intel Open Image Denoise: albedo and normals first,
                            filters once per size; without it, a custom à-trous filter
                 Gas        the frame's gas as a NanoVDB grid (8³ tiles are leaves), maximum
                            smoke and flame in each tile; delta tracking with flames,
                            ratio tracking for shadows, noise-free marching for the denoiser
                 PathTracer progressive path tracer: GGX, translucency, glass and water, smoke
                            and fire, sun with MIS, thin lens, AOVs, custom à-trous filter
                            (when OIDN is absent), ACES; deterministic
                 Save       PNG, EXR with Z, albedo and normals
                 Cycles     rendering via Blender's Cycles: meshes, instances, Principled BSDF,
                            glass and water letting sunlight through, gas as grids in a box
                            (Principled Volume), sun, sky, camera; scene rotated to
                            Z up; image during rendering via a display driver, at the end
                            passes via an output driver
src/pg/gpu/      Gpu        compute through Vulkan, loaded at run time (Vulkan): devices,
                            memory on the device, kernels in order (Batch), GPU time
                 shaders    GLSL compute kernels, compiled to SPIR-V at build time and
                            carried in the program
                 SelfTest   memory, a sum and Jacobi sweeps against the CPU, the same to
                            the bit (prototype gpu, docs/gpu.md)
src/pg/gl/       Gl, Camera, Png, HeadlessContext — OpenGL without dependencies
                 Preview    shader preview on a body
                 Volume     volume rendering of the simulation: floor, objects, water, rain,
                            displayed geometry, guides; glass as two peeled layers
                            of front-facing surfaces (depth peeling), composited with gas along the ray
tests/           59 core tests (invariants, SOP nodes) + 27 for the language and expressions + 7 for
                 digital assets + 10 for topology, loops and background cooking + 25 for the shader graph + 86 for simulation, water, rain, objects,
                 models, geometry in the network and animation + 11 for cache and export + 11 for checkpoints,
                 preview and profiling + 5 for JPEG and video
bench/           measurements of the claims the architecture relies on
cli/             headless demo, OBJ export
tools/prototype/  prototype — an editor with two networks, simulation (default) and shaders,
                 on a shared node canvas; viewport with selection and gizmo
                 (SimViewport, Gizmo), drawn on a thread of its own in a shared
                 context (ViewThread), as are the nodes' thumbnails (ThumbThread);
                 the animated parameters as curves over the frames, their keys
                 dragged (SimCurves); displayed geometry and attribute spreadsheet
                 (SimGeometry); editing the displayed geometry -- points, edges and faces
                 with the mouse, handle, group, delete, brush as network nodes (SimElements);
                 digital assets: diving in and back out, Make Asset,
                 promote (SimAssets); disk cache and export (SimRunner: frames in memory
                 and read from disk on demand, menu); background bake as a separate process
                 with progress, cancel and resume from a checkpoint (Bake); wedge -- parameter
                 variants, bake after bake (Wedge); step profile in the overview; rendering
                 sequences and video frame by frame with progress (RenderJob); windowless context
                 for commands (Offscreen: EGL, otherwise a hidden GLFW window);
                 commands list/gen/check/render/sim (sim --cache/--from-cache/--export, video,
                 --checkpoint/--resume/--preview)
                 and cook (geometry without simulation to a file, hash for determinism)
examples/        shader graphs, a sample user library, simulation networks, assets (assets/)
```

The shader graph is described separately in [docs/shader-graph.md](docs/shader-graph.md),
smoke and fire simulation in [docs/pyro.md](docs/pyro.md), geometry in the editor's
network (nodes as SOPs, display flag, attribute spreadsheet, geometry as the shape for
simulations) in [docs/geometry.md](docs/geometry.md), keyframes and
moving obstacles in [docs/animation.md](docs/animation.md), digital assets
in [docs/assets.md](docs/assets.md), the disk cache
and export to PLY, OpenVDB and OBJ in [docs/cache.md](docs/cache.md), images
and video in [docs/render.md](docs/render.md).

The `pg` namespace remains even after the project was renamed to Prototype.

---

## 9. What the prototype can actually do

The prototype exists to **validate the invariants by measurement**, not to be a product.

### Done and tested

| | |
|---|---|
| ✅ | COW attributes, topology and groups — with buffer-identity tests |
| ✅ | Four attribute classes, groups, string table, `gather` |
| ✅ | Cook engine: pull evaluation, versioning, LRU cache with a budget |
| ✅ | Time dependency including transitive propagation |
| ✅ | Cycle detection when connecting |
| ✅ | Deterministic `parallelFor` / `parallelReduce`, thread pool |
| ✅ | Wrangle language: variables, control flow, functions, arrays, strings, matrices and quaternions; runs over points, primitives, vertices and detail; reading arbitrary elements and inputs, neighbor search (k-d tree), creating and deleting geometry; parameters from `ch()`; results independent of the thread count |
| ✅ | 28 node types (box, sphere, tube, scatter, copy to points, file, polyextrude, subdivide, clip, volume to polygons…), content hash, OBJ reading and writing, headless CLI |
| ✅ | Volumes in geometry (dense value grids, COW) |
| ✅ | 271 tests · clean under ASan, UBSan and **ThreadSanitizer** |
| ✅ | Shader graph: nodes from text, 4 targets, editor; every node validated with glslang and spirv-val |
| ✅ | Smoke and fire simulation from nodes: sources, forces, obstacles; MAC grid, multigrid, bitwise identical on 1 and 4 threads; editor and `prototype sim` |
| ✅ | Water (FLIP): free-surface pressure (CG with multigrid, ghost fluid, walls covered by bodies), bitwise identical on 1 and 4 threads; water surface with reflection and refraction |
| ✅ | Rain and wind: drops from a cloud, gusts traveling with the wind, splashes off objects, ripples on water (wave equation), wet floor; bitwise identical on 1 and 4 threads |
| ✅ | Shot camera: looking through the camera in the editor with an image frame, camera from view, rendering and sequences through the camera (editor and `prototype sim`) |
| ✅ | Geometry in the editor's network: SOP nodes cooked incrementally by the core, display flag, viewport, **geometry spreadsheet**; geometry as the shape of obstacles and sources, simulations back as points and volumes |
| ✅ | Expressions in parameters (`$F`, `$T`, `ch("../node/parameter")`) with dependency tracking and loop detection |
| ✅ | Digital assets: a versioned `.pgasset` library, instances cooked in their own graph, promoted parameters, definitions carried in the network file, cycles rejected; in the editor, Make Asset, diving in and back out |
| ✅ | For-Each loops (pieces, primitives, points, count, feedback) and topology nodes: PolyExtrude, Subdivide, Clip with cut capping (including non-convex), Fuse, Connectivity, Attribute Transfer |
| ✅ | Animation: keys on any parameter (Smooth/Linear/Step), the network frame by frame, moving obstacles with velocity and rotation in the boundary conditions of both gas and water, animated geometry parameters as core expressions |
| ✅ | Simulation disk cache (editor and `prototype sim`), frame-by-frame geometry export: PLY with attributes, **OpenVDB** (verified by reading in OpenVDB 10: voxels and sums match the simulation grid), OBJ |
| ✅ | Destruction: Voronoi Fracture, rigid bodies on Jolt, glued pieces as one body, charges, crushing to dust, debris, displaced air drives the dust ([docs/destruction.md](docs/destruction.md)) |
| ✅ | Concrete: Concrete Fracture — uneven pieces, smallest around the impact, chipped corners, rough fracture surfaces matching on both sides; RBD Solver simulates the straight cut (`proxy`) and draws the detail; `spread` and `rings` keep the damage near the impact point, pieces glued to the foundation stay standing; secondary fracturing: RBD Cluster groups pieces into clusters that break apart only on a hard impact ([docs/destruction.md §2](docs/destruction.md#2-concrete-fracture)) |
| ✅ | Reinforcement: the Rebar node (mesh in a wall, reinforcement cage with stirrups in a beam, oriented to the block); RBD Solver holds pieces on the bars with Jolt plastic constraints (friction in six directions), a bar yields according to the steel or the anchorage, pulls out, bends and snaps; bar state in frames and the cache (version 6), drawn as tubes, USD `/World/rebar` ([docs/destruction.md §2](docs/destruction.md#reinforcement-rebar)) |
| ✅ | Glass: Glass Fracture (radial and concentric cracks around the impact point); the pane stays whole until one of its bonds breaks; glass debris and a tenth of the dust (cache version 7); the renderer draws glass transparent — two layers, Fresnel on both faces, reflection of sky and sun, tint according to the path through the glass; USD glass material and cracks visible from the moment of breaking ([docs/destruction.md §2](docs/destruction.md#glass-glass-fracture)) |
| ✅ | Constraint network as geometry: RBD Constraints turns pieces into a point per body and a line per bond (`strength` as a multiple of Glue, `area`, color by strength); a weakened, deleted or hand-drawn network wired into the RBD Solver's Constraints input acts as glue; RBD Pieces returns the frame's network with `broken` and `time`, bond state in frames and the cache (version 8) ([docs/destruction.md §3](docs/destruction.md#constraint-network-rbd-constraints)) |
| ✅ | Bricks: Brick Wall lays a wall of bricks in a bond (stretcher, English, Flemish, stack) with mortar, plaster and openings with straight reveals; each brick is one piece, broken bricks as two halves of one cluster; RBD Solver treats the mortar as glue, and the wall breaks apart along the joints; examples `brick_wall` (a ball against a brick wall with a window) and `concrete_column` (demolition of a reinforced-concrete column, bare reinforcement cage) ([docs/destruction.md §2](docs/destruction.md#bricks-brick-wall)) |
| ✅ | Debris as particles: it flies out from the edge of a broken bond's face, is slowed by the air, spins, collides with pieces, obstacles and the floor (rays in Jolt), comes to rest and rides along with the piece it lies on; dust trails behind torn-off pieces (`trail`); orientation (`orient`) in frames, cache version 9, RBD Pieces, Python and USD, and Copy to Points orients by it ([docs/destruction.md §3](docs/destruction.md#debris-as-particles)) |
| ✅ | Multithreaded rigid bodies: Jolt on its own pool, impacts from its threads sorted, debris on threads; bitwise identical frames on 1 and 4 threads; Voronoi Fracture cuts a cell only from nearby parts with nearby points (bitwise identical, a tower of 5,628 cells 13× faster); `pgbench_rigid` ([docs/destruction.md §3](docs/destruction.md#how-it-works)) |
| ✅ | **Sparse gas**: Pyro Solver computes and stores only the tiles of 8 × 8 × 8 cells that contain gas, plus those around them that it can reach within a step; pressure via multigrid only on those; with all tiles, bitwise identical to the dense grid; frames and cache (version 10) only with the tiles containing gas; resolution up to 1024; demolition dust at 103.5 M voxels in 19 minutes ([docs/pyro.md §4](docs/pyro.md#sparse-grid-compute-only-where-there-is-gas), `pgbench_pyro`) |
| ✅ | Guided simulation: the RBD Solver's Guide (pieces moved and rotated, e.g. a keyed Transform around the Pivot) drives glued bodies to the pose that best places their points onto the Guide's points; strength, duration, reach and release when the glue breaks; the `guide` attribute; only the piece's pose is taken from the Guide in each frame; example `guided_fall` ([docs/destruction.md §3](docs/destruction.md#guided-simulation-guide)) |
| ✅ | Water surface as a closed mesh with velocity and foam (surface nets, Liquid Surface node) and volume to polygons (Convert Volume), bitwise identical on 1 and 4 threads |
| ✅ | CPU **path tracer**: rays via Intel Embree 4 (without it, a custom BVH with SAH), instances as placements of one mesh, sun with MIS and sky from the Look (floor brightness matches the viewport to within 0.4%), GGX, translucency of grass and leaves, glass and water, smoke, fire and dust via NanoVDB (delta tracking, ratio tracking, smoke color converted to scattering albedo), thin lens, denoising via Intel Open Image Denoise (without it, a custom à-trous filter); deterministic on any number of threads; Render tab in the editor (own thread, interruption), `--renderer path`, EXR with Z, albedo and normals ([docs/pathtracer.md](docs/pathtracer.md)) |
| ✅ | **Cycles** from Blender (5.2) as a library: downloaded and built with the program (with OpenImageIO, Embree, Open Image Denoise), the scene converted to Cycles (meshes and instances, Principled BSDF, glass and water, smoke and fire as a volume, sun, sky, camera); the default renderer of the Render tab (first image from larger pixels, frame by frame during playback), `--renderer cycles`, PNG and EXR with passes ([docs/cycles.md](docs/cycles.md)) |
| ✅ | The whole shot to **USD** without the library: bodies as transforms, debris, water surface, rain, dust as VDB, camera, lights; whatever changes goes into a file per frame (value clips); verified with Pixar's library, 28 validators with no findings ([docs/usd.md](docs/usd.md)) |
| ✅ | **Reading USD** without the library: `.usda`, `.usdc` (versions 0.4.0–0.10.0), `.usdz`; the scene composed as in USD (sublayers, references, payloads, variants, classes, value clips); matchmove camera (USD Camera) and geometry (USD Import) in meters with Y up; transforms, composition and geometry match the USD library ([docs/usd-import.md](docs/usd-import.md)) |
| ✅ | **Python API** `import pg` (pybind11): networks, parameters, expressions and keys, cooking and simulation from a script; attributes and frame data as zero-copy numpy arrays (buffer protocol over the core's shared memory); cache, USD, rendering via `prototype`; a network as Python (`as_code()`) ([docs/python.md](docs/python.md)) |
| ✅ | Rendering to **EXR** without the library: linear light, depth, motion vectors, masks; OpenEXR 3.5 reads it ([docs/render.md](docs/render.md)) |
| ✅ | **Plate**: the shot's footage (PNG, JPEG, EXR, read without libraries, JPEG bit for bit as libjpeg) behind the CG of the shot camera; holdout and shadow catcher (objects and the floor); to EXR, the CG with alpha and a `catcher` pass; where the CG changes nothing, the plate comes out pixel for pixel ([docs/plate.md](docs/plate.md)) |
| ✅ | **Cloth, ropes and soft bodies** (XPBD, akin to Vellum): Cloth Solver from polygons, polylines and closed meshes with pressure; pinned points carried by animation; collisions with the floor, objects, RBD pieces and itself, friction; air, wind and gas flow on faces; frames and cache (version 11, torn cloth version 12), checkpoint, Cloth Geometry; tearing (Tear, the `tear` attribute); RBD pieces as bodies in the cloth step, the cloth slows and carries them; examples `tablecloth`, `flag` and `tarp` ([docs/cloth.md](docs/cloth.md)) |
| ✅ | Video: AVI with Motion JPEG without dependencies (custom JPEG encoder), MP4/MOV/MKV (H.264), WebM (VP9) and GIF via ffmpeg; in the editor, background rendering with progress; from the command line, `sim OUT.mp4` and `render OUT.mp4`; verified by decoding in ffmpeg |

### Measured (4 cores, g++ 13.3, RelWithDebInfo)

| Claim | Measurement |
|---|---|
| COW: 50 nodes, 2M points, 5 attributes | **50 allocations** instead of 250; 3.7× less memory |
| Edit in the middle of a 100-node chain | **48%** of the cold cook time, 51 of 101 nodes |
| Recook without changes | **0.025 ms**, 0 nodes |
| Scaling on 4 threads | **2.99×**, hash bitwise identical on 1/2/4 threads |
| Language interpreter, arithmetic | 38 Mpoints/s (1M points in 26 ms) — the first, simple language |
| Language interpreter, noise | 63 Mpoints/s — the first, simple language |
| Wrangle v2 vs. the first language, same machine (4 cores) | arithmetic 26.6 vs. 29.0 Mpoints/s, noise 38 vs. 50 Mpoints/s |
| 240 frames with a 256 MB cache | 1.9 ms/frame, source cooked **1×** |
| Rigid bodies, demolition tower (593 pieces, 710 bodies), `pgbench_rigid` | 5.3 ms/frame on 1 thread, **3.2 ms** on 4; frames bitwise identical |
| … ten times more pieces (5,628) | 78 ms/frame on 1 thread, **31 ms** on 4; Voronoi cut 1.0 s (previously 13.5 s) |
| Demolition dust, resolution 96 (0.5 M voxels), `pgbench_pyro` | dense 249 ms/frame, **sparse 55 ms** (dense solver previously 164 ms); 119 → 75 MB |
| … resolution 576 (103.5 M voxels), 180 frames | **6.2 s/frame** including rigid bodies, 19 minutes; at most 3.4 GB; at most 23% of the domain is computed |

### Not in the prototype (deliberately)

I/O (materials and PointInstancer from USD, Alembic; VDB only writing dense grids) · JIT · packed primitives and out-of-core ·
Python inside the network (Python SOP) · booleans, geometric queries (xyzdist, primuv) · cloth
simulation · sparse water and GPU simulation

---

## 10. Summary of known simplifications

A summary of what is scattered through the text — every item is deliberate, not an oversight:

1. **Cache memory accounting** is an upper bound (it ignores sharing between entries).
2. **Duplicate cook** of a node reachable through two branches cooked in parallel.
3. **Thread pool** has a single queue under a mutex, without work stealing → TBB.
4. **Hash** is serial byte-wise FNV-1a → parallel xxHash3.
5. **The language** is a typed-tree interpreter, not a JIT; a program that changes
   geometry or writes strings runs on a single thread.
6. **No array or matrix attributes.** The language has arrays and matrices, but only as
   local variables.
7. **`gather` does not compact the string table** — after large deletions, unused
   entries remain in it.
8. **Cycle detection** is O(V) per connection; very large graphs will
   need an incremental variant.
