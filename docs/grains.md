# Sand, gravel and soil: Grain Solver

**Grain Solver** simulates granular materials: sand, gravel, soil, snow. Each
grain is a sphere with its own size. Grains push each other apart, hold together through
friction, and wet ones stick to each other. It is computed like Vellum Grains and POP Grains
in Houdini: with PBD (*Position Based Dynamics*), with many small substeps
per frame and several passes over the contacts in each. Poured grains form a
pile as steep as friction allows. Wet sand stands like a wall. Grains
fall onto the floor, onto objects and onto RBD Solver pieces, and push those pieces
back. The grit that RBD Solver throws out of fractures can be grains directly: it falls
onto the debris, slides off it and settles around it, piling up where more of it fell.

![Sand pouring onto a crate: a small pile on the lid, sliding down the side and a cone on the ground (Cycles)](img/grains-sand.jpg)

```
./build/prototype --example sand_pour                 # in the editor: Play
./build/prototype sim sand_pour pisek.mp4 --frames 210
./build/prototype sim gravel_slide strk.png --every 30 --frames 90
```

## 1. Examples

### Sand poured onto a crate

The **sand_pour** example ([examples/sim/sand_pour.pgsim](../examples/sim/sand_pour.pgsim)):

- **Point Cloud** of 4,000 random points, squeezed by a Transform into a box of
  9 × 5 × 9 cm at a height of 1.3 m above the edge of the crate. This is the nozzle.
- **Point Wrangle** `falling` gives each point the velocity the stream has where
  it is: the lower in the nozzle, the longer it has already been falling (`v = 1.5 + 9.81·t`). The stream is
  thus continuous, not in per-frame batches. Each grain gets its own
  shade of sand (`Cd`).
- **Grain Solver** `sand`: grains with a radius of 5 mm, Friction 0.65, **Emit
  Frames 150**. The nozzle points are taken again every frame, but only where
  no grain is in the way, and they are also thinned out among themselves. That is why a random
  Point Cloud is enough and no grain is ever created inside another grain.
- **Colliders**: a 50 × 40 × 50 cm crate.

The stream lands on the edge of the lid. A small pile grows on the lid, spills over the side,
and a cone forms on the ground next to the crate. After 120 frames there are 37,900
grains in the scene and the simulation takes 120 ms per frame on average (4 cores).

### Gravel from a chute into boxes

The **gravel_slide** example ([examples/sim/gravel_slide.pgsim](../examples/sim/gravel_slide.pgsim)):

- **Point Cloud** of 16,000 points in a box above the top end of the chute. Point
  Wrangle `stones` gives the stones a size from 2.5 to 6 cm (`@pscale`) and a gray
  to brown color. Of points that overlap, only one stone remains,
  so 16,000 points yield 1,836 stones.
- **Grain Solver** `gravel`: Friction 0.45, density 1,500 kg/m³.
- **Colliders**: a chute tilted at 35° with side walls and a back wall, and an **RBD
  Solver** with two empty cardboard boxes (2 kg).

The gravel slides down the chute, spreads across the ground in a fan and hits the boxes.
They move: RBD Solver is among the Grain Solver's colliders, so the coupling is
two-way. The boxes stop the gravel and the gravel pushes the boxes. The simulation takes
about 16 ms per frame.

![Gravel from a chute: it slides down, spreads out and pushes the boxes](img/grains-gravel.jpg)

### Concrete grit as grains

The **shatter_grit** example ([examples/sim/shatter_grit.pgsim](../examples/sim/shatter_grit.pgsim))
is **shatter_blocks** from [destruction.md](destruction.md) — three concrete
blocks in a row and a wrecking ball that passes through them and breaks each one where
it strikes — with one extra node:

- **Grain Solver** `grit` (Friction 0.9) has the Rigid output of RBD Solver in its **Grit**
  input. The grains have no points in Geometry: all of them are grit.
- RBD Solver throws out more grit (`debris 6`).

Every bit of grit that flies out of the piece it came from is a grain from that
step on: it hits the others, lands on the fragments and slides off them,
and settles around the debris in the fracture color. The impact scatters it far, so most
grains lie on the ground individually; where more of it fell in one place, it piles
up. After 75 frames there are about 3,100 grains in the scene, 2,770 of them at rest,
and 150 of those on fragments or on other grains.

```
./build/prototype sim shatter_grit drt.png --frames 75 --renderer cycles
```

![Concrete grit as grains: the debris of three blocks surrounded by grains in the fracture color, some on fragments (Cycles, frame 75)](img/grains-grit.jpg)

![Editor: shatter_grit at frame 75, the Grain Solver grit selected; "3.1 k grains" in the viewport header and the status bar](img/editor-grit.jpg)

## 2. What is what

| node | what it does |
|---|---|
| **Grain Solver** (Simulation) | Grains from the geometry points in **Geometry**. Size is `pscale` (radius, otherwise the Radius parameter), color `Cd`, initial velocity `v`. **Colliders**: objects and RBD Solver (grains push its pieces). **Forces**: wind. **Grit**: the Rigid output of RBD Solver — its grit becomes grains (works even with no points in Geometry). The **Look** output goes into the Looks of the Output node, the **Grains** output into Grain Points. |
| **Grain Points** (Geometry) | The frame's grains as points: `P`, velocity `v`, `pscale` (radius), `id` (the same throughout), `Cd` and `orient` (each grain rotated its own way). You can copy stones onto the points (Copy to Points), export them to PLY or process them further. |

![Editor: sand pouring onto a crate; in the overview the Grains (grain diameter, friction), Count (31,848 grains) and Step rows, "31.8 k grains" in the status bar; the sand node shows the stream and the pile in its node thumbnail](img/editor-grains.jpg)

The Grain Solver's Look draws grains as stone chips. It is the same shader
as for destruction grit, so each grain has its own shape and shade.
The viewport, the path tracer and Cycles all draw them.

## 3. Parameters

| parameter | default | what it does |
|---|---|---|
| Radius | 0.01 m | grain radius where the point has no `pscale` |
| Size Variance | 0.2 | each grain randomly larger or smaller by this much (0.2: 80 to 120 %); grains of a single size stack like oranges in a crate |
| Density | 1,600 kg/m³ | mass; between grains it decides who yields to whom, against RBD pieces how hard the grains push them |
| Friction | 0.6 | friction between grains and against the floor and objects; 0 spreads out, 0.6 dry sand, 1 gravel |
| Cohesion | 0 | how strongly grains a short distance apart pull together and hold: 0 dry sand, 0.5 damp, 1 wet, which stands like a wall |
| Rest Speed | 0.01 m/s | a grain that is touching something and moving more slowly stops: the pile settles and does not creep |
| Emit Frames | 1 | for how many initial frames grains are created from the points; more means a stream |
| Max Grains | 1,000,000 | no more grains are created |
| Floor | on | a floor at height 0 |
| Air Drag | 0 | how quickly a grain takes on the air velocity (wind, Pyro Solver gas flow): 1 sand, 5 dust |
| Damping, Gravity | 0, 9.81 | motion damping and gravity |
| Substeps, Iterations | 10, 4 | substeps per frame and passes over the contacts in each; more means less interpenetration |
| Color | sandy | the color of grains without their own `Cd` |

Pile angle by friction (2,200 to 2,900 grains poured onto one spot,
after 200 frames, test `grains_pile_as_steep_as_their_friction_holds`):
Friction 0.15 gives about 15°, Friction 0.8 about 38°. Dry sand in nature has
30 to 35°.

## 4. How it works

Each frame runs Substeps substeps. In each one:

1. **Prediction.** The velocity gets gravity and air drag, the position
   moves by velocity × step.
2. **Neighbors.** A grid of cells the size of the largest grain (hashed,
   counting-sorted), and in it neighbor lists for each grain. A list is
   built with a margin of half a radius, and rebuilt only when some
   grain has moved by more than half the margin. This is a Verlet list, so a
   resting pile recomputes nothing. Once per frame the grains are
   sorted in memory by cell so that neighbors lie close together.
3. **Contacts** (Iterations passes). Two grains closer than the sum of their radii
   are pushed apart, each by a share given by the masses. The upper grain yields
   more, as if the lower one were heavier (mass scaling with height after
   Macklin et al. 2014, §5.4). The pile thus carries its weight down in a few
   passes and the grains do not sink into each other. **Friction**: the sideways slip of a contact
   during a substep is stopped completely if it is less than Friction × how
   hard the grains are pressed together over all passes (static friction).
   Otherwise it is stopped only by that much (kinetic, three quarters). **Cohesion**:
   grains within half a radius of touching are pulled together and held by friction
   even without being pressed together. Each grain computes from the positions of all grains before the pass
   (Jacobi). Corrections from contacts are averaged among themselves, cohesion pulls among
   all contacts. The result therefore does not depend on the number of threads.
4. **Collisions** with the floor and objects (signed distance fields of the shapes), including friction
   against the motion of the surface. For an RBD piece, how far the grain
   pushed it is recorded.
5. **Velocity** from the difference in positions. A grain pushed out of a deep penetration
   does not fly off: its velocity may not exceed by more than 0.5 m/s the velocity
   it arrived with, or the velocity of the fastest surface it
   touched (a piece that carries it or that hit it). The velocity
   itself is limited, not just its change, so it cannot accumulate even over several consecutive
   substeps. A grain in contact that is barely moving (Rest Speed) stays
   in place.

**RBD pieces** are bodies of their own mass in the grain step. What the grains passed on to them
(displacement, velocity, rotation) is received by RBD Solver at the start of the next step,
just as from cloth. A grain pinched between a piece and the ground does not carry the piece; that is the job of
RBD Solver's floor. A grain next to a piece on the ground does push it, though.

**Emission**: during the first Emit Frames frames, each point becomes a grain
if no other grain is in its way, whether old or just created (a distance
below 0.9 of the sum of the radii). From the second frame on, the point's position is randomly offset
by a tenth of the radius so that the stream is not made of columns.

**Grit as grains** (the Grit input). RBD Solver throws grit out of fractures and impacts
as before, but it does not keep a bit of stone that is outside the piece it came from
(`RigidScene::gritIntoGrains`): after the RBD step, World hands it over to the
grains (`RigidSolver::thrown`, `GrainSolver::add`) with its position, velocity
and a radius of half the bit's size. The grain gets the next grain number and the fracture color
of the pieces (RBD Solver's Inside Color × 0.9, as RBD Solver draws grit).
Glass shards remain RBD Solver grit: they are flat slivers, not grains.
The coupling also makes RBD Solver's pieces colliders for the grains (as if the RBD
Solver were also in Colliders), so grains fall onto the pieces and push them.

Grit is born in the middle of a pile of debris and often inside another grain or a piece.
New grains therefore pass through the contacts without velocity before the first substep
(*pre-stabilization*, Macklin et al. 2014, §4.4): they move apart but
do not fly off. And a grain that a piece is pressing into the floor is squeezed out from under it
sideways (away from the piece's center when it presses straight down) at most at
0.5 m/s, the speed at which grains move apart — never below the floor.

## 5. Frames, cache, checkpoint and export

- **Frame** (`Frame::grains`, cache version 15): positions (float), velocities
  and radii (half precision), grain numbers and colors (bytes). Older caches
  are still read, just without grains.
- **Checkpoint**: positions, velocities, radii, masses, numbers, colors
  and reactions on pieces. A resumed bake continues bit for bit (test
  `state_resumes_the_grains_and_the_pieces_they_push_to_the_bit`), even
  in the middle of pouring.
- **Grain Points** returns the grains as points. Export to PLY and OBJ works as
  for other points ([cache.md](cache.md)).
- **USD**: `/World/grains` as a PointInstancer in each frame layer:
  twelve stone-chip prototypes (the same shapes the renderers draw),
  `positions`, `orientations`, `scales`, `velocities`, `ids`,
  `protoIndices` and `primvars:displayColor` ([usd.md](usd.md)). Grains that
  were grit appear there only once: RBD Solver no longer carries them.
- **Python**: `frame.grains()` returns points with `v`, `pscale`, `id`, `Cd`
  and `orient` ([python.md](python.md)).
- **Step profile**: the grain time is in the editor's overview (the Grains row) and in the
  `time:` line of the `prototype sim` command.

## 6. In the code

| file | what it does |
|---|---|
| `src/pg/sim/Grains.h` | `GrainSettings`, `GrainScene`, `GrainFrame`, `GrainSolver` (step, emission, neighbors, contacts, collisions, reactions, state), `grainPoints` |
| `src/pg/sim/World.h` | `World::grains`, the grain step after cloth, reactions on RBD pieces (`RigidScene::intoGrains`), RBD grit into grains (`RigidScene::gritIntoGrains`) |
| `src/pg/sim/Rigid.h` | `RigidBit`, `RigidSolver::thrown`: the grit the step handed over to grains |
| `src/pg/sim/Network.cpp` | the `grain_solver` (Grit input) and `grain_points` nodes, compilation (`compileGrains`) |
| `src/pg/sim/Frame.cpp` | `drawnBodies`: grains as free points with `pscale`, drawn as chips |
| `src/pg/sim/Cache.cpp` | frame version 15 |
| `src/pg/sim/UsdExport.cpp` | `/World/grains` (PointInstancer) |
| `src/pg/core/Chips.h` | shapes of stone chips and glass shards, their shade and orientation: for the renderers and USD |

## 7. Verification

`tests/test_grains.cpp`, 8 tests:

- a block of 768 grains falls onto the floor, nothing is below it, after 120 frames it is at rest
  (below 5 cm/s), grains penetrate each other on average by less than 4 % of the sum of
  radii and nowhere by more than 30 %; sizes are within the Size Variance limits;
- a poured pile is steeper with more friction (0.15 gives 15°, 0.8 gives 38°)
  and settles;
- a column of 400 grains 33 cm high collapses when dry (9 cm remain), when wet
  (Cohesion 1) it stands (27 cm);
- grains stay outside the object they rest on, and push a piece that gives way;
- wind blows the grains away;
- pouring: grains accumulate, are not created inside each other, each has its own number, Max
  Grains is respected;
- grains as points: `pscale`, `Cd`, unit `orient`, `id`; without a color,
  the look's color; a frame that does not match yields nothing;
- 1 and 4 threads and restoring from state give the same result bit for bit; a truncated state is
  rejected.

`tests/test_grit_grains.cpp`, 3 tests:

- a concrete block falls from 4 m and breaks: the grit is grains (more than 40),
  RBD Solver keeps at most a tenth of it, in the fracture color, no grain below
  the floor, four fifths at rest, some lie on pieces or on each other;
  in no frame is any grain faster than 9.4 m/s (the block lands
  at 8.9 m/s; when the velocity of grains pushed out of the debris accumulated
  over substeps, they flew at up to 15 m/s); without the coupling, the grit stays with RBD Solver
  and there are no grains;
- 1 and 4 threads and restoring from state midway give the same grains and the same pieces;
- network: RBD Solver's Rigid into Grit enables both the grit handover and collisions with pieces,
  fracture color × 0.9, and no warning without points in Geometry.

In addition: the network (node, colliders, RBD Solver among them, Grain Points, missing
points, saving and loading, `tests/test_sim_network.cpp`), cache version 15
and reading version 14 (`tests/test_export.cpp`), a checkpoint of the gravel with boxes
and of the poured sand (`tests/test_state.cpp`), USD (`tests/test_usd.cpp`)
and Python `frame.grains()` (`tests/python/test_pg.py`). All three
examples pass the format test and the "all examples run" test.

## 8. Limitations

- Grains are spheres without rotation. They do not roll, so piles hold even without rolling
  resistance. Shape (gravel vs. sand) comes only from friction, not from geometry.
  `orient` is a random orientation for drawing, not simulated.
- Jacobi averaging converges slowly. Tall piles (hundreds of layers)
  sink into themselves slightly, and where the stream lands, grains can temporarily
  overlap by a third or more. More substeps help.
- Cohesion is simple: a pull toward contact plus extra friction. It has no real
  tensile strength that could be specified in pascals, and no drying.
- Grains do not interact with water (no buoyancy, soaking or washing away) and do not act
  on gas. Gas and wind only carry them along.
- In the grain step, RBD pieces only translate, they do not rotate, and inertia is taken
  as that of a box of their dimensions (as with cloth).
- Collisions with objects use the shape's signed distance field. A thin wall (thinner
  than a grain) can let a fast grain through.
- Grit as grains is a sphere with a radius of half the bit; it has the chip shape only when
  drawn. A grain does not break any further, and gas does not carry it any differently from other
  grains. Glass shards are not grains.
- A grain is more a handful of sand than a real grain: real sand would mean
  billions of grains. On 4 cores, a frame of 38,000 grains takes about 0.12 s and a frame of
  192,000 falling grains about 1 s.

## 9. References

- M. Macklin, M. Müller, N. Chentanez, T.-Y. Kim: *Unified Particle
  Physics for Real-Time Applications*, SIGGRAPH 2014.
- M. Macklin, K. Storey, M. Lu et al.: *Small Steps in Physics Simulation*,
  SCA 2019.
- E. Guendelman, R. Bridson, R. Fedkiw: *Nonconvex Rigid Bodies with
  Stacking*, SIGGRAPH 2003 (shock propagation).
- Y. Zhu, R. Bridson: *Animating Sand as a Fluid*, SIGGRAPH 2005.
