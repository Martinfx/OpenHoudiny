#include "pg/sim/PyroGpu.h"

#include "pg/sim/Poisson.h"
#include "pg/sim/Pyro.h"
#include "pg/sim/Shared.h"

#ifdef PG_HAVE_VULKAN
#include "pg/gpu/Gpu.h"
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
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
    uint32_t flags = 0;
    float f[13] = {};
};
static_assert(sizeof(Push) == 124, "Push must match sparse.glsl");

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

/// Where the multigrid's values start: the inverses of the whole-number
/// diagonals come first.
constexpr uint32_t kFirstValue = 16;

// The planes of the cells' fields (sparse.glsl), and the flags.
constexpr uint32_t kDensityPlane = 0, kTemperaturePlane = 1, kFuelPlane = 2, kFlamePlane = 3, kSteamPlane = 4,
                   kExpansionPlane = 5, kSolidPlane = 6, kPlanes = 7, kCarriedPlanes = 5;
constexpr uint32_t kHasSteam = 1, kHasSolids = 2;

/// The work groups of `tiles` tiles, one a tile, as x by y: no more along x
/// than any device takes.
void groupsOf(size_t tiles, uint32_t& x, uint32_t& y) {
    x = static_cast<uint32_t>(std::clamp<size_t>(tiles, 1, 65535));
    y = static_cast<uint32_t>((tiles + x - 1) / x);
}

/// ... of a list of `n`, 256 a group.
uint32_t listGroups(size_t n) { return static_cast<uint32_t>(std::clamp<size_t>((n + 255) / 256, 1, 65535)); }

double msSince(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}

float intAsFloat(int32_t v) {
    float f = 0.0f;
    std::memcpy(&f, &v, sizeof f);
    return f;
}

uint32_t bitsOf(float v) {
    uint32_t b = 0;
    std::memcpy(&b, &v, sizeof b);
    return b;
}

/// Times a call: its whole into total.
struct Timed {
    double& total;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    ~Timed() { total += msSince(start); }
};

}  // namespace

struct PyroGpu::Impl {
    std::unique_ptr<gpu::Device> device;
    // Binding 0 of the gas's kernels: the four sets' tables (sparse.glsl).
    std::unique_ptr<gpu::Buffer> tables;
    std::shared_ptr<const Tiles> tiled;  ///< the cells the tables, and the buffers, are of
    uint32_t slots[4] = {0, 0, 0, 0}, stored[4] = {0, 0, 0, 0}, storedCount[4] = {0, 0, 0, 0};
    // The velocity (V) and advect's next (N); the cells' fields (S), one plane
    // each; advect's carried fields (O); scratch: the paths (P, six planes),
    // MacCormack's predicted, lows and highs (W, three).
    std::unique_ptr<gpu::Buffer> velocity, next, fields, carried, paths, work;
    uint32_t vel[3] = {0, 0, 0};
    size_t velFloats = 0, plane = 0;
    // The solids, as the CPU found them: the solid cells, then each
    // component's blocked faces with their velocity.
    std::unique_ptr<gpu::Buffer> lists;
    uint32_t solidAt = 0, solidCount = 0, blockedAt[3] = {0, 0, 0}, blockedCount[3] = {0, 0, 0};
    uint64_t solidsVersion = UINT64_MAX;
    std::unique_ptr<gpu::Buffer> wet, knots;
    // The pressure's multigrid, as the PoissonSolver last built it: each
    // level's push constants (all but what a kernel sets), its tiles, and
    // the buffers T, F and B of poisson.glsl. The pressure lives in F, at
    // the finest level's p.
    std::vector<PoissonPush> levels;
    std::vector<uint32_t> levelTiles;
    std::unique_ptr<gpu::Buffer> poissonTables, poissonValues, poissonBits;
    const PoissonSolver* built = nullptr;
    uint64_t generation = 0;
    std::shared_ptr<const Tiles> builtTiles;  ///< the pressure's tiles, as the levels were built for
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
        if (error.empty()) error = device->error().empty() ? "the device failed" : device->error();
        return false;
    }
    double run(gpu::Batch& batch) {
        const double ms = batch.run();
        if (ms > 0.0) times.kernels += ms;
        return ms;
    }
    /// The pressure's place in F, if the levels are built for its tiles.
    bool pressureHere(const PyroSolver& s) const {
        return built == &s.poisson_ && generation == s.poisson_.generation_ && builtTiles == s.pressure_.shared() &&
               !levels.empty();
    }

    // A nested class of PyroSolver's friend, these see its fields.
    bool prepare(PyroSolver& s);
    Push base(const PyroSolver& s) const;
    Push over(const PyroSolver& s, int set, uint32_t& x, uint32_t& y) const;
    static SparseGrid* fieldOf(PyroSolver& s, uint16_t bit, uint32_t& plane);
    static void deviceWrote(PyroSolver& s, uint16_t fields);
    static uint16_t maskFields(Mask mask);
    void addWalls(PyroSolver& s, gpu::Batch& batch, gpu::Buffer& v);
    bool buildLevels(PyroSolver& s, float h);
    void addCycles(gpu::Batch& batch, int cycles);
};

#else

struct PyroGpu::Impl {
    std::string error;
    Times times;
};

#endif

PyroGpu::PyroGpu() : impl_(std::make_unique<Impl>()) {}
PyroGpu::~PyroGpu() = default;

namespace {

std::mutex& choiceMutex() {
    static std::mutex m;
    return m;
}
std::string& choice() {
    static std::string c;
    return c;
}

}  // namespace

void PyroGpu::choose(const std::string& card) {
    std::lock_guard<std::mutex> lock(choiceMutex());
    choice() = card;
}

std::string PyroGpu::chosen() {
    std::lock_guard<std::mutex> lock(choiceMutex());
    return choice();
}

std::unique_ptr<PyroGpu> PyroGpu::open(std::string& why) {
#ifdef PG_HAVE_VULKAN
    auto device = gpu::Device::open(chosen(), why);
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
void PyroGpu::beginStep() { impl_->times = Times{}; }
const PyroGpu::Times& PyroGpu::times() const { return impl_->times; }

#ifndef PG_HAVE_VULKAN

bool PyroGpu::send(PyroSolver&, uint16_t) { return false; }
bool PyroGpu::fetch(PyroSolver&, uint16_t) { return false; }
bool PyroGpu::advect(PyroSolver&, float) { return false; }
bool PyroGpu::quench(PyroSolver&, const std::vector<std::pair<uint32_t, float>>&, float, float) { return false; }
bool PyroGpu::combust(PyroSolver&, float) { return false; }
bool PyroGpu::buoyancy(PyroSolver&, float) { return false; }
bool PyroGpu::vorticity(PyroSolver&, float) { return false; }
bool PyroGpu::force(PyroSolver&, const Force&, size_t, float, bool& done) {
    done = false;
    return false;
}
bool PyroGpu::walls(PyroSolver&) { return false; }
bool PyroGpu::project(PyroSolver&, float, int) { return false; }
bool PyroGpu::dissipate(PyroSolver&, float) { return false; }

#else

/// The buffers laid out for the solver's tiles, their tables on the device,
/// and the solids as the CPU last found them.
bool PyroGpu::Impl::prepare(PyroSolver& s) {
    Impl& m = *this;
    gpu::Device& d = *m.device;
    if (!d.ok()) return m.failed();
    const size_t f = sizeof(float);
    if (m.tiled != s.cells_) {
        const Tiles* sets[4] = {s.cells_.get(), s.faces_[0].get(), s.faces_[1].get(), s.faces_[2].get()};
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
        m.velFloats = 0;
        for (int a = 0; a < 3; ++a) {
            m.vel[a] = static_cast<uint32_t>(m.velFloats);
            m.velFloats += s.vel_[a].size();
        }
        m.plane = s.density_.size();
        if (!m.fit(m.velocity, m.velFloats * f) || !m.fit(m.next, m.velFloats * f) ||
            !m.fit(m.fields, kPlanes * m.plane * f) || !m.fit(m.carried, kCarriedPlanes * m.plane * f) ||
            !m.fit(m.paths, 6 * m.plane * f) || !m.fit(m.work, 3 * m.plane * f)) {
            return m.failed();
        }
        m.tiled = s.cells_;
        // What the device held was laid out on other tiles.
        s.onDevice_ = 0;
        m.solidsVersion = UINT64_MAX;
    }
    if (m.solidsVersion != s.solidsVersion_) {
        std::vector<uint32_t> list(s.solidCells_.begin(), s.solidCells_.end());
        m.solidAt = 0;
        m.solidCount = static_cast<uint32_t>(list.size());
        for (int a = 0; a < 3; ++a) {
            m.blockedAt[a] = static_cast<uint32_t>(list.size());
            m.blockedCount[a] = static_cast<uint32_t>(s.blocked_[a].size());
            for (size_t b = 0; b < s.blocked_[a].size(); ++b) {
                list.push_back(static_cast<uint32_t>(s.blocked_[a][b]));
                list.push_back(bitsOf(s.blockedVel_[a][b]));
            }
        }
        if (list.empty()) list.push_back(0u);
        const size_t bytes = list.size() * sizeof(uint32_t);
        if (!m.fit(m.lists, bytes) || !d.upload(*m.lists, list.data(), bytes) ||
            !d.upload(*m.fields, s.solid_.data(), m.plane * f, kSolidPlane * m.plane * f)) {
            return m.failed();
        }
        m.solidsVersion = s.solidsVersion_;
    }
    return true;
}

/// The push constants every gas kernel starts from.
Push PyroGpu::Impl::base(const PyroSolver& s) const {
    const Impl& m = *this;
    Push p;
    p.nx = s.nx_;
    p.ny = s.ny_;
    p.nz = s.nz_;
    std::copy(m.slots, m.slots + 4, p.slots);
    std::copy(m.vel, m.vel + 3, p.vel);
    p.plane = static_cast<uint32_t>(m.plane);
    p.floorClosed = s.scene_.solver.closedFloor ? 1 : 0;
    return p;
}

/// ... to go over the stored tiles of `set`: 0 the cells, 1 to 3 the faces.
Push PyroGpu::Impl::over(const PyroSolver& s, int set, uint32_t& x, uint32_t& y) const {
    const Impl& m = *this;
    Push p = base(s);
    p.stored = m.stored[set];
    p.slotCount = m.storedCount[set];
    groupsOf(m.storedCount[set], x, y);
    return p;
}

/// The CPU's field of a bit, and the plane it has in S.
SparseGrid* PyroGpu::Impl::fieldOf(PyroSolver& s, uint16_t bit, uint32_t& plane) {
    switch (bit) {
        case PyroSolver::kDensity: plane = kDensityPlane; return &s.density_;
        case PyroSolver::kTemperature: plane = kTemperaturePlane; return &s.temperature_;
        case PyroSolver::kFuel: plane = kFuelPlane; return &s.fuel_;
        case PyroSolver::kFlame: plane = kFlamePlane; return &s.flame_;
        case PyroSolver::kSteam: plane = kSteamPlane; return &s.steam_;
        case PyroSolver::kExpansion: plane = kExpansionPlane; return &s.expansion_;
        default: return nullptr;
    }
}

/// What a stage on the device wrote: newer there than on the CPU.
void PyroGpu::Impl::deviceWrote(PyroSolver& s, uint16_t fields) {
    s.onDevice_ |= fields;
    s.onHost_ &= static_cast<uint16_t>(~fields);
}

/// The walls -- the floor, and the faces the solids block -- on the
/// velocity in `v`.
void PyroGpu::Impl::addWalls(PyroSolver& s, gpu::Batch& batch, gpu::Buffer& v) {
    Impl& m = *this;
    if (s.scene_.solver.closedFloor) {
        uint32_t x = 0, y = 0;
        const Push p = m.over(s, 2, x, y);
        batch.dispatch("pyro_floor", {m.tables.get(), &v}, p, x, y);
    }
    for (int a = 0; a < 3; ++a) {
        if (m.blockedCount[a] == 0) continue;
        Push p = m.base(s);
        p.axis = a;
        p.field = m.blockedAt[a];
        p.slotCount = m.blockedCount[a];
        batch.dispatch("pyro_walls", {m.tables.get(), &v, m.lists.get()}, p, listGroups(m.blockedCount[a]));
    }
}

/// The bits of the fields a mask reads.
uint16_t PyroGpu::Impl::maskFields(Mask mask) {
    if (mask == Mask::Heat) return PyroSolver::kTemperature | PyroSolver::kFuel;
    if (mask == Mask::Smoke) return PyroSolver::kDensity;
    return 0;
}

/// The multigrid on the device for the levels the PoissonSolver has built,
/// each time it builds them again -- keeping the pressure, which lives
/// there.
bool PyroGpu::Impl::buildLevels(PyroSolver& s, float h) {
    Impl& m = *this;
    gpu::Device& d = *m.device;
    PoissonSolver& poisson = s.poisson_;
    SparseGrid& pressure = s.pressure_;
    poisson.build(pressure, h);
    if (m.pressureHere(s)) return true;
    // The pressure the device has newer, before its place goes.
    if ((s.onDevice_ & PyroSolver::kPressure) && !(s.onHost_ & PyroSolver::kPressure) && !m.levels.empty() &&
        m.builtTiles == pressure.shared()) {
        if (!d.download(*m.poissonValues, pressure.data(), pressure.size() * sizeof(float),
                        m.levels[0].p * sizeof(float))) {
            return m.failed();
        }
        s.onHost_ |= PyroSolver::kPressure;
    }
    s.onDevice_ &= static_cast<uint16_t>(~PyroSolver::kPressure);
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
    m.built = nullptr;
    if (!m.fit(m.poissonTables, tb) || !m.fit(m.poissonValues, vb) || !m.fit(m.poissonBits, bb) ||
        !d.upload(*m.poissonTables, tables.data(), tb) || !d.upload(*m.poissonValues, values.data(), vb) ||
        !d.upload(*m.poissonBits, bits.data(), bb)) {
        return m.failed();
    }
    m.built = &poisson;
    m.generation = poisson.generation_;
    m.builtTiles = pressure.shared();
    return true;
}

/// The V-cycles, as PoissonSolver::vcycle runs them, on what the finest
/// level's b and p hold.
void PyroGpu::Impl::addCycles(gpu::Batch& batch, int cycles) {
    Impl& m = *this;
    const size_t f = sizeof(float);
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
}

bool PyroGpu::send(PyroSolver& s, uint16_t fields) {
    Impl& m = *impl_;
    if (!m.prepare(s)) return false;
    uint16_t need = static_cast<uint16_t>(fields & ~s.onDevice_);
    if (!need) return true;
    Timed timed{m.times.copies};
    gpu::Device& d = *m.device;
    const size_t f = sizeof(float);
    // The pressure only where the levels are built for it: project() sends
    // it once they are.
    if ((need & PyroSolver::kPressure) && !m.pressureHere(s)) need &= static_cast<uint16_t>(~PyroSolver::kPressure);
    bool ok = true;
    for (int a = 0; a < 3 && ok; ++a) {
        if (need & PyroSolver::kVel) ok = d.upload(*m.velocity, s.vel_[a].data(), s.vel_[a].size() * f, m.vel[a] * f);
        if (ok && (need & PyroSolver::kVelNext)) {
            ok = d.upload(*m.next, s.velNext_[a].data(), s.velNext_[a].size() * f, m.vel[a] * f);
        }
    }
    for (uint16_t bit = PyroSolver::kDensity; bit <= PyroSolver::kExpansion && ok; bit = static_cast<uint16_t>(bit << 1)) {
        if (!(need & bit)) continue;
        uint32_t plane = 0;
        const SparseGrid* g = Impl::fieldOf(s, bit, plane);
        ok = d.upload(*m.fields, g->data(), m.plane * f, plane * m.plane * f);
    }
    if (ok && (need & PyroSolver::kPressure)) {
        ok = d.upload(*m.poissonValues, s.pressure_.data(), s.pressure_.size() * f, m.levels[0].p * f);
    }
    if (!ok) return m.failed();
    s.onDevice_ |= need;
    return true;
}

bool PyroGpu::fetch(PyroSolver& s, uint16_t fields) {
    Impl& m = *impl_;
    const uint16_t need = static_cast<uint16_t>(fields & ~s.onHost_ & s.onDevice_);
    if (!need) return true;
    if (!m.device->ok()) return m.failed();
    Timed timed{m.times.copies}, all{m.times.total};
    gpu::Device& d = *m.device;
    const size_t f = sizeof(float);
    bool ok = true;
    for (int a = 0; a < 3 && ok; ++a) {
        if (need & PyroSolver::kVel) ok = d.download(*m.velocity, s.vel_[a].data(), s.vel_[a].size() * f, m.vel[a] * f);
        if (ok && (need & PyroSolver::kVelNext)) {
            ok = d.download(*m.next, s.velNext_[a].data(), s.velNext_[a].size() * f, m.vel[a] * f);
        }
    }
    for (uint16_t bit = PyroSolver::kDensity; bit <= PyroSolver::kExpansion && ok; bit = static_cast<uint16_t>(bit << 1)) {
        if (!(need & bit)) continue;
        uint32_t plane = 0;
        SparseGrid* g = Impl::fieldOf(s, bit, plane);
        ok = d.download(*m.fields, g->data(), m.plane * f, plane * m.plane * f);
    }
    if (ok && (need & PyroSolver::kPressure)) {
        ok = d.download(*m.poissonValues, s.pressure_.data(), s.pressure_.size() * f, m.levels[0].p * f);
    }
    if (!ok) return m.failed();
    s.onHost_ |= need;
    return true;
}

bool PyroGpu::advect(PyroSolver& s, float dt) {
    Impl& m = *impl_;
    Timed timed{m.times.total};
    const uint16_t carried = PyroSolver::kDensity | PyroSolver::kTemperature | PyroSolver::kFuel | PyroSolver::kFlame |
                             (s.steamy_ ? PyroSolver::kSteam : 0);
    if (!send(s, PyroSolver::kVel | PyroSolver::kVelNext | carried)) return false;
    const size_t f = sizeof(float);
    gpu::Batch batch(*m.device);
    uint32_t gx = 0, gy = 0;
    Push push = m.over(s, 0, gx, gy);
    push.cells = dt / s.domain_.voxel;  // as advect: velocity x dt, in cells
    // The paths, over the cells; each component of the velocity, over its
    // faces, into N.
    batch.dispatch("pyro_paths", {m.tables.get(), m.velocity.get(), m.paths.get()}, push, gx, gy);
    for (int a = 0; a < 3; ++a) {
        uint32_t fx = 0, fy = 0;
        Push faces = m.over(s, a + 1, fx, fy);
        faces.cells = push.cells;
        faces.axis = a;
        batch.dispatch("pyro_velocity", {m.tables.get(), m.velocity.get(), m.next.get()}, faces, fx, fy);
    }
    // MacCormack for each field, into O, then back to its plane.
    const uint32_t count = s.steamy_ ? 5u : 4u;
    for (uint32_t i = 0; i < count; ++i) {
        Push field = push;
        field.field = static_cast<uint32_t>(i * m.plane);
        batch.dispatch("pyro_predict", {m.tables.get(), m.fields.get(), m.paths.get(), m.work.get()}, field, gx, gy);
        batch.dispatch("pyro_correct", {m.tables.get(), m.fields.get(), m.paths.get(), m.work.get(), m.carried.get()},
                       field, gx, gy);
        batch.copy(*m.carried, *m.fields, m.plane * f, i * m.plane * f, i * m.plane * f);
    }
    // finishAdvect: no gas in the solids, the walls.
    if (m.solidCount > 0) {
        Push solids = m.base(s);
        solids.field = m.solidAt;
        solids.slotCount = m.solidCount;
        batch.dispatch("pyro_unsolid", {m.tables.get(), m.fields.get(), m.lists.get()}, solids, listGroups(m.solidCount));
    }
    m.addWalls(s, batch, *m.next);
    if (m.run(batch) < 0.0) return m.failed();
    // The velocity carried is the velocity now; the one it was, the next
    // step's to write over -- as the CPU swaps them.
    std::swap(m.velocity, m.next);
    for (int a = 0; a < 3; ++a) std::swap(s.vel_[a], s.velNext_[a]);
    const bool velOnHost = (s.onHost_ & PyroSolver::kVel) != 0;
    Impl::deviceWrote(s, PyroSolver::kVel | carried);
    s.onDevice_ |= PyroSolver::kVelNext;
    if (velOnHost) s.onHost_ |= PyroSolver::kVelNext;
    else s.onHost_ &= static_cast<uint16_t>(~PyroSolver::kVelNext);
    // The solid cells' steam, emptied on the device too when it is not
    // carried: the CPU's is 0 there, and stays so.
    return true;
}

bool PyroGpu::quench(PyroSolver& s, const std::vector<std::pair<uint32_t, float>>& wet, float cooled, float steam) {
    Impl& m = *impl_;
    Timed timed{m.times.total};
    const uint16_t touched = PyroSolver::kTemperature | PyroSolver::kSteam | PyroSolver::kFuel | PyroSolver::kFlame;
    if (wet.empty()) return true;
    if (!send(s, touched)) return false;
    std::vector<uint32_t> list;
    list.reserve(2 * wet.size());
    for (const auto& [at, share] : wet) {
        list.push_back(at);
        list.push_back(bitsOf(share));
    }
    const size_t bytes = list.size() * sizeof(uint32_t);
    {
        Timed copies{m.times.copies};
        if (!m.fit(m.wet, bytes) || !m.device->upload(*m.wet, list.data(), bytes)) return m.failed();
    }
    gpu::Batch batch(*m.device);
    Push p = m.base(s);
    p.slotCount = static_cast<uint32_t>(wet.size());
    p.f[0] = cooled;
    p.f[1] = steam;
    batch.dispatch("pyro_quench", {m.tables.get(), m.fields.get(), m.wet.get()}, p, listGroups(wet.size()));
    if (m.run(batch) < 0.0) return m.failed();
    Impl::deviceWrote(s, touched);
    return true;
}

bool PyroGpu::combust(PyroSolver& s, float dt) {
    Impl& m = *impl_;
    Timed timed{m.times.total};
    const uint16_t touched = PyroSolver::kFuel | PyroSolver::kTemperature | PyroSolver::kDensity |
                             PyroSolver::kFlame | PyroSolver::kExpansion;
    if (!send(s, touched)) return false;
    const SolverSettings& settings = s.scene_.solver;
    gpu::Batch batch(*m.device);
    uint32_t x = 0, y = 0;
    Push p = m.over(s, 0, x, y);
    p.f[0] = 1.0f - std::exp(-settings.burnRate * dt);  // as combust()
    p.f[1] = settings.heatRelease;
    p.f[2] = settings.sootRelease;
    p.f[3] = settings.expansion;
    p.f[4] = dt;
    batch.dispatch("pyro_combust", {m.tables.get(), m.fields.get()}, p, x, y);
    if (m.run(batch) < 0.0) return m.failed();
    Impl::deviceWrote(s, touched);
    return true;
}

bool PyroGpu::buoyancy(PyroSolver& s, float dt) {
    Impl& m = *impl_;
    Timed timed{m.times.total};
    const SolverSettings& settings = s.scene_.solver;
    const bool steam = s.steamy_ && settings.steamLift != 0.0f;
    if (!send(s, PyroSolver::kVel | PyroSolver::kTemperature | PyroSolver::kDensity | (steam ? PyroSolver::kSteam : 0))) {
        return false;
    }
    gpu::Batch batch(*m.device);
    uint32_t x = 0, y = 0;
    Push p = m.over(s, 2, x, y);
    p.flags = steam ? kHasSteam : 0u;
    p.f[0] = dt;
    p.f[1] = settings.buoyancy;
    p.f[2] = settings.weight;
    p.f[3] = settings.steamLift;
    batch.dispatch("pyro_buoyancy", {m.tables.get(), m.velocity.get(), m.fields.get()}, p, x, y);
    if (m.run(batch) < 0.0) return m.failed();
    Impl::deviceWrote(s, PyroSolver::kVel);
    return true;
}

bool PyroGpu::vorticity(PyroSolver& s, float dt) {
    Impl& m = *impl_;
    Timed timed{m.times.total};
    if (!send(s, PyroSolver::kVel)) return false;
    const float h = s.domain_.voxel;
    gpu::Batch batch(*m.device);
    uint32_t x = 0, y = 0;
    Push p = m.over(s, 0, x, y);
    // As addVorticity's around() has them: 1 / (cells apart x h).
    p.f[0] = 1.0f / (static_cast<float>(1) * h);
    p.f[1] = 1.0f / (static_cast<float>(2) * h);
    p.f[2] = s.scene_.solver.vorticity * h;
    p.f[3] = dt;
    batch.dispatch("pyro_swirl_centre", {m.tables.get(), m.velocity.get(), m.paths.get()}, p, x, y);
    batch.dispatch("pyro_swirl_curl", {m.tables.get(), m.paths.get(), m.work.get()}, p, x, y);
    batch.dispatch("pyro_swirl_force", {m.tables.get(), m.paths.get(), m.work.get()}, p, x, y);
    for (int a = 0; a < 3; ++a) {
        uint32_t fx = 0, fy = 0;
        Push faces = m.over(s, a + 1, fx, fy);
        std::copy(p.f, p.f + 4, faces.f);
        faces.axis = a;
        batch.dispatch("pyro_swirl_faces", {m.tables.get(), m.velocity.get(), m.paths.get()}, faces, fx, fy);
    }
    if (m.run(batch) < 0.0) return m.failed();
    Impl::deviceWrote(s, PyroSolver::kVel);
    return true;
}

bool PyroGpu::force(PyroSolver& s, const Force& force, size_t index, float dt, bool& done) {
    Impl& m = *impl_;
    done = false;
    Timed timed{m.times.total};
    if (!send(s, PyroSolver::kVel | Impl::maskFields(force.mask))) return false;
    gpu::Batch batch(*m.device);
    if (force.kind == ForceKind::Wind || force.kind == ForceKind::Vortex || force.kind == ForceKind::Attractor) {
        // The force's numbers, as the CPU's addForce works them out, to the
        // device; the rest per face there.
        const uint32_t seed = force.seed * 7919u + s.scene_.solver.seed * 31u;
        std::vector<float> numbers;
        int kind = 0;
        if (force.kind == ForceKind::Wind) {
            kind = 1;
            const Vec3 d = normalize(force.direction);
            numbers = {1.0f - std::exp(-force.strength * dt), d.x, d.y, d.z, force.speed, force.gusts, s.time_,
                       intAsFloat(static_cast<int32_t>(seed))};
        } else if (force.kind == ForceKind::Vortex) {
            kind = 2;
            const Vec3 axis = normalize(force.direction);
            numbers = {1.0f - std::exp(-force.strength * dt), axis.x, axis.y, axis.z, force.center.x, force.center.y,
                       force.center.z, 0.5f * force.height, force.radius, force.speed, force.lift, force.suction};
        } else {
            kind = 3;
            numbers = {dt * force.strength, force.center.x, force.center.y, force.center.z, force.radius};
        }
        const size_t bytes = numbers.size() * sizeof(float);
        {
            Timed copies{m.times.copies};
            if (!m.fit(m.knots, bytes) || !m.device->upload(*m.knots, numbers.data(), bytes)) return m.failed();
        }
        const Vec3 o = s.domain_.origin();
        for (int a = 0; a < 3; ++a) {
            uint32_t x = 0, y = 0;
            Push p = m.over(s, a + 1, x, y);
            p.axis = a;
            p.f[0] = intAsFloat(kind);
            p.f[1] = intAsFloat(static_cast<int32_t>(force.mask));
            p.f[2] = o.x;
            p.f[3] = o.y;
            p.f[4] = o.z;
            p.f[5] = s.domain_.voxel;
            batch.dispatch("pyro_pull", {m.tables.get(), m.velocity.get(), m.fields.get(), m.knots.get()}, p, x, y);
        }
    } else if (force.kind == ForceKind::Turbulence) {
        // The lattice, worked out as the CPU's addForce does, to the device.
        const uint32_t seed = force.seed * 7919u + s.scene_.solver.seed * 31u;
        std::array<Grid, 3>& knots = s.noise_[index];
        const float cellsPerKnot = detail::turbulenceKnots(force, seed, s.domain_, s.time_, knots);
        std::vector<float> all;
        uint32_t at[3] = {0, 0, 0};
        for (int a = 0; a < 3; ++a) {
            at[a] = static_cast<uint32_t>(all.size());
            all.insert(all.end(), knots[static_cast<size_t>(a)].values().begin(),
                       knots[static_cast<size_t>(a)].values().end());
        }
        const size_t bytes = all.size() * sizeof(float);
        {
            Timed copies{m.times.copies};
            if (!m.fit(m.knots, bytes) || !m.device->upload(*m.knots, all.data(), bytes)) return m.failed();
        }
        for (int a = 0; a < 3; ++a) {
            uint32_t x = 0, y = 0;
            Push p = m.over(s, a + 1, x, y);
            p.axis = a;
            const Grid& g = knots[static_cast<size_t>(a)];
            p.f[0] = dt * force.strength;  // as addForce: dt x strength x mask x push
            p.f[1] = cellsPerKnot;
            p.f[2] = intAsFloat(g.nx());
            p.f[3] = intAsFloat(g.ny());
            p.f[4] = intAsFloat(g.nz());
            p.f[5] = intAsFloat(static_cast<int32_t>(at[a]));
            p.f[6] = intAsFloat(static_cast<int32_t>(force.mask));
            batch.dispatch("pyro_turbulence", {m.tables.get(), m.velocity.get(), m.fields.get(), m.knots.get()}, p, x, y);
        }
    } else {
        const float keep = std::exp(-force.strength * dt);  // as addForce
        for (int a = 0; a < 3; ++a) {
            uint32_t x = 0, y = 0;
            Push p = m.over(s, a + 1, x, y);
            p.axis = a;
            p.f[0] = 1.0f - keep;
            p.f[1] = intAsFloat(static_cast<int32_t>(force.mask));
            batch.dispatch("pyro_drag", {m.tables.get(), m.velocity.get(), m.fields.get()}, p, x, y);
        }
    }
    if (m.run(batch) < 0.0) return m.failed();
    Impl::deviceWrote(s, PyroSolver::kVel);
    done = true;
    return true;
}

bool PyroGpu::walls(PyroSolver& s) {
    Impl& m = *impl_;
    Timed timed{m.times.total};
    if (!send(s, PyroSolver::kVel)) return false;
    gpu::Batch batch(*m.device);
    m.addWalls(s, batch, *m.velocity);
    if (m.run(batch) < 0.0) return m.failed();
    Impl::deviceWrote(s, PyroSolver::kVel);
    return true;
}

bool PyroGpu::project(PyroSolver& s, float h, int cycles) {
    Impl& m = *impl_;
    Timed timed{m.times.total};
    if (!m.prepare(s) || !m.buildLevels(s, h)) return false;
    if (!send(s, PyroSolver::kVel | PyroSolver::kExpansion | PyroSolver::kPressure)) return false;
    gpu::Batch batch(*m.device);
    // What flows through walls is 0 before anything is measured.
    m.addWalls(s, batch, *m.velocity);
    uint32_t x = 0, y = 0;
    Push rhs = m.over(s, 0, x, y);
    rhs.field = m.levels[0].b;
    rhs.flags = s.anySolid_ ? kHasSolids : 0u;
    rhs.f[0] = s.domain_.voxel;
    batch.dispatch("pyro_divergence",
                   {m.tables.get(), m.velocity.get(), m.fields.get(), m.poissonValues.get()}, rhs, x, y);
    m.addCycles(batch, cycles);
    for (int a = 0; a < 3; ++a) {
        uint32_t fx = 0, fy = 0;
        Push p = m.over(s, a + 1, fx, fy);
        p.axis = a;
        p.field = m.levels[0].p;
        p.f[0] = h;
        batch.dispatch("pyro_gradient", {m.tables.get(), m.velocity.get(), m.poissonValues.get()}, p, fx, fy);
    }
    m.addWalls(s, batch, *m.velocity);
    if (m.run(batch) < 0.0) return m.failed();
    Impl::deviceWrote(s, PyroSolver::kVel | PyroSolver::kPressure);
    return true;
}

bool PyroGpu::dissipate(PyroSolver& s, float dt) {
    Impl& m = *impl_;
    Timed timed{m.times.total};
    const uint16_t touched = PyroSolver::kDensity | PyroSolver::kTemperature | PyroSolver::kFuel | PyroSolver::kFlame |
                             (s.steamy_ ? PyroSolver::kSteam : 0);
    if (!send(s, touched | PyroSolver::kExpansion)) return false;
    const SolverSettings& settings = s.scene_.solver;
    gpu::Batch batch(*m.device);
    uint32_t x = 0, y = 0;
    Push p = m.over(s, 0, x, y);
    // As dissipate() works them out.
    p.f[0] = std::exp(-settings.smokeDecay * dt);
    p.f[1] = std::exp(-settings.cooling * dt);
    p.f[2] = settings.flameLife > 0.0f ? std::exp(-dt / settings.flameLife) : 0.0f;
    p.f[3] = std::exp(-settings.steamFade * dt);
    p.f[4] = dt;
    p.flags = s.steamy_ ? kHasSteam : 0u;
    batch.dispatch("pyro_dissipate", {m.tables.get(), m.fields.get()}, p, x, y);
    if (m.run(batch) < 0.0) return m.failed();
    Impl::deviceWrote(s, touched);
    return true;
}

#endif

}  // namespace pg::sim
