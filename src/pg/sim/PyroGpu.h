#pragma once
//
// The gas solver's step on the GPU (docs/gpu.md §7): the fields live on the
// device through the step, each stage a few kernels (src/pg/gpu/shaders/
// pyro_*.comp, poisson_*.comp), each kernel the CPU's arithmetic operation
// for operation -- the same fields to the bit.
//
//   advect      the paths of the gas, the velocity carrying itself,
//               MacCormack for smoke, heat, fuel, flame and steam; solids
//               emptied, walls
//   quench      the water's share of the gas, cell by cell as the CPU found it
//   combust     fuel into heat, soot, flame and swelling
//   buoyancy, vorticity, force (turbulence, drag), walls
//   project     the right-hand side, the multigrid's V-cycles, the gradient
//   dissipate
//
// What the device has of each field, and what the CPU has, the PyroSolver
// keeps (its onHost_ and onDevice_): a stage on the device first sends it
// what it has newer, a stage on the CPU first fetches what the device has
// newer -- so a step whose stages are all on the device sends the fields
// once and fetches them once. The sources, the tiles and the solids are
// the CPU's, and the forces the device does not do.
//
// A PyroSolver whose settings ask for the GPU makes one; without a device
// (or a build without Vulkan) it says why and the CPU does the work.
//
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace pg::sim {

class PyroSolver;
struct Force;

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
    /// What went wrong; once a call has failed, the device stays failed.
    const std::string& error() const;

    /// The fields in `fields` (PyroSolver's bits) to the device, and from it.
    bool send(PyroSolver& solver, uint16_t fields);
    bool fetch(PyroSolver& solver, uint16_t fields);

    // The stages, as PyroSolver's. False if the device failed.
    bool advect(PyroSolver& solver, float dt);
    /// `wet`: each wet cell's place in the fields and the share of its gas
    /// the water takes, as the CPU found them; what is left of the heat taken
    /// (1 - the steam's warmth), and the steam a unit of heat makes.
    bool quench(PyroSolver& solver, const std::vector<std::pair<uint32_t, float>>& wet, float cooled, float steam);
    bool combust(PyroSolver& solver, float dt);
    bool buoyancy(PyroSolver& solver, float dt);
    bool vorticity(PyroSolver& solver, float dt);
    /// A force of the scene, if the device does its kind (turbulence, drag):
    /// `done` whether it did.
    bool force(PyroSolver& solver, const Force& force, size_t index, float dt, bool& done);
    bool walls(PyroSolver& solver);
    bool project(PyroSolver& solver, float h, int cycles);
    bool dissipate(PyroSolver& solver, float dt);

    /// Milliseconds since beginStep(): the device's on the kernels, the
    /// copies to it and from it, and all of it.
    struct Times {
        double kernels = 0.0, copies = 0.0, total = 0.0;
    };
    void beginStep();
    const Times& times() const;

private:
    PyroGpu();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace pg::sim
