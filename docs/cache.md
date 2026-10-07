# Disk cache and export

A simulation is computed once and its frames are saved to disk. They can then
be played back, rendered with a different camera or a different look, and
exported without recomputing — in the editor, from the command line, on
another machine. The geometry of any node (water particles, raindrops, gas as
volumes, polygons) goes out in files that other programs read: points to
**PLY**, volumes to **OpenVDB**, polygons to **OBJ**, frame by frame. A whole
shot can also go to **USD** and **Alembic** as a single scene ([usd.md](usd.md),
[alembic.md](alembic.md)). That way a shot goes to Houdini, to Blender and to
renderers.

![Editor: campfire frames loaded from disk (overview: "from fire_cache", status bar: "from disk") and the Simulation menu with Save Cache and Load Cache](img/editor-cache.png)

![Frames 40, 90 and 150 of the campfire exported to VDB, loaded with the OpenVDB 10 library (pyopenvdb) and projected along the z axis: smoke (density) and flame (flame) — outside prototype](img/vdb-export.png)

## 1. Quick start

```bash
# simulate once, frames to disk ('-' instead of OUT.png: no image)
./build/prototype sim campfire_vdb - --cache cache/fire
# render from the cache: nothing is computed
./build/prototype sim campfire_vdb out/fire.png --from-cache cache/fire --every 10
# gas as OpenVDB volumes, one file per frame -- from the cache, no simulation
./build/prototype sim campfire_vdb - --from-cache cache/fire --export-node volumes --export 'out/fire.$F4.vdb'
# water particles as PLY points with velocity, foam and colour
./build/prototype sim liquid_points - --export 'out/water.$F4.ply'
```

In the editor:

1. **Simulation › Save Cache to Disk…** — a folder (selected, or a new one
   typed in); saves all computed frames. They are written in the background:
   a progress window shows how many frames are out and how much time is left,
   and the editor keeps drawing and playing in the meantime. **Stop** (or
   Escape) ends after the frame being written; what has been written stays as
   a valid shorter cache (`cache.txt` lists only those frames). Nothing is
   saved into the folder that frames are currently being read from.
2. **Simulation › Load Cache from Disk…** — frames from the folder replace the
   simulation. The network overview shows "from *folder*", the status bar
   "from disk".
3. **File › Export Geometry Frames…** — the geometry of the displayed node, one
   file per frame (`$F4` in the name is the frame number). The same for any
   geometry node: right-click the node › **Export Geometry Frames…**. Also in
   the background, with a progress window and a Stop button. The
   **Export Geometry…** item writes only the frame on screen.

The `campfire_vdb` example is a campfire with a Gas Volume node (`volumes`)
that returns the gas as three volumes: `density` (smoke), `temperature` and
`flame`.

## 2. Cache on disk

```
cache/fire/
  cache.txt             pgcache 1 / frames 150 / fps 30 / network dc17a5fbb4cdd2c0
  frame.0001.pgframe
  frame.0002.pgframe
  …
```

- A frame is a `sim::Frame` as the editor holds it: gas in half precision
  (smoke, temperature, flame in every cell), the water level byte by byte,
  water particles (position, velocity, whiteness), raindrops and rain
  ripples; from version 2 the positions, rotations and velocities of rigid
  body pieces, from version 3 also the grit and the pieces that were
  pulverised ([destruction.md](destruction.md)), from version 4 the id of
  every particle — water, drops, droplets, grit grains —, the same from
  frame to frame, and the grit velocity, from version 5 the water velocity on
  the solver grid (in the water and about two cells around the surface,
  zeros further out), from which the water surface gets `v` for motion blur ([geometry.md](geometry.md#water-surface-liquid-surface-and-convert-volume)),
  and from version 6 what happened to the rebar — one byte per bar segment in
  a piece: the bar came out of the piece, the bar is broken behind it ([destruction.md](destruction.md#reinforcement-rebar)), and from
  version 7 which grit is glass and which bodies had a joint crack — the glass
  cracks are drawn from that ([destruction.md](destruction.md#glass-glass-fracture)) —, and from version 8 what
  happened to each glue bond and when it broke (a byte and a number per bond; [constraint
  network](destruction.md#constraint-network-rbd-constraints)), and from version 9 how each grit grain is oriented
  (a quaternion in half precision, normalized again after loading; [grit as particles](destruction.md#debris-as-particles)),
  and from version 10 sparse gas: only the tiles of 8 × 8 × 8 cells that
  contain some, and their numbers at the end of the frame ([pyro.md](pyro.md#sparse-grid-compute-only-where-there-is-gas)),
  from versions 11 and 12 cloth, torn as well ([cloth.md](cloth.md)), from version 13
  sparse water: the level only in tiles near the water, the water velocity
  only in the solver tiles that contain some, and the tile numbers again at
  the end of the frame, from version 14 tiles deep in the water only by their
  numbers ([pyro.md](pyro.md#how-water-is-drawn)), from version 15 the grains
  of the Grain Solver: positions, velocities, radii, ids and colors ([grains.md](grains.md)),
  from version 16 pieces that broke during the run: for each break the body,
  the impact point in the rest position, the seed, the number of fragments and
  the time (28 bytes; [breaking during the run](destruction.md#breaking-during-the-simulation)),
  from version 17 the gas steam: its own field in half precision in the same
  tiles as the smoke, only when there is some ([quench.md](quench.md#steam)),
  and from version 18 the gas velocity: three half floats per block of
  2 × 2 × 2 cells in the same tiles, for motion blur ([cycles.md](cycles.md#motion-blur)). The rest geometry
  of the pieces, the bars and the constraint network are in the node network,
  and a frame loaded from disk gets them from it; the fragments are rebuilt
  from it and from the breaks, bit for bit identical (`rigidBroken`), and only
  once for a sequence — each further frame continues from the breaks of the
  previous one. Binary, little-endian, with a `PGFRAME` header and a version
  number; older frames can still be read.
- **Zeros are not written**: a run of zeros is a single number. The campfire
  smoke fills only part of the domain, so 150 frames of a 64 × 96 × 64 grid
  take 84 MB on disk and 138 MB in memory. The gas velocity accounts for
  15 MB of that (without it, 69 and 123 MB).
- `network` is a hash of the network text **without the node positions on the
  canvas**: a moved node is still the same network, a changed parameter is
  not. A cache from a different network (or a different version of the same
  one) is loaded, but both the editor and `prototype sim` warn about it.
- Loaded frames are valid until what is being simulated changes. The first
  such change (a source parameter, a node bypass…) discards them and the
  simulation starts again from frame 1. A change of look or camera does not
  discard them: a cache from disk can be rendered differently.
- The simulation is deterministic, and so frames from the cache are bitwise
  identical to computed ones. A render from the cache is the same PNG file as
  a render after simulation (verified with `cmp`), and VDBs exported from the
  editor and from the command line are byte for byte identical.
- Reading does not trust the file: a frame whose parts do not match its grids
  (cell, particle, ripple counts) is rejected. A damaged file therefore cannot
  send the renderer out of bounds.

## 3. Background bake, checkpoints and preview

A large simulation (blast dust in 576 cells: 6 s per frame, 19 minutes per
shot) is not computed in the editor window. Like Save to Disk in Background
in Houdini, a separate process computes it straight to disk, the editor stays
free and plays the frames back as they arrive.

![Editor during a bake: the overview shows progress 77 / 150, frame time, estimated time to finish and a checkpoint at frame 70; the status bar shows "baking 77 / 150, 26 s left"; frames are played back from disk](img/editor-bake.jpg)

In the editor, the **Simulation** menu:

- **Preview Resolution** — gas and water on coarser grids, half resolution by
  default (campfire 32 × 48 × 32 instead of 64 × 96 × 64, a step of 63 ms
  instead of ~400 ms). For tuning sources, forces and timing. How fine the
  grids of the preview are is set by the **Preview** parameter of the Output
  node (0.25 for quarter); **Open in Preview** opens a network straight in
  preview — a scene that takes hours to compute in full (`flood_crates_hd`).
  The overview shows "Preview — grids half as fine", the status bar
  "preview". A bake is always at full resolution.
- **Bake to Disk…** — a folder (default `<network>_bake`) and in it the whole
  shot at full resolution: the editor writes the network to `network.pgsim`
  and starts `prototype sim network.pgsim - --cache FOLDER --checkpoint 10` as
  a separate process (output goes to `bake.log`). The overview shows the
  progress, the frame time, the estimated time to finish, the frame of the
  last checkpoint and a **Cancel Bake** button; the status bar shows
  "baking 77 / 150, 26 s left". Frames are played back from disk as they
  arrive. At the end a notification arrives with a link to the folder.
  Closing the editor does not stop the bake.
- **Cancel Bake** — the process ends. Finished frames remain, along with the
  last checkpoint.
- **Resume Bake** — continues an interrupted bake (cancelled, crashed, machine
  switched off) from the last checkpoint, not from the beginning. It is
  offered only when the checkpoint in the folder belongs to the same network.

**Playback from disk.** Both Load Cache and a bake read frames only at the
moment they are needed, ahead of the playhead; a cache larger than memory can
be played back and scrubbed. See [Large caches in the viewport](#large-caches-in-the-viewport).

A **checkpoint** (`checkpoint.pgstate`) is the full simulation state
(`WorldSolver::saveState`), not a half-precision frame:

- gas: fields in floats (including steam and source soaking), active tiles,
  cells and walls of obstacles;
- water: particles, their ids, the velocity, distance and pressure grids
  (the pressure is the initial guess for the next solve), counters;
- rain: drops, droplets, ripples on the surface.

Rigid bodies are not in the checkpoint. They are coupled one way (gas, water
and rain flow around them, not the other way round), and their step is cheap
compared with the gas. On restore they are therefore recomputed from frame 1
up to the checkpoint frame: the pieces, their dust and the scenes they give to
the others. Then the rest is loaded. The continuation is **bitwise identical**
to a simulation that never stopped: verified on sparse and dense gas, a moving
source, water, rain on water and a blast with dust (tests), and on a bake that
the editor cancelled and restarted (150 frame files identical according to
`cmp`).

The checkpoint is overwritten every K frames (`--checkpoint K`, the editor
uses 10) and deleted when the bake finishes, because it is large. For dust in
576 cells it holds ~24 million active cells × 9 fields, i.e. just under 1 GB.

`cache.txt` is rewritten after every frame during a bake and tells how far the
bake has got:

```
pgcache 1
frames 43
fps 30
network a5878458fd369a86
of 150            # how many frames the bake is making; absent when it is finished
ms 75.1           # average frame time
checkpoint 40     # the frame whose state is in checkpoint.pgstate
```

Older readers skip unknown lines. Every file (frame, `cache.txt`, checkpoint)
is first written next to its target as `.part` and then renamed. Anyone reading
the folder during a bake finds each file either complete or absent. A process
killed in the middle of a write leaves the finished frames valid.

From the command line:

```bash
# bake with a checkpoint every 10 frames
./build/prototype sim demolition - --cache bake/demo --checkpoint 10
# ... interrupted: continues from the last checkpoint
./build/prototype sim demolition - --cache bake/demo --checkpoint 10 --resume
# quick preview: gas and water on half-resolution grids
./build/prototype sim demolition preview.mp4 --preview 0.5
```

```
$ prototype sim campfire - --cache bk2 --frames 60 --checkpoint 10   # killed at frame 43
$ cat bk2/cache.txt
... frames 43 / of 60 / ms 75.1 / checkpoint 40
$ prototype sim campfire - --cache bk2 --frames 60 --checkpoint 10 --resume
resumed at frame 40 from bk2/checkpoint.pgstate (0.0 s)
campfire: simulated, gas 64 x 96 x 64 cells, 60 frames, from frame 41; simulation 160.4 ms/frame
```

The same in Python, for your own farm:

```python
sim = net.simulate(preview=0.5)          # preview
state = sim.save_state()                 # bytes: checkpoint
later = net.simulate(preview=0.5)
later.load_state(state)                  # the next step() is the frame after the checkpoint
```

### Wedge: parameter variants

When tuning the look of a simulation, several versions are compared at once.
Right-clicking the name of a numeric parameter › **Wedge…** offers a range
(the default is half to one and a half times the value) and a number of
variants (2 to 16). The editor then computes them one after another, each as
a full-resolution bake into its own folder. Wedge TOP in Houdini does the
same.

![Wedge of the campfire turbulence strength: three variants 1.75, 3.5 and 5.25, each computed in a minute and a quarter, with a Show button; notification "3 of 3 variants baked"](img/editor-wedge.jpg)

```
campfire_wedge_turbulence_strength/
  wedge.txt          pgwedge 1 / node turbulence / param strength /
                     variant strength_1 1.75 / variant strength_2 3.5 / ...
  strength_1/        cache as from Bake to Disk (network.pgsim, bake.log, frames)
  strength_2/ ...
```

The overview shows the progress, the bake time and a **Show** button for each
variant. It puts the variant's value into the parameter and plays its frames
back from disk. A variant can be played even while it is still baking.
**Cancel Wedge** stops the variant being baked and those waiting. Finished
variants remain.

From the command line the same is done with a loop over `--set`:

```bash
for s in 1.75 3.5 5.25; do
  ./build/prototype sim campfire - --cache wedge/strength_$s --set turbulence.strength=$s
done
```

### Profile: where the step time goes

The **Profile** section in the simulation overview (collapsed, click to open)
shows, for the frame on screen, how many milliseconds the individual parts of
the step took:

- debris (Jolt) and its conversion into the scenes of the others (obstacles,
  dust);
- gas and its phases: obstacles, tiles, sources, advection, combustion,
  forces, pressure, dissipation;
- water and rain.

![Step profile of a blast: debris 1 %, gas 99 %, of which advection 51 %, pressure 21 %, obstacles 12 %, forces 11 %](img/editor-profile.png)

The command line prints the same as an average over the whole run, also into
`bake.log`:

```
demolition: simulated, gas 88 x 48 x 88 cells, 60 frames; simulation 48.3 ms/frame
time: pieces 4% into scenes 0% gas 96% (solids 6%, tiles 1%, emit 2%, advect 50%, combust 0%, forces 11%, project 25%, dissipate 0%)
```

The profile shows what to speed up or coarsen. For the blast dust it is not
the pressure but the advection (MacCormack for smoke, temperature and
velocity). The profile is kept only in memory. A frame read from disk does not
have it, and the cache format does not change.

### Large caches in the viewport

Simulation frames are held by `sim::FrameStore`: in memory at most as much as
**Simulation › Cache Size** allows (512 MB to 8 GB, default 1.5 GB, from the
command line `prototype --cache-size MB`). What does not fit goes to disk and
is read back as playback proceeds — as in Houdini, where the in-memory cache
is complemented by a disk cache.

![The editor playing 150 frames of a flood from disk with a 64 MB cache: on the timeline a dark band of all frames on disk and a light band of the 70 in memory around the playhead (more ahead of it than behind it); in the overview "150 frames · 70 in memory, 63 MB"](img/editor-big-cache.jpg)

- **Paging to disk** (Cache Size › **Past It to Disk**, on): a simulation
  longer than fits in memory keeps running. The frames farthest from the
  playhead are written to the editor's temporary folder
  (`prototype-frames-<pid>-<n>` in the system temp) and dropped from memory;
  the whole simulation plays back. The folder is deleted along with the frames
  (another network, a parameter change, the editor quitting). Folders left
  behind by an editor that crashed or was killed are deleted by the next
  editor that starts (the process is no longer running and the folder is
  older than 10 minutes). Off: full memory stops the simulation ("cache …
  full"), as before. When the disk is not enough (full disk, at most 64 GB),
  the overview shows "nothing more to disk" and the simulation waits.
- **Read-ahead.** A dedicated thread reads frames ahead of the playhead in the
  direction of playback, as many as the budget allows (at most 240), and
  behind it half that distance. The frames farthest from the playhead leave
  memory first, those behind it twice as soon as those ahead of it. The
  timeline never stops to wait for the disk: playback moves to the next frame
  once it is loaded, and scrubbing shows the last frame with the label
  "reading frame N…" until the one under the playhead arrives. Export,
  render and video wait for their frame.
- **Pieces, cloth and grains** are also prepared for drawing on a dedicated
  thread (`pg/sim/Prepared.h`): the frame under the playhead first, and during
  playback also the next two ahead. Playback moves to the next frame once its
  pieces are ready, and until then scrubbing shows the whole previous frame,
  so the gas and the pieces on screen always come from the same frame. The
  window does not wait meanwhile: a collapsing house (`house_collapse`) used
  to cost the window 160 ms per frame, now it costs only sending the pieces to
  the GPU (~18 ms).
- **Gas, water and rain** are prepared the same way, on another thread: cells
  unpacked from tiles, gas packed into four channels (smoke, temperature,
  flame, steam), the grid coarsened to fit, and the rain streaks. A frame is
  waited for in the same way as the pieces, and one frame beyond the next is
  prepared ahead. The full frame after stopping is also prepared in the
  background, and until then a proxy stays on screen. Finished results that
  nobody wants any more are discarded immediately, because a full frame of a
  large gas can be gigabytes. The window then only sends the data to the GPU.
  The campfire with upres (4 million cells) used to cost the window a median
  of 11 ms on every frame and up to 210 ms for the viewport, plus up to
  174 ms for the node thumbnails. Now the window does only GPU work. Node
  thumbnails draw the bodies, gas and water of a frame as soon as they are
  ready. Until then they show the last finished frame, usually a frame or two
  older, and then redraw.
- **The timeline** shows what is where: a dark band for frames on disk, light
  bands for frames in memory. Overview: "Cache 150 frames · 70 in memory,
  63 MB" and "On Disk 150 frames, 120 MB"; status bar "cache 150 / 150 (63 MB,
  0.1 GB on disk)".
- **Proxy grids** (View › **Proxies**, on): large gas and water frames go to
  the GPU during playback and scrubbing on a coarser grid, at most
  ~4 million cells (2×, 4× or 8× coarser, a cell is the average of those
  under it). As soon as the frame has not changed for 0.35 s, it is uploaded
  in full. The corner of the viewport shows "Frame 8 · 0.27 s · proxy".
  Small frames (campfire, flood at 96) are not reduced.

Measured on 4 cores:

| | |
|---|---|
| `flood_crates` (water 96 × 24 × 48), 150 frames, Cache Size 64 MB | 59–70 frames in memory (~0.9 MB per frame), 120 MB paged to disk; the simulation runs to the end and plays back in full |
| `flood_crates_hd` (512 × 128 × 256, 17.6 million particles) from disk | frame 23–27 MB; read 57–89 ms (average ~70 ms), playback from disk ~15 frames/s; 8 frames in memory 195 MB |
| same, water on the GPU | 16.8 million cells; during playback a proxy grid of 256 × 64 × 128 = 2.1 million (8× less data) |

In code: `FrameStore::get(n)` waits (export, render), `ready(n)` never does —
it returns the frame if it is in memory, otherwise null, and queues the frame
to be read first. `setPlayhead(frame, direction)` controls what is kept and
what is read ahead. Frames are added by the simulation (`add`) with a
generation number: a frame from a simulation that has ended in the meantime
(another network) is not added.

## 4. Export

| extension | what it writes | what reads it |
|---|---|---|
| `.ply` | points with attributes, closed polygons as `face`; binary, little-endian | Houdini, Blender, MeshLab, CloudCompare, ParaView |
| `.vdb` | volumes as OpenVDB float grids (file version 224) | Houdini, Blender, renderers (Arnold, Redshift, V-Ray, Cycles, Karma) |
| `.obj` | points, polygons, lines | almost everything |
| `.usda` | polygons as Mesh, lines, points; without `$F` in `--export`, the whole shot — bodies, grit, dust, camera, lights ([usd.md](usd.md)) | Houdini (Solaris), Blender, usdview, renderers |

The format is determined by the extension (case does not matter). A `.vdb`
from geometry without volumes is not written — the error says that volumes are
made by the Gas Volume node.

### PLY

| point attribute | PLY properties |
|---|---|
| `P` | `x y z` |
| `N` | `nx ny nz` |
| `Cd` | `red green blue` — bytes 0–255; for `vec4` also `alpha` |
| `v` | `vx vy vz` |
| number | its name: `float`, an integer as `int` (exactly, not through float) |
| vector | `name_x name_y name_z` (`_w` for `vec4`) |
| string | skipped |

The same PLY module also reads (`io::readPly`), both ASCII and binary
little-endian: the same names give the same attributes again, byte colors are
divided by 255. It does not trust a header that promises more elements than
the file can hold, and does not even allocate space for them.

### OpenVDB

The writer is our own, without the OpenVDB library, so the core stays free of
dependencies. The file has the layout that OpenVDB 10 writes:

- a version 224 header, a UUID derived from the content (same volumes = same
  bytes), `creator` metadata;
- for each grid a name, type `Tree_float_5_4_3`, metadata `class`, `name`,
  `file_bbox_min`, `file_bbox_max`, `file_voxel_count`, `file_mem_bytes`;
- a `UniformScaleTranslateMap` transform: voxel (i, j, k) has its center at
  `origin + (i + ½, j + ½, k + ½) · voxel`, exactly like a simulation cell;
  the volume sits where the smoke was;
- the tree: root, internal nodes 32³ and 16³, leaves 8³. Non-zero voxels are
  active, only leaves that contain some are written, the background is 0;
- `class` is `fog volume` when the values are positive — that is how
  renderers draw smoke.

Verified by reading in OpenVDB 10.0 (pyopenvdb): the number of active voxels,
their sum, the bounding box and the position of voxel 0 match the simulation
grid to the last bit (campfire, frame 12: `density` 6906 voxels with a sum of
1091.8103, `temperature` 7140 / 8241.4198, `flame` 6489 / 549.1078 — in the
VDB file and in the cache frame).

Gas Volume produces the grids `density`, `temperature` and `flame`: that is how
the pyro shaders of Houdini and Blender name them. The gas velocity goes into
a vector grid `vel` (`Tree_vec3s_5_4_3`, `vector_type` invariant) with a voxel
twice as large, one per block of 2 × 2 × 2 cells: Houdini, Blender and
renderers use it to motion-blur the gas. OpenVDB 10 reads it as a
`Vec3SGrid`, and every smoke voxel has a velocity in it (campfire, frame 24:
4480 voxels, fastest 2.1 m/s).

They are read back by the **VDB Gas** node: it plays the files back as the gas
of the shot, with the same half in every cell as the simulation. Files from
other programs are also read by **VDB Import** — as volumes, or as polygons of
their surface ([vdb.md](vdb.md)).

## 5. Command line

```
prototype sim NETWORK.pgsim|EXAMPLE OUT.png|- [--frames N] [--start N] [--every K] ...
             [--cache DIR [--checkpoint K] [--resume]] [--from-cache DIR] [--export PATH] [--export-node NODE]
             [--preview F]
```

| option | what it does |
|---|---|
| `--cache DIR` | every frame into the folder `DIR` (creates it); after every frame `cache.txt` says how far it has got |
| `--checkpoint K` | with `--cache`: the full simulation state into `DIR/checkpoint.pgstate` every K frames |
| `--resume` | with `--cache`: continues from the checkpoint in the folder (of the same network); the frames before it are already on disk |
| `--preview F` | gas and water on grids F times as fine (0.5: half resolution, at least 16 cells) |
| `--from-cache DIR` | reads frames from the folder instead of simulating; `--frames N` takes at most N of them |
| `--start N`, `--end N` | images and export only from frame N (up to `--end`, which is the same as `--frames`) — a part of the shot for one farm machine; the cache is read from N, but the simulation starts at frame 1 and every frame goes into the cache |
| `--export PATH` | the geometry of the displayed node from every frame into a file; `$F4` is the frame number with four digits, `$F` without zeros. Without them the number is inserted before the extension (`fire.vdb` → `fire.0007.vdb`). Folders are created. Exceptions: `.usda` without `$F` is the whole shot as a single scene, with what changes in per-frame files next to it ([usd.md](usd.md)); `.abc` is the whole shot as a single Alembic archive ([alembic.md](alembic.md)). |
| `--export-node NODE` | the geometry of node `NODE` instead of the displayed one |
| `--folder DIR` | reads the network's relative paths (meshes, OBJ files) from `DIR`, not from the folder of its file — for a network saved elsewhere, as `Network.render` does in Python ([python.md](python.md)) |
| `-` instead of `OUT.png` | no image, only cache and export — also works in a build without EGL |

`--every K` applies only to images: the cache and the export get every frame.

Farm: one machine simulates into the cache, the others each render or export
their own part from it:

```
prototype sim demolition - --cache cache/demo                                   # machine 1
prototype sim demolition shot.png --from-cache cache/demo --start 1 --end 60 --every 1
prototype sim demolition shot.png --from-cache cache/demo --start 61 --end 120 --every 1
```

```
$ prototype sim campfire_vdb - --cache cache/fire
campfire_vdb: simulated, gas 64 x 96 x 64 cells, 150 frames; simulation 73.7 ms/frame
cached 150 frames in cache/fire
$ prototype sim campfire_vdb fire.png --from-cache cache/fire --every 50
wrote fire_0150.png and 2 before it: campfire_vdb, read from cache/fire, 150 frames (5.0 s); reading 0.7 ms/frame, rendering 495 ms/image
$ prototype sim campfire_vdb - --from-cache cache/fire --export-node volumes --export 'out/fire.$F4.vdb'
campfire_vdb: 150 frames read from cache/fire; reading 1.2 ms/frame
exported 150 frames of geometry, the last out/fire.0150.vdb
```

The cache is 84 MB, the 150 VDB files 152 MB (uncompressed, see limitations).

## 6. In code

| file | what it does |
|---|---|
| `src/pg/io/Ply.h` | `formatPly`, `writePly`, `parsePly`, `readPly` |
| `src/pg/io/Vdb.h` | `formatVdb`, `writeVdb` |
| `src/pg/io/Export.h` | `writeGeometry` by extension, `framePath` (`$F4`, `$F`) |
| `src/pg/sim/Cache.h` | `formatFrame` / `parseFrame`, `writeFrame` / `readFrame`, `writeCacheInfo` / `readCacheInfo` (including bake progress), `networkHash`, `writeCheckpoint` / `readCheckpoint`, `writeWhole` (writing via `.part`) |
| `src/pg/sim/State.h` | `StateWriter` / `StateReader`: solver state as bytes, reading checks every length |
| `src/pg/sim/World.h` | `WorldSolver::saveState` / `loadState` (bodies are recomputed), `preview` |
| `src/pg/sim/FrameStore.h` | frames in memory within the budget, paged to disk beyond it, read-ahead ahead of the playhead on a dedicated thread; `get` (waits) and `ready` (never) |
| `tools/prototype/SimRunner.h` | simulation on a dedicated thread, frames in the `FrameStore`; `stream`: cache from disk; `refresh` finds new frames from a bake |
| `tools/prototype/Bake.h` | the bake process (`posix_spawn`), progress, estimate, cancellation, `canResume` |
| `tools/prototype/Wedge.h` | wedge: a queue of bakes, one per parameter value, `wedge.txt` |
| `src/pg/sim/Frame.h` | `Frame::Profile`: where the step time went (parts, gas phases); `WorldSolver::profile` |
| `tools/prototype/SimWorkspace.cpp` | menu, dialogs, saving, loading, export |
| `tools/prototype/Widgets.h` | `FileBrowser::openFolder`: folder selection (including a new one) |

## 7. Tests

`tests/test_export.cpp`, 13 tests:

- PLY round trip with all kinds of attributes — including the integer
  2²⁴ + 1, which a float would not preserve —, byte colors and polygons;
  ASCII with CRLF, `alpha`, `short` and `uint` lists; rejection of big-endian,
  a truncated file, a header that promises more than the file has, and a
  polygon with an out-of-range vertex;
- VDB as OpenVDB reads it: header, metadata, grid positions up to the end of
  the file; class, voxel count, bounding box, determinism, a suffix for two
  grids with the same name;
- sequence numbering (`$F4`, `$F`, no pattern) and writing by extension,
  including errors;
- frame round trips: a made-up frame with all parts and zero runs of all
  lengths, real frames of the campfire, of water with particles and of rain;
  rejection of truncated, newer and nonsensical frames, a flipped byte never
  crashes; a version 3 frame can still be read (without particle ids), and
  version 4 too (without water velocity); ids that are not one per particle
  and a water velocity that is not on the solver grid are rejected;
- a cache folder with `cache.txt` (fps 30, not 29.999998) and a network hash
  without node positions.

`tests/test_frame_store.cpp`, 5 tests: what fits stays in memory; beyond the
budget frames are paged to disk and read back byte for byte identical (and
`clear` deletes the folder); without paging, memory is full; a cache from disk
is read ahead of the playhead and backwards too, what the playhead leaves
goes out of memory, a missing file returns null; and the same from several
threads at once with the simulation, the timeline and export (also under
ThreadSanitizer with no reports).

`tests/test_state.cpp`, 11 tests: continuation from a checkpoint bitwise
identical to an uninterrupted simulation (sparse and dense gas, a moving
source, water, rain on water, a blast with dust; every frame after restore
compared as cache bytes), rejection of a state for a different grid, for
different parts of the world, truncated anywhere, and of a state for a solver
that has already stepped; preview changes only the grids; `cache.txt` with
progress round trip; a checkpoint on disk overwritten as a whole; the step
profile (parts, gas phases within its time, not saved in the cache). Python:
`save_state` / `load_state` and `preview` (`tests/python/test_pg.py`).

## 8. Limitations

- VDB is written only as float grids and the vector `vel`. More can be read:
  compressed, half, level sets, other kinds of vectors ([vdb.md](vdb.md)).
  The water surface as a level set is not written — water goes out as
  particles or as a surface (Liquid Surface, in USD `/World/water`).
- There is no compression inside the VDB (zip, blosc): the files are larger
  than Houdini would write.
- A cache frame holds what the editor shows (half precision), not the solver
  state: simulation continues only from a checkpoint, and there is only one,
  the latest. A loaded cache without a checkpoint is not continued.
- A checkpoint is the state of this build: a different format version is
  rejected, not converted. Rigid bodies are recomputed from the beginning on
  restore. For a blast (593 pieces, gas 96 cells) restoring at frame 120
  takes 0.6 s and the checkpoint is 23 MB; for thousands of pieces and long
  shots it will be more.
- A frame paged to disk is in the cache format (half precision, zero
  compression), not compressed like VDB or Alembic; reading is limited by
  disk speed and unpacking (~340 MB/s per frame). Proxy grids reduce only what
  goes to the GPU, not particles or reading.
- A bake runs on the same machine as the editor (a process, not a farm queue)
  and only on Linux (`/proc/self/exe`, `posix_spawn`).
- PLY reads only `vertex` and `face` elements and skips the others.
- Alembic is only the whole shot as a single archive (`--export shot.abc`,
  [alembic.md](alembic.md)), not a file per frame.
