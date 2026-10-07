#include "pg/sim/PyroGpu.h"

#include "pg/sim/Poisson.h"
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

/// The push constants of src/pg/gpu/shaders/poisson.glsl.
struct PoissonPush {
    int32_t nx = 0, ny = 0, nz = 0;
    uint32_t slots = 0, stored = 0, count = 0;
    uint32_t faceSlots[3] = {0, 0, 0};
    uint32_t p = 0, b = 0, r = 0;
    uint32_t diag = 0, inv = 0;
    uint32_t faces[3] = {0, 0, 0};
    uint32_t on = 0, open = 0;
    int32_t mode = 0, colour = 0;
    float omega = 1.0f, h2 = 0.0f, invH2 = 0.0f;
    uint32_t closed = 0;
    int32_t onx = 0, ony = 0, onz = 0;
    uint32_t oslots = 0, other = 0;
};
static_assert(sizeof(PoissonPush) == 120, "PoissonPush must match poisson.glsl");

/// Where the values start in F: the inverses of the whole-number diagonals
/// come first.
constexpr uint32_t kFirstValue = 16;

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
    // The pressure's multigrid, as the PoissonSolver last built it: each
    // level's push constants (all but what a kernel sets), its tiles, and
    // the buffers T, F and B of poisson.glsl.
    std::vector<PoissonPush> levels;
    std::vector<uint32_t> levelTiles;  ///< stored tiles of each level, for the work groups
    std::unique_ptr<gpu::Buffer> poissonTables, poissonValues, poissonBits;
    const PoissonSolver* built = nullptr;
    uint64_t generation = 0;
    std::string error;
    Times times;

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
    Times times;
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
const PyroGpu::Times& PyroGpu::times() const { return impl_->times; }

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
    m.times.advectKernels = batch.run();
    if (m.times.advectKernels < 0.0) return m.failed();

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
    m.times.advect = msSince(start);
    return true;
#endif
}

bool PyroGpu::solvePressure(PyroSolver& s, float h, int cycles) {
#ifndef PG_HAVE_VULKAN
    (void)s;
    (void)h;
    (void)cycles;
    return false;
#else
    Impl& m = *impl_;
    gpu::Device& d = *m.device;
    if (!d.ok()) return m.failed();
    SparseGrid& pressure = s.pressure_;
    const SparseGrid& divergence = s.divergence_;
    if (!pressure.shared() || pressure.size() == 0) return true;  // as solve(): nothing to do
    const auto start = std::chrono::steady_clock::now();
    PoissonSolver& poisson = s.poisson_;
    poisson.build(pressure, h);

    // The hierarchy, each time the solver builds it again: its tables, the
    // cells that count, and with solids the faces and diagonals.
    if (m.built != &poisson || m.generation != poisson.generation_) {
        std::vector<int32_t> tables;
        std::vector<float> values(kFirstValue, 0.0f);
        std::vector<uint32_t> bits;
        for (int dgn = 0; dgn < 13; ++dgn) values[static_cast<size_t>(dgn)] = PoissonSolver::inverseOf(dgn);
        auto table = [&](const Tiles& t) {
            const uint32_t at = static_cast<uint32_t>(tables.size());
            for (size_t i = 0; i < t.tileCount(); ++i) tables.push_back(t.slot(i));
            return at;
        };
        auto place = [&](const float* from, size_t n) {
            const uint32_t at = static_cast<uint32_t>(values.size());
            if (from) values.insert(values.end(), from, from + n);
            else values.resize(values.size() + n, 0.0f);
            return at;
        };
        m.levels.clear();
        m.levelTiles.clear();
        const size_t count = poisson.coarse_.size() + 1;
        for (size_t l = 0; l < count; ++l) {
            const bool fine = l == 0;
            const SparseGrid& grid = fine ? pressure : poisson.coarse_[l - 1].p;
            const PoissonSolver::Counts& on = fine ? poisson.fineOn_ : poisson.coarse_[l - 1].on;
            const PoissonSolver::Operator& op = fine ? poisson.fineOp_ : poisson.coarse_[l - 1].op;
            const float levelH = fine ? h : poisson.coarse_[l - 1].h;
            const Tiles& tiles = grid.tiles();
            PoissonPush q;
            q.nx = grid.nx();
            q.ny = grid.ny();
            q.nz = grid.nz();
            q.slots = table(tiles);
            q.stored = static_cast<uint32_t>(tables.size());
            for (const uint32_t t : tiles.stored()) tables.push_back(static_cast<int32_t>(t));
            q.count = static_cast<uint32_t>(tiles.stored().size());
            m.levelTiles.push_back(q.count);
            const size_t size = grid.size();
            q.p = place(nullptr, size);
            q.b = place(nullptr, size);
            q.r = place(nullptr, size);
            if (op.diagonal.shared()) {
                q.diag = place(op.diagonal.data(), op.diagonal.size());
                q.inv = place(op.inverse.data(), op.inverse.size());
                if (!op.open.empty()) {
                    q.mode = 1;
                    q.open = static_cast<uint32_t>(bits.size());
                    bits.resize(bits.size() + (op.open.size() + 3) / 4, 0u);
                    for (size_t c = 0; c < op.open.size(); ++c) {
                        bits[q.open + c / 4] |= static_cast<uint32_t>(op.open[c]) << (8 * (c % 4));
                    }
                } else {
                    q.mode = 2;
                    for (int a = 0; a < 3; ++a) {
                        q.faceSlots[a] = table(op.a[a].tiles());
                        q.faces[a] = place(op.a[a].data(), op.a[a].size());
                    }
                }
            }
            q.on = static_cast<uint32_t>(bits.size());
            bits.resize(bits.size() + (on.size() + 31) / 32, 0u);
            for (size_t c = 0; c < on.size(); ++c) {
                if (on[c]) bits[q.on + c / 32] |= 1u << (c % 32);
            }
            q.h2 = levelH * levelH;
            q.invH2 = 1.0f / (levelH * levelH);
            for (int side = 0; side < 6; ++side) q.closed |= (poisson.closed_[side] ? 1u : 0u) << side;
            m.levels.push_back(q);
        }
        if (bits.empty()) bits.push_back(0u);
        const size_t tb = tables.size() * sizeof(int32_t), vb = values.size() * sizeof(float),
                     bb = bits.size() * sizeof(uint32_t);
        if (!m.fit(m.poissonTables, tb) || !m.fit(m.poissonValues, vb) || !m.fit(m.poissonBits, bb) ||
            !d.upload(*m.poissonTables, tables.data(), tb) || !d.upload(*m.poissonValues, values.data(), vb) ||
            !d.upload(*m.poissonBits, bits.data(), bb)) {
            m.built = nullptr;
            return m.failed();
        }
        m.built = &poisson;
        m.generation = poisson.generation_;
    }

    // The first guess and the right-hand side.
    const size_t f = sizeof(float);
    const PoissonPush& top = m.levels[0];
    if (!d.upload(*m.poissonValues, pressure.data(), pressure.size() * f, top.p * f) ||
        !d.upload(*m.poissonValues, divergence.data(), divergence.size() * f, top.b * f)) {
        return m.failed();
    }

    // The V-cycles, as PoissonSolver::vcycle runs them.
    gpu::Batch batch(d);
    const std::initializer_list<const gpu::Buffer*> buffers = {m.poissonTables.get(), m.poissonValues.get(),
                                                               m.poissonBits.get()};
    auto groups = [&](size_t l, uint32_t& x, uint32_t& y) { groupsOf(m.levelTiles[l], x, y); };
    auto relax = [&](size_t l, int sweeps, float omega) {
        uint32_t x = 0, y = 0;
        groups(l, x, y);
        for (int sweep = 0; sweep < sweeps; ++sweep) {
            for (int colour = 0; colour < 2; ++colour) {
                PoissonPush q = m.levels[l];
                q.colour = colour;
                q.omega = omega;
                batch.dispatch("poisson_relax", buffers, q, x, y);
            }
        }
    };
    const size_t last = m.levels.size() - 1;
    auto cycle = [&](auto& self, size_t l) -> void {
        const PoissonPush& level = m.levels[l];
        if (l == last) {
            relax(l, PoissonSolver::coarsestSweeps(level.nx, level.ny, level.nz),
                  PoissonSolver::coarsestOmega(level.nx, level.ny, level.nz));
            return;
        }
        relax(l, PoissonSolver::kPreSmooth, 1.0f);
        uint32_t x = 0, y = 0;
        groups(l, x, y);
        batch.dispatch("poisson_residual", buffers, level, x, y);
        // The residual to the coarser level's right-hand side.
        const PoissonPush& coarse = m.levels[l + 1];
        PoissonPush down = coarse;
        down.onx = level.nx;
        down.ony = level.ny;
        down.onz = level.nz;
        down.oslots = level.slots;
        down.other = level.r;
        uint32_t cx = 0, cy = 0;
        groups(l + 1, cx, cy);
        batch.dispatch("poisson_restrict", buffers, down, cx, cy);
        batch.fill(*m.poissonValues, 0u, static_cast<size_t>(m.levelTiles[l + 1]) * Tiles::kCells * f,
                   static_cast<size_t>(coarse.p) * f);
        self(self, l + 1);
        // Its correction back up.
        PoissonPush up = level;
        up.onx = coarse.nx;
        up.ony = coarse.ny;
        up.onz = coarse.nz;
        up.oslots = coarse.slots;
        up.other = coarse.p;
        batch.dispatch("poisson_prolong", buffers, up, x, y);
        relax(l, PoissonSolver::kPostSmooth, 1.0f);
    };
    for (int c = 0; c < cycles; ++c) cycle(cycle, 0);
    m.times.pressureKernels = batch.run();
    if (m.times.pressureKernels < 0.0) return m.failed();

    std::vector<float> solved(pressure.size());
    if (!d.download(*m.poissonValues, solved.data(), solved.size() * f, top.p * f)) return m.failed();
    std::copy(solved.begin(), solved.end(), pressure.data());
    m.times.pressure = msSince(start);
    return true;
#endif
}

}  // namespace pg::sim
