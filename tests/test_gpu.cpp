// Compute on the GPU through Vulkan (src/pg/gpu): on whatever device this
// machine has -- a CPU pretending to be one (llvmpipe) does as well. Without
// Vulkan, or a build without it, there is nothing to test.
#include "test_framework.h"

#ifdef PG_HAVE_VULKAN

#include "pg/gpu/Gpu.h"
#include "pg/gpu/SelfTest.h"

#include <algorithm>
#include <cstdio>
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

#endif  // PG_HAVE_VULKAN
