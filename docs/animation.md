# Animation: keyframes and moving obstacles

Every numeric parameter of every node in a network (an object's position, a source's fuel,
wind strength, smoke density in the look, camera focal length, box size
in geometry…) can have **keyframes**: a value at a given frame and a way
of getting to the next key. The network is then compiled frame by frame, and
simulations take the world of that frame at each step. Objects that move are
**moving obstacles**: both gas and water take on their motion — a sphere
traveling through a pool raises a wave in front of it and leaves a wake behind; a paddle
spinning in smoke stirs it up.

![Editor: animated sphere position (green field, key diamond), keys on the timeline and the wake in the water](img/editor-animation.png)

![The wake example: a sphere travels through a pool, frames 20, 40 and 60](img/wake.png)

## 1. Quick start

```bash
./build/prototype --example wake          # a sphere pushes water: wave and wake
./build/prototype --example fire_trail    # a torch flies in a loop, a paddle spins in smoke
./build/prototype sim wake out/w.png --every 10
```

In the editor:

1. select an object (e.g. a sphere from Add → Objects),
2. at frame 1, press **K** over the viewport — a key for position, rotation and size,
3. move the playhead (click in the timeline),
4. move the object with the gizmo — because it is animated, a key is written where the
   playhead is,
5. the simulation restarts and the object moves; playback shows what it did
   to the water or smoke.

## 2. Keys in the editor

- **Diamond** at the start of a parameter row: empty gray (only on mouse
  hover) — the parameter is not animated; orange outline — animated, no key
  at this frame; solid orange — a key at this frame. Clicking adds a key
  (the value the parameter has at that moment) or deletes it; the right button
  opens a menu: interpolation to the next key (Smooth, Linear, Step),
  delete key, delete all keys.
- **The field of an animated parameter** is tinted: amber on a key, green
  between keys. The value is the one at the current frame; editing the value of an animated
  parameter writes a key at the current frame (auto-key). A parameter without keys
  is edited as before — the same throughout the simulation.
- **The timeline** shows keys: those of selected nodes prominently, those of other nodes in the
  network dimmed.
- **K** (over the viewport; or Edit → Key Selection) writes position,
  rotation and size keys for the selected objects, sources and forces at the current frame.
- **The gizmo** on an animated node writes keys (translation, rotation and size).
- Resetting a parameter (arrow on the right) also deletes its keys; deleting the last
  key leaves the parameter at its value.
- A duplicated node has the same keys. Undo/redo also reverts keys.

Objects and guides are drawn at the **playhead** frame — immediately, without
waiting for the simulation; gas and water at the last frame computed up to it.

## 3. Interpolation

| | |
|---|---|
| **Smooth** (default) | a cubic curve through the keys: at the first and last key and where the value reverses, with zero slope (ease in and ease out); elsewhere a Catmull-Rom slope, limited so that the curve does not overshoot between keys (Fritsch–Carlson) |
| **Linear** | a straight line to the next key |
| **Step** | holds the key's value until the next one |

Interpolation belongs to a key and applies from it to the next one. Before the first key the
parameter has the first key's value, after the last the last key's. Integer parameters
are rounded; toggles and menus jump (like Step).

## 4. The .pgsim file

Keys are lines `key NAME FRAME INTERPOLATION VALUE` under the node, after its
parameters; the value is written the same way as for `param`:

```
node 2 object 1 ball 0 110
  param size 0.34 0.34 0.34
  key center 1 smooth -0.85 0.19 0
  key center 60 smooth 0.85 0.19 0
```

The `param` line of an animated parameter stays: it is the value for when
the keys are deleted.

## 5. What can be animated

- **Simulation**: sources (position, rotation, size, fuel, smoke, heat,
  velocity…), forces, objects, solver settings that are not the grid
  (buoyancy, cooling, vorticity…), rain (cloud, intensity…).
- **Look**: Volume Look, Water Look, light and sky in the Output — they change
  in the image; when only the look (or the camera) is animated, the simulation runs once
  and the world carries no animation.
- **Camera**: position, rotation, focal length — `prototype sim` as well as sequence renders
  from the editor go through the animated camera.
- **Geometry**: parameters of geometry nodes (box, transform,
  scatter…) — the viewport and the attribute spreadsheet show them at the current frame.

**Cannot be animated** (the value at frame 1 applies throughout; compilation
says so with a warning): size and resolution of the Pyro Solver and Liquid
Solver grid, closed tank walls, frame count and frame rate in the Output.
A shape from geometry (the Shape input) is static — the geometry is taken at frame 1;
make a moving obstacle from an object with its own shape or an OBJ model.

## 6. Expressions

Instead of keys, a parameter can be driven by an **expression** — the same language as wrangle
([wrangle.md](wrangle.md)), just a single expression: `$F * 0.1`, `sin($T * 6) *
0.3`, `ch("../base/sizex") * 2`, `fit($F, 1, 100, 0, 5)`, `rand($F)`.
Each vector component has its own expression (`center.y`), or all share the same
vector expression: `{0, $F * 0.01, 0}`.

- **In the editor:** the **fx** button next to the key diamond switches the parameter to
  text fields (three for a vector). Whatever is typed into them is an expression; a plain
  number is a value. Below the fields is what the expression gives at the current frame, or
  in red, what is wrong with it. Another click on **fx** deletes the expressions and the
  parameter keeps the value it had.
- **Variables:** `$F` frame (integer), `$FF` frame as a floating-point
  number, `$T` time in seconds (`$F / $FPS`), `$FPS` frame rate
  from the Output.
- **Other parameters:** `ch("sizex")` a parameter of the same node, `ch("../box1/sizex")`
  a parameter of node *box1*; a vector component as `size.y` or `sizey`;
  `chv("../box1/size")` the whole vector, `chs()` a string. They are read at the same
  frame, so an expression over an animated parameter changes with it.
- **When it is recomputed:** an expression that reads neither time nor anything that changes over time
  is an ordinary value — it is recomputed only when what it reads changes. An expression with
  `$F` or over an animated parameter makes the node time-dependent: the simulation
  takes it frame by frame like keys, geometry is cooked for each frame.
- **Errors:** an expression that cannot be parsed or evaluated does not change the parameter
  (its value applies) and the editor says why. Expressions that read one
  another in a circle are an error ("round in a loop"), not a freeze.
- **The expression wins** over keys and the value; the parameter keeps the value for when
  the expression is deleted.
- **In the file** it is a line `expr CHANNEL "TEXT"` under the node's keys:

```
node 1 pyro_source 2 fire 0 0
  param fuel 14
  expr center.x "sin($T * 6) * 0.3"
```

- **From the command line:** `--set` also takes an expression:
  `prototype sim campfire fire.mp4 --set 'fire.center.x=sin($T*6)*0.3'`,
  `--set 'fire.center={0, $F*0.01, 0}'`.

## 7. How it works

### The network frame by frame

`Network::compile()` compiles the network at frame 1 (with error checking). When
something is animated, it compiles it again for every frame (silently: problems were reported by
the first frame) — model files and geometry are read and cooked only
once in the process. The result:

- `World::animation` — the world at each frame (shared, compared by
  content: the editor restarts the simulation exactly when the keys or
  values change);
- `Compiled::poses` — what is drawn at each frame: look, objects, camera.

The step that produces frame *n* takes the world of frame *n* (`WorldSolver::step`):
the solvers take over the sources, forces and obstacles (`setScene`) and the grid stays.

### Obstacle motion

The object's **velocity** and **angular velocity** (axis × radians per second, from the difference
of rotations `R₂ R₁ᵀ`) are computed from its position and rotation in adjacent frames. The velocity of a point of the
body is `v + ω × (p − centre)`.

- **Gas**: cell faces next to solid cells (blocked faces) do not have velocity
  0 but the body's velocity at that location. The divergence next to the body takes it into account,
  and pressure then pushes the gas out of the way; likewise, sideways motion of the body drags
  gas along with it. Solid cells are recomputed at every step in which the
  body moves.
- **Water**: the part of a cell face covered by the body carries its velocity — the pressure
  solve receives the flux `o·u + (1 − o)·u_body` (Batty, Bertails, Bridson 2007);
  fully covered faces have the body's velocity. Particles pushed out of the body
  lose only their velocity *into* the body **relative to it**, so the body carries them
  along.
- **Sources** that move also give the gas (and the water jet) their velocity:
  a torch leaves a trail.
- **Rain** lands on objects where they are at that frame.

### Geometry

An animated parameter of a geometry node is bound in `GeometryGraph` as a
core expression (keys evaluated at the cook frame). This makes the node
time-dependent, and the core cache holds its geometry for each frame separately — returning
to an already cooked frame computes nothing.

## 8. Limitations

- Changing a key restarts the whole simulation (like any scene change).
- An obstacle that moves faster than one cell per step can "skip through"
  a thin layer of water or gas; gas inside cells that the body enters
  disappears.
- A shape from geometry and a change of shape (sphere → box) are not animated smoothly.
- There are no curve editors (curve graph) and no keys on individual vector
  components — a key carries the whole value (an expression, however, can be per component).
- An expression cannot see another node's geometry (Houdini has `npoints()`, `bbox()`
  with a path): it reads parameters, time and its own numbers.
