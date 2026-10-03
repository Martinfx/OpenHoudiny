#include "pg/sim/Upres.h"

#include "pg/core/Parallel.h"
#include "pg/sim/Pyro.h"
#include "pg/sim/Shared.h"
#include "pg/sim/State.h"

#include <algorithm>
#include <chrono>
#include <cmath>
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

constexpr float kPi = 3.14159265358979f;

/// The fine grid's longest side at most: its table of tiles stays some
/// megabytes.
constexpr int kMostCells = 2048;

/// The noise the whirls are read from: curl noise worked out once, at
/// kSamples points a lattice unit, repeating every kPeriod units -- a tile
/// of kTile samples a side read trilinearly, some ten times quicker than
/// working the noise out at each cell (as wavelet turbulence keeps its
/// noise, Kim et al. 2008).
constexpr int kPeriod = 16, kSamples = 4, kTile = kPeriod * kSamples;

/// The gradients of improved Perlin noise: the twelve edges of a cube.
constexpr float kGradients[12][3] = {{1, 1, 0},  {-1, 1, 0},  {1, -1, 0}, {-1, -1, 0}, {1, 0, 1},  {-1, 0, 1},
                                     {1, 0, -1}, {-1, 0, -1}, {0, 1, 1},  {0, -1, 1},  {0, 1, -1}, {0, -1, -1}};

/// Curl noise at (x, y, z), in units of its lattice, repeating every
/// kPeriod units: the curl of three gradient noises (quintic, with their
/// derivatives worked out exactly) -- a flow that neither squeezes nor
/// spreads, whirls about a lattice unit across, each component about 1 RMS.
void curlNoise(float x, float y, float z, uint32_t seed, float out[3]) {
    const float fx = std::floor(x), fy = std::floor(y), fz = std::floor(z);
    const int ix = static_cast<int>(fx), iy = static_cast<int>(fy), iz = static_cast<int>(fz);
    const float w[3] = {x - fx, y - fy, z - fz};
    float u[3], du[3];
    for (int a = 0; a < 3; ++a) {
        u[a] = w[a] * w[a] * w[a] * (w[a] * (w[a] * 6.0f - 15.0f) + 10.0f);
        du[a] = 30.0f * w[a] * w[a] * (w[a] * (w[a] - 2.0f) + 1.0f);
    }
    float grad[3][3] = {};  // of each of the three noises, along each axis
    constexpr int kWrap = kPeriod - 1;
    for (int c = 0; c < 8; ++c) {
        const int d[3] = {c & 1, (c >> 1) & 1, c >> 2};
        float wt[3], dwt[3];
        for (int a = 0; a < 3; ++a) {
            wt[a] = d[a] ? u[a] : 1.0f - u[a];
            dwt[a] = d[a] ? du[a] : -du[a];
        }
        const float weight = wt[0] * wt[1] * wt[2];
        const float dw[3] = {dwt[0] * wt[1] * wt[2], wt[0] * dwt[1] * wt[2], wt[0] * wt[1] * dwt[2]};
        const float r[3] = {w[0] - static_cast<float>(d[0]), w[1] - static_cast<float>(d[1]),
                            w[2] - static_cast<float>(d[2])};
        for (int q = 0; q < 3; ++q) {
            const uint32_t h = detail::hash((ix + d[0]) & kWrap, (iy + d[1]) & kWrap, (iz + d[2]) & kWrap,
                                            seed + 977u * static_cast<uint32_t>(q));
            const float* g = kGradients[h % 12u];
            const float v = g[0] * r[0] + g[1] * r[1] + g[2] * r[2];
            for (int a = 0; a < 3; ++a) grad[q][a] += weight * g[a] + dw[a] * v;
        }
    }
    out[0] = grad[2][1] - grad[1][2];
    out[1] = grad[0][2] - grad[2][0];
    out[2] = grad[1][0] - grad[0][1];
}

/// The tile of curl noise for `seed`: three floats a sample, x fastest.
std::vector<float> noiseTile(uint32_t seed) {
    std::vector<float> tile(static_cast<size_t>(kTile) * kTile * kTile * 3);
    const float step = 1.0f / static_cast<float>(kSamples);
    pg::parallelFor(static_cast<size_t>(kTile) * kTile, 16, [&](size_t begin, size_t end) {
        for (size_t row = begin; row < end; ++row) {
            const int j = static_cast<int>(row % kTile), k = static_cast<int>(row / kTile);
            for (int i = 0; i < kTile; ++i) {
                curlNoise(static_cast<float>(i) * step, static_cast<float>(j) * step, static_cast<float>(k) * step,
                          seed, &tile[(row * kTile + static_cast<size_t>(i)) * 3]);
            }
        }
    });
    return tile;
}

/// The tile's noise at (x, y, z), lattice units: trilinear between its
/// samples, round and round.
[[gnu::always_inline]] inline void tileNoise(const float* tile, float x, float y, float z, float out[3]) {
    constexpr int kMask = kTile - 1;
    const float sx = x * kSamples, sy = y * kSamples, sz = z * kSamples;
    const float fx = std::floor(sx), fy = std::floor(sy), fz = std::floor(sz);
    const float tx = sx - fx, ty = sy - fy, tz = sz - fz;
    const int ix = static_cast<int>(fx) & kMask, iy = static_cast<int>(fy) & kMask, iz = static_cast<int>(fz) & kMask;
    const int jx = (ix + 1) & kMask, jy = (iy + 1) & kMask, jz = (iz + 1) & kMask;
    auto at = [&](int i, int j, int k) {
        return tile + ((static_cast<size_t>(k) * kTile + static_cast<size_t>(j)) * kTile + static_cast<size_t>(i)) * 3;
    };
    const float* c[8] = {at(ix, iy, iz), at(jx, iy, iz), at(ix, jy, iz), at(jx, jy, iz),
                         at(ix, iy, jz), at(jx, iy, jz), at(ix, jy, jz), at(jx, jy, jz)};
    for (int a = 0; a < 3; ++a) {
        const float x00 = c[0][a] + (c[1][a] - c[0][a]) * tx, x10 = c[2][a] + (c[3][a] - c[2][a]) * tx;
        const float x01 = c[4][a] + (c[5][a] - c[4][a]) * tx, x11 = c[6][a] + (c[7][a] - c[6][a]) * tx;
        const float y0 = x00 + (x10 - x00) * ty, y1 = x01 + (x11 - x01) * ty;
        out[a] = y0 + (y1 - y0) * tz;
    }
}

/// Where in the noise layer `layer`'s pattern is after it has started
/// afresh `cycle` times: anywhere in the tile, far from the last.
Vec3 offsetOf(uint32_t seed, int layer, int cycle) {
    const uint32_t s = seed * 7919u + 104729u * static_cast<uint32_t>(layer + 1);
    return Vec3(detail::lattice(cycle, 0, 0, s), detail::lattice(cycle, 1, 0, s), detail::lattice(cycle, 2, 0, s)) *
           static_cast<float>(kPeriod);
}

/// Where each octave reads the tile from: away from the others, so that the
/// tile's repeats do not line up.
constexpr float kOctaveShift[4][3] = {{0.0f, 0.0f, 0.0f}, {5.3f, 11.7f, 2.9f}, {9.1f, 3.4f, 13.6f}, {1.8f, 7.5f, 6.2f}};

}  // namespace

// --- settings ------------------------------------------------------------------------

UpresSettings UpresSettings::sanitized() const {
    UpresSettings s = *this;
    s.scale = std::clamp(s.scale, 2, 4);
    s.turbulence = detail::fix(s.turbulence, 0.0f, 100.0f, 1.0f);
    s.swirlSize = detail::fix(s.swirlSize, 0.25f, 64.0f, 2.0f);
    s.swirlLife = detail::fix(s.swirlLife, 0.01f, 100.0f, 0.5f);
    return s;
}

Domain UpresSettings::domain(const Domain& coarse) const {
    const int longest = std::max({coarse.cells[0], coarse.cells[1], coarse.cells[2], 1});
    const int k = std::clamp(sanitized().scale, 2, std::max(2, kMostCells / longest));
    Domain d = coarse;
    for (int a = 0; a < 3; ++a) d.cells[a] = coarse.cells[a] * k;
    d.voxel = coarse.voxel / static_cast<float>(k);
    return d;
}

// --- set-up ----------------------------------------------------------------------------

UpresSolver::UpresSolver(const UpresSettings& settings) : settings_(settings.sanitized()) {}

void UpresSolver::reset(const Domain& coarse) {
    coarse_ = coarse;
    domain_ = settings_.domain(coarse);
    for (int a = 0; a < 3; ++a) n_[a] = domain_.cells[a];
    scale_ = domain_.cells[0] / std::max(coarse.cells[0], 1);
    for (SparseGrid* g : {&density_, &temperature_, &fuel_, &flame_, &solid_}) *g = SparseGrid();
    // Nothing is worked on until there is gas or a source.
    cells_.reset();
    retile(std::make_shared<const Tiles>(n_[0], n_[1], n_[2], std::vector<uint8_t>(), -1));
    coarseTiles_.reset();
    colliders_.clear();
    solidsFound_ = false;
    anySolid_ = false;
    if (noise_.empty()) noise_ = noiseTile(settings_.seed * 31u + 7u);
    for (int l = 0; l < 2; ++l) {
        cycle_[l] = 0;
        offset_[l] = offsetOf(settings_.seed, l, 0);
    }
    // Octaves of whirls from Swirl Size down to some three fine cells
    // across, each a third of an octave weaker in speed, as in a turbulent
    // flow (Kolmogorov: speed as the cube root of size); all together as
    // strong as one.
    const float finest = 2.5f / static_cast<float>(scale_);  // solver cells
    octaves_ = 1;
    while (octaves_ < 4 && settings_.swirlSize / static_cast<float>(1 << octaves_) >= finest) ++octaves_;
    float sum = 0.0f;
    for (int o = 0; o < 4; ++o) {
        octaveWeight_[o] = o < octaves_ ? std::pow(2.0f, -static_cast<float>(o) / 3.0f) : 0.0f;
        sum += octaveWeight_[o] * octaveWeight_[o];
    }
    for (float& w : octaveWeight_) w /= std::sqrt(sum);
    frame_ = 0;
    time_ = 0.0f;
    times_ = Times{};
}

void UpresSolver::retile(std::shared_ptr<const Tiles> cells) {
    // The tiles that are new: their solids are still to be found.
    fresh_.clear();
    for (size_t s = 0; s < cells->stored().size(); ++s) {
        const uint32_t t = cells->stored()[s];
        if (!cells_ || t >= cells_->tileCount() || cells_->slot(t) < 0) fresh_.push_back(static_cast<uint32_t>(s));
    }
    cells_ = std::move(cells);
    // What the gas carries goes on where the tiles do, and the solids found.
    for (SparseGrid* g : {&density_, &temperature_, &fuel_, &flame_, &solid_}) g->retile(cells_);
    // The rest is made again each step: scratch.
    expansion_.reshape(cells_);
    for (int f = 0; f < 4; ++f) {
        predicted_[f].reshape(cells_);
        lows_[f].reshape(cells_);
        highs_[f].reshape(cells_);
    }
    for (SparseGrid& g : forward_) g.reshape(cells_);
}

// --- a frame -------------------------------------------------------------------------

void UpresSolver::step(const PyroSolver& gas) {
    if (!cells_ || !(gas.domain() == coarse_)) reset(gas.domain());
    const Scene& scene = gas.scene();
    const int steps = scene.solver.substeps;
    const float dt = scene.solver.timeStep / static_cast<float>(steps);
    // The solver's time, as it starts its step: this one keeps up with it.
    time_ = gas.time();
    Clock::time_point t0 = Clock::now();
    updateFlow(gas);
    lap(t0, times_.swirl);
    for (int s = 0; s < steps; ++s) {
        updateTiles(gas, dt);
        lap(t0, times_.tiles);
        // The solids, where they are now: found again in every tile when
        // they have moved, in the new tiles alone when only the tiles have.
        if (!solidsFound_ || scene.colliders != colliders_) {
            updateSolids(scene.colliders, true);
        } else if (!fresh_.empty()) {
            updateSolids(scene.colliders, false);
        }
        fresh_.clear();
        lap(t0, times_.solids);
        // The sources, at the fine resolution.
        expansion_.fill(0.0f);
        detail::emitScalars(scene, time_, dt, domain_, anySolid_ ? &solid_ : nullptr, fuel_, density_, temperature_,
                            expansion_);
        lap(t0, times_.emit);
        renewLayers();
        advectShift(dt);
        lap(t0, times_.swirl);
        advect(scene.solver.closedFloor, dt);
        lap(t0, times_.advect);
        combust(scene.solver, dt);
        lap(t0, times_.combust);
        time_ += dt;
    }
    ++frame_;
}

void UpresSolver::updateTiles(const PyroSolver& gas, float dt) {
    const Tiles& now = *cells_;
    const int tn[3] = {now.tilesX(), now.tilesY(), now.tilesZ()};
    auto number = [&](int a, int b, int c) {
        return static_cast<size_t>(a) +
               static_cast<size_t>(tn[0]) * (static_cast<size_t>(b) + static_cast<size_t>(tn[1]) * static_cast<size_t>(c));
    };
    std::vector<uint8_t> busy(now.tileCount(), 0);
    // Tiles with gas in them.
    const float cutoff = gas.scene().solver.cutoff;
    const std::vector<uint32_t>& stored = now.stored();
    pg::parallelFor(stored.size(), 16, [&](size_t begin, size_t end) {
        for (size_t s = begin; s < end; ++s) {
            const size_t base = s * Tiles::kCells;
            bool any = false;
            for (const SparseGrid* g : {&density_, &temperature_, &fuel_, &flame_}) {
                const float* v = g->data() + base;
                for (int c = 0; c < Tiles::kCells && !any; ++c) any = v[c] > cutoff;
                if (any) break;
            }
            if (any) busy[stored[s]] = 1;
        }
    });
    // The boxes of the sources: the gas starts there.
    const float h = domain_.voxel;
    const Vec3 origin = domain_.origin();
    for (const Emitter& e : gas.scene().emitters) {
        if (!e.activeAt(time_)) continue;
        Vec3 lo, hi;
        e.shapeAt(time_).bounds(lo, hi);
        int from[3], to[3];
        bool inside = true;
        for (int a = 0; a < 3; ++a) {
            if (!(hi[a] >= lo[a])) inside = false;
            from[a] = std::clamp(static_cast<int>(std::floor((lo[a] - origin[a]) / h)) - 1, 0, n_[a] - 1) / Tiles::kSide;
            to[a] = std::clamp(static_cast<int>(std::ceil((hi[a] - origin[a]) / h)) + 1, 0, n_[a] - 1) / Tiles::kSide;
        }
        if (!inside) continue;
        for (int c = from[2]; c <= to[2]; ++c) {
            for (int b = from[1]; b <= to[1]; ++b) {
                for (int a = from[0]; a <= to[0]; ++a) busy[number(a, b, c)] = 1;
            }
        }
    }
    // Round each, as far as the gas there goes in a step and a cell more:
    // the solver's flow at the tile and a cell round it, and the whirls on
    // top of it -- at their strongest some three times as fast as their
    // strength. A tile at least, as many as the solver's four hold at most.
    std::vector<size_t> list;
    for (size_t t = 0; t < busy.size(); ++t) {
        if (busy[t]) list.push_back(t);
    }
    std::vector<uint8_t> pads(list.size(), 1);
    const int cn[3] = {coarse_.cells[0], coarse_.cells[1], coarse_.cells[2]};
    const int most = 4 * scale_;
    pg::parallelFor(list.size(), 8, [&](size_t begin, size_t end) {
        for (size_t q = begin; q < end; ++q) {
            const size_t t = list[q];
            const int c0[3] = {static_cast<int>(t % static_cast<size_t>(tn[0])) * Tiles::kSide,
                               static_cast<int>((t / static_cast<size_t>(tn[0])) % static_cast<size_t>(tn[1])) * Tiles::kSide,
                               static_cast<int>(t / (static_cast<size_t>(tn[0]) * static_cast<size_t>(tn[1]))) * Tiles::kSide};
            int lo[3], hi[3];
            for (int a = 0; a < 3; ++a) {
                lo[a] = std::max(c0[a] / scale_ - 1, 0);
                hi[a] = std::min((c0[a] + Tiles::kSide - 1) / scale_ + 1, cn[a] - 1);
            }
            float speed = 0.0f;
            for (int k = lo[2]; k <= hi[2]; ++k) {
                for (int j = lo[1]; j <= hi[1]; ++j) {
                    for (int i = lo[0]; i <= hi[0]; ++i) {
                        if (!strength_.stored(i, j, k)) continue;
                        const size_t c = strength_.index(i, j, k);
                        const float u = centre_[0].data()[c], v = centre_[1].data()[c], w = centre_[2].data()[c];
                        speed = std::max(speed, std::sqrt(u * u + v * v + w * w) + 3.0f * strength_.data()[c]);
                    }
                }
            }
            const float reach = speed * dt / h + 1.0f;
            pads[q] = static_cast<uint8_t>(
                std::clamp(static_cast<int>(std::ceil(reach / static_cast<float>(Tiles::kSide))), 1, most));
        }
    });
    std::vector<uint8_t> state(now.tileCount(), Tiles::Off);
    for (size_t q = 0; q < list.size(); ++q) {
        const size_t t = list[q];
        const int pad = pads[q];
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
    retile(std::make_shared<const Tiles>(n_[0], n_[1], n_[2], std::move(state), -1));
}

void UpresSolver::updateSolids(const std::vector<Collider>& colliders, bool all) {
    if (all) {
        solid_.fill(0.0f);
        anySolid_ = false;
    }
    colliders_ = colliders;
    solidsFound_ = true;
    if (colliders.empty()) return;
    const Tiles& tiles = *cells_;
    const std::vector<uint32_t>& stored = tiles.stored();
    // The tiles to look in, by slot: every one, or the new ones.
    std::vector<uint32_t> slots;
    if (all) {
        slots.resize(stored.size());
        for (size_t s = 0; s < stored.size(); ++s) slots[s] = static_cast<uint32_t>(s);
    } else {
        slots = fresh_;
    }
    if (slots.empty()) return;
    std::vector<int32_t> look(stored.size(), -1);  // where a slot is in `slots`
    for (size_t i = 0; i < slots.size(); ++i) look[slots[i]] = static_cast<int32_t>(i);
    // Which colliders' boxes each of them meets, in the colliders' order: a
    // cell is the first one's that has its centre.
    const float h = domain_.voxel;
    const Vec3 o = domain_.origin();
    std::vector<ShapeInstance> shapes;
    std::vector<Vec3> lows, highs;
    std::vector<std::vector<uint32_t>> meets(slots.size());
    for (size_t c = 0; c < colliders.size(); ++c) {
        shapes.push_back(colliders[c].instance());
        Vec3 lo, hi;
        shapes.back().bounds(lo, hi);
        lows.push_back(lo);
        highs.push_back(hi);
        int from[3], to[3];
        bool inside = true;
        for (int a = 0; a < 3; ++a) {
            if (!(hi[a] >= lo[a])) inside = false;
            from[a] = std::clamp(static_cast<int>(std::floor((lo[a] - o[a]) / h - 0.5f)), 0, n_[a] - 1) / Tiles::kSide;
            to[a] = std::clamp(static_cast<int>(std::ceil((hi[a] - o[a]) / h - 0.5f)), 0, n_[a] - 1) / Tiles::kSide;
        }
        if (!inside) continue;
        for (int z = from[2]; z <= to[2]; ++z) {
            for (int y = from[1]; y <= to[1]; ++y) {
                for (int x = from[0]; x <= to[0]; ++x) {
                    const size_t t = static_cast<size_t>(x) +
                                     static_cast<size_t>(tiles.tilesX()) *
                                         (static_cast<size_t>(y) + static_cast<size_t>(tiles.tilesY()) * static_cast<size_t>(z));
                    const int32_t s = tiles.slot(t);
                    if (s >= 0 && look[static_cast<size_t>(s)] >= 0) {
                        meets[static_cast<size_t>(look[static_cast<size_t>(s)])].push_back(static_cast<uint32_t>(c));
                    }
                }
            }
        }
    }
    std::vector<uint8_t> found(slots.size(), 0);
    float* solid = solid_.data();
    pg::parallelFor(slots.size(), 4, [&](size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i) {
            if (meets[i].empty()) continue;
            const size_t s = slots[i];
            int c0[3];
            tiles.corner(stored[s], c0[0], c0[1], c0[2]);
            for (int z = 0; z < Tiles::kSide; ++z) {
                for (int y = 0; y < Tiles::kSide; ++y) {
                    for (int x = 0; x < Tiles::kSide; ++x) {
                        const Vec3 p(o.x + (static_cast<float>(c0[0] + x) + 0.5f) * h,
                                     o.y + (static_cast<float>(c0[1] + y) + 0.5f) * h,
                                     o.z + (static_cast<float>(c0[2] + z) + 0.5f) * h);
                        for (const uint32_t c : meets[i]) {
                            const Vec3& lo = lows[c];
                            const Vec3& hi = highs[c];
                            if (p.x < lo.x || p.y < lo.y || p.z < lo.z || p.x > hi.x || p.y > hi.y || p.z > hi.z) continue;
                            if (!shapes[c].contains(p)) continue;
                            solid[s * Tiles::kCells + SparseGrid::local(x, y, z)] = 1.0f + static_cast<float>(c);
                            found[i] = 1;
                            break;
                        }
                    }
                }
            }
        }
    });
    anySolid_ = anySolid_ || std::find(found.begin(), found.end(), 1) != found.end();
}

// --- the solver's flow and the whirls -----------------------------------------------------

void UpresSolver::updateFlow(const PyroSolver& gas) {
    const std::shared_ptr<const Tiles>& tiles = gas.density().shared();
    if (tiles != coarseTiles_) {
        // The noise's coordinates go on where the solver's tiles do; where
        // they are new, they start where they are.
        coarseTiles_ = tiles;
        for (SparseGrid& g : centre_) g = SparseGrid(tiles);
        strength_ = SparseGrid(tiles);
        for (int l = 0; l < 2; ++l) {
            for (int a = 0; a < 3; ++a) {
                shift_[l][a].retile(tiles);
                shiftNext_[l][a] = SparseGrid(tiles);
            }
        }
    }
    const int n[3] = {gas.nx(), gas.ny(), gas.nz()};
    // The flow at the centres of the cells: what the fine cells read.
    forEachCounted(*tiles, [&](int i, int j, int k, size_t c) {
        for (int a = 0; a < 3; ++a) {
            const SparseGrid& v = gas.velocity(a);
            centre_[a].data()[c] = 0.5f * (v.at(i, j, k) + v.at(i + (a == 0), j + (a == 1), k + (a == 2)));
        }
    });
    // The whirls of a turbulent flow are as fast as its shear makes them:
    // the vorticity of the solver's flow times a cell -- the speed across a
    // cell it cannot resolve -- grown by the cube root of how many cells
    // across they are (Kolmogorov). Next to a solid, none: the gas there
    // goes the solid's way.
    const float h = coarse_.voxel;
    const float scale = 0.5f * settings_.turbulence * h * std::cbrt(settings_.swirlSize);
    const SparseGrid& solid = gas.solid();
    const bool solids = !gas.scene().colliders.empty();
    forEachCounted(*tiles, [&](int i, int j, int k, size_t c) {
        if (scale <= 0.0f) {
            strength_.data()[c] = 0.0f;
            return;
        }
        const int lo[3] = {std::max(i - 1, 0), std::max(j - 1, 0), std::max(k - 1, 0)};
        const int hi[3] = {std::min(i + 1, n[0] - 1), std::min(j + 1, n[1] - 1), std::min(k + 1, n[2] - 1)};
        // d(component a)/d(axis b), from the centres either side.
        auto d = [&](int a, int b) {
            int minus[3] = {i, j, k}, plus[3] = {i, j, k};
            minus[b] = lo[b];
            plus[b] = hi[b];
            if (plus[b] == minus[b]) return 0.0f;
            return (centre_[a].at(plus[0], plus[1], plus[2]) - centre_[a].at(minus[0], minus[1], minus[2])) /
                   (static_cast<float>(plus[b] - minus[b]) * h);
        };
        const float wx = d(2, 1) - d(1, 2), wy = d(0, 2) - d(2, 0), wz = d(1, 0) - d(0, 1);
        float s = scale * std::sqrt(wx * wx + wy * wy + wz * wz);
        if (solids) {
            const int around[7][3] = {{i, j, k},     {lo[0], j, k}, {hi[0], j, k}, {i, lo[1], k},
                                      {i, hi[1], k}, {i, j, lo[2]}, {i, j, hi[2]}};
            for (const auto& q : around) {
                if (solid.at(q[0], q[1], q[2]) > 0.5f) s = 0.0f;
            }
        }
        strength_.data()[c] = s;
    });
}

void UpresSolver::renewLayers() {
    // Each layer lasts Swirl Life, half of one out of step with the other;
    // as one starts afresh -- its coordinates where they are, its pattern
    // elsewhere in the noise -- it is faded out, and the other in: weights
    // whose squares add up to 1 keep the whirls as strong.
    const float phase = time_ / settings_.swirlLife;
    for (int l = 0; l < 2; ++l) {
        const float p = phase + 0.5f * static_cast<float>(l);
        const int cycle = static_cast<int>(std::floor(p));
        if (cycle != cycle_[l]) {
            cycle_[l] = cycle;
            offset_[l] = offsetOf(settings_.seed, l, cycle);
            for (SparseGrid& g : shift_[l]) g.fill(0.0f);
        }
        weight_[l] = std::sin(kPi * (p - static_cast<float>(cycle)));
    }
}

void UpresSolver::advectShift(float dt) {
    // Semi-Lagrangian, on the solver's grid: each cell takes the coordinates
    // of where its gas came from (RK2), and how far it came.
    const float cells = dt / coarse_.voxel;
    forEachCounted(*coarseTiles_, [&](int i, int j, int k, size_t c) {
        const float x = static_cast<float>(i) + 0.5f, y = static_cast<float>(j) + 0.5f, z = static_cast<float>(k) + 0.5f;
        const float u = centre_[0].data()[c], v = centre_[1].data()[c], w = centre_[2].data()[c];
        SparseGrid::Corners at;
        strength_.cornersAt(x - 0.5f * cells * u, y - 0.5f * cells * v, z - 0.5f * cells * w, at);
        const float back[3] = {x - cells * centre_[0].sampleAt(at), y - cells * centre_[1].sampleAt(at),
                               z - cells * centre_[2].sampleAt(at)};
        const float here[3] = {x, y, z};
        // Coming in from outside the solver's domain: starting there.
        const bool outside = back[0] < 0.0f || back[1] < 0.0f || back[2] < 0.0f ||
                             back[0] > static_cast<float>(coarse_.cells[0]) ||
                             back[1] > static_cast<float>(coarse_.cells[1]) || back[2] > static_cast<float>(coarse_.cells[2]);
        strength_.cornersAt(back[0], back[1], back[2], at);
        for (int l = 0; l < 2; ++l) {
            for (int a = 0; a < 3; ++a) {
                const float carried = outside ? 0.0f : shift_[l][a].sampleAt(at);
                shiftNext_[l][a].data()[c] = carried + back[a] - here[a];
            }
        }
    });
    for (int l = 0; l < 2; ++l) {
        for (int a = 0; a < 3; ++a) std::swap(shift_[l][a], shiftNext_[l][a]);
    }
}

void UpresSolver::turbulence(float x, float y, float z, const SparseGrid::Corners& at, float out[3]) const {
    out[0] = out[1] = out[2] = 0.0f;
    const float strength = strength_.sampleAt(at);
    if (strength <= 1e-6f) return;
    const float k = static_cast<float>(scale_);
    const float cx = x / k, cy = y / k, cz = z / k;
    const float size = settings_.swirlSize;
    for (int l = 0; l < 2; ++l) {
        if (weight_[l] <= 0.0f) continue;
        // Where this layer's noise has been carried to, lattice units.
        const float px = (cx + shift_[l][0].sampleAt(at)) / size + offset_[l].x;
        const float py = (cy + shift_[l][1].sampleAt(at)) / size + offset_[l].y;
        const float pz = (cz + shift_[l][2].sampleAt(at)) / size + offset_[l].z;
        const float w = weight_[l] * strength;
        for (int o = 0; o < octaves_; ++o) {
            const float f = static_cast<float>(1 << o);
            float c[3];
            tileNoise(noise_.data(), px * f + kOctaveShift[o][0], py * f + kOctaveShift[o][1],
                      pz * f + kOctaveShift[o][2], c);
            const float ow = w * octaveWeight_[o];
            out[0] += ow * c[0];
            out[1] += ow * c[1];
            out[2] += ow * c[2];
        }
    }
}

Vec3 UpresSolver::turbulenceAt(int i, int j, int k) const {
    if (!coarseTiles_) return {};
    const float x = static_cast<float>(i) + 0.5f, y = static_cast<float>(j) + 0.5f, z = static_cast<float>(k) + 0.5f;
    const float s = static_cast<float>(scale_);
    SparseGrid::Corners at;
    strength_.cornersAt(x / s, y / s, z / s, at);
    float t[3];
    turbulence(x, y, z, at, t);
    return {t[0], t[1], t[2]};
}

// --- advect ----------------------------------------------------------------------------

void UpresSolver::advect(bool closedFloor, float dt) {
    // MacCormack, as the solver, for the four fields at once: advect,
    // advect back, correct by half the error the round trip shows, clamped
    // to what the first step interpolated from. Where the gas of each fine
    // cell came from and goes to (RK2): the solver's flow there,
    // interpolated, and the whirls at the cell.
    const float cells = dt / domain_.voxel;  // m/s x dt, in fine cells
    const float k = static_cast<float>(scale_);
    const float nx = static_cast<float>(n_[0]), ny = static_cast<float>(n_[1]), nz = static_cast<float>(n_[2]);
    SparseGrid* fields[4] = {&density_, &temperature_, &fuel_, &flame_};
    auto outside = [&](float x, float y, float z) { return x < 0.0f || y < 0.0f || z < 0.0f || x > nx || y > ny || z > nz; };
    forEachCounted(*cells_, [&](int i, int j, int kk, size_t c) {
        const float x = static_cast<float>(i) + 0.5f, y = static_cast<float>(j) + 0.5f, z = static_cast<float>(kk) + 0.5f;
        SparseGrid::Corners at;
        strength_.cornersAt(x / k, y / k, z / k, at);
        float t[3], u[3];
        turbulence(x, y, z, at, t);
        for (int a = 0; a < 3; ++a) u[a] = centre_[a].sampleAt(at) + t[a];
        float ends[2][3];
        for (int e = 0; e < 2; ++e) {
            const float sign = e == 0 ? -1.0f : 1.0f;
            strength_.cornersAt((x + sign * 0.5f * cells * u[0]) / k, (y + sign * 0.5f * cells * u[1]) / k,
                                (z + sign * 0.5f * cells * u[2]) / k, at);
            ends[e][0] = x + sign * cells * (centre_[0].sampleAt(at) + t[0]);
            ends[e][1] = y + sign * cells * (centre_[1].sampleAt(at) + t[1]);
            ends[e][2] = z + sign * cells * (centre_[2].sampleAt(at) + t[2]);
            // A closed floor: what the whirls would take from under it comes
            // from the cells on it.
            if (closedFloor) ends[e][1] = std::max(ends[e][1], 0.0f);
        }
        for (int a = 0; a < 3; ++a) forward_[a].data()[c] = ends[1][a];
        // Gas that comes in through an open side comes from the empty air
        // outside.
        if (outside(ends[0][0], ends[0][1], ends[0][2])) {
            for (int f = 0; f < 4; ++f) predicted_[f].data()[c] = lows_[f].data()[c] = highs_[f].data()[c] = 0.0f;
            return;
        }
        density_.cornersAt(ends[0][0], ends[0][1], ends[0][2], at);
        for (int f = 0; f < 4; ++f) {
            float lo, hi;
            predicted_[f].data()[c] = fields[f]->sampleAt(at, lo, hi);
            lows_[f].data()[c] = lo;
            highs_[f].data()[c] = hi;
        }
    });
    const float* solid = anySolid_ ? solid_.data() : nullptr;
    forEachCounted(*cells_, [&](int, int, int, size_t c) {
        // No gas in a solid.
        if (solid && solid[c] > 0.5f) {
            for (SparseGrid* g : fields) g->data()[c] = 0.0f;
            return;
        }
        const float fx = forward_[0].data()[c], fy = forward_[1].data()[c], fz = forward_[2].data()[c];
        // Where the gas leaves the domain the round trip would come back
        // empty: there, plain semi-Lagrangian.
        if (outside(fx, fy, fz)) {
            for (int f = 0; f < 4; ++f) fields[f]->data()[c] = predicted_[f].data()[c];
            return;
        }
        SparseGrid::Corners at;
        density_.cornersAt(fx, fy, fz, at);
        for (int f = 0; f < 4; ++f) {
            // Only this cell of the field is read here: it takes the result.
            const float roundTrip = predicted_[f].sampleAt(at);
            const float v = predicted_[f].data()[c] + 0.5f * (fields[f]->data()[c] - roundTrip);
            fields[f]->data()[c] = std::clamp(v, lows_[f].data()[c], highs_[f].data()[c]);
        }
    });
}

// --- combust and dissipate ---------------------------------------------------------------

void UpresSolver::combust(const SolverSettings& s, float dt) {
    // As the solver: fuel burns into heat, soot and flame, and the gas
    // swells; smoke thins out, heat cools, flames die, and where the gas
    // swells what it carries spreads over the more room.
    const float share = 1.0f - std::exp(-s.burnRate * dt);
    const float smoke = std::exp(-s.smokeDecay * dt), heat = std::exp(-s.cooling * dt);
    const float flameKeep = s.flameLife > 0.0f ? std::exp(-dt / s.flameLife) : 0.0f;
    float* density = density_.data();
    float* temperature = temperature_.data();
    float* fuel = fuel_.data();
    float* flame = flame_.data();
    float* expansion = expansion_.data();
    forEachCounted(*cells_, [&](int, int, int, size_t c) {
        const float burnt = fuel[c] * share;
        fuel[c] -= burnt;
        temperature[c] += burnt * s.heatRelease;
        density[c] += burnt * s.sootRelease;
        flame[c] += burnt;
        expansion[c] += burnt * s.expansion / dt;
        const float thinner = 1.0f / (1.0f + expansion[c] * dt);
        density[c] = std::max(0.0f, density[c]) * smoke * thinner;
        temperature[c] = std::max(0.0f, temperature[c]) * heat;
        fuel[c] = std::max(0.0f, fuel[c]) * thinner;
        flame[c] = std::max(0.0f, flame[c]) * flameKeep * thinner;
    });
}

// --- state -------------------------------------------------------------------------------

void UpresSolver::saveState(StateWriter& out) const {
    out.pod(static_cast<int32_t>(n_[0]));
    out.pod(static_cast<int32_t>(n_[1]));
    out.pod(static_cast<int32_t>(n_[2]));
    out.pod(static_cast<int32_t>(frame_));
    out.pod(time_);
    out.pod(static_cast<uint8_t>(cells_ ? 1 : 0));
    if (!cells_) return;
    out.tiles(*cells_);
    for (const SparseGrid* g : {&density_, &temperature_, &fuel_, &flame_}) out.values(*g);
    out.pod(static_cast<int32_t>(cycle_[0]));
    out.pod(static_cast<int32_t>(cycle_[1]));
    out.pod(static_cast<uint8_t>(coarseTiles_ ? 1 : 0));
    if (!coarseTiles_) return;
    out.tiles(*coarseTiles_);
    for (const auto& layer : shift_) {
        for (const SparseGrid& g : layer) out.values(g);
    }
}

bool UpresSolver::loadState(StateReader& in, const PyroSolver& gas) {
    int32_t n[3] = {0, 0, 0}, frame = 0, cycle[2] = {0, 0};
    float time = 0.0f;
    uint8_t made = 0, carried = 0;
    if (!in.pod(n[0]) || !in.pod(n[1]) || !in.pod(n[2]) || !in.pod(frame) || !in.pod(time) || !in.pod(made)) {
        return false;
    }
    if (made == 0) {
        // Saved before its first step: nothing to take on.
        return frame == 0 ? true : in.fail();
    }
    const Domain fine = settings_.domain(gas.domain());
    if (n[0] != fine.cells[0] || n[1] != fine.cells[1] || n[2] != fine.cells[2]) return in.fail();
    // Read aside: a state cut short leaves this upres as it was.
    std::shared_ptr<const Tiles> cells, coarse;
    if (!in.tiles(cells) || cells->axis() != -1 || cells->nx() != n[0] || cells->ny() != n[1] || cells->nz() != n[2]) {
        return in.fail();
    }
    SparseGrid fields[4];
    for (SparseGrid& g : fields) {
        g = SparseGrid(cells);
        if (!in.values(g)) return false;
    }
    if (!in.pod(cycle[0]) || !in.pod(cycle[1]) || !in.pod(carried)) return false;
    SparseGrid shift[2][3];
    if (carried) {
        if (!in.tiles(coarse) || coarse->axis() != -1 || coarse->nx() != gas.nx() || coarse->ny() != gas.ny() ||
            coarse->nz() != gas.nz()) {
            return in.fail();
        }
        for (auto& layer : shift) {
            for (SparseGrid& g : layer) {
                g = SparseGrid(coarse);
                if (!in.values(g)) return false;
            }
        }
    }
    reset(gas.domain());
    retile(cells);
    fresh_.clear();
    SparseGrid* mine[4] = {&density_, &temperature_, &fuel_, &flame_};
    for (int f = 0; f < 4; ++f) *mine[f] = std::move(fields[f]);
    for (int l = 0; l < 2; ++l) {
        cycle_[l] = cycle[l];
        offset_[l] = offsetOf(settings_.seed, l, cycle[l]);
    }
    if (carried) {
        // On the tiles the solver had then; the next step takes them on to
        // its own as the solver read them.
        coarseTiles_ = coarse;
        for (SparseGrid& g : centre_) g = SparseGrid(coarse);
        strength_ = SparseGrid(coarse);
        for (int l = 0; l < 2; ++l) {
            for (int a = 0; a < 3; ++a) {
                shift_[l][a] = std::move(shift[l][a]);
                shiftNext_[l][a] = SparseGrid(coarse);
            }
        }
    }
    frame_ = frame;
    time_ = time;
    return true;
}

// --- frames ------------------------------------------------------------------------------

Frame capture(const UpresSolver& upres) {
    Frame f = gasFrame(upres.domain(), upres.tiles(), upres.density(), upres.temperature(), upres.flame());
    f.number = upres.frame();
    f.time = upres.time();
    return f;
}

}  // namespace pg::sim
