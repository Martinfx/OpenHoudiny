# Reading OpenVDB

Programs exchange smoke, fire and distance fields (level sets)
as OpenVDB (`.vdb`): Houdini, Blender, EmberGen, renderers. Prototype has
always written it ([cache.md](cache.md)); now it also **reads** it, without the
OpenVDB library. Two nodes:
- **VDB Import** (geometry) outputs the grids as volumes, or directly as polygons of
  their surface: an obstacle for a simulation, the shape of a smoke or water source;
- **VDB Gas** (simulation) plays the files back as the shot's gas, frame by
  frame. Volume Look draws it like Pyro Solver gas; it is rendered by the
  viewport, the path tracer and Cycles, and it goes into the cache and into exports to USD
  and Alembic.

The same reader also reads volumes from USD scenes (a `Volume` prim with
`OpenVDBAsset` fields): the USD Import node places them according to the transform in the
scene ([usd-import.md](usd-import.md)).

![A fireball from a VDB file (vdb_fireball example): density, temperature and flame, written by prototype's export and rewritten by OpenVDB 10 into half floats with Blosc compression. VDB Gas reads them, nothing is simulated; the fire lights the crates (Cycles)](img/vdb-fireball.jpg)

## 1. Quick start

```bash
./build/prototype sim vdb_fireball out/fireball.png     # fireball from a file, firelight on the crates
./build/prototype sim vdb_rock out/rock.png --every 15  # a water jet hits a boulder from a VDB
# own export and back: campfire to VDB, then VDB Gas with File 'out/fire.$F4.vdb'
./build/prototype sim campfire_vdb - --export-node volumes --export 'out/fire.$F4.vdb'
```

In the editor:
- **Shift+A › Geometry › VDB Import:** grids from a file as volumes, with
  **Surface** as polygons;
- **Shift+A › Simulation › VDB Gas:** connects to the Gas input of the Volume
  Look node in place of the Pyro Solver.

![The editor with the vdb_fireball example: the VDB Gas node (fireball) with a gas thumbnail and the domain the file set up (56 × 56 × 56 cells), its parameters, and in the viewport the fireball from the file between the crates](img/editor-vdb.jpg)

## 2. VDB Import

Each grid is a volume of its own name; a vector grid becomes three
(`vel.x`, `vel.y`, `vel.z`). The volume has every voxel of the box that contains the
grid's active voxels. Inactive voxels in it carry their value, so a level
set has a negative value inside, as OpenVDB holds it.

| Grid | Volume |
|---|---|
| `float`, `double`, `half`, `int32`, `int64` | one volume (`density`, `temperature`, `surface`…) |
| `vec3s`, `vec3d`, `vec3i` | three volumes, one per component |
| `level set` class | with **Surface**, the surface where the distance crosses zero (inside below it) |
| others (`fog volume`…) | with **Surface**, the surface where the value crosses **Iso** (inside above it) |

Parameters:
- **File:** a file, or a numbered sequence (`smoke.$F4.vdb`,
  `smoke.####.vdb`) read one file per frame. A relative path is resolved from the
  network's folder. A frame without a file is empty, with a warning.
- **Grids:** which grids to read, names separated by spaces. An empty field
  means all of them.
- **Frame Offset:** frame f reads file f + offset (a sequence starting at 1001 is
  read from frame 1 with an offset of 1000).
- **Z Up:** the file's world has the Z axis up (Blender). It is rotated around X so
  that Y is up, and vectors with it.
- **Max Voxels:** the maximum number of voxels in a volume, in millions. A larger grid is
  averaged 2 × 2 × 2 voxels into one (or more), and the node reports it.
- **Surface**, **Iso:** polygons instead of volumes, for Object (collisions), Pyro
  Source or Water Source.

## 3. VDB Gas

Plays files back as the shot's gas:
- **Grids:** density (`density`), temperature (`temperature`, in Blender
  `heat`) and flame (`flame`, `flames`, `fire`) are looked up by name;
  the first one the file has wins. Vapor is read only from a grid the node
  names. Scales (**Density Scale**…) convert another program's scale.
- **Velocity:** the vector grid `vel` (or `v`, `velocity`; the names are given by
  **Velocity**), in units per second, times **Velocity Scale**. The frame
  holds it in blocks of 2 × 2 × 2 cells, and the renderers use it for gas motion
  blur ([cycles.md](cycles.md#motion-blur)). A file without it gives
  gas without blur.
- **Domain:** when the network is compiled, the headers of the files of all frames of the
  shot are read (headers only, fast for files of any size; a grid whose
  bounding box OpenVDB did not write into the metadata is read in full), and the domain is made
  so that it holds everything. Like every domain, it stands on the floor around the
  Y axis, so the files' voxels are laid onto its cells: each one moves
  by at most half a voxel. Whatever is below the floor is not drawn; **Move** shifts the gas
  (up, to the middle of the scene).
- **Resolution:** the maximum number of domain cells along the longer side (default 512). When
  the domain would be finer, the files are averaged.
- **Frames:** frame f reads file f + **Frame Offset**. A frame without a file
  has no gas. A file that changes is read again (just as when
  the network changes).
- Nothing is simulated: the gas does not push pieces, water does not extinguish it, Pyro Upres does
  not refine it.

Our own export and back is exact: a campfire written to VDB (Gas Volume,
`--export`) and played back through VDB Gas has the same half in every cell as the
simulation and the same velocity in every block (test), and a render through the same camera (OpenGL) differs from the simulation
by at most 15 out of 255 in pixel brightness, by 0.3 on average. The only difference comes from the smaller
domain (different floor shading behind it).

![A water jet hits a boulder from a VDB file (vdb_rock example): a level set that VDB Import turned into polygons and Object into an obstacle; frames 15, 30 and 45 (Cycles)](img/vdb-rock.jpg)

## 4. What is read

The reader reads files the way OpenVDB reads them:
- **File versions 222 to 225:** anything written by OpenVDB 1.0 (2013) through 13.
  Older files cannot be read even by OpenVDB 13. Version 225 (OpenVDB 13) added half
  grids; those are read too.
- **Compression:** zip (zlib) and Blosc (LZ4, LZ4HC, zlib, BloscLZ; with byte
  shuffle and without, blocks split by bytes), active mask (only active
  values) and full nodes, half floats.
- **Inactive values** in all seven ways OpenVDB writes them
  (background, minus background, a single value, a mask between two…).
- **Tree:** root, internal nodes 32³ and 16³, leaves 8³, and tiles at all
  levels (one value for a whole node, even 4096³ voxels in the root).
- **Instances:** a grid that shares the tree of another grid, with its own
  transform.
- **Stream without offsets** (`io::Stream`): grids are read one after another.
- **Transforms:** translation and scale voxel by voxel, as well as quarter-turn
  rotations and mirroring. A rotated grid, or voxels that are not cubes, are
  resampled by the reader to cubes with an edge equal to the voxel's shortest side.

Grids that are not read (`bool`, `mask`, `string`, points), and grids
in a camera's perspective frustum, are only listed by the node. A corrupted file is
rejected by the reader with a reason.

## 5. Verification

The test files were written by the OpenVDB library itself, and the reader reads them voxel by
voxel against the rules by which they were created:
- **OpenVDB 10.0.1 (pyopenvdb, Blosc LZ4):** smoke and temperature
  (`tests/data/vdb/make_vdb.py`), the same in half floats, a level set sphere,
  tiles from `fill`, a vector grid with non-cubic voxels, a grid
  rotated by 30°, instances and two grids of the same name, `bool` and frustum.
- **OpenVDB 13.0 (built from source, file version 225):** zip with active
  mask and without, uncompressed, inactive values in all ways, `double`,
  `int32`, `int64`, `vec3d`, `vec3i`, a half grid, a float stored as half,
  a quarter-turn rotation, mirroring, root tiles, a stream without offsets
  (`tests/data/vdb/make_vdb13.cpp`).
- **Blosc:** the decompressor decoded all 5376 frames produced by
  c-blosc 1.21 (python-blosc) across codecs, shuffle, type sizes, levels
  and block sizes, byte for byte; 126 thousand corrupted variants it rejected or
  decoded without crashing (under ASan). Six such frames are in
  `tests/data/blosc` (`make_blosc.py`).

**Writing with compression.** The export writes grid values in Blosc frames
(LZ4, swapped bytes: shuffle) as Houdini does, or zipped (zlib),
or uncompressed (`io::VdbCompression`, default Blosc). The compressors are
our own: deflate (LZ77 via hash chains in a 32 KiB window, dynamic
Huffman codes, a stored block where the code would not be smaller), LZ4 blocks
and Blosc 1 frames. Blocks are split into per-type-byte streams where c-blosc 1
always split them, so every version can read the frame. Values under 48 bytes, or those
that compression would not shrink, stay uncompressed, as
OpenVDB does.
- **zlib 1.3 and c-blosc 1.21** (Python) unpack frames from our compressors
  byte for byte: smooth floats, text and empty data, 0 to 4.4 MB. Deflate
  comes out the same size as zlib at level 6 (1,078,712 versus 1,078,709
  bytes), Blosc 10 % larger than LZ4 from c-blosc.
- **Blender 4 (OpenVDB 12)** loads a small cloud (density and vector `vel`)
  uncompressed, zipped and in Blosc. It renders all three in Cycles identically to the
  pixel. A 48³ cloud: 189 kB uncompressed, 72 kB zip, 85 kB Blosc.

Tests — `tests/test_vdb_read.cpp` (24):
- Blosc frames from c-blosc and split blocks;
- all the files above, value by value;
- `vdbGrids` from headers only, averaging of large grids, selection by name,
  Z axis up;
- reading back our own writes, also with zip and Blosc, and our own zlib and Blosc
  frames read back; corrupted files rejected without crashing;
- the VDB Import nodes (volumes, level set surface, sequence) and VDB Gas
  (domain, frames, playback in the WorldSolver, file change), export
  and playback of the campfire bit for bit, including velocity.

## 6. In the code

| File | What it does |
|---|---|
| `src/pg/io/VdbRead.cpp` | Reader: header, descriptors, metadata, transforms, tree, values; dense volume in world space (`readVdb`, `parseVdb`), headers (`vdbGrids`) |
| `src/pg/io/Vdb.cpp` | Writer: header, descriptors, tree, values uncompressed, zip or Blosc |
| `src/pg/io/Blosc.h` | Blosc 1 frames: decompression (BloscLZ, LZ4, zlib, shuffle) and compression (LZ4, shuffle) |
| `src/pg/io/Lz4.h` | LZ4 blocks (shared with USD): reading and writing |
| `src/pg/io/Deflate.h` | deflate and zlib streams: compression (`Inflate.h` reads) |
| `src/pg/nodes/Vdb.cpp` | The VDB Import node (`vdbimport`) |
| `src/pg/sim/VdbGas.h` | Domain from the shot's files and a frame's gas from its file |
| `src/pg/sim/World.cpp` | Playback instead of gas simulation |
| `src/pg/sim/Network.cpp` | The `vdb_import` and `vdb_gas` nodes |
| `examples/vdb/make_vdb.py` | How the example files were created (prototype + pyopenvdb) |
| `tests/data/vdb/make_vdb.py`, `make_vdb13.cpp` | Test files from OpenVDB 10 and 13 |

## 7. Limitations

- **Writing** of float grids and vector `vel`, without half floats and without
  grids of other types ([cache.md](cache.md)). Compression is chosen in code
  (`VdbCompression`); in the export it cannot yet be switched by a parameter.
- **Velocity** is read only from a vector grid (`vec3s`, `vec3d`). Three
  float grids with separate components are not taken as velocity by VDB Gas.
- The **domain** stands on the floor around the Y axis; gas far from it makes a large
  domain (sparse, but with dense conversions on export). **Move** brings it closer.
- **Dense volume:** VDB Import holds every voxel of the box of active voxels,
  hence **Max Voxels**. VDB Gas frames hold only the 8³ tiles that contain gas,
  but when reading, each grid is briefly expanded to the whole box.
- A **VDB Gas frame** is read in full, even when only part of it is drawn.
