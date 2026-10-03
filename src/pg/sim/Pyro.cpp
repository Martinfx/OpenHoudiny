#include "pg/sim/Pyro.h"

#include "pg/core/Parallel.h"
#include "pg/sim/Shared.h"
#include "pg/sim/State.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <unordered_map>
#include <utility>

namespace pg::sim {

namespace {

using Clock = std::chrono::steady_clock;

/// Adds the milliseconds since `t0` to `ms`, and starts again.
void lap(Clock::time_point& t0, double& ms) {
    const Clock::time_point now = Clock::now();
    ms += std::chrono::duration<double, std::milli>(now - t0).count();
    t0 = now;
}

/// The number of cell (i, j, k) in a box of n cells, x fastest: an order
/// that does not depend on the tiles.
uint64_t numberOf(int i, int j, int k, const int n[3]) {
    return static_cast<uint64_t>(i) +
           static_cast<uint64_t>(n[0]) * (static_cast<uint64_t>(j) + static_cast<uint64_t>(n[1]) * static_cast<uint64_t>(k));
}

}  // namespace

using detail::faceOffset;
using detail::forEachIn;
using detail::noise3;

// --- set-up ----------------------------------------------------------------------

PyroSolver::PyroSolver(const Scene& scene) : scene_(scene.sanitized()) { reset(); }

void PyroSolver::setScene(const Scene& scene) {
    const Scene safe = scene.sanitized();
    const Domain d = safe.solver.domain();
    const bool resize = d.cells[0] != domain_.cells[0] || d.cells[1] != domain_.cells[1] ||
                        d.cells[2] != domain_.cells[2] || d.voxel != domain_.voxel ||
                        safe.solver.sparse != scene_.solver.sparse;
    const bool walls = safe.colliders != scene_.colliders || safe.solver.closedFloor != scene_.solver.closedFloor;
    scene_ = safe;
    if (resize) {
        reset();
        return;
    }
    noise_.resize(scene_.forces.size());
    if (walls) updateSolids();
}

void PyroSolver::reset() {
    domain_ = scene_.solver.domain();
    nx_ = domain_.cells[0];
    ny_ = domain_.cells[1];
    nz_ = domain_.cells[2];
    // Sparse, nothing is worked on until there is gas or a source; dense,
    // every tile is.
    for (SparseGrid* g : {&vel_[0], &vel_[1], &vel_[2], &density_, &temperature_, &fuel_, &flame_, &pressure_}) {
        *g = SparseGrid();
    }
    retile(scene_.solver.sparse ? std::make_shared<const Tiles>(nx_, ny_, nz_, std::vector<uint8_t>(), -1)
                                : std::make_shared<const Tiles>(nx_, ny_, nz_));
    noise_.assign(scene_.forces.size(), {});
    frame_ = 0;
    time_ = 0.0f;
    times_ = Times{};
    wet_.clear();
    soaked_.clear();
    updateSolids();
}

void PyroSolver::retile(std::shared_ptr<const Tiles> cells) {
    cells_ = std::move(cells);
    for (int a = 0; a < 3; ++a) {
        faces_[a] = Tiles::faces(*cells_, a);
        vel_[a].retile(faces_[a]);
        velNext_[a] = SparseGrid(faces_[a]);
    }
    // What the gas carries, and the pressure -- the next solve's first guess
    // -- go on where the tiles do.
    for (SparseGrid* g : {&density_, &temperature_, &fuel_, &flame_, &pressure_}) g->retile(cells_);
    // The rest is made again each step, or by updateSolids().
    for (SparseGrid* g : {&solid_, &back_[0], &back_[1], &back_[2], &forward_[0], &forward_[1], &forward_[2],
                          &predicted_, &hi_, &corrected_, &expansion_, &divergence_}) {
        *g = SparseGrid(cells_);
    }
    solidCells_.clear();
    for (int a = 0; a < 3; ++a) {
        blocked_[a].clear();
        blockedVel_[a].clear();
    }
    anySolid_ = false;
}

void PyroSolver::saveState(StateWriter& out) const {
    out.pod(static_cast<int32_t>(nx_));
    out.pod(static_cast<int32_t>(ny_));
    out.pod(static_cast<int32_t>(nz_));
    out.pod(static_cast<uint8_t>(scene_.solver.sparse));
    out.pod(static_cast<int32_t>(frame_));
    out.pod(time_);
    out.tiles(*cells_);
    for (int a = 0; a < 3; ++a) out.values(vel_[a]);
    for (const SparseGrid* g : {&density_, &temperature_, &fuel_, &flame_, &pressure_, &solid_}) out.values(*g);
    out.pod(static_cast<uint8_t>(anySolid_));
    out.list(solidCells_);
    for (int a = 0; a < 3; ++a) {
        out.list(blocked_[a]);
        out.list(blockedVel_[a]);
    }
    out.list(soaked_);
}

bool PyroSolver::loadState(StateReader& in) {
    int32_t n[3] = {0, 0, 0}, frame = 0;
    uint8_t sparse = 0, anySolid = 0;
    float time = 0.0f;
    std::shared_ptr<const Tiles> cells;
    if (!in.pod(n[0]) || !in.pod(n[1]) || !in.pod(n[2]) || !in.pod(sparse) || !in.pod(frame) || !in.pod(time) ||
        !in.tiles(cells)) {
        return false;
    }
    if (n[0] != nx_ || n[1] != ny_ || n[2] != nz_ || (sparse != 0) != scene_.solver.sparse || cells->axis() != -1 ||
        cells->nx() != nx_ || cells->ny() != ny_ || cells->nz() != nz_) {
        return in.fail();
    }
    // Read aside: a state cut short leaves this solver as it was.
    SparseGrid vel[3];
    for (int a = 0; a < 3; ++a) {
        vel[a] = SparseGrid(Tiles::faces(*cells, a));
        if (!in.values(vel[a])) return false;
    }
    SparseGrid fields[6];
    for (SparseGrid& g : fields) {
        g = SparseGrid(cells);
        if (!in.values(g)) return false;
    }
    std::vector<size_t> solidCells, blocked[3];
    std::vector<float> blockedVel[3];
    if (!in.pod(anySolid) || !in.list(solidCells)) return false;
    for (const size_t c : solidCells) {
        if (c >= fields[5].size()) return in.fail();
    }
    for (int a = 0; a < 3; ++a) {
        if (!in.list(blocked[a]) || !in.list(blockedVel[a]) || blocked[a].size() != blockedVel[a].size()) {
            return in.fail();
        }
        for (const size_t f : blocked[a]) {
            if (f >= vel[a].size()) return in.fail();
        }
    }
    std::vector<float> soaked;
    if (!in.list(soaked)) return false;
    retile(cells);
    for (int a = 0; a < 3; ++a) {
        // Onto the faces retile() made, which the other face fields share.
        std::copy(vel[a].values().begin(), vel[a].values().end(), vel_[a].data());
        blocked_[a] = std::move(blocked[a]);
        blockedVel_[a] = std::move(blockedVel[a]);
    }
    SparseGrid* mine[6] = {&density_, &temperature_, &fuel_, &flame_, &pressure_, &solid_};
    for (int f = 0; f < 6; ++f) *mine[f] = std::move(fields[f]);
    solidCells_ = std::move(solidCells);
    soaked_ = std::move(soaked);
    anySolid_ = anySolid != 0;
    frame_ = frame;
    time_ = time;
    PoissonBoundary boundary;
    boundary.closed[2] = scene_.solver.closedFloor;
    boundary.solid = anySolid_ ? &solid_ : nullptr;
    poisson_.setBoundary(boundary);
    return true;
}

Vec3 PyroSolver::worldAt(float x, float y, float z) const {
    const Vec3 o = domain_.origin();
    const float h = domain_.voxel;
    return {o.x + x * h, o.y + y * h, o.z + z * h};
}

void PyroSolver::updateTiles(float dt) {
    if (!scene_.solver.sparse) return;
    const Tiles& now = *cells_;
    const int tn[3] = {now.tilesX(), now.tilesY(), now.tilesZ()};
    auto number = [&](int a, int b, int c) {
        return static_cast<size_t>(a) +
               static_cast<size_t>(tn[0]) * (static_cast<size_t>(b) + static_cast<size_t>(tn[1]) * static_cast<size_t>(c));
    };
    std::vector<uint8_t> busy(now.tileCount(), 0);
    // Tiles with gas in them.
    const float cutoff = scene_.solver.cutoff;
    const std::vector<uint32_t>& stored = now.stored();
    pg::parallelFor(stored.size(), 16, [&](size_t begin, size_t end) {
        for (size_t s = begin; s < end; ++s) {
            const size_t base = s * Tiles::kCells;
            bool gas = false;
            for (const SparseGrid* g : {&density_, &temperature_, &fuel_, &flame_}) {
                const float* v = g->data() + base;
                for (int c = 0; c < Tiles::kCells && !gas; ++c) gas = v[c] > cutoff;
                if (gas) break;
            }
            if (gas) busy[stored[s]] = 1;
        }
    });
    // How fast the air goes: how far the gas may get in a step.
    float speed = 0.0f;
    for (int a = 0; a < 3; ++a) {
        const size_t tiles = faces_[a]->stored().size();
        std::vector<float> most(tiles, 0.0f);
        pg::parallelFor(tiles, 16, [&](size_t begin, size_t end) {
            for (size_t s = begin; s < end; ++s) {
                const float* v = vel_[a].data() + s * Tiles::kCells;
                float m = 0.0f;
                for (int c = 0; c < Tiles::kCells; ++c) m = std::max(m, std::fabs(v[c]));
                most[s] = m;
            }
        });
        for (const float m : most) speed = std::max(speed, m);
    }
    // The boxes of the sources and of the solids that move: the gas starts
    // there, and a moving solid pushes the air.
    const float h = domain_.voxel;
    const Vec3 origin = domain_.origin();
    const int n[3] = {nx_, ny_, nz_};
    auto box = [&](const Vec3& lo, const Vec3& hi) {
        int from[3], to[3];
        for (int a = 0; a < 3; ++a) {
            if (!(hi[a] >= lo[a])) return;
            from[a] = std::clamp(static_cast<int>(std::floor((lo[a] - origin[a]) / h)) - 1, 0, n[a] - 1) / Tiles::kSide;
            to[a] = std::clamp(static_cast<int>(std::ceil((hi[a] - origin[a]) / h)) + 1, 0, n[a] - 1) / Tiles::kSide;
        }
        for (int c = from[2]; c <= to[2]; ++c) {
            for (int b = from[1]; b <= to[1]; ++b) {
                for (int a = from[0]; a <= to[0]; ++a) busy[number(a, b, c)] = 1;
            }
        }
    };
    for (const Emitter& e : scene_.emitters) {
        if (!e.activeAt(time_)) continue;
        Vec3 lo, hi;
        e.shapeAt(time_).bounds(lo, hi);
        box(lo, hi);
    }
    for (const Collider& c : scene_.colliders) {
        if (!c.moves()) continue;
        Vec3 lo, hi;
        c.instance().bounds(lo, hi);
        box(lo, hi);
    }
    // Round them, as far as the fastest air goes in a step and a cell more:
    // a tile at least, four at most.
    const float reach = speed * dt / h + 1.0f;
    const int pad = std::clamp(static_cast<int>(std::ceil(reach / static_cast<float>(Tiles::kSide))), 1, 4);
    std::vector<uint8_t> state(now.tileCount(), Tiles::Off);
    for (size_t t = 0; t < busy.size(); ++t) {
        if (!busy[t]) continue;
        const int a = static_cast<int>(t % static_cast<size_t>(tn[0]));
        const int b = static_cast<int>((t / static_cast<size_t>(tn[0])) % static_cast<size_t>(tn[1]));
        const int c = static_cast<int>(t / (static_cast<size_t>(tn[0]) * static_cast<size_t>(tn[1])));
        for (int z = std::max(c - pad, 0); z <= std::min(c + pad, tn[2] - 1); ++z) {
            for (int y = std::max(b - pad, 0); y <= std::min(b + pad, tn[1] - 1); ++y) {
                for (int x = std::max(a - pad, 0); x <= std::min(a + pad, tn[0] - 1); ++x) state[number(x, y, z)] = Tiles::Whole;
            }
        }
    }
    if (state == now.states()) return;
    retile(std::make_shared<const Tiles>(nx_, ny_, nz_, std::move(state), -1));
    updateSolids();
}

void PyroSolver::updateSolids() {
    Clock::time_point t0 = Clock::now();
    // Only the cells the colliders took last time need clearing, and only the
    // cells in a collider's box testing: the pieces of a demolition are small
    // against the domain, and there are hundreds of them.
    for (const size_t c : solidCells_) solid_.data()[c] = 0.0f;
    solidCells_.clear();
    const float h = domain_.voxel;
    const Vec3 origin = domain_.origin();
    const int n[3] = {nx_, ny_, nz_};
    struct Cell {
        uint64_t number;
        int i, j, k;
        size_t at;
    };
    std::vector<Cell> solids;
    for (size_t c = 0; c < scene_.colliders.size(); ++c) {
        // A cell is solid when its centre is inside a collider -- the first
        // one that has it: solid_ holds 1 + the collider's index, which one
        // it is, for its velocity.
        const ShapeInstance shape = scene_.colliders[c].instance();
        Vec3 lo, hi;
        shape.bounds(lo, hi);
        int from[3], to[3];
        for (int a = 0; a < 3; ++a) {
            // The cells whose centres may be in the box, and one more each side.
            from[a] = std::clamp(static_cast<int>(std::floor((lo[a] - origin[a]) / h - 0.5f)) - 1, 0, n[a]);
            to[a] = std::clamp(static_cast<int>(std::ceil((hi[a] - origin[a]) / h - 0.5f)) + 2, 0, n[a]);
        }
        const float mark = 1.0f + static_cast<float>(c);
        forEachIn(from[0], to[0], from[1], to[1], from[2], to[2], [&](int i, int j, int k) {
            if (!solid_.stored(i, j, k)) return;
            float& cell = solid_.ref(i, j, k);
            if (cell > 0.5f) return;
            const Vec3 p = worldAt(static_cast<float>(i) + 0.5f, static_cast<float>(j) + 0.5f,
                                   static_cast<float>(k) + 0.5f);
            if (p.x < lo.x || p.y < lo.y || p.z < lo.z || p.x > hi.x || p.y > hi.y || p.z > hi.z) return;
            if (shape.contains(p)) cell = mark;
        });
        for (int k = from[2]; k < to[2]; ++k) {
            for (int j = from[1]; j < to[1]; ++j) {
                for (int i = from[0]; i < to[0]; ++i) {
                    if (solid_.stored(i, j, k) && solid_.at(i, j, k) == mark) {
                        solids.push_back({numberOf(i, j, k, n), i, j, k, solid_.index(i, j, k)});
                    }
                }
            }
        }
    }
    std::sort(solids.begin(), solids.end(), [](const Cell& a, const Cell& b) { return a.number < b.number; });
    for (const Cell& c : solids) solidCells_.push_back(c.at);
    anySolid_ = !solidCells_.empty();
    // The faces the solids block -- the six of each solid cell -- and the
    // floor's when it is closed, listed once: the walls are enforced several
    // times a step, by walking these short lists. And what a moving solid
    // gives each: its velocity there, along the face's axis -- the gas next
    // to it is pushed and dragged.
    const bool moving = anySolid_ && std::any_of(scene_.colliders.begin(), scene_.colliders.end(),
                                                 [](const Collider& c) { return c.moves(); });
    for (int a = 0; a < 3; ++a) {
        blocked_[a].clear();
        blockedVel_[a].clear();
        if (!anySolid_) continue;
        const SparseGrid& v = vel_[a];
        const int fn[3] = {v.nx(), v.ny(), v.nz()};
        std::vector<Cell> faces;
        for (const Cell& c : solids) {
            faces.push_back({numberOf(c.i, c.j, c.k, fn), c.i, c.j, c.k, 0});
            const int i = c.i + (a == 0), j = c.j + (a == 1), k = c.k + (a == 2);
            faces.push_back({numberOf(i, j, k, fn), i, j, k, 0});
        }
        if (a == 1 && scene_.solver.closedFloor) {
            for (int k = 0; k < nz_; ++k) {
                for (int i = 0; i < nx_; ++i) {
                    if (v.has(i, 0, k)) faces.push_back({numberOf(i, 0, k, fn), i, 0, k, 0});
                }
            }
        }
        std::sort(faces.begin(), faces.end(), [](const Cell& x, const Cell& y) { return x.number < y.number; });
        faces.erase(std::unique(faces.begin(), faces.end(),
                                [](const Cell& x, const Cell& y) { return x.number == y.number; }),
                    faces.end());
        const int na = n[a];
        for (const Cell& f : faces) {
            blocked_[a].push_back(v.index(f.i, f.j, f.k));
            float given = 0.0f;
            if (moving) {
                const int at = a == 0 ? f.i : a == 1 ? f.j : f.k;
                // The solid cell beside the face: ahead of it, or behind.
                float owner =
                    at < na ? solid_.at(std::min(f.i, nx_ - 1), std::min(f.j, ny_ - 1), std::min(f.k, nz_ - 1)) : 0.0f;
                if (owner < 0.5f && at > 0) owner = solid_.at(f.i - (a == 0), f.j - (a == 1), f.k - (a == 2));
                if (owner >= 0.5f) {  // else the floor
                    const Collider& c = scene_.colliders[static_cast<size_t>(owner - 0.5f)];
                    const Vec3 p = worldAt(static_cast<float>(f.i) + (a == 0 ? 0.0f : 0.5f),
                                           static_cast<float>(f.j) + (a == 1 ? 0.0f : 0.5f),
                                           static_cast<float>(f.k) + (a == 2 ? 0.0f : 0.5f));
                    given = c.velocityAt(p)[a];
                }
            }
            blockedVel_[a].push_back(given);
        }
    }
    PoissonBoundary boundary;
    boundary.closed[2] = scene_.solver.closedFloor;
    boundary.solid = anySolid_ ? &solid_ : nullptr;
    poisson_.setBoundary(boundary);
    // No gas inside a solid.
    for (const size_t c : solidCells_) {
        density_.data()[c] = temperature_.data()[c] = fuel_.data()[c] = flame_.data()[c] = 0.0f;
    }
    enforceWalls();
    lap(t0, times_.solids);
}

void PyroSolver::enforceWalls() {
    if (scene_.solver.closedFloor) {
        SparseGrid& v = vel_[1];
        forEachIn(0, nx_, 0, 1, 0, nz_, [&](int i, int, int k) {
            if (v.has(i, 0, k)) v.ref(i, 0, k) = 0.0f;
        });
    }
    for (int a = 0; a < 3; ++a) {
        float* v = vel_[a].data();
        const std::vector<size_t>& faces = blocked_[a];
        const float* moving = blockedVel_[a].data();
        for (size_t b = 0; b < faces.size(); ++b) v[faces[b]] = moving[b];
    }
}

Vec3 PyroSolver::flowAt(const Vec3& p) const {
    const Vec3 o = domain_.origin();
    const float inv = 1.0f / domain_.voxel;
    const float x = (p.x - o.x) * inv, y = (p.y - o.y) * inv, z = (p.z - o.z) * inv;
    if (!(x >= 0.0f && y >= 0.0f && z >= 0.0f && x <= static_cast<float>(nx_) && y <= static_cast<float>(ny_) &&
          z <= static_cast<float>(nz_))) {
        return {};
    }
    float v[3];
    velocityAt(x, y, z, v);
    return {v[0], v[1], v[2]};
}

void PyroSolver::velocityAt(float x, float y, float z, float out[3]) const {
    out[0] = vel_[0].sample(x + 0.5f, y, z);
    out[1] = vel_[1].sample(x, y + 0.5f, z);
    out[2] = vel_[2].sample(x, y, z + 0.5f);
}

void PyroSolver::step() {
    const int n = scene_.solver.substeps;
    const float dt = scene_.solver.timeStep / static_cast<float>(n);
    for (int s = 0; s < n; ++s) {
        Clock::time_point t0 = Clock::now();
        // New tiles find their solids again: that time is the solids', not
        // the tiles'.
        const double solids = times_.solids;
        updateTiles(dt);
        lap(t0, times_.tiles);
        times_.tiles -= times_.solids - solids;
        emit(dt);
        lap(t0, times_.emit);
        advect(dt);
        lap(t0, times_.advect);
        quench(dt);
        combust(dt);
        lap(t0, times_.combust);
        addForces(dt);
        lap(t0, times_.forces);
        project();
        lap(t0, times_.project);
        dissipate(dt);
        lap(t0, times_.dissipate);
        time_ += dt;
    }
    ++frame_;
}

// --- emit ------------------------------------------------------------------------

namespace {

/// A source at a time: its shape there, and the noise its output flickers
/// with -- noise that rises with the gas.
struct Source {
    const Emitter& e;
    ShapeInstance shape;
    uint32_t seed;
    float rise;

    Source(const Emitter& emitter, const Scene& scene, float time)
        : e(emitter),
          shape(emitter.shapeAt(time)),
          seed(emitter.seed * 7919u + scene.solver.seed),
          rise(time * std::max(length(emitter.velocity), 0.2f)) {}

    /// How much of the source is at a world point: 1 inside, easing to 0 at
    /// its surface.
    float weight(const Vec3& p) const { return shape.falloff(p); }
    float flicker(const Vec3& p) const {
        if (e.flicker <= 0.0f) return 1.0f;
        const float s = 1.0f / e.flickerSize;
        const float noise = noise3(p.x * s, (p.y - rise) * s, p.z * s, seed);
        return std::max(0.0f, 1.0f + e.flicker * 1.5f * (2.0f * noise - 1.0f));
    }
    /// The cells of `domain` it can reach, and one more face along each
    /// axis: [lo, hi).
    void reach(const Domain& domain, int lo[3], int hi[3]) const {
        const float h = domain.voxel;
        const Vec3 origin = domain.origin();
        Vec3 reachLo, reachHi;
        shape.bounds(reachLo, reachHi);
        for (int a = 0; a < 3; ++a) {
            lo[a] = std::clamp(static_cast<int>(std::floor((reachLo[a] - origin[a]) / h)) - 1, 0, domain.cells[a]);
            hi[a] = std::clamp(static_cast<int>(std::ceil((reachHi[a] - origin[a]) / h)) + 1, 0, domain.cells[a]);
        }
    }
};

}  // namespace

void detail::emitScalars(const Scene& scene, float time, float dt, const Domain& domain, const SparseGrid* solid,
                         SparseGrid& fuel, SparseGrid& smoke, SparseGrid& heat, SparseGrid& expansion,
                         const std::vector<float>* soaked) {
    const float h = domain.voxel;
    const Vec3 o = domain.origin();
    for (size_t index = 0; index < scene.emitters.size(); ++index) {
        const Emitter& e = scene.emitters[index];
        if (!e.activeAt(time)) continue;
        const float give = soaked && index < soaked->size() ? std::exp(-(*soaked)[index]) : 1.0f;
        if (give <= 0.0f) continue;
        const Source source(e, scene, time);
        int lo[3], hi[3];
        source.reach(domain, lo, hi);
        forEachIn(lo[0], hi[0], lo[1], hi[1], lo[2], hi[2], [&](int i, int j, int k) {
            if (!smoke.has(i, j, k)) return;
            const size_t c = smoke.index(i, j, k);
            if (solid && solid->data()[c] > 0.5f) return;
            const Vec3 p(o.x + (static_cast<float>(i) + 0.5f) * h, o.y + (static_cast<float>(j) + 0.5f) * h,
                         o.z + (static_cast<float>(k) + 0.5f) * h);
            const float w = source.weight(p);
            if (w <= 0.0f) return;
            const float amount = dt * w * source.flicker(p) * give;
            fuel.data()[c] += e.fuel * amount;
            smoke.data()[c] += e.smoke * amount;
            heat.data()[c] += e.heat * amount;
            expansion.data()[c] += e.expansion * w * give;
        });
    }
}

void PyroSolver::emit(float dt) {
    // What swells this step: the sources ask for it here, the burning adds
    // its own (combust).
    expansion_.fill(0.0f);
    detail::emitScalars(scene_, time_, dt, domain_, anySolid_ ? &solid_ : nullptr, fuel_, density_, temperature_,
                        expansion_, &soaked_);
    for (const Emitter& e : scene_.emitters) {
        if (!e.activeAt(time_)) continue;
        const Source source(e, scene_, time_);
        int lo[3], hi[3];
        source.reach(domain_, lo, hi);

        // Push the gas the source's way -- along its own axes -- and across it
        // a little, so a plume does not stay a column. A moving source drags
        // the gas along.
        const Vec3 push = source.shape.turn().apply(e.velocity) + e.motionVelocityAt(time_) + e.moving;
        const float speed = length(push);
        if (speed <= 0.0f) continue;
        const Vec3 along = push * (1.0f / speed);
        for (int a = 0; a < 3; ++a) {
            SparseGrid& vel = vel_[a];
            const float across = 1.0f - std::fabs(along[a]);
            forEachIn(lo[0], std::min(hi[0] + (a == 0), vel.nx()), lo[1], std::min(hi[1] + (a == 1), vel.ny()), lo[2],
                      std::min(hi[2] + (a == 2), vel.nz()), [&](int i, int j, int k) {
                          if (!vel.has(i, j, k)) return;
                          const Vec3 p = worldAt(static_cast<float>(i) + faceOffset(a, 0),
                                                 static_cast<float>(j) + faceOffset(a, 1),
                                                 static_cast<float>(k) + faceOffset(a, 2));
                          const float w = source.weight(p);
                          if (w <= 0.0f) return;
                          const float m = source.flicker(p);
                          float& v = vel.ref(i, j, k);
                          const float target = push[a] * w * (0.6f + 0.4f * m);
                          if (push[a] > 0.0f) v = std::max(v, target);
                          else if (push[a] < 0.0f) v = std::min(v, target);
                          if (e.flicker > 0.0f && across > 0.0f) {
                              const float s = 6.0f / (e.flickerSize * 14.0f);  // broader than the flicker
                              const float wobble =
                                  noise3(p.x * s + 11.0f * static_cast<float>(a), (p.y - source.rise) * s, p.z * s,
                                         source.seed + 1u + static_cast<uint32_t>(a));
                              v += speed * 0.3f * e.flicker * across * w * (wobble - 0.5f);
                          }
                      });
        }
    }
    enforceWalls();
}

// --- advect ----------------------------------------------------------------------

void PyroSolver::advect(float dt) {
    const float cells = dt / domain_.voxel;  // velocity x dt, in cells
    // Where the gas of each cell was a step ago, and where it will be (RK2).
    forEachCounted(*cells_, [&](int i, int j, int k, size_t c) {
        const float x = static_cast<float>(i) + 0.5f, y = static_cast<float>(j) + 0.5f,
                    z = static_cast<float>(k) + 0.5f;
        const float u = 0.5f * (vel_[0].at(i, j, k) + vel_[0].at(i + 1, j, k));
        const float v = 0.5f * (vel_[1].at(i, j, k) + vel_[1].at(i, j + 1, k));
        const float w = 0.5f * (vel_[2].at(i, j, k) + vel_[2].at(i, j, k + 1));
        for (const float sign : {-1.0f, 1.0f}) {
            float mid[3];
            velocityAt(x + sign * 0.5f * cells * u, y + sign * 0.5f * cells * v, z + sign * 0.5f * cells * w, mid);
            SparseGrid* out = sign < 0.0f ? back_ : forward_;
            out[0].data()[c] = x + sign * cells * mid[0];
            out[1].data()[c] = y + sign * cells * mid[1];
            out[2].data()[c] = z + sign * cells * mid[2];
        }
    });

    // The velocity carries itself: plain semi-Lagrangian, stable at any step.
    for (int a = 0; a < 3; ++a) advectVelocity(a, cells);
    for (int a = 0; a < 3; ++a) std::swap(vel_[a], velNext_[a]);

    advectScalar(density_);
    advectScalar(temperature_);
    advectScalar(fuel_);
    advectScalar(flame_);
    for (const size_t c : solidCells_) {
        density_.data()[c] = temperature_.data()[c] = fuel_.data()[c] = flame_.data()[c] = 0.0f;
    }
    enforceWalls();
}

void PyroSolver::faceVelocity(int axis, int i, int j, int k, float out[3]) const {
    out[axis] = vel_[axis].at(i, j, k);
    const int n = axis == 0 ? nx_ : axis == 1 ? ny_ : nz_;
    // The cells on either side of the face, kept inside the grid.
    int below[3] = {i, j, k}, above[3] = {i, j, k};
    below[axis] = std::max(below[axis] - 1, 0);
    above[axis] = std::min(above[axis], n - 1);
    for (int b = 0; b < 3; ++b) {
        if (b == axis) continue;
        // Each cell's two faces along b.
        const SparseGrid& v = vel_[b];
        const int e[3] = {b == 0, b == 1, b == 2};
        out[b] = 0.25f * (v.at(below[0], below[1], below[2]) + v.at(below[0] + e[0], below[1] + e[1], below[2] + e[2]) +
                          v.at(above[0], above[1], above[2]) + v.at(above[0] + e[0], above[1] + e[1], above[2] + e[2]));
    }
}

void PyroSolver::advectVelocity(int axis, float cells) {
    const float ox = faceOffset(axis, 0), oy = faceOffset(axis, 1), oz = faceOffset(axis, 2);
    const float nx = static_cast<float>(nx_), ny = static_cast<float>(ny_), nz = static_cast<float>(nz_);
    const bool floor = scene_.solver.closedFloor;
    SparseGrid& out = velNext_[axis];
    forEachCounted(*faces_[axis], [&](int i, int j, int k, size_t c) {
        const float x = static_cast<float>(i) + ox, y = static_cast<float>(j) + oy, z = static_cast<float>(k) + oz;
        float v0[3], v1[3];
        faceVelocity(axis, i, j, k, v0);
        velocityAt(x - 0.5f * cells * v0[0], y - 0.5f * cells * v0[1], z - 0.5f * cells * v0[2], v1);
        const float px = x - cells * v1[0], py = y - cells * v1[1], pz = z - cells * v1[2];
        // Gas that comes in through an open side comes from the still air
        // outside. (Carrying in the velocity at the side instead, a flow that
        // sucks gas in -- the low pressure in a vortex -- would feed on itself.)
        if (px < 0.0f || px > nx || pz < 0.0f || pz > nz || py > ny || (py < 0.0f && !floor)) {
            out.data()[c] = 0.0f;
            return;
        }
        // This component where the gas came from. Grid::sample puts sample
        // (0, 0, 0) at 0.5: shift by what the faces lack of it.
        out.data()[c] = vel_[axis].sample(px + (0.5f - ox), py + (0.5f - oy), pz + (0.5f - oz));
    });
}

void PyroSolver::advectScalar(SparseGrid& field) {
    // MacCormack: advect, advect the result back, correct by half the error
    // that round trip shows, clamp to what the first step interpolated from.
    // (The pressure's right-hand side is free until project(): the lows.)
    SparseGrid& lows = divergence_;
    forEachCounted(*cells_, [&](int, int, int, size_t c) {
        float lo, hi;
        predicted_.data()[c] = field.sample(back_[0].data()[c], back_[1].data()[c], back_[2].data()[c], true, lo, hi);
        lows.data()[c] = lo;
        hi_.data()[c] = hi;
    });
    const float nx = static_cast<float>(nx_), ny = static_cast<float>(ny_), nz = static_cast<float>(nz_);
    forEachCounted(*cells_, [&](int, int, int, size_t c) {
        const float fx = forward_[0].data()[c], fy = forward_[1].data()[c], fz = forward_[2].data()[c];
        // Where the gas leaves the domain the round trip would come back
        // empty and the correction add what is not there: smoke would pile up
        // at the open boundaries. There, plain semi-Lagrangian.
        if (fx < 0.0f || fy < 0.0f || fz < 0.0f || fx > nx || fy > ny || fz > nz) {
            corrected_.data()[c] = predicted_.data()[c];
            return;
        }
        const float roundTrip = predicted_.sample(fx, fy, fz);
        const float v = predicted_.data()[c] + 0.5f * (field.data()[c] - roundTrip);
        corrected_.data()[c] = std::clamp(v, lows.data()[c], hi_.data()[c]);
    });
    std::swap(field, corrected_);
}

// --- combust -----------------------------------------------------------------------

void PyroSolver::combust(float dt) {
    const SolverSettings& s = scene_.solver;
    const float share = 1.0f - std::exp(-s.burnRate * dt);
    forEachCounted(*cells_, [&](int, int, int, size_t c) {
        const float burnt = fuel_.data()[c] * share;
        fuel_.data()[c] -= burnt;
        temperature_.data()[c] += burnt * s.heatRelease;
        density_.data()[c] += burnt * s.sootRelease;
        flame_.data()[c] += burnt;
        expansion_.data()[c] += burnt * s.expansion / dt;
    });
}

// --- water ---------------------------------------------------------------------------

namespace {

// How fast water puts out the gas of a cell, 1/s at Quench 1: a cell full
// of a Liquid Solver's water, all but a twentieth of its heat in a frame.
constexpr float kWaterRate = 90.0f;
// A Rain's drop cools a column of the air it falls through as wide as
// this, m^2 -- the spray it breaks into, the air it drags down -- as fast
// as kDropRate would a cell it filled. Over a while each cell then gets as
// much of it as rain falls, whatever the cells' size: a downpour of 2500
// drops a second on a square metre cools the gas some 3 times a second.
constexpr float kDropReach = 1e-4f;
constexpr float kDropRate = 12.0f;
// How fast the water a source gets soaks it, for each 1/s of that: under
// water a fire is out in a few frames; in a downpour, in a few seconds.
constexpr float kSoak = 0.4f;
// The heat the water takes: what is left of it in the steam, which rises a
// little.
constexpr float kSteamWarmth = 0.1f;

}  // namespace

void PyroSolver::setWater(const Water& water) {
    wet_.clear();
    if (water.empty()) return;
    const float h = domain_.voxel;
    const Vec3 o = domain_.origin();
    const int n[3] = {nx_, ny_, nz_};
    // Each cell's rate, summed in the order the water is given: the same
    // bits whatever the map does.
    std::unordered_map<uint64_t, float> rate;
    auto add = [&](const Vec3& p, float r) {
        const int i = static_cast<int>(std::floor((p.x - o.x) / h));
        const int j = static_cast<int>(std::floor((p.y - o.y) / h));
        const int k = static_cast<int>(std::floor((p.z - o.z) / h));
        if (i < 0 || j < 0 || k < 0 || i >= nx_ || j >= ny_ || k >= nz_) return;
        rate[numberOf(i, j, k, n)] += r;
    };
    const float particle = kWaterRate * water.particleVolume / (h * h * h);
    for (const Vec3& p : water.particles) add(p, particle);
    // A drop along its way, at every half cell of it: the air it goes
    // through this step, over the step's time.
    const float perCell = kDropRate * kDropReach / (h * h * h * scene_.solver.timeStep);
    const size_t drops = std::min(water.dropFrom.size(), water.dropTo.size());
    for (size_t d = 0; d < drops; ++d) {
        const Vec3 a = water.dropFrom[d], b = water.dropTo[d];
        const float way = length(b - a);
        const int samples = std::clamp(static_cast<int>(std::ceil(way / (0.5f * h))), 1, 4096);
        const float each = way / static_cast<float>(samples);
        for (int s = 0; s < samples; ++s) {
            const float t = (static_cast<float>(s) + 0.5f) / static_cast<float>(samples);
            add(a + (b - a) * t, perCell * std::max(each, 0.5f * h));
        }
    }
    wet_.assign(rate.begin(), rate.end());
    std::sort(wet_.begin(), wet_.end());
}

void PyroSolver::quench(float dt) {
    const SolverSettings& s = scene_.solver;
    if (wet_.empty() || s.quench <= 0.0f) return;
    const int64_t nx = nx_, nxy = static_cast<int64_t>(nx_) * ny_;
    auto cellOf = [&](uint64_t number, int& i, int& j, int& k) {
        k = static_cast<int>(static_cast<int64_t>(number) / nxy);
        const int64_t rest = static_cast<int64_t>(number) - static_cast<int64_t>(k) * nxy;
        j = static_cast<int>(rest / nx);
        i = static_cast<int>(rest - static_cast<int64_t>(j) * nx);
    };
    // The gas where the water is: each cell its own, nothing summed across.
    pg::parallelFor(wet_.size(), 4096, [&](size_t begin, size_t end) {
        for (size_t w = begin; w < end; ++w) {
            int i, j, k;
            cellOf(wet_[w].first, i, j, k);
            if (!density_.has(i, j, k)) continue;
            const size_t c = density_.index(i, j, k);
            const float share = 1.0f - std::exp(-s.quench * wet_[w].second * dt);
            const float taken = temperature_.data()[c] * share;
            temperature_.data()[c] -= taken * (1.0f - kSteamWarmth);
            density_.data()[c] += taken * s.steam;
            fuel_.data()[c] -= fuel_.data()[c] * share;
            flame_.data()[c] -= flame_.data()[c] * share;
        }
    });
    // The sources of fire it falls on soak: by how much water the cells of
    // each get, on the average over the source.
    soaked_.resize(scene_.emitters.size(), 0.0f);
    const float h = domain_.voxel;
    const Vec3 o = domain_.origin();
    for (size_t e = 0; e < scene_.emitters.size(); ++e) {
        const Emitter& emitter = scene_.emitters[e];
        if (!emitter.activeAt(time_) || (emitter.fuel <= 0.0f && emitter.heat <= 0.0f)) continue;
        const Source source(emitter, scene_, time_);
        int lo[3], hi[3];
        source.reach(domain_, lo, hi);
        auto centre = [&](int i, int j, int k) {
            return Vec3(o.x + (static_cast<float>(i) + 0.5f) * h, o.y + (static_cast<float>(j) + 0.5f) * h,
                        o.z + (static_cast<float>(k) + 0.5f) * h);
        };
        double weight = 0.0;
        for (int k = lo[2]; k < hi[2]; ++k) {
            for (int j = lo[1]; j < hi[1]; ++j) {
                for (int i = lo[0]; i < hi[0]; ++i) weight += source.weight(centre(i, j, k));
            }
        }
        if (weight <= 0.0) continue;
        double water = 0.0;
        for (const auto& [number, r] : wet_) {
            int i, j, k;
            cellOf(number, i, j, k);
            if (i < lo[0] || j < lo[1] || k < lo[2] || i >= hi[0] || j >= hi[1] || k >= hi[2]) continue;
            water += static_cast<double>(r) * source.weight(centre(i, j, k));
        }
        soaked_[e] += dt * s.quench * kSoak * static_cast<float>(water / weight);
    }
}

// --- forces --------------------------------------------------------------------------

void PyroSolver::addForces(float dt) {
    const SolverSettings& s = scene_.solver;
    // Buoyancy on the vertical faces, from the cells below and above them.
    SparseGrid& vy = vel_[1];
    forEachCounted(*faces_[1], [&](int i, int j, int k, size_t c) {
        float heat = 0.0f, smoke = 0.0f, n = 0.0f;
        if (j > 0) {
            heat += temperature_.at(i, j - 1, k);
            smoke += density_.at(i, j - 1, k);
            n += 1.0f;
        }
        if (j < ny_) {
            heat += temperature_.at(i, j, k);
            smoke += density_.at(i, j, k);
            n += 1.0f;
        }
        vy.data()[c] += dt * (s.buoyancy * heat - s.weight * smoke) / n;
    });
    if (s.vorticity > 0.0f) addVorticity(dt);
    for (size_t f = 0; f < scene_.forces.size(); ++f) addForce(scene_.forces[f], f, dt);
    enforceWalls();
}

void PyroSolver::addVorticity(float dt) {
    // Vorticity confinement: find the swirls, push along them. Worked out at
    // the cell centres, then spread to the faces -- in advect's scratch.
    const float h = domain_.voxel;
    SparseGrid* centre = back_;
    SparseGrid* curl = forward_;
    SparseGrid& curlLength = predicted_;
    forEachCounted(*cells_, [&](int i, int j, int k, size_t c) {
        centre[0].data()[c] = 0.5f * (vel_[0].at(i, j, k) + vel_[0].at(i + 1, j, k));
        centre[1].data()[c] = 0.5f * (vel_[1].at(i, j, k) + vel_[1].at(i, j + 1, k));
        centre[2].data()[c] = 0.5f * (vel_[2].at(i, j, k) + vel_[2].at(i, j, k + 1));
    });
    // Neighbours along each axis, kept inside the grid; the differences are
    // over the distance between them.
    struct Around {
        int minus[3][3], plus[3][3];
        float scale[3];
    };
    auto around = [&](int i, int j, int k) {
        const int im = std::max(i - 1, 0), ip = std::min(i + 1, nx_ - 1);
        const int jm = std::max(j - 1, 0), jp = std::min(j + 1, ny_ - 1);
        const int km = std::max(k - 1, 0), kp = std::min(k + 1, nz_ - 1);
        return Around{{{im, j, k}, {i, jm, k}, {i, j, km}},
                      {{ip, j, k}, {i, jp, k}, {i, j, kp}},
                      {1.0f / (static_cast<float>(ip - im) * h), 1.0f / (static_cast<float>(jp - jm) * h),
                       1.0f / (static_cast<float>(kp - km) * h)}};
    };
    auto value = [](const SparseGrid& g, const int at[3]) { return g.at(at[0], at[1], at[2]); };
    forEachCounted(*cells_, [&](int i, int j, int k, size_t c) {
        const Around n = around(i, j, k);
        // d(component a)/d(axis b)
        auto d = [&](int a, int b) {
            return (value(centre[a], n.plus[b]) - value(centre[a], n.minus[b])) * n.scale[b];
        };
        const float wx = d(2, 1) - d(1, 2);
        const float wy = d(0, 2) - d(2, 0);
        const float wz = d(1, 0) - d(0, 1);
        curl[0].data()[c] = wx;
        curl[1].data()[c] = wy;
        curl[2].data()[c] = wz;
        curlLength.data()[c] = std::sqrt(wx * wx + wy * wy + wz * wz);
    });
    // The force, at the centres: `centre` is free again.
    const float strength = scene_.solver.vorticity * h;
    forEachCounted(*cells_, [&](int i, int j, int k, size_t c) {
        const Around n = around(i, j, k);
        float g[3];
        for (int b = 0; b < 3; ++b) {
            g[b] = (value(curlLength, n.plus[b]) - value(curlLength, n.minus[b])) * n.scale[b];
        }
        const float len = std::sqrt(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]) + 1e-6f;
        const float gx = g[0] / len, gy = g[1] / len, gz = g[2] / len;
        const float wx = curl[0].data()[c], wy = curl[1].data()[c], wz = curl[2].data()[c];
        centre[0].data()[c] = strength * (gy * wz - gz * wy);
        centre[1].data()[c] = strength * (gz * wx - gx * wz);
        centre[2].data()[c] = strength * (gx * wy - gy * wx);
    });
    for (int a = 0; a < 3; ++a) {
        const int n = a == 0 ? nx_ : a == 1 ? ny_ : nz_;
        SparseGrid& vel = vel_[a];
        forEachCounted(*faces_[a], [&](int i, int j, int k, size_t c) {
            const int f = a == 0 ? i : a == 1 ? j : k;  // face f is between cells f-1 and f
            float force = 0.0f, count = 0.0f;
            if (f > 0) {
                force += centre[a].at(i - (a == 0), j - (a == 1), k - (a == 2));
                count += 1.0f;
            }
            if (f < n) {
                force += centre[a].at(i, j, k);
                count += 1.0f;
            }
            vel.data()[c] += dt * force / count;
        });
    }
}

float PyroSolver::maskAt(Mask mask, int axis, int i, int j, int k) const {
    if (mask == Mask::Everywhere) return 1.0f;
    const int f = axis == 0 ? i : axis == 1 ? j : k;
    const int n = axis == 0 ? nx_ : axis == 1 ? ny_ : nz_;
    auto amount = [&](int x, int y, int z) {
        return mask == Mask::Heat ? temperature_.at(x, y, z) + fuel_.at(x, y, z) : density_.at(x, y, z);
    };
    float m = 0.0f;
    if (f > 0) m += amount(i - (axis == 0), j - (axis == 1), k - (axis == 2));
    if (f < n) m += amount(i, j, k);
    return std::min(m, 1.0f);
}

void PyroSolver::addForce(const Force& force, size_t index, float dt) {
    const uint32_t seed = force.seed * 7919u + scene_.solver.seed * 31u;
    detail::addForce(force, seed, domain_, time_, dt, vel_, noise_[index],
                     [&](int a, int i, int j, int k) { return maskAt(force.mask, a, i, j, k); });
}

// --- project -----------------------------------------------------------------------

float PyroSolver::divergence(int i, int j, int k) const {
    return (vel_[0].at(i + 1, j, k) - vel_[0].at(i, j, k) + vel_[1].at(i, j + 1, k) - vel_[1].at(i, j, k) +
            vel_[2].at(i, j, k + 1) - vel_[2].at(i, j, k)) /
           domain_.voxel;
}

void PyroSolver::project() {
    const float h = domain_.voxel;
    enforceWalls();  // what flows through walls is 0 before anything is measured
    forEachCounted(*cells_, [&](int i, int j, int k, size_t c) {
        divergence_.data()[c] =
            anySolid_ && solid_.data()[c] > 0.5f ? 0.0f : divergence(i, j, k) - expansion_.data()[c];
    });
    // The pressure of the previous step is the first guess.
    poisson_.solve(pressure_, divergence_, h, scene_.solver.pressureCycles);
    // Subtract its gradient. The open sides hold p = 0 on the face -- a ghost
    // cell outside holds minus the cell inside; past the tiles worked on, the
    // still air holds p = 0. What this does to the faces of walls and solids
    // does not count: they go back to 0 right after.
    for (int a = 0; a < 3; ++a) {
        const int n = a == 0 ? nx_ : a == 1 ? ny_ : nz_;
        SparseGrid& vel = vel_[a];
        forEachCounted(*faces_[a], [&](int i, int j, int k, size_t c) {
            const int f = a == 0 ? i : a == 1 ? j : k;  // face f is between cells f-1 and f
            const int bi = i - (a == 0), bj = j - (a == 1), bk = k - (a == 2);
            const float ahead = f < n ? pressure_.at(i, j, k) : -pressure_.at(bi, bj, bk);
            const float behind = f > 0 ? pressure_.at(bi, bj, bk) : -pressure_.at(i, j, k);
            vel.data()[c] -= (ahead - behind) / h;
        });
    }
    enforceWalls();
}

// --- dissipate ---------------------------------------------------------------------

void PyroSolver::dissipate(float dt) {
    const SolverSettings& s = scene_.solver;
    const float smoke = std::exp(-s.smokeDecay * dt), heat = std::exp(-s.cooling * dt);
    const float flame = s.flameLife > 0.0f ? std::exp(-dt / s.flameLife) : 0.0f;
    forEachCounted(*cells_, [&](int, int, int, size_t c) {
        // Where the burning gas swells -- by expansion x dt of its volume this
        // step -- what it carries spreads over the more room. Advection alone
        // keeps a value as it moves: right where the gas neither swells nor
        // shrinks, wrong here. Swollen fuel that stayed as rich would burn
        // and swell again, and an explosion would feed on itself until it
        // filled the domain.
        const float thinner = 1.0f / (1.0f + expansion_.data()[c] * dt);
        density_.data()[c] = std::max(0.0f, density_.data()[c]) * smoke * thinner;
        temperature_.data()[c] = std::max(0.0f, temperature_.data()[c]) * heat;
        fuel_.data()[c] = std::max(0.0f, fuel_.data()[c]) * thinner;
        flame_.data()[c] = std::max(0.0f, flame_.data()[c]) * flame * thinner;
    });
}

double PyroSolver::meanDivergence() const {
    double sum = 0.0;
    size_t cells = 0;
    for (int k = 0; k < nz_; ++k) {
        for (int j = 0; j < ny_; ++j) {
            for (int i = 0; i < nx_; ++i) {
                if (!density_.has(i, j, k)) continue;
                if (anySolid_ && solid_.at(i, j, k) > 0.5f) continue;
                sum += std::fabs(divergence(i, j, k) - expansion_.at(i, j, k));
                ++cells;
            }
        }
    }
    return cells ? sum / static_cast<double>(cells) : 0.0;
}

Grid lightTransmittance(const SparseGrid& density, const float towardsLight[3], float extinctionPerCell, int divisor) {
    divisor = std::max(1, divisor);
    const int lx = std::max(1, density.nx() / divisor), ly = std::max(1, density.ny() / divisor),
              lz = std::max(1, density.nz() / divisor);
    Grid out(lx, ly, lz, 1.0f);
    const float len = std::sqrt(towardsLight[0] * towardsLight[0] + towardsLight[1] * towardsLight[1] +
                                towardsLight[2] * towardsLight[2]);
    if (len <= 0.0f) return out;
    const float d[3] = {towardsLight[0] / len, towardsLight[1] / len, towardsLight[2] / len};
    const float sx = static_cast<float>(density.nx()) / static_cast<float>(lx);
    const float sy = static_cast<float>(density.ny()) / static_cast<float>(ly);
    const float sz = static_cast<float>(density.nz()) / static_cast<float>(lz);
    const float step = static_cast<float>(divisor);  // in density cells
    const float nx = static_cast<float>(density.nx()), ny = static_cast<float>(density.ny()),
                nz = static_cast<float>(density.nz());
    detail::forEachCell(out, [&](int i, int j, int k) {
        // Start half a step towards the light: a cell does not shade itself.
        float x = (static_cast<float>(i) + 0.5f) * sx + d[0] * 0.5f * step;
        float y = (static_cast<float>(j) + 0.5f) * sy + d[1] * 0.5f * step;
        float z = (static_cast<float>(k) + 0.5f) * sz + d[2] * 0.5f * step;
        float depth = 0.0f;
        while (x >= 0.0f && y >= 0.0f && z >= 0.0f && x <= nx && y <= ny && z <= nz && depth < 16.0f) {
            depth += density.sample(x, y, z) * extinctionPerCell * step;
            x += d[0] * step;
            y += d[1] * step;
            z += d[2] * step;
        }
        out.at(i, j, k) = std::exp(-depth);
    });
    return out;
}

}  // namespace pg::sim
