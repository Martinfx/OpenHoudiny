# Compute on the GPU (Vulkan)

Prototype computes on the CPU's threads. The solvers that cost the most — the
gas of the Pyro Solver first — are to move to the graphics card, through
**Vulkan compute**: it runs on NVIDIA, AMD and Intel cards, on Linux,
FreeBSD and Windows, with the drivers the card already has.

What is there now: the foundation (`pg::gpu`) — the devices, memory on the
device, kernels compiled when the program is built and carried in it,
`prototype gpu`, which measures a device against the CPU — and the first
of the gas solver's work on it: **the Pyro Solver's GPU switch steps the gas
on the graphics card** — advection, combustion, forces, pressure — with the
same result to the bit as on the CPU (§7).

## 1. Quick start

```bash
./build/prototype gpu                     # the devices; the best GPU measured against the CPU
./build/prototype gpu --device nvidia     # the device whose name has "nvidia" in it
./build/prototype gpu --device 1          # the second device in the list
./build/prototype gpu --device llvmpipe   # a CPU pretending to be a GPU (Mesa's lavapipe): slow, the same results
```

On a machine with no GPU but Mesa's lavapipe (four cores):

```
Vulkan devices:
  0  llvmpipe (LLVM 20.1.2, 256 bits) -- CPU, 16094 MB, llvmpipe Mesa 25.2.8-0ubuntu0.24.04.2 (LLVM 20.1.2), Vulkan 1.4.318
on 0, llvmpipe (LLVM 20.1.2, 256 bits):
            GPU             CPU, 4 threads
  memory    3.9 GB/s        44.0 GB/s       0.1x    y = 2x + y over 4M numbers: right
  sum       97.54 ms                                of the 4M numbers: the same to the bit as on the CPU
  pressure  0.17 Gcells/s   0.43 Gcells/s   0.4x    Jacobi sweeps over 128^3 cells: the same to the bit as on the CPU
```

A CPU device is never chosen on its own, only when named: it is slower than
the program's own threads. It is there to test the kernels on a machine
without a GPU.

## 2. Building

Nothing is linked against Vulkan. The program opens the Vulkan loader
(`libvulkan.so.1`, `vulkan-1.dll`) when a device is first asked for; without
it, or without a device, everything runs on the CPU as before.

To build the GPU part, CMake needs the **Vulkan headers** and
**glslangValidator**, which compiles the kernels to SPIR-V. To run it, the
**Vulkan loader** and the card's driver with Vulkan.

| System | To build | To run |
|---|---|---|
| FreeBSD | `pkg install vulkan-headers glslang` | `pkg install vulkan-loader`; NVIDIA: `nvidia-driver` (its Vulkan driver comes with it); `vulkan-tools` for `vulkaninfo` |
| Debian, Ubuntu | `sudo apt install libvulkan-dev glslang-tools` | `libvulkan1`; NVIDIA's driver brings its own Vulkan driver, AMD and Intel: `mesa-vulkan-drivers` (lavapipe too) |
| Windows | the Vulkan SDK (LunarG); CMake finds it by `VULKAN_SDK` | the card's driver |

Configuring says what it found:

```
-- GPU compute: Vulkan (headers /usr/local/include, /usr/local/bin/glslangValidator)
```

or why it is left out. `-DPG_WITH_VULKAN=OFF` leaves it out on purpose. A
build without it says so when asked: `prototype gpu` prints that GPU
compute is not in this build.

## 3. Which device

| | |
|---|---|
| `PG_GPU=1`, `PG_GPU=nvidia` | the device by its place in the list, or by a part of its name (any case) |
| `PG_GPU=none` (`cpu`, `off`) | no GPU: everything on the CPU |
| nothing | the best GPU that will do: a discrete card first, then an integrated one |
| `PG_GPU_VALIDATE=1` | Vulkan's validation layer checks every call (`vulkan-validation-layers`, `vulkan-validationlayers` on Debian), and what it finds goes to stderr: for development |

`--device` of `prototype gpu` names a device the same way as `PG_GPU`, and
wins over it.

A device will do if it has what the kernels need: Vulkan 1.1,
`VK_KHR_push_descriptor`, 8 storage buffers to a kernel, 128 bytes of push
constants and work groups of 256. The current drivers of NVIDIA, AMD and
Intel cards have them, Mesa's on Linux and FreeBSD too. The list says what
a device lacks if it does not.

## 4. What `prototype gpu` measures

Each test runs on the device and then the same on the CPU's threads (all
there are, or `--threads N`), and the results are compared.

| Row | What | Why |
|---|---|---|
| memory | `y = 2x + y` over 32 million numbers, five times; bytes read and written a second | the solvers are bound by memory, not by arithmetic |
| sum | the 32 million numbers added up, in blocks of 256, the halves of each block one onto the other, then the same over the sums | the sums of a solver (how far the pressure is from solved) stay the same to the bit |
| pressure | Jacobi sweeps of the Poisson equation over 256³ cells, ten of them; cells a second | what the pressure of the gas solver does most |
| divide | division and the square root, done in integers (§5), over a million numbers of every kind — normal, subnormal, infinite, NaN — against the CPU's `/` and `std::sqrt` | the solvers divide and take roots, and must get the CPU's bits |
| tiny | the device's own products of numbers near 0: whether it keeps numbers below 2^-126, as the CPU does, or flushes them to 0 | gas thinning out comes near 0 |

On a CPU device, or with `--quick`: 4 million numbers and 128³ cells.

The device's time is measured on the device (timestamps), without the
uploads. The CPU's by the clock. The command fails (exit code 1) if a result
is not right, or not the same as on the CPU.

## 5. The same result on the GPU

The program's promise is that a scene gives the same result to the bit on
any number of threads ([ARCHITECTURE.md](../ARCHITECTURE.md)). On the GPU
thousands of threads run at once, in an order nobody chooses. The kernels
are written so that this order does not matter:

- every number written is computed by one thread, from what was there
  before the kernel ran — no thread adds into another's result;
- sums go in a fixed tree, never into one place with atomics;
- `precise` forbids the compiler to fuse a multiply with an add, or to
  change the order of operations.

So a device gives the same result every time. Kernels made only of
additions, subtractions and multiplications give the CPU's result to the
bit, as `prototype gpu` shows on every device: every GPU rounds those as
IEEE 754 says, as the CPU does. A division it need not — Vulkan allows it
2.5 units in the last place, and NVIDIA's is a reciprocal and a product —
nor a square root. So the kernels divide and take roots with
`exactDiv` and `exactSqrt` (`src/pg/gpu/shaders/exact.glsl`): long division
and the root digit by digit, on the integers of the numbers' bits, rounded
as IEEE 754 rounds — the CPU's bits on every device, subnormal numbers,
infinities and NaN included. (The multigrid's sweeps do better still:
they multiply by a reciprocal the CPU worked out once.)

Numbers below 2^-126 — subnormal — a GPU may flush to 0 unless asked to
keep them. Every kernel is built twice: as it is, and asking for them to be
kept (`keep.glsl`); a device that can be asked (NVIDIA, AMD and Intel with
current drivers) runs the second. `prototype gpu` measures what the device
does with them either way.

Functions like `exp` and `sin` are computed differently by each vendor's
hardware and by the CPU's library: a solver that uses them will be the
same on one device every time, and close to the CPU's, but not the same to
the bit. The gas solver works them out on the CPU, once a step, and gives
the kernels the numbers.

## 6. For programmers: `pg::gpu`

```cpp
#include "pg/gpu/Gpu.h"

std::string why;
auto device = pg::gpu::Device::open("", why);  // PG_GPU's choice, else the best GPU
if (!device) return computeOnTheCpu();          // `why` says why not

auto x = device->buffer(n * sizeof(float)), y = device->buffer(n * sizeof(float));
device->upload(*x, xs.data(), n * sizeof(float));
device->upload(*y, ys.data(), n * sizeof(float));

struct { uint32_t n; float a; } push{uint32_t(n), 2.0f};
const double ms = pg::gpu::Batch(*device).dispatch("saxpy", {x.get(), y.get()}, push, groups).run();
device->download(*y, ys.data(), n * sizeof(float));
if (ms < 0.0) report(device->error());
```

- **Kernels** are GLSL compute shaders in `src/pg/gpu/shaders/NAME.comp`
  (`#version 450`, then the include directive and `#include "keep.glsl"`):
  storage buffers at bindings 0 to 7 in the order `dispatch` gets them,
  push constants of up to 128 bytes. CMake compiles each to SPIR-V (Vulkan
  1.1) twice — as it is, and keeping subnormal numbers — into the program;
  the kernel is then known by its file's name. A new `.comp` is picked up
  by the next build. A kernel that divides or takes a root includes
  `exact.glsl`.
- **Buffer** is memory on the device. `upload` and `download` go through
  64 MB of memory both the CPU and the device see, and wait.
- **Batch** records kernels, copies and fills, each seeing what those
  before it wrote. `run()` sends them to the device and waits: the
  device's time in milliseconds, below 0 if something failed.
- Once a call fails, the device stays failed (`ok()` false, `error()` says
  why) and does nothing more: the caller checks at the end.
- A device is used by one thread at a time.

The tests (`./build/pgtests gpu_`) run on any device there is, lavapipe
included, and are skipped without one.

## 7. The gas on the GPU

The Pyro Solver's **GPU** (Domain) steps the gas on the graphics card. The
fields live there through the step; every stage of it is a few kernels
(`src/pg/gpu/shaders/pyro_*.comp`, `poisson_*.comp`):

| Stage | On the device |
|---|---|
| advect | the paths of the gas through the velocity (RK2), the velocity carrying itself, MacCormack for smoke, heat, fuel, flame and steam; solids emptied, walls |
| quench | the water's share of the gas in each wet cell — which cells, and how much, the CPU works out from the water |
| combust | fuel into heat, soot, flame and swelling |
| forces | buoyancy (heat and steam lift, smoke weighs), vorticity confinement, turbulence, drag; walls |
| project | the right-hand side, the multigrid's V-cycles over the sparse tiles — with solids too, whose faces and diagonals the CPU works out when it builds the levels — the gradient, walls |
| dissipate | smoke thins, heat cools, flame and steam fade, the swelling spreads what the gas carries |

What stays on the CPU: the sources (their shapes, meshes among them), which
tiles to keep, where the solids are, and the wind, vortex and attractor
forces. The solver keeps track of which copy of each field is current —
the CPU's or the device's — and a stage fetches only what the other side
has newer: a step without those forces sends the fields after the sources
and fetches them once, at the end of the frame. With substeps, once a frame.

```bash
./build/prototype sim campfire out/fire.png --set gpu=1          # the campfire, on the GPU
./build/pgbench_pyro 160 --example campfire --set gpu=1          # where the time goes; on the GPU and back
```

In the editor: select the **Pyro Solver**, tick **GPU** in its Domain
section, and simulate. The **Profile** section under the timeline's frame
says, below the gas's time, what the card does — `GPU: on the GPU, NVIDIA
GeForce GTX 1060 6GB`, and which forces the CPU does, if any — or why the
CPU does it all.
From Python: `solver["gpu"] = 1`. `PG_GPU=nvidia` picks the card when there
are several.

**The same to the bit.** Each kernel does what the CPU does for its cell,
operation for operation: the same lookups in the sparse tiles, the same
trilinear weights, `std::min` and `std::clamp` as the CPU takes them, the
same order of the sums, no fused multiply-add. Where the CPU divided — by
the diagonal of the pressure's equation — both now multiply by 1 / the
diagonal, which the CPU works out once: a GPU's division is not rounded as
the CPU's is (Vulkan allows it 2.5 units in the last place), its products
are. And the build tells the compiler not to fuse a multiply with an add
(`-ffp-contract=off`), whatever `-march` it is given.

A scene comes out the same with the switch on or off: `pgbench_pyro`
prints the same fingerprint either way — the campfire, the demolition's dust
with its moving pieces — and `pgtests gpu_gas` steps, side by side, a fire,
smoke round a ball, a fire with every kind of force in two substeps a
frame, and water falling through a fire, comparing every field after every
step.

**What it costs.** Each frame the fields the sources touched go to the
device, and all of them come back for the frame: on a card in a PCIe slot a
few milliseconds for a campfire. `pgbench_pyro` says how long the kernels
took, how long the copies, and all of it.

Without a GPU, or when the device fails, the CPU does it all — the solver
says why (`pgbench_pyro`, `prototype sim`: "the CPU does it all: …") and the
simulation goes on the same. A CPU pretending to be a GPU (lavapipe) is used
only when `PG_GPU` names it: it computes the same, slower than the
program's own threads.

## 8. What comes next

1. **Measured on a real card** — the same scenes on the CPU and on a GTX
   1060, here.
2. **The rest on the device** — which tiles to keep, worked out there; the
   sources' shapes; the wind, the vortex and the attractor — so that only
   the frame comes back.
