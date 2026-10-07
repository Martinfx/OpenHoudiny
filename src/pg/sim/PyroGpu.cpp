#include "pg/sim/PyroGpu.h"

#include "pg/sim/Pyro.h"

#ifdef PG_HAVE_VULKAN
#include "pg/gpu/Gpu.h"
#endif

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <vector>

namespace pg::sim {

#ifdef PG_HAVE_VULKAN

namespace {

/// The push constants of src/pg/gpu/shaders/sparse.glsl, as std430 lays
/// them out.
struct Push {
    int32_t nx = 0, ny = 0, nz = 0;
    uint32_t slots[4] = {0, 0, 0, 0};
    uint32_t stored = 0;
    uint32_t slotCount = 0;
    uint32_t vel[3] = {0, 0, 0};
    uint32_t field = 0;
    uint32_t plane = 0;
    int32_t axis = 0;
    int32_t floorClosed = 0;
    float cells = 0.0f;
};
static_assert(sizeof(Push) == 68, "Push must match sparse.glsl");

/// The work groups of `tiles` tiles, one a tile, as x by y: no more along x
/// than any device takes.
void groupsOf(size_t tiles, uint32_t& x, uint32_t& y) {
    x = static_cast<uint32_t>(std::clamp<size_t>(tiles, 1, 65535));
    y = static_cast<uint32_t>((tiles + x - 1) / x);
}

double msSince(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}

}  // namespace

struct PyroGpu::Impl {
    std::unique_ptr<gpu::Device> device;
    // Binding 0 of every kernel: the four sets' tables (sparse.glsl).
    std::unique_ptr<gpu::Buffer> tables;
    // The velocity, before and after; the fields carried, before and
    // after; the paths (back, forward); MacCormack's predicted, lows, highs.
    std::unique_ptr<gpu::Buffer> velocity, next, fields, carried, paths, work;
    std::shared_ptr<const Tiles> tiled;  ///< the cells the tables are of
    uint32_t slots[4] = {0, 0, 0, 0}, stored[4] = {0, 0, 0, 0}, storedCount[4] = {0, 0, 0, 0};
    std::string error;
    double kernelMs = 0.0, totalMs = 0.0;

    /// `b` of at least `bytes`: made again, with room to grow, when it is
    /// smaller.
    bool fit(std::unique_ptr<gpu::Buffer>& b, size_t bytes) {
        if (b && b->bytes() >= bytes) return true;
        b.reset();
        b = device->buffer(std::max<size_t>(bytes + bytes / 4, 4));
        return b != nullptr;
    }
    bool failed() {
        error = device->error();
        return false;
    }
};

#else

struct PyroGpu::Impl {
    std::string error;
    double kernelMs = 0.0, totalMs = 0.0;
};

#endif

PyroGpu::PyroGpu() : impl_(std::make_unique<Impl>()) {}
PyroGpu::~PyroGpu() = default;

std::unique_ptr<PyroGpu> PyroGpu::open(std::string& why) {
#ifdef PG_HAVE_VULKAN
    auto device = gpu::Device::open("", why);
    if (!device) return nullptr;
    std::unique_ptr<PyroGpu> g(new PyroGpu());
    g->impl_->device = std::move(device);
    return g;
#else
    why = "GPU compute is not in this build (docs/gpu.md)";
    return nullptr;
#endif
}

const std::string& PyroGpu::device() const {
#ifdef PG_HAVE_VULKAN
    return impl_->device->info().name;
#else
    return impl_->error;
#endif
}

const std::string& PyroGpu::error() const { return impl_->error; }
double PyroGpu::kernelMs() const { return impl_->kernelMs; }
double PyroGpu::totalMs() const { return impl_->totalMs; }

bool PyroGpu::advect(PyroSolver& s, float dt) {
#ifndef PG_HAVE_VULKAN
    (void)s;
    (void)dt;
    return false;
#else
    Impl& m = *impl_;
    gpu::Device& d = *m.device;
    if (!d.ok()) return m.failed();
    const auto start = std::chrono::steady_clock::now();

    // The tables, when the tiles have changed.
    const Tiles* sets[4] = {s.cells_.get(), s.faces_[0].get(), s.faces_[1].get(), s.faces_[2].get()};
    if (m.tiled != s.cells_) {
        std::vector<int32_t> table;
        for (int set = 0; set < 4; ++set) {
            const Tiles& t = *sets[set];
            m.slots[set] = static_cast<uint32_t>(table.size());
            for (size_t i = 0; i < t.tileCount(); ++i) table.push_back(t.slot(i));
            m.stored[set] = static_cast<uint32_t>(table.size());
            m.storedCount[set] = static_cast<uint32_t>(t.stored().size());
            for (const uint32_t tile : t.stored()) {
                table.push_back(static_cast<int32_t>(tile | (t.state(tile) == Tiles::FirstLayer ? 0x80000000u : 0u)));
            }
        }
        const size_t bytes = table.size() * sizeof(int32_t);
        if (!m.fit(m.tables, bytes) || !d.upload(*m.tables, table.data(), bytes)) return m.failed();
        m.tiled = s.cells_;
    }

    // The velocity, before and as it is now after (the faces that do not
    // count keep what they have).
    Push push;
    push.nx = s.nx_;
    push.ny = s.ny_;
    push.nz = s.nz_;
    std::copy(m.slots, m.slots + 4, push.slots);
    size_t velFloats = 0;
    for (int a = 0; a < 3; ++a) {
        push.vel[a] = static_cast<uint32_t>(velFloats);
        velFloats += s.vel_[a].size();
    }
    const size_t plane = s.density_.size();
    push.plane = static_cast<uint32_t>(plane);
    push.cells = dt / s.domain_.voxel;  // as advect: velocity x dt, in cells
    push.floorClosed = s.scene_.solver.closedFloor ? 1 : 0;
    std::vector<SparseGrid*> carried = {&s.density_, &s.temperature_, &s.fuel_, &s.flame_};
    if (s.steamy_) carried.push_back(&s.steam_);
    const size_t f = sizeof(float);
    if (!m.fit(m.velocity, velFloats * f) || !m.fit(m.next, velFloats * f) ||
        !m.fit(m.fields, carried.size() * plane * f) || !m.fit(m.carried, carried.size() * plane * f) ||
        !m.fit(m.paths, 6 * plane * f) || !m.fit(m.work, 3 * plane * f)) {
        return m.failed();
    }
    for (int a = 0; a < 3; ++a) {
        if (!d.upload(*m.velocity, s.vel_[a].data(), s.vel_[a].size() * f, push.vel[a] * f) ||
            !d.upload(*m.next, s.velNext_[a].data(), s.velNext_[a].size() * f, push.vel[a] * f)) {
            return m.failed();
        }
    }
    for (size_t i = 0; i < carried.size(); ++i) {
        if (!d.upload(*m.fields, carried[i]->data(), plane * f, i * plane * f)) return m.failed();
    }

    gpu::Batch batch(d);
    uint32_t gx = 0, gy = 0;
    // The paths, over the cells.
    push.stored = m.stored[0];
    push.slotCount = m.storedCount[0];
    groupsOf(m.storedCount[0], gx, gy);
    batch.dispatch("pyro_paths", {m.tables.get(), m.velocity.get(), m.paths.get()}, push, gx, gy);
    // Each component of the velocity, over its faces.
    for (int a = 0; a < 3; ++a) {
        Push faces = push;
        faces.axis = a;
        faces.stored = m.stored[a + 1];
        faces.slotCount = m.storedCount[a + 1];
        uint32_t fx = 0, fy = 0;
        groupsOf(m.storedCount[a + 1], fx, fy);
        batch.dispatch("pyro_velocity", {m.tables.get(), m.velocity.get(), m.next.get()}, faces, fx, fy);
    }
    // MacCormack for each field.
    for (size_t i = 0; i < carried.size(); ++i) {
        Push field = push;
        field.field = static_cast<uint32_t>(i * plane);
        batch.dispatch("pyro_predict", {m.tables.get(), m.fields.get(), m.paths.get(), m.work.get()}, field, gx, gy);
        batch.dispatch("pyro_correct",
                       {m.tables.get(), m.fields.get(), m.paths.get(), m.work.get(), m.carried.get()}, field, gx, gy);
    }
    m.kernelMs = batch.run();
    if (m.kernelMs < 0.0) return m.failed();

    // Back: all of it read first, so a failure leaves the solver as it was.
    std::vector<float> velocity(velFloats), fields(carried.size() * plane);
    if (!d.download(*m.next, velocity.data(), velFloats * f) ||
        !d.download(*m.carried, fields.data(), fields.size() * f)) {
        return m.failed();
    }
    for (int a = 0; a < 3; ++a) {
        std::copy_n(velocity.data() + push.vel[a], s.velNext_[a].size(), s.velNext_[a].data());
    }
    for (size_t i = 0; i < carried.size(); ++i) std::copy_n(fields.data() + i * plane, plane, carried[i]->data());
    m.totalMs = msSince(start);
    return true;
#endif
}

}  // namespace pg::sim
