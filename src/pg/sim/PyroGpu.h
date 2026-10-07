#pragma once
//
// The gas solver's work on the GPU (docs/gpu.md): for now advect -- the
// paths of the gas, the velocity carrying itself, MacCormack for smoke,
// heat, fuel, flame and steam -- with the kernels of src/pg/gpu/shaders/
// pyro_*.comp, each the CPU's arithmetic operation for operation: the same
// fields to the bit.
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
    const std::string& error() const;
    /// Milliseconds the device spent on the last advect's kernels, and on
    /// it all, with the copies there and back.
    double kernelMs() const;
    double totalMs() const;

private:
    PyroGpu();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace pg::sim
