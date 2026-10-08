# Smoke, fire, water and rain: simulation from nodes

A real gas simulation on a 3D grid, the same principle as Pyro in Houdini and
EmberGen, and water from particles, like FLIP in Houdini. Smoke and fire move
here because it follows from the flow equations: heat rises, vortices curl,
fuel burns, the gas expands and flows around obstacles. Water falls, splashes,
swells into waves and keeps its volume ([§5](#5-water)). Rain falls from a cloud,
the wind slants it in gusts, it splashes off objects and makes ripples on water
([§6](#6-rain-and-wind)). The shot camera determines where the render is taken from
([§2](#camera-and-shot)). A simulation is built
from **nodes**: sources, forces and obstacles feed into solvers, solvers into looks, and looks
into the output. The shader effects
from [shader-graph.md §6](shader-graph.md#6-effects-fire-and-smoke) only fake motion
with noise on a single surface.

![Campfire, explosion and tornado, resolution 96](img/pyro.gif)

![Camera shots: a campfire by a pond in the rain (lakeside) and a campfire in a storm (storm)](img/vfx.png)

![Editor: the Simulation network with a tornado, the selected Vortex node and its guides in the viewport](img/editor-sim.png)

Everything is in the `prototype` program. The editor opens on the **Simulation**
network with an empty scene: **Shift+A** in the viewport adds fire, smoke, water, rain,
an object or a camera (together with the solver, look and Output that go with it);
ready-made scenes are in **File › Examples**. Without a window, the `prototype sim` command
computes and renders the simulation — even straight to video ([render.md](render.md)).

Contents:
[1. Quick start](#1-quick-start) ·
[2. Editor](#2-editor) ·
[3. The simulation network](#3-the-simulation-network) ·
[4. How the simulation works](#4-how-the-simulation-works) ·
[5. Water](#5-water) ·
[6. Rain and wind](#6-rain-and-wind) ·
[7. How it is drawn](#7-how-it-is-drawn) ·
[8. Performance and determinism](#8-performance-and-determinism) ·
[9. Verification](#9-verification) ·
[10. How to add a node](#10-how-to-add-a-node) ·
[11. What you need to know](#11-what-you-need-to-know) ·
[12. Limitations and what production does](#12-limitations-and-what-production-does) ·
[13. References](#13-references)

---

## 1. Quick start

```bash
./build/prototype                          # editor: empty scene (Shift+A adds fire, water, rain)
./build/prototype --example campfire       # built-in example: campfire
./build/prototype --example tornado        # another built-in example
./build/prototype moje.pgsim               # your own network

# no window: the last frame, or every k-th one as a numbered sequence
./build/prototype sim campfire fire.png
./build/prototype sim examples/sim/explosion.pgsim out/boom.png --every 2
./build/prototype sim lakeside shot.png    # shot through the network's camera, 1280 × 720
./build/prototype sim lakeside out/shot.png --every 1   # the whole shot, frame by frame
./build/prototype sim tornado t.png --set vortex.speed=3 --set solver.resolution=128
./build/prototype sim --list               # built-in examples, by purpose (fire, wind, water, rain...)

# straight to video: the whole shot, frame by frame (.avi with nothing extra, .mp4 via ffmpeg)
./build/prototype sim campfire fire.mp4
./build/prototype sim examples/sim/explosion.pgsim boom.avi --every 2
```

`sim` loads the network from a file, or takes a built-in example by name.
`--set NODE.PARAMETER=VALUE` changes a parameter before the simulation; when only
one node has a parameter of that name, `--set PARAMETER=VALUE` is enough. The number
of frames comes from the Output node, `--frames N` overrides it, `--resolution` overrides
the solver resolution. The command prints how long a simulation step took and how long one
image took. It renders through EGL without a window, so it works on a server too; without a GPU
a software driver such as Mesa llvmpipe is enough. With `--every K` it saves frames
K, 2K, 3K… and names them by frame number (`boom_0002.png`, `boom_0004.png`,
…). Video (`.avi`, and with ffmpeg also `.mp4`, `.webm`, `.gif`) gets every
frame, or every K-th one with `--every K` ([render.md](render.md)).
`--guides` draws guides into the image: the domain, sources and forces.
`--threads N` computes on N threads (the default is as many as the
machine has). Frames are identical on any number of threads, so this
can be used to verify that a network is deterministic (`--cache` on one and on four threads
gives the same files).

If the network has a camera connected to Output, `sim` renders through its view and
at its image resolution (`--size` overrides it). `--yaw`, `--pitch` and
`--distance` move away from the camera and show the scene from an orbit around it.

The older `prototype pyro OUT.png --preset fire` still works: it runs an example
(`fire` is the campfire).

## 2. Editor

The editor has the same layout for both networks, simulation and shaders. You switch
between the networks with the toggle in the middle of the top bar.

| panel | what it shows |
|---|---|
| **Viewport** (top left) | the scene: gas on the floor with a shadow, objects, guides; click selection and the gizmo. The **Render** tab next to it in the panel header shows the same scene from the path tracer or from Cycles |
| **Timeline** (bottom left) | playback, cached frames, the playhead |
| **Parameters** (top right) | parameters of the selected node; with nothing selected, an overview of the network |
| **Network** (bottom right) | the node network |

The borders between panels can be dragged.

The interface font is **Inter** (Regular and SemiBold, SIL OFL 1.1 license),
compiled into the program: the same on every system, with Czech characters and with the symbols
the editor writes (×, ·, …, arrows). Characters that Inter lacks are filled in by the system
DejaVu if it is installed; code uses the monospaced DejaVu Sans Mono, otherwise
Dear ImGui's own font. Helper texts in the viewport (frame number,
"simulating…", the hint at the bottom, the selected node) sit on dark labels, so
they read as well over a white sky as over a dark floor. The toolbar
on the left fits into a short window: first it shrinks the buttons, then it continues
in a second column, and it never covers the axes in the corner. **Escape** closes the open
menu or popup, only the topmost one (dialogs have their own Cancel), and does not reach
the panels below it. Help lists the shortcuts in a table, keys in one column and
what they do in the other. Dialogs have their buttons on the right, the main one orange.

### Network

| action | how |
|---|---|
| add a node | **Tab** or right-click on empty space: a searchable menu at the mouse; the up and down arrows move the highlighted node, Enter takes it. The list below the search field scrolls; the menu always fits in the window |
| connect | drag from pin to pin; green = it fits, red = it does not fit and the tooltip says why |
| add an already connected node | drag from a pin into empty space: it offers only nodes that fit the pin |
| move or remove a wire | drag by the connected input; dropped into empty space, it disappears. Ctrl+click on a wire removes it |
| pan, zoom | middle button or Alt+left; the wheel zooms around the mouse |
| selection | click, drag a box, Shift adds, Ctrl toggles, Ctrl+A all |
| frame / lay out | **F** the selection (or everything), **L** automatic layout into columns |
| delete / duplicate | Del or X / Ctrl+D |
| bypass | **B**: the node stays in the network, but the simulation skips it |

Keys belong to the panel under the mouse, as in Houdini. The node header has the color
of its category; below the name is a summary of what the node does (`fuel 14 · heat 1`).
A node that does not lead to the output is dimmed. A problem is shown by a red or yellow
badge; the tooltip over it says what is wrong. When zoomed far out, the texts
on the nodes are hidden (while they would be smaller than about 8 pixels) and the node name is
written next to it in a small but legible font, as in Houdini: below the node and,
when there is no room there, to its right. A name is written only where it does not cover
another node or another name. The current and selected node take priority, then the node under
the mouse, the displayed one, one with a problem and finally the most connected one. Other names
are shown by the tooltip over the node. A network with dozens of nodes, which the editor fits
entirely into the panel on opening, is thus readable and the texts do not overlap.

### Parameters

Parameters are divided into collapsible sections. The label lights up
when the value differs from the default; the ↺ icon on the right resets it. A slider is
filled from the left edge to the value. The network overview (gas, cells, inputs,
camera, frames, cache, bake, wedge) has labels in one column and values in
the other. The tooltip over
a label explains what the parameter does, and gives its name for `--set` and
the slider range. A slider has the range where the parameter is useful. Ctrl+click
lets you type a number outside it too; limits only enforce physical sense (for example no
negative fuel). Vectors have color-coded axes, x red, y green, z blue, and
choices are several buttons side by side.

### Viewport: objects and the gizmo

The viewport is controlled as in 3D programs. A click selects what is under the mouse:
an object, a source, a force guide or a domain edge (which selects the solver). The selection is
shared between the viewport and the network: a node selected by clicking in the scene is highlighted in the network too, and
the Parameters panel shows its parameters. The object under the mouse lights up subtly;
the selected one has an orange border and outline.

![Viewport: a selected object, the gizmo for moving along local axes, the toolbar on the left](img/editor-objects.png)

| action | how |
|---|---|
| select | click; **Shift** or **Ctrl** adds / removes; a click on empty space clears the selection |
| tool | **Q** select, **W** move, **E** rotate, **R** scale (toolbar on the left) |
| move | drag an axis arrow, the plane square between two axes, or the dot in the middle (in the screen plane) |
| rotate | drag an axis circle, or the outer circle (around the view direction) |
| scale | drag the cube at the end of an axis; the middle cube changes all three |
| snapping | the magnet button, or hold **Ctrl** while dragging: 5 cm, 15°, ×0.1 |
| local / world axes | a button on the toolbar; scale is always in the object's axes |
| add | **Shift+A**, right-click › Add, the + button or the **Add** menu |
| delete, duplicate | **Del** / **X**, **Ctrl+D** (the copy stands next to the original and collides like it) |
| focus | **F** on the selection (or on everything), double-click on an object |
| cancel a drag | **Esc** undoes what the gizmo moved |
| camera | left drag off the gizmo orbits, middle or Shift+left pans, right drag and the wheel zoom |

The gizmo is the same size at any distance; the axes are colored x red,
y green, z blue, and the handle under the mouse turns yellow. While dragging it shows next to the mouse
how much it has moved, rotated or scaled. Dragging works with the values
the node had at the start, so rounding does not accumulate, and in the history it is
one step (**Ctrl+Z**). While dragging, the simulation waits: it starts again only
with where the object ended up. Several nodes can be selected at once; they move
together and rotation turns them around their common center.

The gizmo knows which parameters a node has (`NodeType::handles`): an object and a source
have position, rotation and size; a vortex has position, axis, radius and height; an attractor
position and radius; wind only direction (rotation turns it); a rain cloud position and
size.

**Add** (Shift+A, right-click) adds an object (sphere, box, cylinder, cone,
torus), a source (fire, smoke), water, weather (rain, storm) or a force (wind,
vortex, turbulence, attractor, drag) where the mouse points on the floor. An object stands
on the floor, gets its own color and is immediately connected to Colliders of all
solvers and of rain. Fire and smoke are connected to a solver and, when there is none, one is created
together with a look and an output. Forces are connected to Forces of the solvers and of rain.

**G** turns on the guides: the domain outline, sources (in orange, with a velocity arrow and
motion path), vortices and attractors, wind arrows, the rain cloud with arrows showing where
the drops fall, the outline of selected objects. Each force is drawn once, even if
it acts in several solvers.
The camera icon saves a snapshot as a PNG.

### Camera and shot

![Editor: the view through the shot camera of the pond in the rain, the 1280 × 720 image frame and the camera parameters](img/editor-camera.png)

The camera (the **Camera** node) is the shot: where it looks at the scene from, with which
lens and how large the image is. Connected to the **Camera** input of the
Output node, it determines what `prototype sim`, **File › Render Image** and
**Render Frames** render. Without a camera, the viewport view is rendered.

| action | how |
|---|---|
| add a camera | **Shift+A › Shot › Camera**: it sees what the viewport currently sees and is connected to Output |
| look through the camera | **0** (on the numeric keypad too), the eye button in the viewport header |
| camera from view | **Ctrl+Alt+0**: the camera moves and turns so that it sees what the viewport sees; its lens and image stay |
| move, rotate | select the camera pyramid by clicking, gizmo **W** and **E** |
| leave the camera | drag or wheel in the viewport, **F**, double-click |

When looking through the camera, the viewport shows the image frame in the camera's aspect ratio
(for 1280 × 720 it is 16 : 9) with the surroundings darkened. At the top are the camera's
name, focal length and resolution. Moving the mouse leaves the camera view;
the camera stays where it was. A shot is thus tuned the same way as in Blender: find
the view, **Ctrl+Alt+0**, check with **0**.

The lens behaves like a full-frame camera's (sensor 24 mm high): the vertical
field of view is `2 · atan(12 / focal)`. 38 mm gives 35° like the viewport,
24 mm is wide-angle, 85 mm portrait, 200 mm a telephoto that flattens
depth. The camera looks along its −z axis; rotation works as for objects (degrees
around x, then y, then z).

The render has no guides and no selection highlight, and is done at twice the resolution
averaged down (edge anti-aliasing).

### Timeline and cache

The simulation runs in its own thread and remembers every frame. The orange bar
on the timeline shows how far it has been computed. Playback and moving along the timeline (click,
drag, **Space**, **Home**, **End**, arrows) then recompute nothing.

- A change that alters the simulation (source, force, solver) restarts it from
  frame 1. The playhead stays where it was, and the viewport shows the last
  finished frame before it until the simulation catches up ("simulating… 12 of 45").
- A change of look (the Volume Look node) only re-renders the frame.
- Frames are kept at half precision (half float): 6 B per cell, at
  64 × 96 × 64 cells 2.4 MB per frame, 150 frames 350 MB. Memory for the cache
  is 1.5 GB (the **Simulation › Cache Size** menu); when it runs out, the simulation
  stops and the status line shows "full".
- **Simulation › Simulate Ahead** turns off computing ahead,
  **Simulate Again** discards the cache and starts over. The cache size,
  spilling to disk and Simulate Ahead stay as they were.
- **Simulation › Save Cache to Disk** saves the computed frames to a folder
  (in the background, with a progress window and a Stop button),
  **Load Cache from Disk** takes them from there instead of simulating (on disk they are
  a fraction of the size thanks to the skipped zeros, [cache.md](cache.md)).

### Files, undo

**Ctrl+N/O/S**, **Ctrl+Shift+S**, examples in **File › Examples**; **New**
gives an empty scene. An opened example is saved via Save As.

Unsaved changes are never lost without asking. New, Open, Open Asset, an example,
**Quit** (Ctrl+Q) and closing the window all ask first:

- **Save** (Enter) saves and then continues. A network without a file is saved via
  Save As, and if you cancel the dialog, nothing happens.
- **Don't Save** (D) discards the changes.
- **Cancel** (Escape) leaves everything as it is.

Inside an asset, Save saves its new version and then the scene. Quit asks
about both networks, simulation and shaders. A file is written in full next to the original
and only then replaces it, so a crash or a full disk in the middle of saving
leaves the original file intact (assets likewise). Saving under the name of a file
that already exists requires confirmation: Save a second time (Replace) overwrites it,
Escape withdraws the question.

**Autosave.** The editor continuously saves an unsaved network (and the shader graph)
on the side: right after the first change and then at most every 30 s. Backups go to
`~/.local/state/prototype/recovery` (`$XDG_STATE_HOME/prototype/recovery`),
elsewhere with `--recovery DIR`. After saving, discarding the changes or quitting,
the backup disappears. When the editor crashes or something kills it, the next launch
offers it (**Recover Unsaved Work**):

- **Recover** (Enter) opens it unsaved, and Ctrl+S saves it where it
  was.
- **Discard** deletes it.
- **Later** leaves it for next time; the offer is also in **File › Recover Unsaved
  Work**.

Inside an asset, the scene is backed up as it was when the asset was entered.
**File › Render
Image** saves a frame, **Render Frames** the whole shot as numbered PNGs
and **Render Video** the whole shot to video — both frame by frame in the background, with a progress
window and a Stop button, and through the camera if the network has one ([render.md](render.md)). **Export Geometry** and **Export Geometry Frames** write
the geometry of the displayed node to PLY, OBJ or OpenVDB — the frame on screen,
or all of them ([cache.md](cache.md)). All frames — Export Geometry Frames,
**Export USD Scene**, **Export Alembic** and Save Cache — are written in the background
with a progress window: meanwhile the editor draws and plays back, but the network does not
change until the end. **Stop** finishes after the frame in progress, and what has been written stays
complete: the cache, the scene and the archive end with the last written frame. The file dialog shows folders and files
with the given extension, and the path can be typed; where a folder is chosen, it also accepts the open one.

**Ctrl+Z / Ctrl+Shift+Z** restores whole states of the network. A drag of a slider or a node
is one step, not a hundred. The same goes for a quick series of changes, for example a brush
shrunk notch by notch: the step is recorded once there has been 0.4 s of quiet. The history holds
at most 300 states and 256 MB, the oldest go first. The network is serialised to text
only on a change, not several times per frame, so even a large sculpt does not slow
the editor down.

## 3. The simulation network

```
[Pyro Source] ──Source───┐
[Turbulence] ───Force────┼──▶ [Pyro Solver] ──Gas──▶ [Volume Look] ──Look──┐
[Object] ───────Collider─┘                                                 │
[Water Source] ─Water──────▶ [Liquid Solver] ─Liquid─▶ [Water Look] ──Look──┼──▶ [Output]
[Wind] ─────────Force──────▶ [Rain] ───────────────────────────────Look──┘      ▲
[Camera] ──────────────────────────────────────────────────────────────Camera───┘
```

Pins have a type and a color: **Source** orange, **Force** turquoise,
**Collider** blue, **Gas** purple, **Look** green, **Water** and
**Liquid** blue (water, [§5](#5-water)), **Camera** light gray (the shot,
[§2](#camera-and-shot)). Rain ([§6](#6-rain-and-wind)) is a solver and a look
in one: its output is directly an Output layer. An output goes only into
an input of the same type. The solver inputs Sources, Forces and Colliders take
any number of wires (drawn as a small rectangle instead of a circle). Forces are
applied in the order in which they were connected. Only what leads to the
Output node is simulated.

Objects are an exception: they always belong to the scene, are drawn and cast shadows even
when they are not connected anywhere. Connected to a solver's Colliders, they additionally stand
in its way. A backdrop that collides with nothing is therefore an object without a wire.

The network is translated into a scene description for the solver
([`src/pg/sim/Network.h`](../src/pg/sim/Network.h)) and at the same time watches for anything that
would not do what the user expects:

- Output, a look or a solver is missing (error, there is nothing to simulate); a look without
  a solver (error, the other layers keep running);
- a solver has no source; a source adds nothing; a source is outside the domain or inside
  an obstacle; an obstacle is outside the domain (warning);
- an unknown node type from a file from a newer version is preserved and reported.

### Nodes

**Objects** (Objects) — the solid bodies of the scene.

| node | parameters |
|---|---|
| Object | `shape` (sphere, box, cylinder, cone, torus, mesh); `file` (an OBJ file, when the shape is mesh); `center` (position of the center), `rotation` (degrees around x, then y, then z), `size` (width, height, depth in the object's own axes: diameter of a sphere, edges of a box, width and thickness of a torus, dimensions of a model); `color` |

The shape fills `[-size/2, size/2]` in its own axes: a cylinder and a cone stand
along their y axis (the cone has its tip at the top), a torus lies in the xz plane and is
`size.y` thick, and a model has its bounding box stretched to `size`. Different
sizes stretch the shape: a sphere becomes an ellipsoid. A color change does not restart
the simulation.

An object, a smoke source and a water source have a **Shape** input: the geometry in it
(from nodes of the Geometry category — cube, sphere, copy to points, OBJ file…)
is their shape instead of their own. Details in [geometry.md](geometry.md).

#### Models from OBJ

![The arch example: a stone arch and a rock from OBJ files, the selected arch with the gizmo and the file path](img/editor-mesh.png)

An object and a source can both have the **mesh** shape: a model from an OBJ file (a rock,
a statue, a car, a chimney). **Add › Mesh…** (Shift+A) opens a dialog, stands the model
on the floor where the mouse points, and connects it to Colliders like other
objects. If the model in the file is huge or tiny (over 3 m or under
5 cm), it is scaled down or up to 80 cm. Otherwise it keeps its dimensions.
The parameters show the path and a **…** button. A relative path is read from the folder
of the network file (for built-in examples from `examples/sim`), so a network and its models
can be copied together. In `.pgsim` the path is in quotes:
`param file "../models/arch.obj"`.

From OBJ, vertices and faces are read in all notations (`f 1 2 3`, `1/1`, `1//1`,
`1/1/1`, negative indices). Polygons are split into triangles; normals,
textures and materials are skipped. Numbers are read independently of the locale.

For the simulation, a **distance field** (SDF,
[`src/pg/sim/Mesh.h`](../src/pg/sim/Mesh.h)) is baked once from the triangles on a grid of 48 cells along the
longest side of the model. The procedure is like Bridson's makelevelset3:

1. the exact distance to the nearest triangle in a narrow band around the faces;
2. fast sweeping extends it to the whole grid;
3. inside and outside are determined by counting ray crossings along x, y and z
   (an odd count means inside) and a two-out-of-three vote. A model with a small
   hole is thus not turned inside out.

From the field one can then quickly find whether a point is inside and how far it is from the
surface: the solver uses this to mark solid cells, and a source from a model (a burning
car) emits in a band below the surface. The same file (path, size, modification
time) is read and baked only once, as long as something uses it.

The renderer draws the actual triangles: it rasterises them into a buffer (normal,
which object, distance), from which the main pass shades them like other
bodies. Normals are smoothed between faces that bend by less than 60°, so
round stays round and the edges of a box stay sharp. A model casts shadows on the floor, on other
bodies and on smoke by a ray marching through the distance field
(at most 4 models with shadows; further ones are only drawn).

**Sources** (Sources) — where the gas is created.

| node | parameters |
|---|---|
| Pyro Source | `shape`, `file`, `center`, `rotation`, `size` as for an object; `fuel`, `smoke`, `heat` per second; `velocity` (the gas leaves the source at least this fast, in the source's own axes, so a rotated source points elsewhere); `expansion` (1/s: how fast the gas in the source expands — it pushes it in all directions like an explosion or air displaced by a collapse); `flicker`, `flicker_size`, `seed` (flickering by noise that rises with the source); `start`, `end` (time window: the flash of an explosion); `motion` (static, circle, sway), `motion_size`, `motion_period` |

A sphere is a campfire, a box a burning log or a vent, a torus a gas burner.

**Forces** (Forces) — each has a `mask`: it acts everywhere, only where there is heat (or
fuel), or only where there is smoke.

| node | what it does | parameters |
|---|---|---|
| Turbulence | random vortices that change over time | `strength`, `scale` (size of the vortices), `speed` (how many times per second they change), `seed` |
| Wind | wind, steady or in gusts | `direction`, `speed`, `strength` (how quickly the gas takes on the wind speed), `gusts` |
| Vortex | spins the gas around an axis, carries it along the axis and sucks it towards it | `center`, `axis`, `radius`, `height` (0 = through the whole domain), `speed`, `lift`, `suction`, `strength` |
| Attractor | attracts towards a point; a negative strength repels | `center`, `radius`, `strength` |
| Drag | slows down: thick, still air | `strength` |

**Pyro Solver** (Simulation):

| section | parameters |
|---|---|
| Domain | `size` (width, height, depth in meters; stands on the floor), `resolution` (cells along the longest side, 16–1024), `closed_floor`, `sparse` (compute only tiles with gas, on by default), `cutoff` (below this value of smoke, heat, fuel and flame the solver releases a tile), `gpu` (step the gas on the graphics card through Vulkan, the same result to the bit; [gpu.md](gpu.md#7-the-gas-on-the-gpu)) |
| Time | `substeps`, `pressure_cycles`, `seed` (the frame rate is shared, in the Output node) |
| Motion | `buoyancy`, `weight` (weight of the smoke), `vorticity` (vortices that a coarse grid smears out) |
| Combustion | `burn_rate`, `heat_release`, `soot_release`, `expansion`, `flame_life` |
| Dissipation | `cooling`, `smoke_decay` |
| Water | `quench` (how strongly the Liquid Solver's water and the Rain drops put out the fire they get into: 0 not at all), `steam` (how much steam — a separate white gas field — each unit of heat taken by the water makes), `steam_lift` (how strongly the steam rises), `steam_fade` (how fast it thins out, 1/s), `evaporate` (how fast the fire evaporates water and drops that are in it: 0 not at all); see [quench.md](quench.md) |

**Pyro Upres** (Simulation): the Pyro Solver's gas again, on a grid two to
four times finer, with vortices that the coarse grid cannot hold
([§4](#upres-coarse-simulation-fine-image)). The Pyro Solver goes into its Gas input,
its Gas output into Volume Look (or into Gas Volume): the frames then hold its
gas instead of the solver's gas. Bypassed, it passes on the solver's gas.

| section | parameters |
|---|---|
| Upres | `scale` (how many fine cells correspond to one solver cell along each axis: 2 is eight times more cells, 3 twenty-seven times, 4 sixty-four times; the fine grid has at most 2048 cells along the longest side) |
| Whirls | `turbulence` (how strongly the fine gas swirls where the solver's flow rotates: 1 as in turbulent flow, more for a wilder fire, 0 with no new vortices — just the solver's gas carried more finely), `swirl_size` (the largest added vortices in solver cells; smaller ones are added along with them), `swirl_life` (seconds for which the vortex pattern is carried with the gas before it turns into a new one), `seed` |

No upres parameter can be animated: the grid and the vortices are given by the first
frame.

**Volume Look** (Render): smoke color and density, `occlusion`; steam color
and density (`steam_color`, `steam_density`, [quench.md](quench.md#steam));
fire brightness, the temperature where it starts to glow (`flame_start`) and where it glows white-hot
(`flame_range`), `fire_light` (how the fire lights the smoke, the floor and
obstacles). Changing it does not restart the simulation.

**Output** (Render): the end of the network and the shared environment of the whole scene. The
Looks input takes any number of layers (a gas, water or rain look); all of them are
simulated at one frame rate and drawn into one image. The
Camera input takes one camera: the view through which it renders.

| section | parameters |
|---|---|
| Output | `frames` (length of the timeline), `fps` (frames per second for all solvers), `preview` (how fine the gas and water grids are that the editor preview computes, Simulation → Preview Resolution: 0.5 half, 0.25 quarter), `open_preview` (the editor opens the network straight in preview: a scene that cannot be computed in full while working) |
| Sun | `light_azimuth`, `light_elevation`, `light_color`, `light_intensity` |
| Sky | `sky_color`, `sky_intensity` |
| Image | `exposure`, `floor` (a disabled floor hides nothing: even geometry below y = 0, such as a terrain valley, is visible), `ground_color` (ground color: asphalt, concrete, dust), `grid` (a grid on the ground every 10 cm and every meter; turn off for a shot), `sky_behind` (sky behind the scene instead of the dark studio background: haze brightest at the horizon and a glow around the sun — outdoors, smoke against the light) |

**Camera** (Render): the shot ([§2](#camera-and-shot)).

| section | parameters |
|---|---|
| Camera | `center` (where it stands), `rotation` (degrees around x: tilt, then y: turn, then z; at zero it looks along −z, horizontally) |
| Lens | `focal` (focal length in mm, full frame) |
| Image | `width`, `height` (render resolution, also the aspect ratio of the frame) |

### The .pgsim file

Plain text, one fact per line, stable order, so networks compare well
in git. Only parameters that differ from their defaults are saved:

```
pgsim 1
# A campfire: a flickering ball of fuel just above the floor.
node 1 pyro_source 2 fire 0 0        # id, type, type version, name, x y in the editor
  param fuel 14
  param heat 1
  param velocity 0 0.4 0
  param flicker 0.7
node 2 turbulence 1 turbulence 0 96
  param strength 3.5
node 3 pyro_solver 2 solver 250 28
  param buoyancy 0.9
node 4 volume_look 2 look 490 28
node 5 output 2 output 720 28
  param fps 24
link 1.source -> 3.sources
link 2.force -> 3.forces
link 3.gas -> 4.gas
link 4.look -> 5.look
```

Choices are written by name (`param motion circle`), toggles as `on`/`off`,
vectors as three numbers. `bypass` on its own line bypasses the node. A file
with an unknown parameter or a wire that does not fit is loaded and the issue is
printed; an unknown node type is preserved along with its parameters, so that a newer program
can read it.

Files from older versions are loaded: a node carries the version of its type, and an obsolete
type is converted to its successor on loading. Sphere Source and Box Source become
Pyro Source with a sphere or box shape (the radius is converted to a size),
Sphere Collider and Box Collider become Object. Pyro Solver version 1 had
`fps`, and Volume Look version 1 had the sun, sky, `exposure` and `floor`; on
loading they move to the Output node they lead to (a look that leads to
no Output discards them). A saved file already has the new types. A test
checks that the examples are in exactly the form in which the program saves them.

### Examples

![Examples: campfire, torch, fire in the wind, explosion, smoke, smoke around a sphere, tornado](img/sim-examples.png)

| example | what it shows |
|---|---|
| `campfire` | flickering fuel, turbulence only in the heat, dark soot |
| `torch` | a source that circles: the flame trails behind it |
| `windy_fire` | gusty wind, the flames bend, the smoke drifts off to the side |
| `explosion` | 0.2 s of fuel with large expansion: a fireball, then a mushroom of soot |
| `smoke` | warm smoke in the sun |
| `smoke_sphere` | smoke hits a sphere, spreads over it and flows around it |
| `smoke_plume` | a ground smoke plume rolling at the camera, after a stock element: a wide, flat burst of heavy, cold smoke and two jets beside it swell into one low dome of billows; two turbulences and a breeze along the ground, a Pyro Upres twice as fine for the curls; lit from behind the camera; the camera low in the box, swallowed at the end |
| `tornado` | a vortex with suction and lift picks up smoke from the floor |
| `obstacles` | smoke between objects: it hits a slanted plate, flows up along it and rises to a torus; the sphere on the floor is a backdrop without collisions |
| `arch` | models from OBJ: wind drives smoke through a stone arch and around a rock ([`examples/models`](../examples/models)) |
| `dam_break` | water: a block of water in the corner of a tank bursts, flows around a pillar, climbs up the opposite wall and sloshes over |
| `waterfall` | water from a spring on a ledge falls onto a slanted plate, runs down it and fills a pool |
| `splash` | a ball of water falls into a pool: a crown splash, then the cavity closes and shoots up a column (Worthington jet) |
| `rain_pond` | rain on a pond: ripples on the surface, splashes off a rock, a gusty breeze and a wet floor; the camera low above the water |
| `storm` | a campfire in a storm: wind gusts flatten the flames and tear away the smoke, the rain slants in the same wind and splashes off the logs; the camera low by the fire |
| `lakeside` | a shot: a campfire on the shore of a pond in the rain. Smoke, water, rain, wind and objects in one network (17 nodes), the pond sunk into terrain made of boxes, the camera low above the water |
| `scatter_fire` | geometry as a source: points scattered over a grid, a wrangle gives them a size, a small flame on each; a sphere from the Sphere node hangs in the smoke ([geometry.md](geometry.md)) |
| `liquid_points` | the simulation back as geometry: water particles from Liquid Points, a wrangle colors them by velocity, the attribute spreadsheet shows them |
| `rock_garden` | geometry as an obstacle: a sphere copied to scattered points and squashed is the shape of rocks that it rains on |
| `wake` | animation: a sphere with position keys travels through a pool, the water takes on its motion — a wave in front of it, a wake behind it ([animation.md](animation.md)) |
| `fire_trail` | animation: a torch flies in a loop and leaves a trail of fire and smoke, a paddle animated around y swirls the smoke above it |
| `campfire_vdb` | export: a campfire with a Gas Volume node whose volumes go to OpenVDB frame by frame ([cache.md](cache.md)) |
| `vdb_fireball` | reading OpenVDB: a fireball from a file — VDB Gas plays it back as gas, nothing is simulated, the fire lights up the crates ([vdb.md](vdb.md)) |
| `vdb_rock` | reading OpenVDB: a stream of water hits a boulder from a level set, which VDB Import turned into polygons and an Object obstacle ([vdb.md](vdb.md)) |
| `alembic_shot` | Alembic: a backdrop and a moving camera from Blender, a fire between crates and its smoke along the back wall ([alembic.md](alembic.md)) |
| `campfire_rain` | water and fire: a campfire burns for a second, then a downpour comes — drops that fall through the flames cool them, those that land on the fire soak it; within a few seconds the flames disappear and white steam rises from the wet logs ([quench.md](quench.md)) |
| `fire_douse` | a campfire put out with a bucket of water: a ball of water falls on the fire, the flames disappear within a few frames, a white cloud of steam billows through the dark smoke and the water runs off across the ground ([quench.md](quench.md)) |
| `fire_hose` | putting out a fire with a hose: a stream of water is aimed for two seconds into a fire of logs — a small part of it evaporates in the flames, the rest gradually puts the fire out; white steam rises through the dark smoke and thins out at the top ([quench.md](quench.md)) |
| `rain_fill` | a downpour fills a stone tank: every drop that falls into the water ripples it and adds to it — the level rises by 7 cm in five seconds and floods a step ([quench.md](quench.md)) |
| `campfire_upres` | upres: a campfire computed at resolution 64 and drawn at 192 — Pyro Upres ×3 adds vortices that the coarse grid cannot hold: the flames tear, the soot curls at the edges; in half the time and memory of a simulation at 192 ([§4](#upres-coarse-simulation-fine-image)) |
| `demolition` | destruction: the demolition of a tower block between houses — charges on the ground floor, the tower collapses into its footprint and the floors crush; dust from impacts, crushing and broken bonds is driven into the streets by the displaced air ([destruction.md](destruction.md)) |
| `wall_collapse` | destruction up close: the façade of a brick house flies out into the street, pieces roll towards the camera just above the asphalt and a low sun shines through the dust ([destruction.md](destruction.md)) |
| `concrete_wall` | reinforced concrete: a wrecking ball breaks through a wall on a plinth — Concrete Fracture with rough fractures and chipped corners, a network of bars (Rebar) on which the pieces around the hole hang, `rings 2` keeps the damage around the ball, dust from the fractures ([destruction.md](destruction.md#2-concrete-fracture)) |
| `concrete_drop` | reinforced concrete and secondary fracturing: a beam with a rebar cage cracks over a box, bends and hangs on the reinforcement — RBD Cluster, glue inside the chunks thirty times stronger, Rebar ([destruction.md](destruction.md#reinforcement-rebar)) |
| `glass_window` | glass: a ball flies through a window, in slow motion (120 frames per second) — Glass Fracture, a spiderweb of cracks only at the moment of impact, shards and glittering grit, transparent glass with reflections ([destruction.md](destruction.md#glass-glass-fracture)) |
| `brick_wall` | bricks: a wrecking ball breaks through a house's brick wall in English bond — Brick Wall, mortar as glue, the wall cracks along the joints, the hole is stepped by courses, some bricks break in two; a window with glass next to the hole stays intact ([destruction.md](destruction.md#bricks-brick-wall)) |
| `constraint_network` | a constraint network: RBD Constraints turns the glue of a concrete wall into geometry, a wrangle weakens the bonds across a corner-to-corner line — the ball breaks out a corner and the wall cracks exactly along the line, the rest stands; RBD Pieces returns the frame's network with the broken bonds ([destruction.md](destruction.md#constraint-network-rbd-constraints)) |
| `debris_stairs` | debris as particles: a charge cuts a concrete column on a landing, the column topples down the stairs and breaks; the debris bounces down the stairs and stays lying on the steps, dust trails behind the thrown pieces (`trail`) ([destruction.md](destruction.md#debris-as-particles)) |
| `concrete_column` | reinforced concrete: blasting a column at half its height — Concrete Fracture and a rebar cage (Rebar), the concrete around the charge flies apart and disappears in dust, leaving the bare cage with pieces of concrete hanging on it ([destruction.md](destruction.md#seventh-example-blasting-a-reinforced-concrete-column)) |
| `guided_fall` | a guided simulation: blasting a concrete chimney into the street — a keyed Transform around the edge of the notch is the RBD Solver's Guide, the chimney falls exactly between two houses and breaks up freely on the road (`guide_let_go`, `guide_reach`) ([destruction.md](destruction.md#guided-simulation-guide)) |
| `shatter_blocks` | fracturing at runtime: a ball passes through three whole concrete blocks and each breaks where it was hit — fragments smallest around the impact, rough fractures, dust and debris ([destruction.md](destruction.md#breaking-during-the-simulation)) |
| `shatter_grit` | debris as grains: `shatter_blocks` with a Grain Solver whose Grit input takes the RBD Solver — each bit of debris becomes a grain as soon as it flies out of a piece: it hits the others, lands on fragments, slides off them and lies around the rubble, piled on itself where more of it fell ([grains.md](grains.md#concrete-grit-as-grains)) |
| `wood_beam` | wood: a steel ball breaks through a wooden beam — Wood Fracture splits it into long splinters along the grain, and the splinters the ball hits break at runtime with frayed ends ([destruction.md](destruction.md#wood-wood-fracture)) |

The files are in [`examples/sim`](../examples/sim) and CMake compiles them into the
program. `prototype sim campfire` therefore works without any files alongside.
The campfire and smoke are also the scenes the solver tests are built on: a test
checks that the network gives exactly `Scene::fire()` and `Scene::smoke()`.

## 4. How the simulation works

The domain is a box on the floor divided into cubic cells. The cell edge is the
longest side of the domain divided by the resolution, and the cell counts are rounded up
to a multiple of 8 so that the multigrid can halve them several times. Each cell holds
fields:

| field | meaning |
|---|---|
| `density` | smoke, soot: what absorbs and scatters light |
| `temperature` | heat: lifts the gas upwards |
| `fuel` | fuel that has not burned yet |
| `flame` | fuel burned during the last `flame_life`: where the flame is |
| `velocity` | flow velocity, three components |

One step ([`src/pg/sim/Pyro.h`](../src/pg/sim/Pyro.h)) has six phases:

1. **emit** — sources add fuel, smoke and heat and push the gas in their
   direction. The weight falls off smoothly towards the edge of the source (smoothstep); the output varies
   with noise that rises with the source: the flame flickers. The source velocity
   is "at least this much": gas that is already moving faster is not slowed by the source.
   A moving source drags the gas along with it.
2. **advect** — the flow carries all fields, including the velocity itself.
3. **combust** — part of the fuel burns (`burn_rate` per second) and turns
   into heat, soot and flame. Burning gas expands.
4. **forces** — heat rises, soot sinks, vorticity confinement restores
   vortices, then the network's forces in the order of their wires.
5. **project** — pressure ensures the gas is incompressible, except where it
   expands through burning. Nothing flows through the floor or an obstacle.
6. **dissipate** — smoke thins out, heat cools, flames die down; what the gas carries
   is diluted where it expands.

### Advection

The simplest method is to take the value from a cell and move it by the velocity.
With a large step, however, it is unstable and the simulation blows up. Instead,
the **semi-Lagrangian method** is used (Stam, *Stable Fluids*, 1999): for each
cell, it finds *where* the gas flowed in from during the step, and reads the value there
by trilinear interpolation. Such a method is stable at any step size. The path
back is computed with second-order Runge-Kutta: first half a step, then a full step
with the velocity from the midpoint. Thanks to this, vortices do not dissolve into spirals.

Interpolation, however, blurs. Smoke, heat, fuel and flame are therefore advected
with the **MacCormack** method (Selle et al., 2008): the result is advected once more,
this time against the flow, and compared with what was in the cell at the start.
This round-trip test reveals the error, and the result is corrected by half
of it. So that the correction does not create new extrema, it is clamped to the range of the eight
values it was interpolated from. On open boundaries the correction is
turned off: the path there leads out of the domain, from which zero comes back, and the "correction"
would add smoke near the boundary.

With the Pyro Solver's **GPU** on, all of this runs on the graphics card,
each cell computed as here, operation for operation: the same gas to the
bit ([gpu.md](gpu.md#7-the-gas-on-the-gpu)).

### The MAC grid

Smoke, heat, fuel and flame are at cell centers. Velocity is
**staggered** (MAC grid, Harlow and Welch, 1965): the x component lies on the faces between
cells in the x direction, the y component on the faces in the y direction, and so on. A grid of `n`
cells therefore has `n + 1` faces in a given direction. The divergence of a cell, that is, how much
gas flows out of it, is then computed exactly from its six faces:

```
div = (u[i+1] − u[i] + v[j+1] − v[j] + w[k+1] − w[k]) / h
```

On a grid with everything at the centers, the divergence would be computed across every other cell. Even and odd
cells would then not "see" each other, and the pressure would form a checkerboard in them
that the projection cannot remove. The staggered grid also makes obstacles easy:
the face between gas and a solid cell has zero velocity, and that is it.

### Projection and pressure

The gas in the simulation is incompressible: as much flows out of a cell as flows into it.
Forces and advection violate this property, so it is corrected at the end of the step.
A pressure `p` is found whose gradient exactly balances the divergence, and it is
subtracted from the velocity. Finding the pressure is a Poisson equation:

```
sum over open faces (p[neighbour] − p) / h² = div − expansion
```

Where fuel burns, the divergence should match the expansion of the gas (`expansion`).
There the gas really does increase.

The equation is solved with a **geometric multigrid**
([`Poisson.h`](../src/pg/sim/Poisson.h)). Simple iterations (Jacobi,
Gauss-Seidel) quickly even out the error between neighboring cells, but an error
that stretches smoothly across the whole domain shrinks only
slightly with each pass. The finer the grid, the worse. Multigrid sends the smooth part of the error
to a grid with half the resolution, where it is no longer so smooth, and repeats this down
to a few cells. Then it adds the corrections on the way back up. Such a V-cycle removes about
90 % of the error **at any resolution**. Two V-cycles per step are enough
(`pressure_cycles`), because it starts from the pressure of the previous step. Smoothing is done
with red-black Gauss-Seidel: the cells are colored like a checkerboard and each
half reads only cells of the other color. Each half therefore runs in parallel, and the result
still does not depend on the number of threads — nor on whether it runs on the CPU or, with the
Pyro Solver's **GPU** on, on the graphics card ([gpu.md](gpu.md#7-the-gas-on-the-gpu)).

Boundaries and obstacles are built directly into the operator:

- **Open faces** (sides, ceiling, floor when it is not closed) hold zero pressure
  at the face, like the atmosphere outside: the cell beyond the face has minus the pressure
  of the cell inside.
- **A closed floor** is a wall: nothing is summed across it (Neumann condition).
- **Obstacles:** each cell face has a coefficient of 1 (open) or 0 (leads
  into a solid cell). On a coarser multigrid level, the coefficient is the average of the four
  fine faces that the coarse face covers. Solid cells have zero pressure and
  their faces zero velocity. Multigrid with an obstacle thus converges almost
  as fast as without it.

### Open boundaries: the air outside is still

Gas that flows in through an open face flows in from the surrounding still air:
its velocity is zero and only pressure gives it velocity. The first version instead
took the velocity that the gas had near the face (extrapolation, as many
solvers do). A vortex that reached the ceiling then sucked in air along its axis; that air
flowed in with the velocity the vortex gave it, the vortex accelerated it, and the face sent that
velocity inside again. The velocity grew without limit (20 m/s after two
seconds for a vortex spinning at 1.2 m/s), until the smoke broke up into
individual points. With still air outside, the vortex keeps its velocity below its own
(test `pyro_a_vortex_through_the_open_top_stays_bounded`). The same bug
had previously silently kept the outflow going even after an explosion burned out: the smoke then
left through the ceiling faster than it should have.

### Sparse grid: compute only where there is gas

The dust of a demolition occupies only a few percent of the cells in a 90 × 48 × 90 m domain; the rest
is still air. Houdini (Sparse Pyro) and production with OpenVDB therefore keep
and compute only the cells near the gas; this solver does the same when
`sparse` is on (the default in the node).

- **Tiles.** Each field is stored in tiles of 8 × 8 × 8 cells
  ([`SparseGrid.h`](../src/pg/sim/SparseGrid.h)): a tile table for the whole
  domain and values only for the active ones. A cell outside them reads 0, still
  empty air. Fields of the same shape share tiles, a cell has the same index in all
  of them, and a loop over active cells reads and writes all fields
  at once.
- **Which tiles.** Before each step, the solver releases tiles where smoke,
  heat, fuel and flame have all dropped below `cutoff`. Around the remaining ones, around sources
  and around moving bodies it adds as many tiles as the fastest
  air can reach in a step, plus one cell (at least one tile, at most
  four). The gas thus never flows past the edge.
- **MAC faces.** Velocities lie on cell faces. A tile holds the faces
  of its cells; the far faces of its last cells are the first layer of
  the next tile, which the face grid adds for itself (`Tiles::faces`).
- **Pressure.** The equation holds in active cells, and outside them p = 0, as on
  the open side of the domain. On coarser multigrid levels a cell is
  active only when all eight of its children are active (as in McAdams, Sifakis
  and Teran, 2010): still air thus never reaches further on the coarse grid
  than on the fine one. At first it was the other way round (active when at least one), and at
  resolution 288 the demolition dust blew up into NaN around frame 90: coarse
  cells half in the air sent up corrections that missed, and together with
  bodies they closed off some regions like walls. Now each V-cycle reduces the
  residual twenty to a hundred times. Body faces are held at the finest level as
  bits (six in a byte per cell), not as three face grids.
- **Identical bits.** With all tiles active (`sparse` off) the
  solver is dense and gives bit-identical fields to the previous dense version. This is verified
  by the fingerprint of the last frame in `pgbench_pyro --dense`: `3adea3c11ea39814`
  before the rewrite and after it. (The sweeps have since multiplied by 1 / the diagonal
  instead of dividing by it, as the GPU's do — [gpu.md](gpu.md#7-the-gas-on-the-gpu) —
  and the bits changed with that.)
- **Cost.** Air outside the tiles is still. A pressure wave does not propagate through the whole
  domain, only through the tiles around the gas. For dust and smoke this is not visible
  in the image (measurements in chapter 8). For a very fast explosion more Substeps help:
  the edge then grows in smaller steps.

Frames hold only tiles with gas (`Frame::gasTiles`, cache version 10).
The renderer, the VDB/USD export and Python assemble the full grid from them only when
they need it.

### Expansion dilutes

Semi-Lagrangian advection carries a value as it is: a parcel of gas that neither
expands nor compresses has the same smoke density after moving. Where
burning gas swells, however, this is wrong: the same amount of fuel spreads
into a larger volume. Without dilution, the swollen fuel stayed just as dense,
kept burning and swelled again. An explosion thus filled the whole
domain within a few frames (100 % of cells in flame, in the test
`pyro_burning_gas_thins_out_as_it_swells`). Smoke, fuel and flame are therefore
diluted in a cell that expands by `expansion × dt` of its volume during a step, by
dividing by `1 + expansion × dt`. The temperature stays: gas that has heated up
stays hot.

### Forces

- **Buoyancy:** `buoyancy × temperature − weight × smoke`. It acts on the vertical component
  of velocity, that is, on the faces between vertically stacked cells.
- **Vorticity confinement** (Fedkiw et al., 2001): the numerical diffusion
  of the semi-Lagrangian method smears out small vortices, and without them smoke looks like
  cotton wool. The force finds places where the gas rotates (the curl of velocity `ω`) and keeps
  it spinning: `ε · h · (N × ω)`, where `N` points towards stronger vortices.
- **Turbulence:** a random force on a coarse grid (a node every `scale`),
  which changes smoothly `speed` times per second. On its own it would only
  compress and stretch the gas; the projection keeps only its rotational part. Thanks to it,
  smoke tears into vortices and flames lick.
- **Wind** and **vortex** do not push the gas but **pull it towards a velocity**: the velocity
  approaches the target every second by a fraction given by `strength`
  (`v += (1 − e^(−strength·dt)) · (target − v)`). However long they act, they never
  accelerate anything beyond their own speed. A vortex has a target composed of three parts: around the axis
  `speed` (fastest at half the radius, profile `4x(1 − x)`), along the axis
  `lift` and towards the axis `suction`. It fades out smoothly at the edge of the cylinder. A tornado
  forms when a vortex with suction and lift picks up smoke from the floor.
- **Attractor** accelerates towards a point the more, the closer it is (`(1 − d/r)²`); a negative one
  repels. **Drag** damps velocity exponentially.
- **Mask** limits a force to heat (or fuel), or to smoke; the fraction is
  derived from the values in the cells next to the face on which the velocity lies.

### Heat and flame separately

Heat must last a long time so that it carries smoke upwards. If the fire were drawn
from temperature, it would look like a glowing column as tall as the whole smoke plume. Houdini
solves this the same way as this simulation: **heat** lifts the gas and cools slowly,
**flame** is freshly burning fuel and lasts a fraction of a second (`flame_life`).
Fire is drawn from the flame and gets its color from the temperature.

### Upres: coarse simulation, fine image

![The campfire after 72 frames: the solver at resolution 64, the same with upres ×3, and the solver computed directly at 192](img/pyro-upres.jpg)

A grid twice as fine has eight times more cells, and each one costs pressure, forces
and velocity advection. Production therefore tunes the simulation coarsely, where it runs
fast, and adds fine detail to it separately: upres (Pyro Upres in Houdini).
The **Pyro Upres** node ([`src/pg/sim/Upres.h`](../src/pg/sim/Upres.h)) takes
the motion of the gas as the solver computed it, and uses it to carry a fine copy of what
the gas carries — smoke, heat, fuel and flame — on a grid `scale` times finer.
It returns nothing to the solver. Pressure, buoyancy and forces are not computed on the fine grid,
which is why it is so much cheaper.

Each frame, just before the solver step (with the flow the solver will use in that
step to carry its gas, so that the two stay together):

1. **tiles**: the fine grid is sparse, in tiles of 8 × 8 × 8 fine
   cells. The ones with gas and sources are computed, and around them as far as
   the gas can travel in a step — according to the flow in that tile, not the fastest
   place in the domain;
2. **sources** add fuel, smoke and heat directly at the fine resolution: the edges
   of the source and its flickering are as fine as the grid (the same code as in the solver,
   which gives the solver bit-identical frames as before);
3. **advection** MacCormack as in the solver, all four fields at once:
   the solver's flow interpolated into the fine cells, plus vortices;
4. **burning** and **fading** as in the solver.

The vortices are **curl noise** (Bridson, Hourihan and Nordenstam, 2007): the curl
of three noises, that is, a flow that neither compresses nor dilutes the gas. They are as strong
as vortices of that size would be in turbulent flow: the vorticity of the solver's flow
times the cell (the velocity across a cell that the coarse grid cannot resolve),
scaled up by the cube root of how many cells the vortices span (Kolmogorov). Where
the flow does not rotate, there are no vortices: smoke that stands still stays calm (test:
bit-identical with and without vortices). Nor near obstacles; the gas goes there wherever the
body lets it. `swirl_size` is in solver cells (default 2: what the coarse
grid just cannot hold); smaller octaves are added down to roughly three fine
cells, each a third of an octave weaker.

The noise is carried with the flow: its coordinates are advected on the solver's grid,
so the vortices move with the gas and stretch with it, instead of the gas flowing through
a stationary pattern. Stretched noise would soon break up into streaks, however, so the
coordinates are in two layers that are alternately renewed every `swirl_life` seconds and
blended (Neyret, 2003). Sin and cos weights, whose squares sum to 1,
keep the strength of the vortices constant. The noise is computed once into a periodic tile of 64³
samples and read trilinearly, as wavelet turbulence keeps its noise (Kim
et al., 2008); this is about ten times faster than computing it in every cell.

Frames hold the upres gas instead of the solver's gas: the viewport, the path tracer and
Cycles, the cache, export to OpenVDB and USD and the Gas Volume node all get the fine
grid as if the solver had computed it. A bypassed upres passes on the solver's gas:
the motion is tuned coarsely and the upres is switched on for the final image. It is in the checkpoint
and continues from a saved state bit-identically, as if it had never stopped, and on
any number of threads it gives the same bits.

The image shows the `campfire_upres` example after 72 frames. On the left, the solver
at resolution 64; in the middle, the same with upres ×3 (144 × 192 × 144 fine
cells); on the right, the solver computed directly at 192. Upres does not invent the large rolling
vortices that a fine solver computes — the coarse grid does not have them — but it adds
fine detail: the flames tear, the smoke curls at the edges, and the motion
of the whole stays the one that was tuned coarsely. What it costs is in
[§8](#upres-after-the-solver).

![Editor: the campfire with a Pyro Upres node between the solver and the look; the info shows the fine grid 144 × 192 × 144](img/editor-upres.jpg)

## 5. Water

![A dam break, a waterfall on a slanted plate and a drop falling into a pool, resolution 64](img/water.png)

Unlike smoke, water has a **surface**: where it ends, air begins, and everything
happens on it: waves, splashes, drops. That is why it is simulated differently, with the
**FLIP** method (Zhu and Bridson, 2005), on which the FLIP solver in Houdini is built. Water
is carried by **particles**, eight in each full cell, and each remembers its velocity.
The **grid** (the same MAC grid as for gas) serves to make the water keep its
volume.

### Network

```
[Water Source] ──Water────┐
[Wind] ──────────Force────┼──▶ [Liquid Solver] ──Liquid──▶ [Water Look] ──Look──▶ [Output]
[Object] ────────Collider─┘
```

Output takes a smoke look and a water look at the same time. Both are simulated at one
frame rate and drawn into one image. Objects and forces can be
connected to both solvers at once.

| node | parameters |
|---|---|
| **Water Source** (Sources) | `shape`, `file`, `center`, `rotation`, `size` as for an object; `mode`: `fill` fills the shape with water once, when the source starts (a block of water, a pool), `flow` pours water out of it (a hose, a fountain, a spring); `velocity` the velocity of the outflowing water in the source's axes; `seed`; `start`, `end` |
| **Liquid Solver** (Simulation) | Domain: `size`, `resolution` (cells along the longest side, 16 to 1024), `closed_sides` (a tank with walls; off: water overflows the edges and disappears), `sparse` (compute only tiles around the water, on by default; off: all tiles, the same water bit for bit, more memory and time); Motion: `gravity`, `flip` (Splash: 1 lively, splashing water, 0 smooth and thick; typically 0.9 to 0.98); Time: `substeps`, `seed` |
| **Water Look** (Render) | `color` (color of deep water), `clarity` (how far you can see into the water, in meters), `foam` (how white the foam and spray are), `surface` (draw the surface; off: the water is simulated, only its particles are visible via Liquid Points) |

In the viewport, water is added via **Shift+A → Water**: *Block of Water*,
*Fountain*, *Hose*. When there is no water solver yet, one is created together with a Water Look
connected to Output, and the scene's objects are connected to it as obstacles. A water
source is selected by clicking and moved with the gizmo like an object.

### How it works

One substep ([`src/pg/sim/Liquid.h`](../src/pg/sim/Liquid.h)):

1. **emit** — a `fill` source fills its shape: into each eighth of a cell inside the shape
   where there is no particle yet, it adds one, at a place given by a hash of the cell and
   substep. A `flow` source does this in every substep and sets the water in its shape
   to its velocity, so it pushes it out.
2. **to the grid** — particle velocities are averaged onto the cell faces
   (trilinear weights). The surface is also computed from the particles: the distance to a sphere
   around the weighted average of the surrounding particles (Zhu and Bridson). A cell whose
   center is below the surface is water.
3. **forces** — gravity, then the network's forces. Wind carries spray: water that is
   flying and has few particles around it (fewer than 12 in a 3 × 3 × 3 cell cube;
   a full cell has 8) is pulled towards the wind velocity with full strength.
   It barely moves the water itself: air is a thousand times lighter and pushes only on the
   surface, so a breeze over a pond leaves the surface flat.
4. **pressure** — makes the water incompressible. In the air the pressure is zero; nothing
   flows through the wall of a body.
5. **to the particles** — each particle adds how much the grid velocity at its
   location has changed (FLIP), mixed with the grid velocity itself (PIC)
   in the ratio `flip`. Pure PIC would blur and slow the water down; pure FLIP is
   lively but noisy.
6. **motion** — particles move with the grid velocity (second-order Runge-Kutta),
   out of bodies and away from the tank walls. Whatever flows out through an open side or flies
   above the domain disappears.

There are as many substeps as needed so that no particle moves by more than two cells,
and at least `substeps`.

**Sparse water.** Water grids are kept in tiles of 8 × 8 × 8 cells
([`SparseGrid.h`](../src/pg/sim/SparseGrid.h)) as with sparse gas, but only
where there is water: tiles with particles, tiles of sources that are about to
pour, and all tiles around them. Everything a substep does reaches only a few cells from
the particles (the particle kernel 1.2 cells, velocity extended beyond the water
4 layers, pressure neighbors), that is, deep into the surrounding tile. The air above a lake
and the empty half of a flood domain cost neither memory nor time. What happens outside
the tiles never reaches the particles, so sparse water is dense water bit
for bit: `sparse` off keeps all tiles, and a test verifies this on a tank
with a hose, a moving body, wind and vortices. Particles are sorted into cells
in the order of the whole grid (x fastest, then y, then z), not by tile, so that
faces and cells gather their contributions in the same order. Bodies have their own
tiles, around each collider: the distance at cell corners,
face openness and cells with their center inside the body.

Pressure ([`FreeSurface.h`](../src/pg/sim/FreeSurface.h)) is solved on the water
tiles, and each coarser multigrid level on the tiles above the tiles of the
finer one. A coarse cell, however, reaches further than the tile below it (on the second level
by four fine cells, on the fourth by sixteen), so walls and bodies are kept on
each level separately, independently of the water (`SolidLevels`): a cell outside the water
tiles is solid and a face open exactly as it would be on a dense grid.
The conjugate gradient sums run in the row order of the whole grid, however
the tiles lie. The pressure is thus identical bit for bit to that on a dense grid;
this is verified by the fingerprints of all water examples and by a test with a pool in the corner of a tank
and a solid block far from it.

The flood with crates (`pgbench_liquid`, 60 frames, 4 threads): at resolution 192
(192 × 48 × 96 cells) dense water takes 1495 ms per frame and 259 MB, sparse 1444 ms
and 231 MB. At 256 × 64 × 128, until the water fills most of the domain, sparse is
2× to 2.4× faster (frames 10 and 20: 1.7 and 4.3 s instead of 4.2 and 8.9 s)
and needs 425 MB instead of 601 MB. A small domain that the water fills (dam
break at 64) is computed equally fast by both.

**Free-surface pressure** ([`FreeSurface.h`](../src/pg/sim/FreeSurface.h)).
The equation is the same as for gas, only it is solved on water cells and the pressure at
the surface is zero. Two things make it more accurate than cell-sized stair steps:

- **Ghost fluid** (Gibou et al., 2002): the surface crosses the line joining the centers
  of a water cell and an air cell at a fraction θ, which is known from the distance field.
  Zero pressure lies there, not at the center of the air cell; the face has a weight of
  1/θ in the equation. A calm surface therefore does not stand on stair steps.
- **Face openness** (Batty, Bertails and Bridson, 2007): a body that crosses
  cells at an angle covers part of a face. The weight of the face is the fraction that
  remained open; it is computed from the bodies' distance field at the cell corners.
  Water therefore runs down a slanted plate smoothly, not in steps.

The water region is irregular and changes every step, so multigrid alone
would converge slowly. The equation is therefore solved with the **conjugate
gradient method** preconditioned by one multigrid V-cycle (McAdams, Sifakis and
Teran, 2010). On coarser grids a cell is water if at least one
of its eight children has water, and a face is open as the average of the four it covers.
The V-cycle is not exactly symmetric, so the flexible variant
of the method is used (Polak–Ribière β). A relative residual of 10⁻⁴ is reached in 10 to
20 iterations and is computed only in the grid rows where there is water.

**Enclosed pockets.** Water that neither air nor an open side of the
tank can reach is a pocket: for example two cells of water between a crate, the bottom and a wall. The pressure
in it is determined only up to a constant, and only when as much flows in as
flows out. A body that moves into the pocket breaks this balance. Then
the equation has no solution and the conjugate gradients run off to infinity. That is how
the flood with crates blew up: a two-cell pocket and a pressure of 10¹⁵. The solver therefore
finds the pockets in every step. It searches the water breadth-first from the cells next to air through
open faces; what it does not find this way are pockets. From the right-hand side of each pocket it then
subtracts its average, so whatever flows into the pocket compresses the water in it equally in all
cells. The average pressure of the pocket stays as it was when it entered
the step. The search costs about 1–2 % of the pressure time and the result is the same on
any number of threads.

**Determinism.** Every step, particles are sorted into cells with a stable
counting sort. The transfer to the grid goes in slabs two cells thick,
first the even ones, then the odd ones. A particle writes at most into the neighboring slab, so
two slabs of the same color never write to the same place, and each place receives
contributions in the same order. The result is bit-identical on any number of
threads.

**Foam** is a property of a particle. White is spray (a particle in the air with
few others around it) and fast water near the surface. In 0.8 s it fades to
a third.

### How water is drawn

A frame carries the surface as a distance field on a grid **twice as fine**
as the solver. The particle spheres are averaged (lone particles are slightly larger
so that the spray holds together), the field is smoothed and **recomputed into a true
distance** (fast sweeping, Zhao 2005). This matters: a ray can then safely jump
through the field and will not skip a thin sheet of water. Distance and foam
take one byte per cell each.

From the sparse solver, a frame carries only the **tiles** of 8 × 8 × 8 cells near the water
(`WaterFrame::tiles`); a cell outside them is air further away than the distance
band reaches, with no foam. A tile **deep in the water** (every cell further below the
surface than the band reaches, and with no foam) is carried only by its number
(`deepTiles`), provided it does not lie on the edge of the domain and all 26 tiles around it
have water: the surface cannot pass around such a tile. The water velocity, from which the surface gets `v` for motion
blur, is carried only in the solver tiles where there is some (`flowTiles`). The cache
is thus 2.3× to 3.3× smaller (12 frames of `dam_break`: 5.2 MB instead of 17.1 MB,
`splash`: 14.7 MB instead of 47.8 MB), and on a large domain where water occupies
a fraction, all the more so. The surface (`waterMesh`: the Liquid Surface node, USD export,
Cycles) is built by surface nets only around the tiles: a cube whose corners all lie
outside them is entirely in the air, so the mesh comes out the same, points
and quads in the same order, as over all cells. The viewport uploads
all cells into a 3D texture; a grid larger than 2²⁸ cells or 2048 per side
is reduced to averages of blocks of 2, 4 or 8 cells. On all nine water
examples, the USD export (mesh, `v`, `foam`) from the sparse solver, from the dense one
and from the sparse cache is identical bit for bit.

The shader finds the surface by **sphere tracing**, and at the hit point:

- it **reflects** the sky, the sun (a sharp highlight) and objects, the more, the more
  obliquely the surface is viewed (the Fresnel effect, Schlick's approximation);
- it **refracts** light (index of refraction 1.33) and follows the refracted ray to the floor or
  to an object under water. What is visible fades with the length of the path through water and takes on
  the water color (`color`, `clarity`): shallow water is clear, deep water has its own
  color;
- it draws **foam** white and matte;
- **shadow**: sunlight passing through the water to the floor is slightly weakened.

Smoke is drawn in front of the water; what is behind the surface is hidden by the water.

### Performance

| example | cells | particles | ms per frame |
|---|---|---|---|
| `dam_break` | 64 × 40 × 32 | 104 thousand | 110 |
| `waterfall` | 64 × 40 × 32 | 80 to 130 thousand | 100 to 125 |
| `splash` | 64 × 56 × 64 | 280 thousand | 240 |

(4 cores, Xeon 2.1 GHz; a frame has 1 to 4 substeps.) A quarter of a substep
goes to the transfer to the grid, half to pressure. After a batch of work, the pool threads keep
watching for more for a while before they go to sleep: waking a sleeping thread takes longer than
many a batch. This sped up the gas too.

### A large run

The `flood_crates_hd` example is the flood with crates at the resolution of a final
shot: 512 × 128 × 256 cells of 1.6 cm (16.7 million) and 17.6 million
particles. `pgbench_liquid 512 --cache DIR` computed it on four cores in
1 h 14 min, 44 frames, and wrote them to a cache from which it is rendered:

| frame | s per frame | tiles with particles | tiles in the solver | substeps | memory |
|---|---|---|---|---|---|
| 10 | 31 | 14.6 % | 17.7 % | 6 | 2.2 GB |
| 20 | 65 | 16.6 % | 24.1 % | 12 | 2.7 GB |
| 30 | 123 | 24.7 % | 42.4 % | 16 | 4.1 GB |
| 40 | 146 | 37.6 % | 67.5 % | 12 | 5.7 GB |

The sparse solver saves the most at the start, when the water is standing in the tank: it holds less than
a fifth of the tiles. The domain is low (2 m), and a wave that spreads across the whole
yard occupies two thirds of them; a frame then takes over two minutes, partly because
fast water needs more substeps. All 120 frames would take an estimated 5 to
6 hours. A frame in the cache is 28 MB at frame 10 and 121 MB at frame 44 (all
44 together 2.5 GB); taking it from the solver and writing it took 5.7 s at frame 10
and 19 s at frame 40, a fifth less since then. A render in Cycles (1280 × 720,
64 samples, Open Image Denoise) takes 2 to 4 minutes per frame and holds up to
8.8 GB: the surface is a field on a grid twice as fine as the solver (1024 × 256
× 512) and the mesh from it has 3.7 million quads at frame 44.

## 6. Rain and wind

![Rain on a pond and a campfire in a storm](img/rain.png)

A raindrop is not water for FLIP. It is small, falls at an almost constant velocity and
its shape does not matter. What matters is where it flies, where it lands and what it does there.
The **Rain** node therefore simulates drops as separate particles
([`src/pg/sim/Rain.h`](../src/pg/sim/Rain.h)): they fall from a cloud, the wind carries them,
they splash off the floor and objects, and they make ripples on the water surface. It is
cheap: a storm with 9,500 drops in the air and 11,000 splash droplets
takes 0.6 ms per step on 4 cores.

### Network

```
[Wind] ─────Force────┐
[Object] ───Collider─┴──▶ [Rain] ──Look──▶ [Output] ◀──Look── [Water Look] ◀── [Liquid Solver]
```

Rain is a solver and a look at once, and is itself an Output layer. Its Forces
take wind, and possibly turbulence, a vortex, an attractor or drag; its Colliders take the
objects it rains on. When Output also draws water, the drops land on its
surface. The same wind is connected to the Pyro Solver, the Liquid Solver and the rain:
smoke, water spray and drops then all follow the same wind.

| node | parameters |
|---|---|
| **Rain** (Simulation) | Cloud: `center`, `size` (the cloud: drops are created in this box and it rains below it); Rain: `rate` (drops per second per m²: 100 drizzle, 800 rain, 3,000 downpour), `speed` (fall speed in m/s: around 7 for rain, less for drizzle), `splash` (how many droplets fly off a solid surface), `ripples` (how strongly a drop ripples the water), `fill` (by how many millimeters per second the water the drops fall into rises — as if all the rain under the cloud fell into it; 0 nothing, a real drop is too little water for that; 5 to 20 fills a pool during a shot, [quench.md](quench.md)), `seed`; Time: `start`, `end`; Look: `color`, `opacity`, `streak` (line length as a fraction of a frame: motion blur), `wet` (how wet the floor is) |
| **RBD Solver** (Simulation) | Pieces: `attribute`; Physics: `density`, `friction`, `bounce`, `gravity`, `floor`; Glue: `glue` (glue strength in kPa, 0 without glue); Time: `substeps`, `rest` (bodies at rest freeze until something hits them); Dust: `dust`, `impact_dust`, `dust_size`, `debris`, `air`; Look: `color`, `inside_color`, `inside_group`. Inputs Pieces (geometry with `piece`) and Colliders; outputs Look (to Output: the pieces are drawn where they landed), Rigid (to RBD Pieces), Collider (pieces as moving obstacles for water, gas and rain) and Dust (a smoke source for the Pyro Solver); [destruction.md](destruction.md) |

In the viewport, rain is in **Shift+A → Weather**:

- *Rain* puts a cloud over the whole scene, connects it to Output, and connects to it
  all objects as obstacles and the wind that is already in the scene;
- *Rainstorm* is denser and faster rain with larger splashes. When there is no wind
  in the scene, it adds gusty wind and connects it to the rain and to the solvers.

The cloud is selected by clicking on its box; the gizmo moves it (**W**) and changes
its size (**R**).

### How it works

1. **Creation.** Each step creates `rate × cloud area × dt` drops; a fraction of a drop
   waits for the next step. Where a drop is created is determined by a hash of its sequence
   number. When it rains from the start (`start` 0), the air below the cloud is full of drops
   already in the first step, drops that had been falling before: as many as fall
   during the time it takes to fall to the ground, distributed from the cloud to the ground and shifted by the wind. Those
   that would already have landed in an object or in water are skipped. Rain
   with a later `start` begins falling only from the cloud.
2. **Motion.** A drop's velocity approaches the air velocity plus its own fall,
   each step by a fraction `1 − e^(−1.4·dt)`. A drop thus settles in about 0.7 s,
   which for 7 m/s is exactly `v/g`. The air velocity is given by the wind, gusts included (see
   below). Turbulence shakes the drop, drag slows it, a vortex swirls it and an attractor
   pulls it.
3. **Impact.** On the floor or into an object: the drop disappears and `splash`
   droplets fly out of it, up from the surface (0.6 to 1.5 m/s) and to the sides. They live 0.12 to
   0.3 s, fall under gravity and disappear when they land again. Into water: the drop
   disappears, knocks a ring into the surface and occasionally a droplet jumps up (a Worthington
   jet in miniature). Where the surface is, the Liquid Solver says
   (`distanceToSurface`).
4. **Ripples.** Above the water lies a height grid, at most 256 cells along the longer side
   and at least half a solver cell. A drop stamps a crater with a rim around it into it,
   profile `(1 − q)·e^(−q)` with `q = (r/w)²`, so as much water goes down as
   goes up. The wave equation `h'' = c²∇²h − k·h'` spreads it into a ring
   that propagates at 0.35 m/s and fades with a damping of 3/s. The Laplacian also takes
   the diagonal neighbors (isotropic 9-point): with four, the rings would come out
   square. The edges reflect the waves. Whatever remains of the sum of heights after rounding
   is subtracted every step; otherwise the surface would slowly sink.

**Determinism.** Each drop moves on its own (in parallel), impacts are
processed one by one in drop order and droplets are created in the same
order. The result is bit-identical on any number of threads.

### Gusty wind

Wind with `gusts` > 0 does not blow constantly. The gust strength is noise in time, but
it is not the same everywhere at a given moment: **the gust front travels with the wind**. What
blows here now blows `Δt` later `speed · Δt` further downwind:

```
velocity(p, t) = d · speed · (1 + gusts · (2 · noise(0.8 · (t − p·d / speed)) − 1))
```

where `d` is the wind direction. That is how it is in reality: a gust is air that
arrives. Smoke in the front bends first on the windward side and rain slants
gradually as the front passes through the cloud. The same function
([`Shared.h`](../src/pg/sim/Shared.h), `windAt`) drives gas, water spray
and rain.

### How rain is drawn

- **A drop is a line** from where it is, back along its velocity, as long as
  the distance the drop travels during `streak` of a frame (shutter motion blur). It is drawn
  as a narrow rectangle oriented on screen, and its tail fades out.
- **A drop is small**, 2.5 mm. The line is about 1.3 px wide. A distant drop
  covers only part of that width and is correspondingly fainter; a near one is wider. In the distance,
  rain therefore blends into a haze, and up close it has individual lines. Without this,
  dense rain looks like a white curtain.
- **The color** is the rain color lit by the sky and a little by the sun, with the same
  exposure and tone mapping as the rest of the image. In overcast light the
  drops are darker.
- **Ripples** tilt the surface normal according to the height slope, and reflections and refraction
  show them. They are best seen when looking along the surface, as in reality.
- **The wet floor** is darker (by `wet`/2) and reflects the sky according to Fresnel.

A frame carries six numbers per drop and the ripples as half-precision numbers,
tens of kilobytes in total.

## 7. How it is drawn

The renderer ([`src/pg/gl/Volume.h`](../src/pg/gl/Volume.h)) draws the scene in
world coordinates: the domain as it stands on the floor, the floor with its grid,
obstacles and guides. It receives the frame at half precision
([`sim::Frame`](../src/pg/sim/Frame.h)) and uploads it into an RGB16F 3D texture
as is, without conversion. The fragment shader then, for each pixel:

1. finds the first solid body on the ray: an obstacle (spheres and boxes analytically)
   or the floor;
2. lights it with the sun (in the shadow of smoke and other bodies), the sky and the glow
   of the fire;
3. marches through the gas from front to back up to the body. Each step removes light according to
   the Beer-Lambert law and adds the light that the step itself emits or
   scatters. Within a step this is integrated analytically: the light the step
   adds is also attenuated by the step itself;
4. writes the depth of the body so that guides drawn afterwards disappear behind an obstacle
   and below the floor.

What is in the image:

- **Smoke self-shadowing**: a ray from each cell towards the sun, on the GPU
  at half resolution. Each layer of the 3D texture is one pass of the
  fragment shader into that layer (`glFramebufferTextureLayer`). It is recomputed
  only for a new frame, new light, density or obstacle.
  Obstacles count towards the shadow too.
- **Shadow on the floor:** a ray from a floor point towards the sun through smoke and
  obstacles.
- **Fire glow** on the floor and obstacles: the domain is divided into blocks (six
  along the longest side) and each block is a lamp with the summed light of its
  flames. The summing is done on the GPU, again only for a new frame or a new fire
  look.
- **Scattering:** the Henyey-Greenstein phase function, mostly forward. Smoke
  against the sun therefore glows at its edges. Skylight is attenuated where there is
  dense smoke nearby (from a mipmap of the same texture).
- **Fire** glows like a black body: color according to Planck's law (Tanner
  Helland's approximation) from 1000 K to 3000 K, brightness with the fourth power of temperature
  (the Stefan-Boltzmann law). Soot in a flame absorbs less light than
  cooled soot; it is precisely the glowing soot that really gives a flame its yellow color.
- **Tone mapping** ACES (Narkowicz's approximation), then sRGB.
- **Against artifacts:** the ray start and each sample are offset by noise
  (interleaved gradient noise); otherwise banding would appear. Density near
  open faces and near the ceiling fades out smoothly; otherwise the head of a smoke column
  at the ceiling would look as if cut off by a lid.

The view is an orbit around a point (yaw, pitch, distance) with a field
of view and a roll around the view direction. The network's camera is converted to it
(`gl::orbitThrough`) and back (`gl::cameraFrom`), so the viewport, the render
in the editor and `prototype sim` compute the view the same way.

Water is drawn in the same pass ([§5](#how-water-is-drawn)). Rain comes
only after it: the drop lines are drawn over the image with a depth test, so
an object that is closer hides them.

The editor viewport redraws only when something changes: the frame, the look,
the camera, the size, the guides.

## 8. Performance and determinism

A campfire frame on 4 cores (`pgbench_pyro --example campfire`, average of
60 frames, without rendering), in ms: the dense solver before the sparse grid, today's
sparse one (the default) and today's dense one (`sparse` off):

| resolution | domain cells | dense before | sparse | dense |
|---|---|---|---|---|
| 48 (32 × 48 × 32) | 49 thousand | 18 | 29 | 31 |
| 72 (48 × 72 × 48) | 166 thousand | 44 | 56 | 74 |
| 96 (64 × 96 × 64) | 393 thousand | 89 | 98 | 148 |
| 144 (96 × 144 × 96) | 1.3 million | 238 | 238 | 508 |
| 192 (128 × 192 × 128) | 3.1 million | 556 | **458** | 1204 |

The campfire occupies a large part of its small domain, so the sparse grid does not save
much here, and at low resolution it is held back by tile management. The dense mode is
roughly twice as slow as before: every neighbor read goes through the tile
table. It pays off, however, for a large domain with gas in part of it (below).
A scene without obstacles is computed with a simpler operator without face weights; the compiler
inlines the neighbor sums directly into the smoother loop, the smoother visits only cells
of one color, and prolongation from the coarse grid to the fine one is done separately per
axis.

### Demolition dust: dense and sparse

`pgbench_pyro` steps the `demolition` example (the tower, its pieces and the dust) at
the given resolutions and prints the frame time, the split of time between the phases
of the step, memory, and what part of the domain the dust and the moving air occupy
(`--dense` computes densely). On 4 cores:

| resolution | domain voxels | dense | sparse | computed at most |
|---|---|---|---|---|
| 96 (96 × 56 × 96), 90 frames | 0.5 million | 249 ms/frame, 119 MB | **55 ms**, 75 MB | — |
| 576 (576 × 312 × 576), 180 frames | 103.5 million | does not fit (over 10 GB) | **6.2 s**, 3.4 GB | 23 % of the domain |

At resolution 576 a cell is 16 cm and the whole shot (180 frames, rigid
bodies included) takes 19 minutes; with a render of every thirtieth frame, 21 minutes
and at most 5 GB of memory. The dust itself is in at most 12 % of the tiles, the solver computes
at most 23 % (including the tiles around it and around the falling pieces); the rest of the domain is
still air. Step time: advection 44 %, finding the cells occupied by pieces 20 %
(hundreds of moving pieces every frame), pressure 19 %, forces 8 %. The divergence
that remains after pressure is at most 0.4 1/s.

The dense mode is now slower than the dense solver was before the sparse grid
(249 versus 164 ms/frame at resolution 96). Every neighbor read goes through
the tile table. The sparse mode, the default in the node, is even so 3× faster
than the old dense solver.

![Demolition dust in 103.5 million voxels: frames 60, 90, 120 and 150](img/demolition-576.jpg)

### Upres after the solver

`pgbench_pyro --upres K` puts a Pyro Upres after the solver and prints how long
its frame takes, where the time goes and how much of the fine grid it computes. Campfire, 60 frames,
4 cores, in ms per frame (solver + upres) and peak memory during the run:

| simulation | image grid | ms/frame | memory |
|---|---|---|---|
| solver 64 | 48 × 64 × 48 | 38 | 24 MB |
| solver 64 + upres ×2 | 96 × 128 × 96 | 38 + 53 | 84 MB |
| solver 128 | 88 × 128 × 88 | 147 | 116 MB |
| solver 64 + upres ×3 | 144 × 192 × 144 | 39 + 138 | 167 MB |
| solver 96 + upres ×2 | 128 × 192 × 128 | 79 + 125 | 205 MB |
| solver 192 | 128 × 192 × 128 | 358 | 334 MB |
| solver 64 + upres ×4 | 192 × 256 × 192 | 38 + 244 | 290 MB |
| solver 256 | 176 × 256 × 176 | 739 | 672 MB |

Upres is 1.6× to 2.6× faster than a solver with an equally fine grid and takes
about half the memory. Of its time, 80 to 88 % goes to advection, the rest to
tiles and vortices; a third to a half of the fine grid is computed, and just under
half of those tiles have gas (the rest is the margin the gas can reach).

The simulation upholds the core's invariant I5: the same scene gives **bit-identical**
fields on any number of threads. Each loop is a `pg::parallelFor` over rows
of cells or over tiles, each cell is written by exactly one work item, and nothing is summed
across cells; tiles are taken and released according to the values in them,
in the order of their numbers. A test verifies this with all features at once: two sources (one moving),
all forces and an obstacle, comparing all fields on 1 and on 4 threads.

## 9. Verification

[`tests/test_pyro.cpp`](../tests/test_pyro.cpp) (27 tests):

- grid interpolation; a domain of multiples of 8 cells;
- the projection removes divergence; the default two cycles remove over 97 % of it;
  multigrid converges equally fast at every resolution, also with a closed
  floor and with a sphere in the middle;
- hot smoke rises; fuel burns into heat and soot;
- a source adds only within its time window, a moving source follows its path,
  a box source fills a box;
- an obstacle keeps the gas out and the flow goes around it; nothing flows through a closed floor;
  wind carries smoke downwind;
- a vortex spins around its axis, lift and suction act, height limits it; a vortex reaching
  the ceiling stays bounded; expansion dilutes;
- bit-identical result on 1 and on 4 threads, sparse and dense (sparse
  also with identical tiles);
- sparse grid: with all tiles it samples bit-identically to the dense one (including the limits
  for MacCormack), holds only its own tiles, preserves the values of tiles
  that remained when rearranging; MAC faces have the first layer of the next tile; pressure
  on tiles in the middle of a larger grid converges and is 0 outside them; with bodies
  on irregular tiles in a deep hierarchy each V-cycle reduces the
  residual at least by half; a column
  of smoke in a wide domain occupies at most a third of the cells and has as much smoke as
  in the dense one (±1 %, measured 0.01 %) at the same height (±1 cm); with no gas,
  no tile remains;
- the solver corrects nonsensical inputs (NaN, zero step, negative velocities);
- shadows; half float: exact, round to even, infinity, NaN.

[`tests/test_upres.cpp`](../tests/test_upres.cpp) (7 tests): the upres
grid is the solver grid `scale` times finer (2 to 4, at most 2048 cells);
without vortices it carries as much smoke as the solver (±15 %, measured 9 %) with the same
center of mass (within 0.75 of a solver cell, measured 0.23) and computes less than half of the
fine grid; vortices refine smoke and flame (differences between neighboring cells
over 1.3× larger, measured 2.1× and 1.9×), leave the same amount of gas (±20 %) and
are not faster than the solver's fastest flow; smoke that stands still is
bit-identical with and without vortices; no smoke gets into a sphere above the source;
bit-identical result on 1 and on 4 threads (also with a moving source
and an obstacle); the corner lookup for several grids at once reads bit-identically to
`sample()`. In `test_state.cpp` upres continues from a checkpoint bit-identically
(also across a renewal of the noise layer and with an obstacle), and a state without upres does not load into a world
with upres; in `test_sim_network.cpp` the node is translated into the world,
a bypassed one passes on the solver's gas, a keyed scale is reported, and without a solver on
the input it is an error.

[`tests/test_sim_network.cpp`](../tests/test_sim_network.cpp) (22 tests):
the node type table is consistent (and every default value is written and
read back identically), names and wires, parameter limits including numbers that
read the same with every standard library, files round-trip, what is preserved from
a file, compilation into a scene and a look, network problems, the examples
give `Scene::fire()` and `Scene::smoke()`, all examples run and are
exactly in their saved form, files from older versions are converted, objects are
in the scene whether connected or not, and a new color does not resimulate anything;
`fps` and the light from version 1 files move to Output; the world
(`WorldSolver`) steps all solvers at one rate and a second gas look
is reported; water nodes are translated into the world and the look, smoke and water
in one Output run together and whatever is missing is reported; rain is an Output
layer with wind and obstacles; a cloud below the floor, zero density and a second
rain are reported; the camera looks where it is pointed (also with a different "up" and
straight down), a 38 mm lens has 35° and 12 mm a right angle, there is one camera per
Output and it belongs to the file.

[`tests/test_liquid.cpp`](../tests/test_liquid.cpp) (14 tests): free-surface
pressure converges within 30 iterations and the residual matches even when recomputed
from the operator; in water enclosed in pockets (48 cells under a crate, 2 between a crate
and the bottom) into which more flows in than flows out, the pressure is computed within 30 iterations,
stays finite and each pocket keeps the pressure level it came with,
bit-identically on 1 and 4 threads (without pocket balancing the solution does not converge);
still water stays still and the pressure at the bottom is `g × depth`;
a dam break flows to the wall and does not lose a single particle; a ball of water
falls freely (velocity `g t` to within a percent); a `fill` source fills its shape
with eight particles per cell once, `flow` flows only during its time and where it
points; water does not get into a body and runs off it; it flows out through open sides;
particles keep their ids; a frame carries the surface (negative distance inside,
positive outside, cell to cell) and the water velocity on the solver
grid (at the water; zero above it); wind carries falling drops and leaves a pond
flat (the old wind on the surface blew the water to one wall); bit-identical
result on 1 and on 4 threads, also with a body and turbulence; nonsensical inputs
are corrected.

[`tests/test_rain.cpp`](../tests/test_rain.cpp) (9 tests): the air below
the cloud is full of drops down to the ground from the first step, and exactly as many land
as `rate` says (to within 2 %); drops fall at their speed and slant in the wind
(3 m downwind over a 7 m fall); gust fronts travel with the wind and change only its
strength, not its direction; drops do not stay inside an object and droplets fly up from it;
drops and droplets keep their ids; what should land in the water does, nothing passes below the surface, and ripples stay
ripples; rain starts and stops on time and a late one starts at the cloud; bit-identical
result on 1 and on 4 threads (with water, wind, turbulence and an object);
nonsensical inputs are corrected.

[`tests/test_shapes.cpp`](../tests/test_shapes.cpp) (5 tests): rotation to
angles and back (also beyond 180° and in gimbal lock), inside and outside, distances where
a ray meets each shape (also rotated and stretched) and with what normal,
the source falloff towards the edge of the shape.

[`tests/test_mesh.cpp`](../tests/test_mesh.cpp) (6 tests): OBJ in all
face notations (including negative indices, polygons, CRLF, locale-independent
numbers) and malformed files; the distance field against the exact distance of a
cube; a model with a hole is still correctly inside and outside; rays; a model
placed in the world (moved, rotated, stretched); a file is read once and
again after a change. In the network tests: a path with a space and quotes round-trips,
a relative path from the network's folder, a missing file is reported and replaced by
a box.

Everything is clean under AddressSanitizer, UBSan and ThreadSanitizer. Under TSan and
ASan the editor also ran with the simulation thread and scripted input (`prototype
--script`: mouse, keys and screenshots from a file): adding a node via
Tab, connecting by dragging, rapid parameter changes that repeatedly restart the
simulation, undo and redo, moving along the timeline, switching networks and quitting
in the middle of a step; click selection, dragging the gizmo for move, rotate and
scale, multi-object selection, duplication, deletion, adding via Shift+A and
the context menu. No data race or memory error in our code;
the only reports left were inside X11, GLX and Mesa, which are not
instrumented for sanitizers.

The editor interface has its own tests that need neither a window nor OpenGL:
[`tests/test_editor_ui.cpp`](../tests/test_editor_ui.cpp), the program
`pgeditortests` (in `ctest` as `editor_ui`), 19 tests. Dear ImGui runs in them
only in memory: frames are built, input is fed into them, nothing is
drawn. They verify that:

- the font is the compiled-in Inter (Regular and SemiBold) and has Czech characters and
  the symbols the editor writes;
- Escape closes only the topmost menu: the submenu, then the menu. It leaves a dialog to the dialog.
  A keypress that closed a menu no longer reaches the panel below it;
- the node menu takes the node highlighted with the arrows on Enter, the up arrow from the first
  goes to the last; a menu with 150 nodes opened at the bottom edge
  fits entirely in the window;
- overview rows have their values in one column, after the widest label;
- a tab in a panel header switches on click;
- node names in a zoomed-out network do not cover any node or another name: a network
  of 120 nodes packed more densely than the names are wide, and two nodes one above the other, where the name
  of the upper one must go to the right; node thumbnails make room for themselves without the network
  moving;
- unsaved changes: without changes the action is performed immediately. With changes a question comes:
  Escape does nothing, D discards the changes and Enter saves and then continues. For a network
  without a file the action waits until a file is chosen and written;
- saving over an existing file asks first, Enter a second time
  overwrites it and Escape withdraws the question, not the dialog;
- autosave writes right after a change and then at most every 30 s. A backup of an editor
  that is still running is not offered, a backup of a crashed one is (including whose it is
  and what text it has). After saving or discarding, the backup disappears;
- the history (undo) records a change once, when it settles, and only then saves
  the state. It holds at most its byte budget. The state key changes with an edit
  and with moving a node;
- frame-by-frame background work (`FrameJob`) makes the frames in order and reports
  how it went. When stopped, it finishes after the frame it is working on and keeps the finished ones.
  A frame that fails ends it, and the report says why and how far it got. If
  it disappears while running, it stops and waits.

The whole editor is then exercised by `ctest` scripts under xvfb
([`tests/editor/run_editor.py`](../tests/editor/run_editor.py), the
`editor_*` tests in a build with the editor). A script is played back into the window, and what the editor
did is read from the files it left behind: what it saved, what it set aside for recovery,
and a screenshot, which is produced only if the editor is still running.

- **Quit with changes:** Save saves and quits, Don't Save leaves the file and the backup
  disappears, Cancel leaves the editor running.
- **Example without a file:** on quitting it is saved via Save As. The second time it
  asks, and Enter overwrites the file.
- **Crash:** the editor is killed (`kill -9`), started again from another folder,
  Enter recovers the network and Ctrl+S saves it to the original file. The backup folder
  then stays empty.
- **Simulate Again:** four times in a row while the campfire is simulating.
- **Cache and export in the background:** Save Cache to Disk and Export USD Scene
  (stopped with Escape) run on their own thread and the window keeps playing. What they write
  holds together: `cache.txt` lists as many frames as there are in the folder,
  and the scene ends with the frame that the last VDB belongs to.
- **Selection while the selection tree is being built:** a grid with a million primitives.
  A click and a box right after switching to points (2) wait for the tree and select.
  Ctrl+G makes a group from the selection and Ctrl+S saves it.

## 10. How to add a node

The editor, the files and the command line all take nodes from one table
([`src/pg/sim/Network.cpp`](../src/pg/sim/Network.cpp), `buildTypes()`).
A new node is registered there and appears everywhere by itself: in the Tab menu, in the parameter
panel, in files and in `--set`.

1. **Physics**, if it is new: data into [`Scene.h`](../src/pg/sim/Scene.h)
   (for example a new `ForceKind`), the computation into [`Pyro.cpp`](../src/pg/sim/Pyro.cpp)
   (`addForce`), limits into `Scene::sanitized()`.
2. **Node type:** name, label, category, help, pins (the type determines what
   may be connected) and parameters. Each parameter has a section, a kind (number, integer,
   toggle, vector, color, choice), a default value, a slider
   range, physical limits, a unit and help.
3. **Compilation:** in `Network::compile()`, convert the parameters into the scene.
4. **Guide** in the viewport, if the node has a shape:
   `gl::sceneGuides()` in [`Volume.cpp`](../src/pg/gl/Volume.cpp).
5. **Test:** the table test checks it by itself; add a physics test.

## 11. What you need to know

- **Vector calculus:** gradient, divergence, curl. Divergence says
  how much flows out of a point, curl how much it rotates. The whole simulation can be read as
  "move, add forces, remove divergence".
- **The Navier-Stokes equations** for an incompressible fluid, without
  viscosity: the Euler equations. Numerical diffusion adds that viscosity anyway.
- **Boundary conditions:** Dirichlet (a given value, here zero pressure on an
  open face) and Neumann (a given flux, here zero flux through a wall). §4 shows
  that even the "obvious" choice of what flows in through an open face decides
  stability.
- **Stam, *Stable Fluids* (1999)** — the foundation of a whole class of methods: semi-Lagrangian
  advection plus projection.
- **Bridson, *Fluid Simulation for Computer Graphics*** — the best book to
  start with: the MAC grid, projection, boundaries, solid bodies.
- **Fedkiw, Stam, Jensen, *Visual Simulation of Smoke* (2001)** — vorticity
  confinement.
- **Selle et al., *An Unconditionally Stable MacCormack Method* (2008).**
- **Briggs, *A Multigrid Tutorial*** — multigrid explained clearly.
- **Wrenninge, *Production Volume Rendering*** and **PBRT, the chapter on
  volumes** — raymarching, phase functions, shadows.
- **The Houdini Pyro documentation** — what production uses of this and how it is
  set up: Pyro Solver, the fields `flame`, `temperature`, `density`, `fuel`,
  POP Axis Force (the model for the Vortex node).

## 12. Limitations and what production does

This simulation is a prototype that shows how Pyro works and measures what
it costs. Compared with production:

1. **Dense grid.** Every cell is computed, even an empty one. Production stores only
   the cells near the smoke (OpenVDB, NanoVDB on the GPU) and the domain grows with the smoke. The
   `Grid` interface is small here precisely so that it can be swapped out.
2. **CPU.** Houdini has its solvers in OpenCL, EmberGen simulates in real time on the
   GPU. Each phase here is a loop over cells without shared state, so it can be
   converted directly into a compute shader.
3. **Velocity is advected with the semi-Lagrangian method**, which blurs.
   MacCormack or BFECC for velocity too, or FLIP, keeps vortices longer.
4. **Obstacles are coarse.** Shapes (sphere, box, cylinder, cone, torus)
   and models from OBJ are converted to a distance field that has only 48 cells
   along the model, so fine details disappear in collisions. Moving obstacles
   pass their velocity to gas and water ([animation.md](animation.md)), but
   a fast obstacle skips through a thin layer. In production the field is baked more finely
   (and sparsely, OpenVDB) and the step is shortened according to the speed of the obstacles.
5. **Single scattering.** Production renderers (Karma, Arnold) compute
   multiple scattering, thanks to which dense smoke is brighter inside.
6. **The cache holds the image, not the solver state.** Frames go to disk and to OpenVDB
   ([cache.md](cache.md)), but at half precision and without velocities, so
   you cannot continue simulating from a cached frame. Production also saves the solver state
   and continues from a saved frame.
7. **Water is coarse.** Resolution 64 gives drops and sheets centimeters thick,
   and sparse spray is assembled from particles into lumpy shapes. Production computes
   grids sparsely (OpenVDB), with tens of millions of particles, builds the surface
   from anisotropic kernels (Yu and Turk, 2013) and simulates spray, foam and bubbles
   separately (whitewater). Viscosity and surface tension are missing.
8. **Smoke and water do not know about each other.** Each solver has its own domain; water does not
   put out fire and smoke does not move water. Likewise rain: it does not add water to a pool,
   does not put out fire, is not carried by smoke (it is by wind), and only the floor gets wet, not
   objects. Ripples are a height grid above the surface that the water solver does not see.
   Production makes rain from particles the same way, but there the drops also become part of FLIP
   once they land, and wetness is painted into the objects' textures.
9. **The render is a preview.** Raymarching in OpenGL with single scattering, without
   depth of field and without motion blur of gas and water (only the raindrops are
   blurred). Production renders the same volumes with path tracing
   (Karma, Arnold, RenderMan) and combines the layers in compositing (Nuke).
10. **The simulation is not a geometry network node.** The next step is a
   `pyrosolver` node: the core's cook engine already knows time dependency and
   frame caching ([ARCHITECTURE.md §4.3](../ARCHITECTURE.md#43-time-as-a-dependency-dimension)).

## 13. References

- J. Stam: *Stable Fluids*, SIGGRAPH 1999.
- R. Fedkiw, J. Stam, H. W. Jensen: *Visual Simulation of Smoke*, SIGGRAPH 2001.
- A. Selle, R. Fedkiw, B. Kim, Y. Liu, J. Rossignac: *An Unconditionally
  Stable MacCormack Method*, J. Sci. Comput. 2008.
- R. Bridson: *Fluid Simulation for Computer Graphics*, 2nd ed., CRC Press 2015.
- W. L. Briggs, V. E. Henson, S. F. McCormick: *A Multigrid Tutorial*, SIAM 2000.
- M. Wrenninge: *Production Volume Rendering*, CRC Press 2012.
- T. Kim, N. Thürey, D. James, M. Gross: *Wavelet Turbulence for Fluid
  Simulation*, SIGGRAPH 2008 — upres: noise added to a coarse simulation according to
  the energy the grid cannot resolve.
- R. Bridson, J. Hourihan, M. Nordenstam: *Curl-Noise for Procedural Fluid
  Flow*, SIGGRAPH 2007.
- F. Neyret: *Advected Textures*, SCA 2003 — two layers of advected
  coordinates that are alternately renewed and blended.
- M. Pharr, W. Jakob, G. Humphreys: *Physically Based Rendering*, 4th ed.,
  ch. 11 and 14.
- J. Jimenez: *Next Generation Post Processing in Call of Duty: Advanced
  Warfare*, SIGGRAPH 2014 (interleaved gradient noise).
- Y. Zhu, R. Bridson: *Animating Sand as a Fluid*, SIGGRAPH 2005 (FLIP and
  the surface from particles).
- C. Batty, F. Bertails, R. Bridson: *A Fast Variational Framework for
  Accurate Solid-Fluid Coupling*, SIGGRAPH 2007.
- F. Gibou, R. Fedkiw, L.-T. Cheng, M. Kang: *A Second-Order-Accurate
  Symmetric Discretization of the Poisson Equation on Irregular Domains*,
  J. Comput. Phys. 2002 (ghost fluid).
- A. McAdams, E. Sifakis, J. Teran: *A Parallel Multigrid Poisson Solver
  for Fluids Simulation on Large Grids*, SCA 2010.
- H. Zhao: *A Fast Sweeping Method for Eikonal Equations*, Math. Comp. 2005.
- R. Gunn, G. D. Kinzer: *The Terminal Velocity of Fall for Water Droplets
  in Stagnant Air*, J. Meteorology 1949 (drops fall at 2 to 9 m/s).
- K. Garg, S. K. Nayar: *Photorealistic Rendering of Rain Streaks*,
  SIGGRAPH 2006 (what a drop's streak looks like with motion blur).
- SideFX: Houdini documentation, *Pyro*, *FLIP Solver* and *POP Axis Force*.
