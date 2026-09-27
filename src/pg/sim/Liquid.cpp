#include "pg/sim/Liquid.h"

#include "pg/core/Parallel.h"
#include "pg/sim/Shared.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace pg::sim {

using detail::faceOffset;
using detail::fix;
using detail::forEachCell;

namespace {

constexpr float kHuge = std::numeric_limits<float>::max();
/// A particle's radius, cells: eight to a cell, half a cell apart, each a
/// little wider than that, so that the surface closes between them.
constexpr float kRadius = 0.6f;
/// How far round a point the surface looks for particles, cells.
constexpr float kReach = 2.0f * kRadius;
/// Cells a particle may move in a substep.
constexpr float kCfl = 2.0f;
constexpr int kMaxSubsteps = 32;
/// How close a particle may come to a solid or a wall, cells.
constexpr float kMargin = 0.1f;
/// A face less open than this is closed: slivers only make the pressure stiff.
constexpr float kLeastOpen = 0.05f;
/// The pressure solve: relative residual, and iterations at most.
constexpr float kTolerance = 1e-4f;
constexpr int kMaxIterations = 80;
/// Layers of faces the velocity is carried out to, past the water.
constexpr int kExtrapolation = 4;
/// Seconds foam takes to fade to a third.
constexpr float kFoamLife = 0.8f;

/// How much of the segment between two corners is inside (distance < 0).
float insideFraction(float a, float b) {
    if (a < 0.0f && b < 0.0f) return 1.0f;
    if (a < 0.0f) return a / (a - b);
    if (b < 0.0f) return b / (b - a);
    return 0.0f;
}

/// How much of a square face is inside a solid, from the solid's distance at
/// its corners, in order round it: the shape between them taken as straight
/// cuts (Batty's fraction_inside).
float insideFraction(float c0, float c1, float c2, float c3) {
    float v[4] = {c0, c1, c2, c3};
    const int inside = (v[0] < 0.0f) + (v[1] < 0.0f) + (v[2] < 0.0f) + (v[3] < 0.0f);
    auto turn = [&] { std::rotate(v, v + 1, v + 4); };
    switch (inside) {
        case 0: return 0.0f;
        case 4: return 1.0f;
        case 1:  // a corner cut off
            while (!(v[0] < 0.0f)) turn();
            return 0.5f * insideFraction(v[0], v[3]) * insideFraction(v[0], v[1]);
        case 3:  // all but a corner
            while (v[0] < 0.0f) turn();
            return 1.0f - 0.5f * (1.0f - insideFraction(v[0], v[3])) * (1.0f - insideFraction(v[0], v[1]));
        default: {
            while (!(v[0] < 0.0f) || !(v[1] < 0.0f || v[2] < 0.0f)) turn();
            if (v[1] < 0.0f) return 0.5f * (insideFraction(v[0], v[3]) + insideFraction(v[1], v[2]));  // a side
            // Two opposite corners: the middle decides whether they join.
            if (0.25f * (v[0] + v[1] + v[2] + v[3]) < 0.0f) {
                return 1.0f - 0.5f * (1.0f - insideFraction(v[0], v[3])) * (1.0f - insideFraction(v[2], v[3])) -
                       0.5f * (1.0f - insideFraction(v[2], v[1])) * (1.0f - insideFraction(v[0], v[1]));
            }
            return 0.5f * insideFraction(v[0], v[1]) * insideFraction(v[0], v[3]) +
                   0.5f * insideFraction(v[2], v[1]) * insideFraction(v[2], v[3]);
        }
    }
}

/// A number in [0, 1) from a hash.
float unit(uint32_t h) { return static_cast<float>(h & 0xFFFFFFu) / 16777216.0f; }

/// f(p) for every particle p, sorted into cells by `cellStart`, in parallel
/// over slabs two layers of cells thick: the even slabs, then the odd ones. A
/// particle writes to the grid no further than the layers either side of its
/// own, so two slabs of a colour -- a slab apart -- never write the same
/// place, and each place takes what it takes from the slabs in the same
/// order, whatever the threads.
template <class F>
void bySlabs(const std::vector<uint32_t>& cellStart, int nx, int ny, int nz, const F& f) {
    const size_t layer = static_cast<size_t>(nx) * static_cast<size_t>(ny);
    const int slabs = (nz + 1) / 2;
    for (int colour = 0; colour < 2; ++colour) {
        const size_t count = static_cast<size_t>((slabs - colour + 1) / 2);
        pg::parallelFor(count, 1, [&](size_t begin, size_t end) {
            for (size_t t = begin; t < end; ++t) {
                const int z0 = 2 * (colour + 2 * static_cast<int>(t)), z1 = std::min(z0 + 2, nz);
                for (uint32_t p = cellStart[layer * static_cast<size_t>(z0)]; p < cellStart[layer * static_cast<size_t>(z1)]; ++p) {
                    f(p);
                }
            }
        });
    }
}

/// Turns `phi` -- below 0 inside, 0 on the surface, any unit -- into the
/// distance to that surface in cells, signed, cut off at `band` cells. The
/// cells either side of the surface take it from where it crosses the lines
/// to their neighbours; the rest are swept in all eight diagonal directions,
/// each solving |grad d| = 1 from the neighbours behind it (Zhao 2005). A
/// distance a ray can step along safely: the averaged spheres are not one.
void redistance(Grid& phi, float scale, float band) {
    const int nx = phi.nx(), ny = phi.ny(), nz = phi.nz();
    const size_t sy = static_cast<size_t>(nx), sz = sy * static_cast<size_t>(ny);
    std::vector<float> d(phi.size(), band);
    std::vector<uint8_t> fixed(phi.size(), 0);
    const float* v = phi.data();
    // 1. At the surface: a plane through the crossings along each axis.
    detail::forEachCell(phi, [&](int i, int j, int k) {
        const size_t c = phi.index(i, j, k);
        const bool inside = v[c] < 0.0f;
        const int at[3] = {i, j, k};
        const int n[3] = {nx, ny, nz};
        const size_t step[3] = {1, sy, sz};
        float sum = 0.0f;
        for (int a = 0; a < 3; ++a) {
            float nearest = 2.0f;
            for (int side = -1; side <= 1; side += 2) {
                const int b = at[a] + side;
                if (b < 0 || b >= n[a]) continue;
                const float w = v[side < 0 ? c - step[a] : c + step[a]];
                if ((w < 0.0f) == inside) continue;
                nearest = std::min(nearest, v[c] / (v[c] - w));
            }
            if (nearest <= 1.0f) sum += 1.0f / std::max(nearest * nearest, 1e-8f);
        }
        if (sum > 0.0f) {
            d[c] = std::min(1.0f / std::sqrt(sum), band);
            fixed[c] = 1;
        }
    });
    // 2. Out from there.
    for (int sweep = 0; sweep < 8; ++sweep) {
        const int di = sweep & 1 ? -1 : 1, dj = sweep & 2 ? -1 : 1, dk = sweep & 4 ? -1 : 1;
        for (int k = dk > 0 ? 0 : nz - 1; k >= 0 && k < nz; k += dk) {
            for (int j = dj > 0 ? 0 : ny - 1; j >= 0 && j < ny; j += dj) {
                for (int i = di > 0 ? 0 : nx - 1; i >= 0 && i < nx; i += di) {
                    const size_t c = phi.index(i, j, k);
                    if (fixed[c]) continue;
                    float a = std::min(i > 0 ? d[c - 1] : band, i < nx - 1 ? d[c + 1] : band);
                    float b = std::min(j > 0 ? d[c - sy] : band, j < ny - 1 ? d[c + sy] : band);
                    float e = std::min(k > 0 ? d[c - sz] : band, k < nz - 1 ? d[c + sz] : band);
                    if (a > b) std::swap(a, b);
                    if (b > e) std::swap(b, e);
                    if (a > b) std::swap(a, b);
                    if (a >= band) continue;
                    float u = a + 1.0f;
                    if (u > b) {
                        u = 0.5f * (a + b + std::sqrt(std::max(2.0f - (a - b) * (a - b), 0.0f)));
                        if (u > e) {
                            const float sum = a + b + e;
                            u = (sum + std::sqrt(std::max(sum * sum - 3.0f * (a * a + b * b + e * e - 1.0f), 0.0f))) / 3.0f;
                        }
                    }
                    d[c] = std::min(d[c], u);
                }
            }
        }
    }
    // 3. The sign back, in the units asked for.
    float* out = phi.data();
    for (size_t c = 0; c < phi.size(); ++c) out[c] = (out[c] < 0.0f ? -d[c] : d[c]) * scale;
}

}  // namespace

// --- the scene -----------------------------------------------------------------------

LiquidScene LiquidScene::sanitized() const {
    const LiquidSettings ds;
    LiquidScene s = *this;
    LiquidSettings& v = s.solver;
    v.size = fix(v.size, 0.1f, 20.0f, ds.size);
    v.resolution = std::clamp(v.resolution, 16, 256);
    v.timeStep = fix(v.timeStep, 1e-4f, 1.0f, ds.timeStep);
    v.substeps = std::clamp(v.substeps, 1, 16);
    v.flip = fix(v.flip, 0.0f, 1.0f, ds.flip);
    v.gravity = fix(v.gravity, -100.0f, 100.0f, ds.gravity);
    const WaterSource dw;
    for (WaterSource& w : s.sources) {
        w.center = fix(w.center, -kHuge, kHuge, dw.center);
        w.rotation = fix(w.rotation, -kHuge, kHuge, Vec3());
        w.size = fix(w.size, 0.005f, kHuge, dw.size);
        w.velocity = fix(w.velocity, -1000.0f, 1000.0f, Vec3());
        w.start = fix(w.start, 0.0f, kHuge, 0.0f);
        w.end = fix(w.end, 0.0f, kHuge, 0.0f);
    }
    detail::sanitize(s.forces);
    detail::sanitize(s.colliders);
    return s;
}

LiquidScene LiquidScene::damBreak() {
    LiquidScene s;
    s.solver.size = Vec3(2.0f, 1.0f, 1.0f);
    s.solver.resolution = 48;
    WaterSource w;
    w.shape = Shape::Box;
    w.center = Vec3(-0.7f, 0.35f, 0.0f);
    w.size = Vec3(0.6f, 0.7f, 1.0f);
    s.sources.push_back(w);
    return s;
}

// --- set-up ----------------------------------------------------------------------------

LiquidSolver::LiquidSolver(const LiquidScene& scene) : scene_(scene.sanitized()) {
    domain_ = scene_.solver.domain();
    for (int a = 0; a < 3; ++a) n_[a] = domain_.cells[a];
    const int nx = n_[0], ny = n_[1], nz = n_[2];
    for (int a = 0; a < 3; ++a) {
        const int fx = nx + (a == 0), fy = ny + (a == 1), fz = nz + (a == 2);
        vel_[a] = Grid(fx, fy, fz);
        old_[a] = Grid(fx, fy, fz);
        weight_[a] = Grid(fx, fy, fz);
        valid_[a].assign(vel_[a].size(), 0);
    }
    phi_ = Grid(nx, ny, nz, kReach * domain_.voxel);
    pressure_ = Grid(nx, ny, nz);
    rhs_ = Grid(nx, ny, nz);
    cells_.assign(domain_.cellCount(), FreeSurfaceSolver::Air);
    cellStart_.assign(domain_.cellCount() + 1, 0);
    noise_.assign(scene_.forces.size(), {});
    filled_.assign(scene_.sources.size(), 0);
    updateSolids();
}

Vec3 LiquidSolver::worldAt(float x, float y, float z) const {
    const Vec3 o = domain_.origin();
    const float h = domain_.voxel;
    return {o.x + x * h, o.y + y * h, o.z + z * h};
}

Vec3 LiquidSolver::toCells(const Vec3& p) const {
    const Vec3 o = domain_.origin();
    const float inv = 1.0f / domain_.voxel;
    return {(p.x - o.x) * inv, (p.y - o.y) * inv, (p.z - o.z) * inv};
}

void LiquidSolver::updateSolids() {
    const int nx = n_[0], ny = n_[1], nz = n_[2];
    const float h = domain_.voxel;
    // The colliders' distance at the grid's corners, cut off a few cells
    // away: only near a solid does it matter.
    const float far = 3.0f * h;
    solidPhi_ = Grid(nx + 1, ny + 1, nz + 1, far);
    anySolid_ = false;
    for (const Collider& c : scene_.colliders) {
        const ShapeInstance shape = c.instance();
        Vec3 lo, hi;
        shape.bounds(lo, hi);
        const Vec3 a = toCells(lo), b = toCells(hi);
        const int i0 = std::clamp(static_cast<int>(std::floor(a.x)) - 3, 0, nx + 1);
        const int i1 = std::clamp(static_cast<int>(std::ceil(b.x)) + 4, 0, nx + 1);
        const int j0 = std::clamp(static_cast<int>(std::floor(a.y)) - 3, 0, ny + 1);
        const int j1 = std::clamp(static_cast<int>(std::ceil(b.y)) + 4, 0, ny + 1);
        const int k0 = std::clamp(static_cast<int>(std::floor(a.z)) - 3, 0, nz + 1);
        const int k1 = std::clamp(static_cast<int>(std::ceil(b.z)) + 4, 0, nz + 1);
        detail::forEachIn(i0, i1, j0, j1, k0, k1, [&](int i, int j, int k) {
            const float d = std::min(shape.distance(worldAt(static_cast<float>(i), static_cast<float>(j),
                                                            static_cast<float>(k))),
                                     far);
            float& v = solidPhi_.at(i, j, k);
            v = std::min(v, d);
        });
    }
    anySolid_ = std::any_of(solidPhi_.values().begin(), solidPhi_.values().end(), [](float v) { return v < 0.0f; });

    // How open each face is: what the solids leave of it, the floor always
    // closed, the sides closed round a tank, the top open to the sky.
    const bool closedSides = scene_.solver.closedSides;
    for (int a = 0; a < 3; ++a) {
        Grid& open = open_[a];
        open = Grid(nx + (a == 0), ny + (a == 1), nz + (a == 2), 1.0f);
        forEachCell(open, [&](int i, int j, int k) {
            const int f = a == 0 ? i : a == 1 ? j : k;
            if (a == 1 && f == 0) {
                open.at(i, j, k) = 0.0f;
                return;
            }
            if (a != 1 && closedSides && (f == 0 || f == n_[a])) {
                open.at(i, j, k) = 0.0f;
                return;
            }
            if (!anySolid_) return;
            // The face's corners, round it.
            float c[4];
            if (a == 0) {
                c[0] = solidPhi_.at(i, j, k), c[1] = solidPhi_.at(i, j + 1, k);
                c[2] = solidPhi_.at(i, j + 1, k + 1), c[3] = solidPhi_.at(i, j, k + 1);
            } else if (a == 1) {
                c[0] = solidPhi_.at(i, j, k), c[1] = solidPhi_.at(i + 1, j, k);
                c[2] = solidPhi_.at(i + 1, j, k + 1), c[3] = solidPhi_.at(i, j, k + 1);
            } else {
                c[0] = solidPhi_.at(i, j, k), c[1] = solidPhi_.at(i + 1, j, k);
                c[2] = solidPhi_.at(i + 1, j + 1, k), c[3] = solidPhi_.at(i, j + 1, k);
            }
            const float left = 1.0f - insideFraction(c[0], c[1], c[2], c[3]);
            open.at(i, j, k) = left < kLeastOpen ? 0.0f : left;
        });
    }
    solidCell_.assign(domain_.cellCount(), 0);
    if (anySolid_) {
        forEachCell(phi_, [&](int i, int j, int k) {
            float sum = 0.0f;
            for (int q = 0; q < 8; ++q) sum += solidPhi_.at(i + (q & 1), j + ((q >> 1) & 1), k + (q >> 2));
            solidCell_[phi_.index(i, j, k)] = sum < 0.0f ? 1 : 0;
        });
    }
}

float LiquidSolver::distanceToSurface(const Vec3& p) const {
    const Vec3 g = toCells(p);
    const float far = kReach * domain_.voxel;
    if (g.x < 0.0f || g.y < 0.0f || g.z < 0.0f || g.x > static_cast<float>(n_[0]) || g.y > static_cast<float>(n_[1]) ||
        g.z > static_cast<float>(n_[2])) {
        return far;
    }
    return phi_.sample(g.x, g.y, g.z);
}

float LiquidSolver::solidDistance(const Vec3& p) const {
    if (!anySolid_) return 3.0f * domain_.voxel;
    const Vec3 g = toCells(p);
    // Corners sit at whole cell units; Grid::sample puts values at half ones.
    return solidPhi_.sample(g.x + 0.5f, g.y + 0.5f, g.z + 0.5f);
}

// --- a step ----------------------------------------------------------------------------

float LiquidSolver::maxSpeed() const {
    const Vec3* v = velocity_.data();
    return pg::parallelReduce(
        velocity_.size(), 16384, 0.0f,
        [&](size_t begin, size_t end) {
            float m = 0.0f;
            for (size_t p = begin; p < end; ++p) m = std::max(m, dot(v[p], v[p]));
            return std::sqrt(m);
        },
        [](float a, float b) { return std::max(a, b); });
}

void LiquidSolver::step() {
    const float h = domain_.voxel;
    const float frame = scene_.solver.timeStep;
    const float most = frame / static_cast<float>(scene_.solver.substeps);
    const float least = frame / static_cast<float>(kMaxSubsteps);
    // The fastest a source pours: its water moves that fast from the start.
    float pour = 0.0f;
    for (const WaterSource& s : scene_.sources) pour = std::max(pour, length(s.velocity));
    lastSubsteps_ = 0;
    lastIterations_ = 0;
    float left = frame;
    while (left > 1e-6f * frame) {
        const float speed = std::max(maxSpeed(), pour);
        float dt = speed > 0.0f ? std::min(most, kCfl * h / speed) : most;
        dt = std::max(dt, least);
        if (dt >= 0.999f * left) {
            dt = left;
        } else if (dt > 0.5f * left) {
            dt = 0.5f * left;  // two even halves, not a step and a sliver
        }
        substep(dt);
        left -= dt;
        ++lastSubsteps_;
    }
    sortParticles();  // for the questions asked between steps
    ++frame_;
}

void LiquidSolver::substep(float dt) {
    sortParticles();
    emit();
    toGrid();
    // The velocity the particles brought, carried out past the water -- the
    // FLIP update subtracts it, so it has to be there wherever a particle
    // may look.
    for (int a = 0; a < 3; ++a) {
        const float* w = weight_[a].data();
        const float* o = open_[a].data();
        uint8_t* valid = valid_[a].data();
        for (size_t f = 0; f < valid_[a].size(); ++f) valid[f] = w[f] > 1e-6f && o[f] > 0.0f;
    }
    extrapolate(kExtrapolation);
    for (int a = 0; a < 3; ++a) std::copy(vel_[a].values().begin(), vel_[a].values().end(), old_[a].data());
    addForces(dt);
    project(dt);
    extrapolate(kExtrapolation);
    toParticles(dt);
    advect(dt);
    time_ += dt;
    ++substepCount_;
}

void LiquidSolver::sortParticles() {
    const int nx = n_[0], ny = n_[1], nz = n_[2];
    const size_t n = position_.size();
    std::vector<uint32_t> cellOf(n);
    cellPos_.resize(n);
    pg::parallelFor(n, 8192, [&](size_t begin, size_t end) {
        for (size_t p = begin; p < end; ++p) {
            const Vec3 g = toCells(position_[p]);
            const int i = std::clamp(static_cast<int>(std::floor(g.x)), 0, nx - 1);
            const int j = std::clamp(static_cast<int>(std::floor(g.y)), 0, ny - 1);
            const int k = std::clamp(static_cast<int>(std::floor(g.z)), 0, nz - 1);
            cellOf[p] = static_cast<uint32_t>(phi_.index(i, j, k));
        }
    });
    // A stable counting sort: the particles of a cell keep their order.
    std::fill(cellStart_.begin(), cellStart_.end(), 0u);
    for (size_t p = 0; p < n; ++p) ++cellStart_[cellOf[p] + 1];
    for (size_t c = 0; c + 1 < cellStart_.size(); ++c) cellStart_[c + 1] += cellStart_[c];
    std::vector<uint32_t> to(n);
    {
        std::vector<uint32_t> next(cellStart_.begin(), cellStart_.end() - 1);
        for (size_t p = 0; p < n; ++p) to[p] = next[cellOf[p]]++;
    }
    std::vector<Vec3> position(n), velocity(n);
    std::vector<float> foam(n);
    pg::parallelFor(n, 8192, [&](size_t begin, size_t end) {
        for (size_t p = begin; p < end; ++p) {
            position[to[p]] = position_[p];
            velocity[to[p]] = velocity_[p];
            foam[to[p]] = foam_[p];
            cellPos_[to[p]] = toCells(position_[p]);
        }
    });
    position_.swap(position);
    velocity_.swap(velocity);
    foam_.swap(foam);
}

void LiquidSolver::emit() {
    const float h = domain_.voxel;
    const int nx = n_[0], ny = n_[1];
    bool added = false;
    for (size_t s = 0; s < scene_.sources.size(); ++s) {
        const WaterSource& src = scene_.sources[s];
        if (!src.activeAt(time_)) continue;
        if (src.mode == WaterMode::Fill) {
            if (filled_[s]) continue;
            filled_[s] = 1;
        }
        const ShapeInstance shape = src.instance();
        const Vec3 jet = shape.turn().apply(src.velocity);
        Vec3 lo, hi;
        shape.bounds(lo, hi);
        const Vec3 a = toCells(lo), b = toCells(hi);
        int c0[3], c1[3];
        for (int d = 0; d < 3; ++d) {
            c0[d] = std::clamp(static_cast<int>(std::floor(a[d])), 0, n_[d]);
            c1[d] = std::clamp(static_cast<int>(std::ceil(b[d])), 0, n_[d]);
        }
        // A flow pushes the water in it out at its velocity.
        if (src.mode == WaterMode::Flow) {
            pg::parallelFor(position_.size(), 8192, [&](size_t begin, size_t end) {
                for (size_t p = begin; p < end; ++p) {
                    const Vec3& x = position_[p];
                    if (x.x < lo.x || x.y < lo.y || x.z < lo.z || x.x > hi.x || x.y > hi.y || x.z > hi.z) continue;
                    if (shape.contains(x)) velocity_[p] = jet;
                }
            });
        }
        // A particle in each eighth of a cell inside the shape that has none,
        // at a place in it the hash picks.
        const uint32_t seed = src.seed * 7919u + scene_.solver.seed * 31u + substepCount_ * 104729u;
        for (int k = c0[2]; k < c1[2]; ++k) {
            for (int j = c0[1]; j < c1[1]; ++j) {
                for (int i = c0[0]; i < c1[0]; ++i) {
                    const size_t c = static_cast<size_t>(i) +
                                     static_cast<size_t>(nx) * (static_cast<size_t>(j) + static_cast<size_t>(ny) * static_cast<size_t>(k));
                    if (solidCell_[c]) continue;
                    unsigned taken = 0;
                    for (uint32_t p = cellStart_[c]; p < cellStart_[c + 1]; ++p) {
                        const Vec3& g = cellPos_[p];
                        const unsigned sub = (g.x - static_cast<float>(i) >= 0.5f ? 1u : 0u) |
                                             (g.y - static_cast<float>(j) >= 0.5f ? 2u : 0u) |
                                             (g.z - static_cast<float>(k) >= 0.5f ? 4u : 0u);
                        taken |= 1u << sub;
                    }
                    for (int sub = 0; sub < 8; ++sub) {
                        if (taken & (1u << sub)) continue;
                        const int z = 8 * k + sub;
                        const Vec3 g(static_cast<float>(i) + 0.5f * static_cast<float>(sub & 1) +
                                         0.5f * unit(detail::hash(i, j, z, seed)),
                                     static_cast<float>(j) + 0.5f * static_cast<float>((sub >> 1) & 1) +
                                         0.5f * unit(detail::hash(i, j, z, seed ^ 0x68BC21EBu)),
                                     static_cast<float>(k) + 0.5f * static_cast<float>(sub >> 2) +
                                         0.5f * unit(detail::hash(i, j, z, seed ^ 0x02E5BE93u)));
                        const Vec3 p = worldAt(g.x, g.y, g.z);
                        if (!shape.contains(p)) continue;
                        if (anySolid_ && solidDistance(p) < kMargin * h) continue;
                        position_.push_back(p);
                        velocity_.push_back(jet);
                        foam_.push_back(0.0f);
                        added = true;
                    }
                }
            }
        }
    }
    if (added) sortParticles();
}

void LiquidSolver::splat(int factor, Grid& weight, Grid* centre, Grid* radius, Grid& foam) const {
    // Reaching less than a cell and a quarter, a particle writes no further
    // than the layers of cells either side of its own -- bySlabs() needs that
    // -- on this grid and on one twice as fine, whose centres sit a quarter
    // of a cell in.
    static_assert(kReach < 1.25f, "a particle must stay within the layers next to its own");
    const int fx = n_[0] * factor, fy = n_[1] * factor, fz = n_[2] * factor;
    weight = Grid(fx, fy, fz);
    for (int a = 0; a < 3; ++a) centre[a] = Grid(fx, fy, fz);
    if (radius) *radius = Grid(fx, fy, fz);
    foam = Grid(fx, fy, fz);
    const float scale = static_cast<float>(factor);
    const float reach = kReach * scale, reach2 = reach * reach;
    const int nx = n_[0], ny = n_[1], nz = n_[2];
    bySlabs(cellStart_, nx, ny, nz, [&](uint32_t p) {
        const Vec3& g = cellPos_[p];
        const Vec3 q = g * scale;  // in the cells of this grid
        // A particle with few round it -- spray, a sheet torn thin -- is
        // drawn wider, so that what it is part of stays whole.
        float r = kRadius;
        if (radius) {
            const int ci = std::clamp(static_cast<int>(g.x), 0, nx - 1), cj = std::clamp(static_cast<int>(g.y), 0, ny - 1),
                      ck = std::clamp(static_cast<int>(g.z), 0, nz - 1);
            const size_t c = static_cast<size_t>(ci) + static_cast<size_t>(nx) * (static_cast<size_t>(cj) + static_cast<size_t>(ny) * static_cast<size_t>(ck));
            const float many = std::min(static_cast<float>(cellStart_[c + 1] - cellStart_[c]), 8.0f) / 8.0f;
            r = kRadius * (1.0f + 0.4f * (1.0f - many));
        }
        const int i0 = std::max(static_cast<int>(std::ceil(q.x - 0.5f - reach)), 0);
        const int i1 = std::min(static_cast<int>(std::floor(q.x - 0.5f + reach)), fx - 1);
        const int j0 = std::max(static_cast<int>(std::ceil(q.y - 0.5f - reach)), 0);
        const int j1 = std::min(static_cast<int>(std::floor(q.y - 0.5f + reach)), fy - 1);
        const int k0 = std::max(static_cast<int>(std::ceil(q.z - 0.5f - reach)), 0);
        const int k1 = std::min(static_cast<int>(std::floor(q.z - 0.5f + reach)), fz - 1);
        const float white = foam_[p];
        for (int k = k0; k <= k1; ++k) {
            const float dz = static_cast<float>(k) + 0.5f - q.z;
            for (int j = j0; j <= j1; ++j) {
                const float dy = static_cast<float>(j) + 0.5f - q.y;
                const float dyz = dy * dy + dz * dz;
                if (dyz >= reach2) continue;
                for (int i = i0; i <= i1; ++i) {
                    const float dx = static_cast<float>(i) + 0.5f - q.x;
                    const float d2 = dx * dx + dyz;
                    if (d2 >= reach2) continue;
                    const float s = 1.0f - d2 / reach2;
                    const float w = s * s * s;
                    const size_t c = weight.index(i, j, k);
                    weight.data()[c] += w;
                    centre[0].data()[c] += w * g.x;
                    centre[1].data()[c] += w * g.y;
                    centre[2].data()[c] += w * g.z;
                    if (radius) radius->data()[c] += w * r;
                    foam.data()[c] += w * white;
                }
            }
        }
    });
}

float LiquidSolver::sphereDistance(const Vec3& x, float weight, const Vec3& centre, float radius) const {
    if (weight <= 0.0f) return kReach;
    return length(x - centre * (1.0f / weight)) - radius / weight;
}

void LiquidSolver::toGrid() {
    const int nx = n_[0], ny = n_[1], nz = n_[2];
    // Each particle's velocity onto the faces round it, weighted by how near
    // they are along each axis (trilinear): the sums first, in vel_ and
    // weight_, then their ratio.
    for (int a = 0; a < 3; ++a) {
        vel_[a].fill(0.0f);
        weight_[a].fill(0.0f);
    }
    bySlabs(cellStart_, nx, ny, nz, [&](uint32_t p) {
        const Vec3& g = cellPos_[p];
        const Vec3& v = velocity_[p];
        for (int a = 0; a < 3; ++a) {
            Grid& sum = vel_[a];
            Grid& weight = weight_[a];
            const float px = g.x - faceOffset(a, 0), py = g.y - faceOffset(a, 1), pz = g.z - faceOffset(a, 2);
            const float bx = std::floor(px), by = std::floor(py), bz = std::floor(pz);
            const float fx = px - bx, fy = py - by, fz = pz - bz;
            const int i0 = static_cast<int>(bx), j0 = static_cast<int>(by), k0 = static_cast<int>(bz);
            const float va = v[a];
            for (int q = 0; q < 8; ++q) {
                const int i = i0 + (q & 1), j = j0 + ((q >> 1) & 1), k = k0 + (q >> 2);
                if (i < 0 || j < 0 || k < 0 || i >= sum.nx() || j >= sum.ny() || k >= sum.nz()) continue;
                const float w = ((q & 1) ? fx : 1.0f - fx) * (((q >> 1) & 1) ? fy : 1.0f - fy) * ((q >> 2) ? fz : 1.0f - fz);
                const size_t f = sum.index(i, j, k);
                weight.data()[f] += w;
                sum.data()[f] += w * va;
            }
        }
    });
    for (int a = 0; a < 3; ++a) {
        float* v = vel_[a].data();
        const float* w = weight_[a].data();
        pg::parallelFor(vel_[a].size(), 16384, [&](size_t begin, size_t end) {
            for (size_t f = begin; f < end; ++f) v[f] = w[f] > 1e-6f ? v[f] / w[f] : 0.0f;
        });
    }
    // The surface, and which cells are water.
    const float h = domain_.voxel;
    splat(1, sphereWeight_, sphereCentre_, nullptr, sphereFoam_);
    forEachCell(phi_, [&](int i, int j, int k) {
        const size_t c = phi_.index(i, j, k);
        const Vec3 x(static_cast<float>(i) + 0.5f, static_cast<float>(j) + 0.5f, static_cast<float>(k) + 0.5f);
        const float w = sphereWeight_.data()[c];
        const float d = sphereDistance(x, w, Vec3(sphereCentre_[0].data()[c], sphereCentre_[1].data()[c], sphereCentre_[2].data()[c]),
                                       w * kRadius);
        phi_.data()[c] = d * h;
        cells_[c] = d < 0.0f ? FreeSurfaceSolver::Liquid
                             : solidCell_[c] ? FreeSurfaceSolver::Solid : FreeSurfaceSolver::Air;
    });
}

void LiquidSolver::addForces(float dt) {
    const float h = domain_.voxel;
    const float fall = scene_.solver.gravity * dt;
    float* vy = vel_[1].data();
    pg::parallelFor(vel_[1].size(), 16384, [&](size_t begin, size_t end) {
        for (size_t f = begin; f < end; ++f) vy[f] -= fall;
    });
    for (size_t f = 0; f < scene_.forces.size(); ++f) {
        const Force& force = scene_.forces[f];
        const uint32_t seed = force.seed * 7919u + scene_.solver.seed * 31u;
        if (force.kind == ForceKind::Wind) {
            // Wind blows on the surface and on what flies: the faces within a
            // cell and a half of the air.
            detail::addForce(force, seed, domain_, time_, dt, vel_, noise_[f], [&](int a, int i, int j, int k) {
                const int at = a == 0 ? i : a == 1 ? j : k;
                float outer = -kHuge;
                if (at > 0) outer = std::max(outer, phi_.at(i - (a == 0), j - (a == 1), k - (a == 2)));
                if (at < n_[a]) outer = std::max(outer, phi_.at(i, j, k));
                return outer > -1.5f * h ? 1.0f : 0.0f;
            });
        } else {
            detail::addForce(force, seed, domain_, time_, dt, vel_, noise_[f], [](int, int, int, int) { return 1.0f; });
        }
    }
}

void LiquidSolver::project(float dt) {
    const float h = domain_.voxel;
    const int nx = n_[0], ny = n_[1], nz = n_[2];
    // What flows out of each cell of water, through the open part of its faces.
    forEachCell(rhs_, [&](int i, int j, int k) {
        const size_t c = rhs_.index(i, j, k);
        if (cells_[c] != FreeSurfaceSolver::Liquid) {
            rhs_.data()[c] = 0.0f;
            return;
        }
        const float out = open_[0].at(i + 1, j, k) * vel_[0].at(i + 1, j, k) - open_[0].at(i, j, k) * vel_[0].at(i, j, k) +
                          open_[1].at(i, j + 1, k) * vel_[1].at(i, j + 1, k) - open_[1].at(i, j, k) * vel_[1].at(i, j, k) +
                          open_[2].at(i, j, k + 1) * vel_[2].at(i, j, k + 1) - open_[2].at(i, j, k) * vel_[2].at(i, j, k);
        rhs_.data()[c] = -(h / dt) * out;
    });
    pressureSolver_.setSystem(cells_, open_, phi_);
    lastIterations_ += pressureSolver_.solve(pressure_, rhs_, kTolerance, kMaxIterations);

    // The gradient of the pressure, off each face next to water. Past the
    // surface the pressure is 0 where the surface crosses the face; past an
    // open side, on the side.
    const float scale = dt / h;
    const int n[3] = {nx, ny, nz};
    for (int a = 0; a < 3; ++a) {
        Grid& v = vel_[a];
        const Grid& weight = weight_[a];
        uint8_t* valid = valid_[a].data();
        forEachCell(v, [&](int i, int j, int k) {
            const size_t f = v.index(i, j, k);
            const float open = open_[a].at(i, j, k);
            if (open <= 0.0f) {  // a wall: nothing through it
                v.data()[f] = 0.0f;
                valid[f] = 0;
                return;
            }
            const int at = a == 0 ? i : a == 1 ? j : k;
            const bool hasBehind = at > 0, hasAhead = at < n[a];
            const size_t behind = hasBehind ? phi_.index(i - (a == 0), j - (a == 1), k - (a == 2)) : 0;
            const size_t ahead = hasAhead ? phi_.index(i, j, k) : 0;
            const bool wetBehind = hasBehind && cells_[behind] == FreeSurfaceSolver::Liquid;
            const bool wetAhead = hasAhead && cells_[ahead] == FreeSurfaceSolver::Liquid;
            if (!wetBehind && !wetAhead) {
                valid[f] = weight.data()[f] > 1e-6f;  // spray: as the particles fly
                return;
            }
            float drop;  // pressure ahead - pressure behind
            if (wetBehind && wetAhead) {
                drop = pressure_.data()[ahead] - pressure_.data()[behind];
            } else if (wetBehind) {
                const float theta = hasAhead ? FreeSurfaceSolver::surfaceFraction(phi_.data()[behind], phi_.data()[ahead]) : 0.5f;
                drop = -pressure_.data()[behind] / theta;
            } else {
                const float theta = hasBehind ? FreeSurfaceSolver::surfaceFraction(phi_.data()[ahead], phi_.data()[behind]) : 0.5f;
                drop = pressure_.data()[ahead] / theta;
            }
            v.data()[f] -= scale * drop;
            valid[f] = 1;
        });
    }
}

void LiquidSolver::extrapolate(int layers) {
    for (int a = 0; a < 3; ++a) {
        Grid& v = vel_[a];
        std::vector<uint8_t>& valid = valid_[a];
        std::vector<uint8_t> next;
        const int fx = v.nx(), fy = v.ny(), fz = v.nz();
        const size_t sy = static_cast<size_t>(fx), sz = sy * static_cast<size_t>(fy);
        for (int layer = 0; layer < layers; ++layer) {
            next = valid;
            // Only faces not yet known are written, only known ones read.
            forEachCell(v, [&](int i, int j, int k) {
                const size_t f = v.index(i, j, k);
                if (valid[f]) return;
                float sum = 0.0f;
                int count = 0;
                auto take = [&](size_t g) {
                    if (valid[g]) {
                        sum += v.data()[g];
                        ++count;
                    }
                };
                if (i > 0) take(f - 1);
                if (i < fx - 1) take(f + 1);
                if (j > 0) take(f - sy);
                if (j < fy - 1) take(f + sy);
                if (k > 0) take(f - sz);
                if (k < fz - 1) take(f + sz);
                if (count > 0) {
                    v.data()[f] = sum / static_cast<float>(count);
                    next[f] = 1;
                }
            });
            valid.swap(next);
        }
        // Walls stay walls.
        const float* open = open_[a].data();
        float* vv = v.data();
        for (size_t f = 0; f < v.size(); ++f) {
            if (open[f] <= 0.0f) vv[f] = 0.0f;
        }
    }
}

Vec3 LiquidSolver::sampleVelocity(const Grid* vel, const Vec3& g) const {
    return {vel[0].sample(g.x + 0.5f, g.y, g.z), vel[1].sample(g.x, g.y + 0.5f, g.z), vel[2].sample(g.x, g.y, g.z + 0.5f)};
}

Vec3 LiquidSolver::velocityAt(const Vec3& p) const { return sampleVelocity(vel_, toCells(p)); }

void LiquidSolver::toParticles(float dt) {
    const float flip = scene_.solver.flip;
    const float h = domain_.voxel;
    const float fade = std::exp(-dt / kFoamLife);
    const int nx = n_[0], ny = n_[1], nz = n_[2];
    pg::parallelFor(position_.size(), 4096, [&](size_t begin, size_t end) {
        for (size_t p = begin; p < end; ++p) {
            const Vec3& g = cellPos_[p];
            const Vec3 now = sampleVelocity(vel_, g);
            const Vec3 before = sampleVelocity(old_, g);
            const Vec3 v = (velocity_[p] + (now - before)) * flip + now * (1.0f - flip);
            velocity_[p] = v;
            // Foam: spray -- a particle flying with few others round it --
            // and water at the surface thrown about fast; it fades.
            const int i = std::clamp(static_cast<int>(g.x), 0, nx - 1);
            const int j = std::clamp(static_cast<int>(g.y), 0, ny - 1);
            const int k = std::clamp(static_cast<int>(g.z), 0, nz - 1);
            const size_t c = phi_.index(i, j, k);
            const float speed = length(v);
            float white = 0.0f;
            if (phi_.data()[c] > -h) white = detail::smoothstep(2.5f, 5.0f, speed);
            const bool sparse = cellStart_[c + 1] - cellStart_[c] < 4;
            if (sparse && cells_[c] != FreeSurfaceSolver::Liquid) {
                white = std::max(white, 0.9f * detail::smoothstep(1.0f, 2.5f, speed));
            }
            foam_[p] = std::max(foam_[p] * fade, white);
        }
    });
}

void LiquidSolver::advect(float dt) {
    const float h = domain_.voxel;
    const float margin = kMargin * h;
    const Vec3 lo = domain_.origin(), hi = lo + domain_.size();
    const bool closedSides = scene_.solver.closedSides;
    std::vector<uint8_t> gone(position_.size(), 0);
    pg::parallelFor(position_.size(), 4096, [&](size_t begin, size_t end) {
        for (size_t p = begin; p < end; ++p) {
            Vec3 x = position_[p];
            Vec3 v = velocity_[p];
            // Second-order Runge-Kutta through the grid's velocity.
            const Vec3 mid = x + velocityAt(x) * (0.5f * dt);
            x = x + velocityAt(mid) * dt;
            // Out of the solids, and no longer moving into them.
            if (anySolid_) {
                const float d = solidDistance(x);
                if (d < margin) {
                    const Vec3 g = toCells(x);
                    const float e = 0.5f;
                    Vec3 normal(solidPhi_.sample(g.x + 0.5f + e, g.y + 0.5f, g.z + 0.5f) -
                                    solidPhi_.sample(g.x + 0.5f - e, g.y + 0.5f, g.z + 0.5f),
                                solidPhi_.sample(g.x + 0.5f, g.y + 0.5f + e, g.z + 0.5f) -
                                    solidPhi_.sample(g.x + 0.5f, g.y + 0.5f - e, g.z + 0.5f),
                                solidPhi_.sample(g.x + 0.5f, g.y + 0.5f, g.z + 0.5f + e) -
                                    solidPhi_.sample(g.x + 0.5f, g.y + 0.5f, g.z + 0.5f - e));
                    normal = normalize(normal);
                    if (length(normal) > 0.5f) {
                        x = x + normal * (margin - d);
                        const float into = dot(v, normal);
                        if (into < 0.0f) v = v - normal * into;
                    }
                }
            }
            // The floor, and the walls of a tank.
            if (x.y < lo.y + margin) {
                x.y = lo.y + margin;
                v.y = std::max(v.y, 0.0f);
            }
            if (closedSides) {
                for (const int a : {0, 2}) {
                    if (x[a] < lo[a] + margin) {
                        x[a] = lo[a] + margin;
                        v[a] = std::max(v[a], 0.0f);
                    } else if (x[a] > hi[a] - margin) {
                        x[a] = hi[a] - margin;
                        v[a] = std::min(v[a], 0.0f);
                    }
                }
            } else if (x.x < lo.x || x.x > hi.x || x.z < lo.z || x.z > hi.z) {
                gone[p] = 1;
            }
            if (x.y > hi.y) gone[p] = 1;  // up out of the domain
            if (!std::isfinite(x.x + x.y + x.z + v.x + v.y + v.z)) gone[p] = 1;
            position_[p] = x;
            velocity_[p] = v;
        }
    });
    // Those gone, taken out; the rest keep their order.
    size_t kept = 0;
    for (size_t p = 0; p < position_.size(); ++p) {
        if (gone[p]) continue;
        position_[kept] = position_[p];
        velocity_[kept] = velocity_[p];
        foam_[kept] = foam_[p];
        ++kept;
    }
    position_.resize(kept);
    velocity_.resize(kept);
    foam_.resize(kept);
}

// --- what is drawn ---------------------------------------------------------------------

double LiquidSolver::volume() const {
    const double h = domain_.voxel;
    return static_cast<double>(position_.size()) * h * h * h / 8.0 * 1000.0;
}

void LiquidSolver::surfaceField(int factor, float band, Grid& distance, Grid& foam) const {
    factor = std::clamp(factor, 1, 2);  // twice as fine at most: bySlabs() needs it
    const int fx = n_[0] * factor, fy = n_[1] * factor, fz = n_[2] * factor;
    distance = Grid(fx, fy, fz, band);
    foam = Grid(fx, fy, fz, 0.0f);
    if (position_.empty()) return;
    Grid weight, centre[3], radius;
    splat(factor, weight, centre, &radius, foam);
    const float h = domain_.voxel;
    const float inv = 1.0f / static_cast<float>(factor);
    forEachCell(distance, [&](int i, int j, int k) {
        const size_t c = distance.index(i, j, k);
        const float w = weight.data()[c];
        if (w <= 0.0f) return;
        const Vec3 x((static_cast<float>(i) + 0.5f) * inv, (static_cast<float>(j) + 0.5f) * inv,
                     (static_cast<float>(k) + 0.5f) * inv);
        const float d =
            sphereDistance(x, w, Vec3(centre[0].data()[c], centre[1].data()[c], centre[2].data()[c]), radius.data()[c]) * h;
        distance.data()[c] = std::clamp(d, -band, band);
        foam.data()[c] = std::clamp(foam.data()[c] / w, 0.0f, 1.0f);
    });
    // Both smoothed -- 1 2 1 along each axis -- or the particles show
    // through: as bumps, as a surface that frays where they are few, as
    // speckled foam. The foam twice as much: it is a haze, not a surface.
    for (Grid* field : {&distance, &foam}) {
        Grid other = *field;
        const int passes = field == &foam ? 6 : 3;
        for (int pass = 0; pass < passes; ++pass) {
            const int a = pass % 3;
            const Grid& from = pass % 2 == 0 ? *field : other;
            Grid& to = pass % 2 == 0 ? other : *field;
            const int n = a == 0 ? fx : a == 1 ? fy : fz;
            const size_t stride =
                a == 0 ? 1 : a == 1 ? static_cast<size_t>(fx) : static_cast<size_t>(fx) * static_cast<size_t>(fy);
            forEachCell(from, [&](int i, int j, int k) {
                const size_t c = from.index(i, j, k);
                const int at = a == 0 ? i : a == 1 ? j : k;
                const float left = at > 0 ? from.data()[c - stride] : from.data()[c];
                const float right = at < n - 1 ? from.data()[c + stride] : from.data()[c];
                to.data()[c] = 0.25f * left + 0.5f * from.data()[c] + 0.25f * right;
            });
        }
        if (passes % 2 == 1) *field = other;  // the last pass went into `other`
    }
    // And made a distance again, one a ray can step along: in cells of this
    // grid, then world units.
    const float cell = h * inv;
    redistance(distance, cell, band / cell);
}

}  // namespace pg::sim
