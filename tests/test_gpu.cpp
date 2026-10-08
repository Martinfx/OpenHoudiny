// Compute on the GPU through Vulkan (src/pg/gpu): on whatever device this
// machine has -- a CPU pretending to be one (llvmpipe) does as well. Without
// Vulkan, or a build without it, there is nothing to test.
#include "test_framework.h"

#ifdef PG_HAVE_VULKAN

#include "pg/gpu/Gpu.h"
#include "pg/gpu/SelfTest.h"
#include "pg/sim/Pyro.h"
#include "pg/sim/PyroGpu.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <numeric>
#include <string>
#include <vector>

using namespace pg;

namespace {

/// A device, any: null, saying why, without one.
std::unique_ptr<gpu::Device> anyDevice() {
    std::string why;
    auto device = gpu::Device::open("", why, true);
    if (!device) std::printf("    (no Vulkan device: %s -- skipped)\n", why.c_str());
    return device;
}

}  // namespace

TEST(gpu_kernels_compute_what_the_cpu_does) {
    auto device = anyDevice();
    if (!device) return;
    // Sizes no work group divides: the last groups go part of the way.
    const gpu::SelfTest r = gpu::selfTest(*device, 300001, 37);
    CHECK_EQ(r.error, std::string());
    CHECK(r.saxpyRight);
    // A sum and Jacobi sweeps the same to the bit as the CPU's.
    CHECK_EQ(r.gpuSum, r.cpuSum);
    CHECK(r.jacobiSame);
    CHECK(r.gpuGBs > 0.0 && r.gpuCellsPerS > 0.0);
}

TEST(gpu_buffers_go_there_and_back) {
    auto device = anyDevice();
    if (!device) return;
    std::vector<uint32_t> words(100000);
    std::iota(words.begin(), words.end(), 7u);
    auto a = device->buffer(words.size() * 4), b = device->buffer(words.size() * 4);
    CHECK(a && b);
    CHECK(device->upload(*a, words.data(), words.size() * 4));
    // Copied, a stretch of it filled; read back whole and in part.
    gpu::Batch batch(*device);
    batch.copy(*a, *b, words.size() * 4).fill(*b, 0xdeadbeefu, 400, 800);
    CHECK(batch.run() >= 0.0);
    std::vector<uint32_t> back(words.size());
    CHECK(device->download(*b, back.data(), back.size() * 4));
    for (size_t i = 200; i < 300; ++i) words[i] = 0xdeadbeefu;
    CHECK(back == words);
    uint32_t one = 0;
    CHECK(device->download(*b, &one, 4, 4 * 999));
    CHECK_EQ(one, words[999]);
    // Past the end: refused, and the device says so from then on.
    CHECK(!device->download(*b, back.data(), 8, words.size() * 4 - 4));
    CHECK(!device->ok());
    CHECK(device->error().find("past the end") != std::string::npos);
}

TEST(gpu_device_is_chosen_by_name_or_not_at_all) {
    std::string why;
    CHECK(gpu::Device::open("none", why) == nullptr);
    CHECK(why.find("off") != std::string::npos);
    const std::vector<gpu::DeviceInfo> list = gpu::devices(&why);
    if (list.empty()) {
        std::printf("    (no Vulkan device: %s -- skipped)\n", why.c_str());
        return;
    }
    // By a part of its name, by its place in the list; a name no device has
    // says which there are.
    for (const gpu::DeviceInfo& d : list) {
        if (!d.usable) continue;
        auto byIndex = gpu::Device::open(std::to_string(d.index), why);
        CHECK(byIndex && byIndex->info().name == d.name);
    }
    CHECK(gpu::Device::open("a device nobody makes", why) == nullptr);
    CHECK(why.find(list[0].name) != std::string::npos);
    // A kernel the program does not carry: the device says which.
    auto device = anyDevice();
    if (!device) return;
    auto buffer = device->buffer(16);
    gpu::Batch batch(*device);
    batch.dispatch("no_such_kernel", {buffer.get()}, nullptr, 0, 1);
    CHECK(batch.run() < 0.0);
    CHECK(device->error().find("no_such_kernel") != std::string::npos);
    std::vector<std::string> kernels = gpu::kernels();
    CHECK(std::find(kernels.begin(), kernels.end(), "jacobi") != kernels.end());
}

TEST(gpu_division_and_square_root_are_the_cpus_to_the_bit) {
    auto device = anyDevice();
    if (!device) return;
    // Every pair of the numbers at the edges: zeros, infinities, NaN, the
    // least and the greatest, subnormal and normal, ones and twos and thirds.
    const float inf = std::numeric_limits<float>::infinity();
    const float edges[] = {0.0f, -0.0f, inf, -inf, std::numeric_limits<float>::quiet_NaN(),
                           std::numeric_limits<float>::max(), std::numeric_limits<float>::min(),
                           std::numeric_limits<float>::denorm_min(), 3.0f * std::numeric_limits<float>::denorm_min(),
                           std::numeric_limits<float>::min() * 0.75f, std::numeric_limits<float>::min() * 1.25f,
                           1.0f, -1.0f, 2.0f, 3.0f, 1.0f / 3.0f, 0.1f, 1e-30f, 1e30f, 7e-39f, 1.5e-45f,
                           16777215.0f, 16777217.0f, std::nextafter(1.0f, 2.0f), std::nextafter(1.0f, 0.0f),
                           std::nextafter(2.0f, 0.0f)};
    std::vector<float> x, y;
    for (const float a : edges) {
        for (const float b : edges) {
            x.push_back(a);
            y.push_back(b);
        }
    }
    gpu::ArithmeticCheck r = gpu::checkArithmetic(*device, x, y);
    CHECK_EQ(r.error, std::string());
    CHECK_EQ(r.divisionWrong, size_t{0});
    CHECK_EQ(r.sqrtWrong, size_t{0});
    CHECK_EQ(r.productWrong, size_t{0});
    // And millions of numbers of every kind.
    for (uint32_t seed = 1; seed <= 3; ++seed) {
        r = gpu::checkArithmetic(*device, size_t{1} << 21, seed);
        CHECK_EQ(r.error, std::string());
        CHECK_EQ(r.divisionWrong, size_t{0});
        CHECK_EQ(r.sqrtWrong, size_t{0});
        CHECK_EQ(r.productWrong, size_t{0});
        if (!r.exact()) std::printf("    first wrong: %.9g and %.9g\n", static_cast<double>(r.x), static_cast<double>(r.y));
    }
    std::printf("    subnormal products the device flushed: %zu (%s)\n", r.subnormalsLost,
                device->info().keepsSubnormals ? "it keeps them when asked" : "it cannot be asked to keep them");
}

namespace {

/// PG_GPU set to the first device for as long as it lives -- a CPU one too,
/// which the solvers would not take on their own -- unless it is set
/// already. False without a device.
struct AnyDeviceForSolvers {
    bool ok = false;
    bool set = false;
    AnyDeviceForSolvers() {
        if (const char* v = std::getenv("PG_GPU"); v && *v) {
            ok = std::strcmp(v, "none") != 0;
            return;
        }
        std::string why;
        const std::vector<gpu::DeviceInfo> list = gpu::devices(&why);
        for (const gpu::DeviceInfo& d : list) {
            if (!d.usable) continue;
            setenv("PG_GPU", std::to_string(d.index).c_str(), 1);
            ok = set = true;
            return;
        }
        std::printf("    (no Vulkan device: %s -- skipped)\n", why.c_str());
    }
    ~AnyDeviceForSolvers() {
        if (set) unsetenv("PG_GPU");
    }
};

bool sameBits(const sim::SparseGrid& a, const sim::SparseGrid& b) {
    return a.tiles() == b.tiles() && a.size() == b.size() &&
           std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}

/// Steps `scene` on the CPU and with the GPU, side by side -- with `water`
/// in the gas, if any: every field the same to the bit after every step.
void checkGasOnBoth(sim::Scene scene, int steps, bool solids, const sim::PyroSolver::Water& water = {}) {
    scene.solver.gpu = false;
    sim::PyroSolver cpu(scene);
    scene.solver.gpu = true;
    sim::PyroSolver gpu(scene);
    for (int s = 0; s < steps; ++s) {
        if (!water.empty()) {
            cpu.setWater(water);
            gpu.setWater(water);
        }
        cpu.step();
        gpu.step();
        CHECK(gpu.gpu() != nullptr);
        if (!gpu.gpu()) {
            std::printf("    %s\n", gpu.gpuNote().c_str());
            return;
        }
        bool same = sameBits(cpu.density(), gpu.density()) && sameBits(cpu.temperature(), gpu.temperature()) &&
                    sameBits(cpu.fuel(), gpu.fuel()) && sameBits(cpu.flame(), gpu.flame()) &&
                    sameBits(cpu.steam(), gpu.steam());
        for (int a = 0; a < 3; ++a) same = same && sameBits(cpu.velocity(a), gpu.velocity(a));
        same = same && sameBits(cpu.solid(), gpu.solid());
        CHECK(same);
        if (!same) {
            std::printf("    step %d differs\n", s + 1);
            return;
        }
        // The step on the device.
        CHECK(gpu.gpu()->times().kernels > 0.0);
    }
    // Water in the gas makes steam.
    if (!water.empty()) CHECK(gpu.steamy() && gpu.steam().sum() > 0.0);
    // With solids in the gas: the multigrid's faces and diagonals too.
    const std::vector<float>& solid = gpu.solid().values();
    CHECK_EQ(std::any_of(solid.begin(), solid.end(), [](float v) { return v > 0.5f; }), solids);
    std::printf("    %d steps, %zu cells at the end, the same to the bit; %s\n", steps, gpu.activeCells(),
                gpu.gpuNote().c_str());
}

}  // namespace

TEST(gpu_gas_steps_are_the_cpus_to_the_bit) {
    AnyDeviceForSolvers device;
    if (!device.ok) return;
    // Fire: fuel, flame, a closed floor; tiles come and go as it rises.
    sim::Scene fire = sim::Scene::fire();
    fire.solver.resolution = 40;
    checkGasOnBoth(fire, 24, false);
    // Smoke round a ball, the floor open: solids, gas leaving at every side.
    sim::Scene smoke = sim::Scene::smoke();
    smoke.solver.resolution = 32;
    smoke.solver.closedFloor = false;
    sim::Collider ball;
    ball.center = Vec3(0.0f, 0.5f, 0.0f);
    ball.size = Vec3(0.25f);
    smoke.colliders.push_back(ball);
    checkGasOnBoth(smoke, 20, true);
}

TEST(gpu_gas_with_every_force_water_and_substeps_is_the_cpus_to_the_bit) {
    AnyDeviceForSolvers device;
    if (!device.ok) return;
    // Every kind of force, each with another mask: drag and turbulence on the
    // device, wind, a vortex and an attractor on the CPU between them -- the
    // fields going there and back mid-step. Two substeps a frame.
    sim::Scene fire = sim::Scene::fire();
    fire.solver.resolution = 32;
    fire.solver.substeps = 2;
    sim::Force wind;
    wind.kind = sim::ForceKind::Wind;
    wind.strength = 2.0f;
    wind.speed = 0.6f;
    wind.gusts = 0.5f;
    wind.mask = sim::Mask::Smoke;
    sim::Force drag;
    drag.kind = sim::ForceKind::Drag;
    drag.strength = 0.5f;
    drag.mask = sim::Mask::Heat;
    sim::Force vortex;
    vortex.kind = sim::ForceKind::Vortex;
    vortex.direction = Vec3(0.0f, 1.0f, 0.0f);
    vortex.speed = 1.5f;
    vortex.lift = 0.2f;
    vortex.suction = 0.3f;
    vortex.radius = 0.4f;
    vortex.height = 1.0f;
    sim::Force pull;
    pull.kind = sim::ForceKind::Attractor;
    pull.center = Vec3(0.2f, 0.8f, 0.0f);
    pull.strength = 3.0f;
    fire.forces.push_back(wind);
    fire.forces.push_back(drag);
    fire.forces.push_back(vortex);
    fire.forces.push_back(pull);
    checkGasOnBoth(fire, 12, false);
    // Water falling through a fire: quenched, steam made, lifted and faded;
    // the floor open.
    sim::Scene wet = sim::Scene::fire();
    wet.solver.resolution = 32;
    wet.solver.closedFloor = false;
    sim::PyroSolver::Water water;
    water.particleVolume = 2e-6f;
    for (int i = 0; i < 400; ++i) {
        const float t = static_cast<float>(i) / 400.0f;
        water.particles.push_back(Vec3(0.12f * std::sin(40.0f * t), 0.1f + 0.5f * t, 0.12f * std::cos(37.0f * t)));
    }
    water.dropFrom.push_back(Vec3(0.05f, 0.9f, 0.0f));
    water.dropTo.push_back(Vec3(0.05f, 0.2f, 0.0f));
    checkGasOnBoth(wet, 16, false, water);
}

#endif  // PG_HAVE_VULKAN
