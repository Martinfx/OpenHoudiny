#pragma once
//
// The gas solver's work on the GPU (docs/gpu.md): advect -- the paths of the
// gas, the velocity carrying itself, MacCormack for smoke, heat, fuel, flame
// and steam (src/pg/gpu/shaders/pyro_*.comp) -- and the pressure's multigrid
// (poisson_*.comp), each kernel the CPU's arithmetic operation for
// operation: the same fields to the bit.
//
// A PyroSolver whose settings ask for the GPU makes one when it first
// advects; without a device (or a build without Vulkan) it says why and the
// CPU goes on doing the work.
//
#include <memory>
#include <string>

namespace pg::sim {

class PyroSolver;

class PyroGpu {
public:
    /// On the device PG_GPU names, else the best GPU; null, with `why`,
    /// without one.
    static std::unique_ptr<PyroGpu> open(std::string& why);
    ~PyroGpu();
    PyroGpu(const PyroGpu&) = delete;
    PyroGpu& operator=(const PyroGpu&) = delete;

    /// The device's name.
    const std::string& device() const;
    /// `solver`'s advect(dt) up to its solids and walls: the fields carried
    /// on the device and back. False, with error(), if the device failed --
    /// nothing of the solver changed then.
    bool advect(PyroSolver& solver, float dt);
    /// `solver`'s pressure solve, as its PoissonSolver::solve(pressure,
    /// divergence, h, cycles) would make it. False, with error(), if the
    /// device failed -- the pressure as it was then.
    bool solvePressure(PyroSolver& solver, float h, int cycles);
    const std::string& error() const;

    /// Milliseconds of the last step: the device's on the kernels, and the
    /// whole of it with the copies there and back.
    struct Times {
        double advectKernels = 0.0, advect = 0.0, pressureKernels = 0.0, pressure = 0.0;
    };
    const Times& times() const;

private:
    PyroGpu();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace pg::sim
