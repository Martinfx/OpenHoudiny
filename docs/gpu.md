# Compute on the GPU (Vulkan)

Prototype computes on the CPU's threads. The solvers that cost the most — the
gas of the Pyro Solver first — are to move to the graphics card, through
**Vulkan compute**: it runs on NVIDIA, AMD and Intel cards, on Linux,
FreeBSD and Windows, with the drivers the card already has.

What is there now is the foundation (`pg::gpu`): the devices, memory on the
device, kernels compiled when the program is built and carried in it, and
`prototype gpu`, which measures a device against the CPU. **The solvers do
not use it yet** — that is the next step (§7).

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
additions, multiplications and divisions give the CPU's result to the bit,
as `prototype gpu` shows on every device. Functions like `exp` and `sin`
are computed differently by each vendor's hardware and by the CPU's
library: a solver that uses them will be the same on one device every time,
and close to the CPU's, but not the same to the bit.

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
  (`#version 450`): storage buffers at bindings 0 to 7 in the order
  `dispatch` gets them, push constants of up to 128 bytes. CMake compiles
  each to SPIR-V (Vulkan 1.1) into the program; the kernel is then known by
  its file's name. A new `.comp` is picked up by the next build.
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

## 7. What comes next

1. **Advection on the GPU** — the gas solver's sparse tiles on the device,
   the fields carried by the velocity: 44 % of a pyro step on the CPU.
2. **Pressure on the GPU** — multigrid over the tiles: 25 %.
3. **The whole step on the device** — sources, combustion, forces; the
   fields stay there between steps and come back only for the frame. A
   switch on the Pyro Solver: CPU or GPU, and the same scene measured on
   both.
