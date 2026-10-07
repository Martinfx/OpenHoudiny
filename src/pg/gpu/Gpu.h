#pragma once
//
// Compute on the GPU through Vulkan: the memory the solvers keep on the
// graphics card, the kernels they run there and the order they run them in.
// Vulkan is loaded when a device is opened: a machine without it, or without
// a GPU, runs everything on the CPU as before.
//
//   Device::open   the GPU PG_GPU names, or the best there is
//   Buffer         memory on the device: uploaded, downloaded
//   Batch          kernels run one after another, each seeing what those
//                  before it wrote; run() waits for them
//
// The kernels are GLSL compute shaders (src/pg/gpu/shaders/*.comp), compiled
// to SPIR-V when the program is built and carried in it. Each binds up to
// kMaxBuffers storage buffers, in order, and up to kMaxPush bytes of push
// constants.
//
// A device is used by one thread at a time.
//
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

namespace pg::gpu {

/// A Vulkan device, as the loader lists it.
struct DeviceInfo {
    int index = 0;           ///< its place in the list: what PG_GPU may name it by
    std::string name;
    std::string kind;        ///< "discrete GPU", "integrated GPU", "virtual GPU", "CPU", "other"
    std::string driver;      ///< its driver, and the driver's version
    std::string api;         ///< the Vulkan it speaks: "1.4.312"
    uint64_t memory = 0;     ///< bytes of its own memory
    uint32_t subgroup = 0;   ///< threads that run in step
    bool cpu = false;        ///< a CPU pretending to be one: llvmpipe, SwiftShader
    /// It keeps numbers below 2^-126 when the kernels ask it to, as the CPU
    /// does (shaderDenormPreserveFloat32); else they may come out 0.
    bool keepsSubnormals = false;
    bool usable = false;     ///< it has what the kernels need -- if not, why in `missing`
    std::string missing;
};

/// The Vulkan devices of this machine; empty, with `why`, without Vulkan.
std::vector<DeviceInfo> devices(std::string* why = nullptr);

class Device;
class Batch;

/// Memory on the device -- of the device that made it, which it must not
/// outlive.
class Buffer {
public:
    ~Buffer();
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;

    size_t bytes() const { return bytes_; }

private:
    friend class Device;
    friend class Batch;
    Buffer(Device& device, size_t bytes);

    Device& device_;
    size_t bytes_ = 0;
    struct Handles;
    std::unique_ptr<Handles> handles_;
};

class Device {
public:
    static constexpr int kMaxBuffers = 8;
    static constexpr size_t kMaxPush = 128;

    /// The device `choice` names: its index in devices(), or a part of its
    /// name ("nvidia", "llvmpipe") -- empty: PG_GPU's choice, else the best
    /// GPU, a discrete one first. A CPU pretending to be one only when named,
    /// or with `cpuToo`; "none" or "cpu" opens nothing. Null, with `why`, if
    /// there is nothing to open.
    static std::unique_ptr<Device> open(const std::string& choice, std::string& why, bool cpuToo = false);
    ~Device();
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    const DeviceInfo& info() const { return info_; }
    /// False once a call to Vulkan failed -- why in error(); all that follows
    /// does nothing.
    bool ok() const { return error_.empty(); }
    const std::string& error() const { return error_; }

    /// `bytes` of memory on the device, of what it held before -- zeros on
    /// some devices, anything on others.
    std::unique_ptr<Buffer> buffer(size_t bytes);
    /// Copies from and to the CPU's memory, waiting until they are done.
    bool upload(Buffer& to, const void* from, size_t bytes, size_t offset = 0);
    bool download(const Buffer& from, void* to, size_t bytes, size_t offset = 0);

    struct Impl;
    Impl& impl() { return *impl_; }

private:
    Device();
    friend class Buffer;
    friend class Batch;
    bool fail(const std::string& what);
    /// The memory uploads and downloads go through, made the first time.
    bool staging();

    DeviceInfo info_;
    std::string error_;
    std::unique_ptr<Impl> impl_;
};

/// Kernels and copies run on the device in the order given, each seeing all
/// those before it wrote.
class Batch {
public:
    explicit Batch(Device& device);
    ~Batch();
    Batch(const Batch&) = delete;
    Batch& operator=(const Batch&) = delete;

    /// The kernel `shader` (the name of its .comp) over x by y by z work
    /// groups, `buffers` bound to its bindings 0, 1, ..., `push` its push
    /// constants.
    Batch& dispatch(const char* shader, std::initializer_list<const Buffer*> buffers, const void* push,
                    size_t pushBytes, uint32_t x, uint32_t y = 1, uint32_t z = 1);
    template <class Push>
    Batch& dispatch(const char* shader, std::initializer_list<const Buffer*> buffers, const Push& push, uint32_t x,
                    uint32_t y = 1, uint32_t z = 1) {
        return dispatch(shader, buffers, &push, sizeof push, x, y, z);
    }
    Batch& copy(const Buffer& from, Buffer& to, size_t bytes, size_t fromOffset = 0, size_t toOffset = 0);
    /// `bytes` of `buffer` from `offset` set to `word` in every 4 bytes.
    Batch& fill(Buffer& buffer, uint32_t word, size_t bytes, size_t offset = 0);

    /// Runs what was given, waits for it, and is empty again: how long the
    /// device took, milliseconds; below 0 if it failed (the device's error).
    double run();

private:
    void begin();
    Device& device_;
    struct State;
    std::unique_ptr<State> state_;
};

/// The kernels the program carries: their names.
std::vector<std::string> kernels();

}  // namespace pg::gpu
