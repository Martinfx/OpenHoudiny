# Python API

The `pg` module exposes the prototype to Python, much as `hou` exposes Houdini.
A script using it can:
- build a node network or open an existing one;
- set parameters, expressions and keys;
- cook geometry and read it as numpy arrays **without copying**;
- simulate frame by frame, save a cache and write the shot to USD;
- render an image or a video.

This lets a studio plug the prototype into its pipeline: scripts on the farm, simulation
checks, generating variants, hooking into in-house tools.

The module does not need OpenGL, so it runs even on a headless server. Images
and videos are drawn by the `prototype` program, which the module launches as a separate process.

## 1. Building

```bash
cmake -B build                      # PG_BUILD_PYTHON is on when python3-dev is found
cmake --build build -j
PYTHONPATH=build/python python3 -c "import pg; print(pg.examples())"
ctest --test-dir build -R python    # module tests
```

- **Dependencies:** pybind11 (BSD) is downloaded at configure time, just like Jolt or ImGui.
  Offline, it is enough to set `FETCHCONTENT_SOURCE_DIR_PYBIND11` to a local copy.
  When pybind11 is not available at all, CMake skips the module with a warning and builds the rest.
- **Python version:** the module is built for one version, the one CMake finds.
  Choose another with `-DPython3_EXECUTABLE=/cesta/k/python3`.
- **Where the module goes:** `build/python/pg/` contains `__init__.py` and the `_pg` extension.
  Adding this folder to `PYTHONPATH` is enough.
- **numpy:** recommended. Without it, arrays are `memoryview`s over the same memory.
- **Sanitizers:** `PG_SANITIZE` disables the module, because a library built with a sanitizer will not load
  into a regular Python. Because of the module, the libraries are built as position-independent code.

## 2. Quick start

```python
import pg

net = pg.Network()
column = net.add("box", "column", size=(0.6, 3.0, 0.6), center=(0, 1.5, 0))
pieces = net.add("voronoi_fracture", "pieces", count=25)
column.connect(pieces)                      # the first output and input whose types match

geo = pieces.geometry()                     # cooks the node at frame 1
geo.P                                       # numpy (N, 3) float32, no copy
geo.prims["piece"]                          # the piece of each primitive

rbd = net.add("rbd_solver", glue=0.0)
pieces.connect(rbd).connect(net.add("output", frames=60))
sim = net.simulate()
with pg.UsdExport("out/column.usda", sim) as usd:
    for frame in sim.run():                 # frame by frame
        usd.add()
print(sim.current.rigid.centres)            # where the piece centres are

net.layout().save("out/column.pgsim")       # opens in the editor
net.render("out/column.mp4")                # video via prototype
```

## 3. Network and nodes

`pg.Network` is the same network as in the editor and in `.pgsim` files.

| What | How |
|---|---|
| new, from a file, an example | `pg.Network()`, `pg.Network.load("scene.pgsim")`, `pg.Network.example("demolition")`, `pg.examples()` |
| save | `net.save("scene.pgsim")`, `net.text()` |
| node | `net.add("box", "name", size=(1, 2, 1))` returns a `pg.Node`; `net["box1"]`, `net.nodes("box")`, `"box1" in net`, `net.remove(node)` |
| node types | `pg.node_types()`, `pg.node_type("box")`: parameters, inputs, outputs and help; `pg.assets()` for digital assets, `pg.load_assets(folder)` |
| parameter | `node["size"]`, `node["size"] = (1, 2, 3)`, `node.set(count=40, seed=3)`, `node.param("size")` (kind, default value, range, unit, help), `node.reset("size")` |
| expression | `node.expression("size.y", "$F * 0.1")`, `ch("../box1/sizex")` and others as in the editor |
| keys | `node.key("center", 1, (0, 0, 0)).key("center", 24, (0, 5, 0), "linear")`, `node.value("center", 12)`, `node.keys("center")`, `node.clear_keys("center")` |
| connections | `a.connect(b)` (returns `b`, so calls can be chained), `net.connect(a, b, output="look", input="look")`, `net.disconnect(a, b)`, `net.links()` |
| flags | `node.display()`, `net.displayed`, `node.bypass = True`, `node.position = (x, y)`, `net.layout()` arranges the nodes for the editor |
| check | `net.problems()`: `(level, node, message)`, i.e. what the editor would show on the nodes |

- **Values by parameter kind:**
  - numbers as `float`, integers as `int`, toggles as `bool`;
  - vectors and colors as triples;
  - a menu choice as a name (`clip["keep"] = "below"`), text and code as `str`.
- **Text as in files:** a value can also be given as text, the way files and `--set` write it:
  `box["size"] = "1 2 3"`.
- **Errors:** an unknown parameter, a nonsensical value or a connection whose types do not match
  raises `pg.Error` with a list of what the node has.

### `as_code()`: the network as Python

`net.as_code()` returns the source code of a `build()` function that rebuilds the network, node by node.
It is the counterpart of `asCode()` in Houdini. The code contains:
- parameters that are not at their defaults;
- expressions, keys and flags;
- node positions and connections.

Wrangle code is written as `r'''…'''`. A test verifies on all examples that the network built by this
code is identical to the original.

## 4. Geometry as numpy

`node.geometry(frame)` (or `net.cook(node, frame)`) returns a `pg.Geometry`. The core cooks
incrementally: whatever has not changed since the last cook is taken from the cache.

| What | How |
|---|---|
| attributes | `geo.points`, `geo.vertices`, `geo.prims`, `geo.detail`: tables by name, `geo.points["Cd"]`, `geo.points.type("Cd")` (`int`, `float`, `vector3`…), `list(geo.points)` |
| positions | `geo.P` is the same as `geo.points["P"]` |
| topology | `sizes, points = geo.topology()`: the corner count of each primitive and the point of each corner, like `faceVertexCounts` and `faceVertexIndices` in USD; `geo.primitive_starts()`, `geo.closed()`, `geo.primitive(i)` |
| groups | `geo.groups`, `cls, members = geo.group("inside")`, `geo.set_group(...)` |
| volumes | `geo.volumes`, `geo.volume("density").values[i, j, k]`, `geo.add_volume(name, values, origin, voxel)` |
| building | `pg.Geometry()`, `add_points(P)`, `add_polygons(sizes, points)`, `add_polylines(...)`, `append(other)` |
| new values | `geo.points["Cd"] = colors`, `geo.points.set("mask", values, "int")`, `del geo.points["mask"]` |
| files | `geo.save("x.ply" \| ".obj" \| ".vdb" \| ".usda" \| ".mtlx")` (`.usda` with materials, `.mtlx` materials only, [materialx.md](materialx.md)), `pg.Geometry.load("x.obj" \| "x.ply")` |
| instances | `geo.prototypes` (a list of `pg.Geometry`), `geo.instance_count`, `geo.add_prototype(g)` returns the number for the `instance` attribute, `geo.clear_prototypes()`, `geo.unpack()` turns instances into copies ([vegetation.md](vegetation.md)); `.obj` and `.ply` get copies, `.usda` a PointInstancer |

- **No copy:** the arrays point straight into the core's memory. Reading one again gives the same address,
  and so does a cooked node from the cache (a test verifies this). An array keeps the geometry alive for as long as it exists.
- **Read-only:** the core shares that memory between geometries (copy-on-write), so
  writing to an array raises `ValueError`. New values come in by assigning to the table:
  - this copies the values;
  - the geometry is thereby detached and from then on has its own buffers, like a node in the core;
  - the cooked geometry in the cache stays as it was.
- **The type of a new attribute** is inferred from the data:
  - rows of 2, 3 or 4 numbers are vectors;
  - integers are `int`, everything else `float`;
  - a list of strings is `string`.
- **Volumes** are indexed `[i, j, k]` like voxels, even though x varies fastest in memory
  (a strided view, again without a copy).

## 5. Simulation

`net.simulate()` (or `pg.Simulation(net)`) compiles the network as it is at that moment and simulates
frame by frame. Later changes to the network no longer affect it. `net.simulate(cache="folder")` reads
frames from the cache instead of simulating.

| What | How |
|---|---|
| step | `frame = sim.step()`, `for frame in sim.run(120): …`, `sim.frame`, `sim.frames`, `sim.fps`, `sim.current` |
| frame geometry | `sim.geometry()` of the displayed node, `sim.geometry(node)` of another: Liquid Surface, RBD Pieces and others see exactly this frame |
| gas | `frame.gas("density" \| "temperature" \| "flame" \| "steam")[i, j, k]` (float16; steam is zero everywhere water did not produce it, [quench.md](quench.md#steam)), `frame.gas_domain()` |
| water | `frame.water.positions`, `.velocities` (float16), `.foam`, `.ids`, `.flow` (velocity on the grid, `[i, j, k, axis]`), `.surface()` (a mesh like Liquid Surface), `.litres` |
| rain | `frame.rain.positions`, `.velocities`, `.ids`, `.droplet_positions`…, `.ripples()` |
| bodies | `frame.rigid.centres`, `.velocities` (of the centers), `.spins`, `.rotations` (x, y, z, w), `.translations`, `.vanished`, `.grit` (x, y, z, size), `.grit_velocities`, `.grit_ids`, `.grit_orient` (the orientation of each grit chip: x, y, z, w), `.pieces()`; rebar `.rebar()` (bars as polylines with `width` and `v`), `.rebar_state` (1 the bar has come out of the piece, 2 it is snapped behind it), `.rebar_stations` (body, from, to, bar); glass `.grit_glass` (1 glass grit), `.unglued` (bodies whose bond broke); constraint network `.network()` (a point per body, a line per bond, `broken`, `time`), `.joint_state` (0 holding, 1 broken, 2 never held), `.joint_time` (when it broke) |
| cloth | `frame.cloth()`: the Cloth Solver's cloth as geometry where its points are, with `v` and `N`; when torn, with detached points and reconnected primitives, separated ropes as separate lines ([cloth.md](cloth.md)) |
| grains | `frame.grains()`: the Grain Solver's grains as points with `v`, `pscale` (radius), `id`, `Cd` and `orient` ([grains.md](grains.md)) |
| camera | `sim.camera()`: position, rotation, focal length, image dimensions at this frame |
| cache | `frame.save("cache")`, `sim.write_cache_info("cache")`, `sim.cache("cache", frames=120)`, `pg.Frame.read("cache", 7)` |
| USD | `with pg.UsdExport("shot.usda", sim) as usd:` and `usd.add()` after each step, or `sim.export_usd("shot.usda")` ([usd.md](usd.md)) |
| Alembic | `with pg.AbcExport("shot.abc", sim) as abc:` and `abc.add()` after each step, or `sim.export_alembic("shot.abc")` ([alembic.md](alembic.md)) |
| image | `net.render("out.png" \| ".exr" \| ".mp4", frames=…, every=…, size="1920x1080", from_cache="cache")`, `pg.run("sim", …)` |

- **Body pose:** a point `p` in the rest position is now at `rotate(rotations[b], p) + translations[b]`.
  `centres` is where the body's center is now (the center of the box around its shape at rest).
- **Threads:** simulation and cooking release the GIL, so other Python threads run in the meantime. However,
  two threads should not use one network at the same time.
- **Render:** `net.render` saves the network to a temporary file and runs `prototype sim`.
  It reads the network's relative paths (meshes) from `net.folder` via the new `--folder` option. The program is found
  from the build, or via the `PG_PROTOTYPE` variable, otherwise on `PATH`.

### Reading USD

`pg.UsdStage("shot.usd")` opens a `.usd`, `.usda`, `.usdc` or `.usdz` file with the program's own
reader and composes it as USD does: sublayers, references, payloads, variants,
value clips ([usd-import.md](usd-import.md)). It does not need the `pxr` library for this.

| What | How |
|---|---|
| stage | `.up_axis`, `.meters_per_unit`, `.start_time_code`, `.end_time_code`, `.time_codes_per_second`, `.default_prim`, `.files` (loaded layers), `.warnings` |
| prims | `stage.prims()` (defined ones; `all=True` also over, class and inactive), `stage.prims(type="Mesh")`, `stage.prim("/Set/beam")` |
| prim | `.path`, `.type`, `.children`, `.properties()`, `.get("points", time)`, `.varies(name)`, `.sample_times(name)`, `.targets(rel)`, `.metadata("kind")`, `.world(time)`, `.local(time)` (4 × 4 matrices, row-major) |
| camera | `stage.cameras()`, `stage.camera(path=None, time=None)`: world matrix in meters with Y up, focal length, apertures and their offsets, clipping, whether it moves |
| geometry | `stage.geometry(time, prims=["/Set"], proxy=False, metres=True)` → `pg.Geometry`, like the USD Import node; whatever could not be read is in `stage.notes` |
| time | `stage.time_code(frame, fps)`: which time code a prototype frame reads (like the USD Import and USD Camera nodes) |

The `time` is the stage's time code; without it, frame 1 is read, i.e. `startTimeCode`.

### Images

Images are read and written by the same in-house readers and writers that the plate goes through
([plate.md](plate.md#5-images-without-libraries)). No libraries are needed for this.

```python
pixels, linear = pg.read_picture("plate.1001.exr")   # rows × columns × RGBA, float32
pg.write_picture("plate.1001.jpg", pixels, quality=92)
```

- **`pg.read_picture(path)`** reads PNG, JPEG or OpenEXR (detected from the content).
  It returns the pixels and `linear`: `True` for EXR (linear light), `False` for PNG and JPEG
  (display values, 0 to 1).
- **`pg.write_picture(path, pixels, quality=92)`** writes a file according to its extension.
  - Pixels: rows × columns, with 1 to 4 channels (gray, gray and alpha, RGB, RGBA),
    in float32, float64, uint8 or uint16. Bytes mean 0 to 255.
  - `.png` and `.jpg` get display values in 8 bits. JPEG has no alpha.
    PNG has no alpha when it is 1 everywhere.
  - `.exr` gets linear light in half float.

## 6. Examples

- **[`examples/python/matchmove.py`](../examples/python/matchmove.py)** — a shot from USD:
  - what the file contains, its camera at the first and last frame;
  - the set as geometry and its dimensions from numpy arrays;
  - a network with USD Import and USD Camera nodes, smoke from a fire in the set, an image through the shot camera.

  ```bash
  PYTHONPATH=build/python python3 examples/python/matchmove.py --picture out/matchmove.png
  ```

  When a plate has been shot (script below), the smoke goes over it and the set is a shadow catcher.

- **[`examples/usd/make_plate.py`](../examples/usd/make_plate.py)** — the plate of the sample
  shot ([plate.md](plate.md)):
  - the set from `shot.usda` rendered with the matchmove camera via `net.render`;
  - "film" in numpy: soft lens, vignetting, halation, color grading and grain;
  - `pg.read_picture` and `pg.write_picture` for 72 JPEGs `courtyard.1001.jpg`…

  ```bash
  PYTHONPATH=build/python python3 examples/usd/make_plate.py
  ```

- **[`examples/python/demolition.py`](../examples/python/demolition.py)** — the scene from step 2
  (blowing up a tower block in a city) built purely from Python:
  - `build()` was written by `as_code()` from `examples/sim/demolition.pgsim`;
  - the script simulates, writes each frame to the cache and the shot to USD;
  - with `--video` it renders it from the cache;
  - a test verifies that `build()` produces exactly the example's network.

  ```bash
  PYTHONPATH=build/python python3 examples/python/demolition.py --frames 60 --video out/demolition.mp4
  ```

- **[`examples/python/fracture_stats.py`](../examples/python/fracture_stats.py)** — a column
  broken into pieces:
  - the volume of each piece computed from numpy arrays (together exactly the volume of the column);
  - the pieces falling and their centers when they land;
  - the shot to USD and a network for the editor.

## 7. How it works

```
pg/__init__.py      classes Network, Node, Geometry, Simulation, Frame, UsdExport, AbcExport, UsdStage, UsdPrim (Python);
                    read_picture, write_picture
_pg (C++)           pybind11 over sim::Network, GeometryGraph, WorldSolver, UsdExport, AbcExport, usd::Stage
```

- **Zero-copy arrays:** `_pg.Array` is an object implementing the buffer protocol (PEP 3118). It carries a pointer
  into memory, a format (`f`, `e`, `i`, `I`, `B`), a shape, strides and an owner, a shared pointer
  to the geometry or frame. `numpy.asarray(array)` creates an array over it without a copy.
- **Geometry snapshot:** each view is created over a copy of the `Geometry` that shares
  buffers (copy-on-write as in the core). When the geometry later changes, the view keeps the old
  memory and never points into freed memory.
- **Network and cooking:** `pg.Network` holds a `sim::Network` and its own `GeometryGraph`. Cooking is therefore
  incremental, as in the editor.
- **Simulation:** `pg.Simulation` holds a copy of the network, `Compiled`, a `WorldSolver` (or reads the cache)
  and its own graph, whose nodes that turn the simulation back into geometry see the current frame.
- **Assets:** on import, digital assets are loaded as at program startup: those that
  ship with the program, and those from the `$PROTOTYPE_ASSETS` folders and the user's.

## 8. Tests

`tests/python/test_pg.py` (29 tests; `ctest -R python` runs all three files):
- **Networks:**
  - node types and examples;
  - parameters of all kinds (vector, menu choice by name, toggle, text, code) and errors;
  - connections by type, flags and network checks;
  - expressions and keys;
  - saving and loading without changes;
  - `as_code()` rebuilds every example.
- **Examples:** `build()` from `demolition.py` is the network of the demolition example.
- **Geometry:**
  - numpy arrays over core memory, the same address also from the cache, read-only;
  - changes to a copy leave the cache alone;
  - geometry built in Python to PLY and OBJ and back, with strings and groups;
  - a volume indexed `[i, j, k]`.
- **Simulation:**
  - rain_pond frame by frame: water, rain, surface, camera;
  - a checkpoint resumes bit for bit, the preview is coarser;
  - a bucket of water on a campfire: steam as its own gas field (`gas("steam")`),
    zero before the water;
  - the cache and reading from it;
  - RBD pieces: rest points moved by the pose match `pieces()`;
  - cloth, grains, concrete, RBD Cluster, rebar, glass, runtime fracturing and wood,
    brick wall, constraint network, grit, Guide;
  - the shot to USD, verified with the `pxr` library when it is installed.
- **Image:** an image via `prototype` (skipped when OpenGL is not available).

`tests/python/test_usd.py` (9 tests):
- **Sample shot:** a stage of three layers, a camera in meters with Y up, the set as
  geometry, the `matchmove` network through the shot camera.
- **Against the USD library** (skipped without `usd-core` and numpy): random transforms of all
  kinds, a shot from multiple files (variants, references, classes, a sublayer with an offset and a different FPS,
  instanceable), geometry in world space, value clips including between frames, and 80 random shots
  with value clips (strength relative to layers, templates, jumps in time, manifest).

`tests/python/test_picture.py` (12 tests):
- **Reading:** JPEG like libjpeg, EXR in linear light; for what is not an image, it says why.
- **Writing:** PNG, JPEG and EXR back to the value (including reversed arrays and multiple deflate blocks);
  for what cannot be written, it says why.
- **Against Pillow and OpenEXR** (skipped without them): 60 random JPEGs bit for bit like
  libjpeg, 40 PNGs to the value, written PNGs and JPEGs read identically by Pillow, 48 EXRs of all compressions
  like the OpenEXR library.
- **Plate through the renderer** (skipped without OpenGL): the plate comes out pixel for pixel, even under
  a holdout and a catcher; a CG object covers it and darkens it with its shadow; the EXR carries only the CG, alpha and
  the `catcher` pass.

## 9. Limitations

- **There is no Python inside the network yet.** A node whose geometry is computed by a script (the Python SOP
  in Houdini) is missing. Geometry from Python can get into the network only via a file and the File node.
- **Out-of-process rendering:** the image is drawn by `prototype`, so it simulates again unless it is given
  `from_cache`.
- **Threads:** only one Python thread at a time may use a network or a simulation.
- **Module size:** in a RelWithDebInfo build it is around 100 MB, because it carries the debug
  information of all the libraries. In a Release build it is much smaller.
- **float16:** water and gas arrays are in half precision, as the frame holds them.
  `astype("float32")` makes a float copy of them.
