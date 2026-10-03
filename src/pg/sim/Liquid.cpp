#include "pg/sim/Liquid.h"

#include "pg/core/Parallel.h"
#include "pg/sim/Shared.h"
#include "pg/sim/State.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

namespace pg::sim {

using detail::faceOffset;
using detail::fix;
using detail::forEachCell;

namespace {

using Clock = std::chrono::steady_clock;

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
/// How much a wind moves the body of the water, for what it moves spray:
/// air, a thousandth as heavy, pushes on its surface alone.
constexpr float kWindOnWater = 1.2e-3f;
/// Particles in the 3 x 3 x 3 cells round one (a full cell holds 8): below
/// the first, spray the wind carries; above the second, the body of the
/// water.
constexpr float kSprayCrowd = 12.0f, kBodyCrowd = 40.0f;

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

/// f(p) for every particle p, sorted into cells -- the particles of the
/// layer of cells k from `layerStart`[k] on -- in parallel over slabs two
/// layers of cells thick: the even slabs, then the odd ones. A particle
/// writes to the grid no further than the layers either side of its own, so
/// two slabs of a colour -- a slab apart -- never write the same place, and
/// each place takes what it takes from the slabs in the same order, whatever
/// the threads.
template <class F>
void bySlabs(const std::vector<uint32_t>& layerStart, int nz, const F& f) {
    const int slabs = (nz + 1) / 2;
    for (int colour = 0; colour < 2; ++colour) {
        const size_t count = static_cast<size_t>((slabs - colour + 1) / 2);
        pg::parallelFor(count, 1, [&](size_t begin, size_t end) {
            for (size_t t = begin; t < end; ++t) {
                const int z0 = 2 * (colour + 2 * static_cast<int>(t)), z1 = std::min(z0 + 2, nz);
                for (uint32_t p = layerStart[static_cast<size_t>(z0)]; p < layerStart[static_cast<size_t>(z1)]; ++p) f(p);
            }
        });
    }
}

/// Where cell (i, j, k) of a sparse grid is in its data(), -1 when its tile
/// is not kept.
inline int64_t find(const SparseGrid& g, int i, int j, int k) {
    const int32_t s = g.slotOf(i, j, k);
    return s < 0 ? -1 : static_cast<int64_t>(s) * Tiles::kCells + static_cast<int64_t>(SparseGrid::local(i, j, k));
}

/// The tiles of a grid twice as fine as one of `tiles`: each tile the eight
/// under it.
std::shared_ptr<const Tiles> finer(const Tiles& tiles) {
    const int n[3] = {2 * tiles.nx(), 2 * tiles.ny(), 2 * tiles.nz()};
    int t[3];
    for (int a = 0; a < 3; ++a) t[a] = (n[a] + Tiles::kSide - 1) / Tiles::kSide;
    std::vector<uint8_t> state(static_cast<size_t>(t[0]) * static_cast<size_t>(t[1]) * static_cast<size_t>(t[2]),
                               Tiles::Off);
    for (const uint32_t tile : tiles.stored()) {
        int c[3];
        tiles.corner(tile, c[0], c[1], c[2]);
        for (int q = 0; q < 8; ++q) {
            const int x = c[0] / 4 + (q & 1), y = c[1] / 4 + ((q >> 1) & 1), z = c[2] / 4 + (q >> 2);
            if (x >= t[0] || y >= t[1] || z >= t[2]) continue;
            state[static_cast<size_t>(x) +
                  static_cast<size_t>(t[0]) * (static_cast<size_t>(y) + static_cast<size_t>(t[1]) * static_cast<size_t>(z))] =
                Tiles::Whole;
        }
    }
    return std::make_shared<const Tiles>(n[0], n[1], n[2], std::move(state), -1);
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
    v.size = fix(v.size, 0.1f, 1000.0f, ds.size);
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
    // No tiles yet: sortParticles() keeps those of the sources and the water.
    const auto none = std::make_shared<const Tiles>(n_[0], n_[1], n_[2], std::vector<uint8_t>(), -1);
    phi_ = SparseGrid(none, kReach * domain_.voxel);  // no water anywhere near: as far as a particle reaches
    pressure_ = SparseGrid(none);
    retile(none);
    layerStart_.assign(static_cast<size_t>(n_[2]) + 1, 0);
    noise_.assign(scene_.forces.size(), {});
    filled_.assign(scene_.sources.size(), 0);
    updateSolids();
    sortParticles();
}

void LiquidSolver::retile(std::shared_ptr<const Tiles> tiles) {
    tiles_ = std::move(tiles);
    // The velocity -- what the questions between steps read -- the pressure,
    // the next solve's first guess, and the surface go on where the tiles
    // do; the rest each substep makes again.
    for (int a = 0; a < 3; ++a) {
        faces_[a] = Tiles::faces(*tiles_, a);
        vel_[a].retile(faces_[a]);
        old_[a] = SparseGrid(faces_[a]);
        weight_[a] = SparseGrid(faces_[a]);
        valid_[a].assign(vel_[a].size(), 0);
    }
    phi_.retile(tiles_);
    pressure_.retile(tiles_);
    rhs_ = SparseGrid(tiles_);
    sphereWeight_ = SparseGrid(tiles_);
    for (SparseGrid& g : sphereCentre_) g = SparseGrid(tiles_);
    sphereFoam_ = SparseGrid(tiles_);
    cells_.assign(phi_.size(), FreeSurfaceSolver::Air);
}

void LiquidSolver::saveState(StateWriter& out) const {
    for (int a = 0; a < 3; ++a) out.pod(static_cast<int32_t>(n_[a]));
    out.pod(static_cast<int32_t>(frame_));
    out.pod(time_);
    out.pod(substepCount_);
    out.pod(made_);
    out.pod(static_cast<int32_t>(lastSubsteps_));
    out.pod(static_cast<int32_t>(lastIterations_));
    out.list(position_);
    out.list(velocity_);
    out.list(foam_);
    out.list(id_);
    out.list(filled_);
    // The grids, on their tiles.
    out.tiles(*tiles_);
    for (int a = 0; a < 3; ++a) out.values(vel_[a]);
    out.values(phi_);
    out.values(pressure_);
}

bool LiquidSolver::loadState(StateReader& in) {
    int32_t n[3] = {0, 0, 0}, frame = 0, substeps = 0, iterations = 0;
    float time = 0.0f;
    uint32_t count = 0, made = 0;
    std::vector<Vec3> position, velocity;
    std::vector<float> foam;
    std::vector<uint32_t> id;
    std::vector<uint8_t> filled;
    std::shared_ptr<const Tiles> tiles;
    if (!in.pod(n[0]) || !in.pod(n[1]) || !in.pod(n[2]) || !in.pod(frame) || !in.pod(time) || !in.pod(count) ||
        !in.pod(made) || !in.pod(substeps) || !in.pod(iterations) || !in.list(position) || !in.list(velocity) ||
        !in.list(foam) || !in.list(id) || !in.list(filled) || !in.tiles(tiles)) {
        return false;
    }
    // Of this grid.
    if (n[0] != n_[0] || n[1] != n_[1] || n[2] != n_[2] || tiles->axis() != -1 || tiles->nx() != n_[0] ||
        tiles->ny() != n_[1] || tiles->nz() != n_[2]) {
        return in.fail();
    }
    // Read aside: a state cut short leaves this solver as it was.
    SparseGrid vel[3];
    for (int a = 0; a < 3; ++a) {
        vel[a] = SparseGrid(Tiles::faces(*tiles, a));
        if (!in.values(vel[a])) return false;
    }
    SparseGrid phi(tiles, phi_.background()), pressure(tiles);
    if (!in.values(phi) || !in.values(pressure)) return false;
    // And whole.
    const size_t particles = position.size();
    const bool fits = velocity.size() == particles && foam.size() == particles && id.size() == particles &&
                      filled.size() == scene_.sources.size();
    if (!fits) return in.fail();
    position_ = std::move(position);
    velocity_ = std::move(velocity);
    foam_ = std::move(foam);
    id_ = std::move(id);
    filled_ = std::move(filled);
    retile(tiles);
    // Onto the faces retile() made, which the other face grids share.
    for (int a = 0; a < 3; ++a) std::copy(vel[a].values().begin(), vel[a].values().end(), vel_[a].data());
    phi_ = std::move(phi);
    pressure_ = std::move(pressure);
    frame_ = frame;
    time_ = time;
    substepCount_ = count;
    made_ = made;
    lastSubsteps_ = substeps;
    lastIterations_ = iterations;
    sortParticles();  // as a step leaves them: in order, for the questions asked between steps
    return true;
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

void LiquidSolver::setScene(const LiquidScene& scene) {
    LiquidScene next = scene;
    // The grid stays as it is.
    next.solver.size = scene_.solver.size;
    next.solver.resolution = scene_.solver.resolution;
    next.solver.closedSides = scene_.solver.closedSides;
    next.solver.sparse = scene_.solver.sparse;
    next = next.sanitized();
    const bool walls = next.colliders != scene_.colliders;
    scene_ = next;
    noise_.resize(scene_.forces.size());
    filled_.resize(scene_.sources.size(), 0);
    if (walls) updateSolids();
}

Vec3 LiquidSolver::solidVelocity(const Vec3& p) const {
    if (!movingSolid_) return {};
    size_t nearest = 0;
    float best = 1e30f;
    for (size_t c = 0; c < shapes_.size(); ++c) {
        const float d = shapes_[c].distance(p);
        if (d < best) {
            best = d;
            nearest = c;
        }
    }
    return scene_.colliders[nearest].velocityAt(p);
}

void LiquidSolver::updateSolids() {
    const auto started = Clock::now();
    struct Count {
        double& ms;
        Clock::time_point t0;
        ~Count() { ms += std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); }
    } count{times_.solids, started};
    const int nx = n_[0], ny = n_[1], nz = n_[2];
    const float h = domain_.voxel;
    shapes_.clear();
    for (const Collider& c : scene_.colliders) shapes_.push_back(c.instance());
    // The colliders' distance at the grid's corners, cut off a few cells
    // away: only near a solid does it matter -- and only there is it kept,
    // in the tiles of corners round each collider's box.
    const float far = 3.0f * h;
    const int cn[3] = {nx + 1, ny + 1, nz + 1};
    int ct[3];
    for (int a = 0; a < 3; ++a) ct[a] = (cn[a] + Tiles::kSide - 1) / Tiles::kSide;
    auto cornerTile = [&](int a, int b, int c) {
        return static_cast<size_t>(a) +
               static_cast<size_t>(ct[0]) * (static_cast<size_t>(b) + static_cast<size_t>(ct[1]) * static_cast<size_t>(c));
    };
    struct Box {
        int lo[3] = {0, 0, 0}, hi[3] = {0, 0, 0};
    };
    std::vector<Box> boxes(shapes_.size());
    std::vector<uint8_t> corners(static_cast<size_t>(ct[0]) * static_cast<size_t>(ct[1]) * static_cast<size_t>(ct[2]),
                                 Tiles::Off);
    for (size_t c = 0; c < shapes_.size(); ++c) {
        Vec3 lo, hi;
        shapes_[c].bounds(lo, hi);
        const Vec3 a = toCells(lo), b = toCells(hi);
        Box& box = boxes[c];
        for (int d = 0; d < 3; ++d) {
            box.lo[d] = std::clamp(static_cast<int>(std::floor(a[d])) - 3, 0, n_[d] + 1);
            box.hi[d] = std::clamp(static_cast<int>(std::ceil(b[d])) + 4, 0, n_[d] + 1);
        }
        if (box.lo[0] >= box.hi[0] || box.lo[1] >= box.hi[1] || box.lo[2] >= box.hi[2]) continue;
        for (int z = box.lo[2] / Tiles::kSide; z <= (box.hi[2] - 1) / Tiles::kSide; ++z) {
            for (int y = box.lo[1] / Tiles::kSide; y <= (box.hi[1] - 1) / Tiles::kSide; ++y) {
                for (int x = box.lo[0] / Tiles::kSide; x <= (box.hi[0] - 1) / Tiles::kSide; ++x) {
                    corners[cornerTile(x, y, z)] = Tiles::Whole;
                }
            }
        }
    }
    solidPhi_ = SparseGrid(std::make_shared<const Tiles>(cn[0], cn[1], cn[2], std::move(corners), -1), far);
    for (size_t c = 0; c < shapes_.size(); ++c) {
        const Box& box = boxes[c];
        const ShapeInstance& shape = shapes_[c];
        detail::forEachIn(box.lo[0], box.hi[0], box.lo[1], box.hi[1], box.lo[2], box.hi[2], [&](int i, int j, int k) {
            const float d = std::min(shape.distance(worldAt(static_cast<float>(i), static_cast<float>(j),
                                                            static_cast<float>(k))),
                                     far);
            float& v = solidPhi_.ref(i, j, k);
            v = std::min(v, d);
        });
    }
    const std::vector<float>& distances = solidPhi_.values();
    anySolid_ = std::any_of(distances.begin(), distances.end(), [](float v) { return v < 0.0f; });

    // The tiles of cells with a corner kept: the rest are clear of every
    // solid -- open, holding none.
    const int tn[3] = {(nx + Tiles::kSide - 1) / Tiles::kSide, (ny + Tiles::kSide - 1) / Tiles::kSide,
                       (nz + Tiles::kSide - 1) / Tiles::kSide};
    std::vector<uint8_t> state(static_cast<size_t>(tn[0]) * static_cast<size_t>(tn[1]) * static_cast<size_t>(tn[2]),
                               Tiles::Off);
    if (anySolid_) {
        const Tiles& kept = solidPhi_.tiles();
        for (int k = 0; k < tn[2]; ++k) {
            for (int j = 0; j < tn[1]; ++j) {
                for (int i = 0; i < tn[0]; ++i) {
                    // A tile's cells reach the corners of its own tile of
                    // corners, and the first of the next ones.
                    bool near = false;
                    for (int q = 0; q < 8 && !near; ++q) {
                        const int x = i + (q & 1), y = j + ((q >> 1) & 1), z = k + (q >> 2);
                        near = x < ct[0] && y < ct[1] && z < ct[2] && kept.slot(cornerTile(x, y, z)) >= 0;
                    }
                    if (near) {
                        state[static_cast<size_t>(i) +
                              static_cast<size_t>(tn[0]) * (static_cast<size_t>(j) + static_cast<size_t>(tn[1]) * static_cast<size_t>(k))] =
                            Tiles::Whole;
                    }
                }
            }
        }
    }
    solidTiles_ = std::make_shared<const Tiles>(nx, ny, nz, std::move(state), -1);

    // How open each face is: what the solids leave of it, the floor always
    // closed, the sides closed round a tank, the top open to the sky.
    const bool closedSides = scene_.solver.closedSides;
    auto wall = [&](int a, int i, int j, int k) {
        const int f = a == 0 ? i : a == 1 ? j : k;
        return (a == 1 && f == 0) || (a != 1 && closedSides && (f == 0 || f == n_[a]));
    };
    for (int a = 0; a < 3; ++a) {
        solidOpen_[a] = SparseGrid(Tiles::faces(*solidTiles_, a), 1.0f);
        SparseGrid& open = solidOpen_[a];
        forEachCounted(open.tiles(), [&](int i, int j, int k, size_t f) {
            if (wall(a, i, j, k)) {
                open.data()[f] = 0.0f;
                return;
            }
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
            open.data()[f] = left < kLeastOpen ? 0.0f : left;
        });
    }
    solidCell_.assign(solidTiles_->stored().size() * Tiles::kCells, 0);
    forEachCounted(*solidTiles_, [&](int i, int j, int k, size_t c) {
        float sum = 0.0f;
        for (int q = 0; q < 8; ++q) sum += solidPhi_.at(i + (q & 1), j + ((q >> 1) & 1), k + (q >> 2));
        solidCell_[c] = sum < 0.0f ? 1 : 0;
    });
    // Moving solids: on each face they cover, the velocity of the nearest
    // one there -- what the water next to it is pushed with.
    movingSolid_ = anySolid_ && std::any_of(scene_.colliders.begin(), scene_.colliders.end(),
                                            [](const Collider& c) { return c.moves(); });
    for (int a = 0; a < 3; ++a) {
        solidVel_[a] = SparseGrid(Tiles::faces(*solidTiles_, a));
        if (!movingSolid_) continue;
        SparseGrid& vel = solidVel_[a];
        forEachCounted(vel.tiles(), [&](int i, int j, int k, size_t f) {
            if (open(a, i, j, k) >= 1.0f || wall(a, i, j, k)) return;
            const Vec3 p = worldAt(static_cast<float>(i) + (a == 0 ? 0.0f : 0.5f), static_cast<float>(j) + (a == 1 ? 0.0f : 0.5f),
                                   static_cast<float>(k) + (a == 2 ? 0.0f : 0.5f));
            vel.data()[f] = solidVelocity(p)[a];
        });
    }
}

float LiquidSolver::open(int axis, int i, int j, int k) const {
    const int f = axis == 0 ? i : axis == 1 ? j : k;
    if (axis == 1 && f == 0) return 0.0f;  // the floor
    if (axis != 1 && scene_.solver.closedSides && (f == 0 || f == n_[axis])) return 0.0f;  // a wall
    return solidOpen_[axis].at(i, j, k);  // 1 away from the solids
}

bool LiquidSolver::solidCellAt(int i, int j, int k) const {
    const int32_t s = solidTiles_->slot(solidTiles_->tileOf(i, j, k));
    return s >= 0 && solidCell_[static_cast<size_t>(s) * Tiles::kCells + SparseGrid::local(i, j, k)] != 0;
}

float LiquidSolver::solidVelocityAt(int axis, int i, int j, int k) const {
    return movingSolid_ ? solidVel_[axis].at(i, j, k) : 0.0f;
}

void LiquidSolver::boxCells(const ShapeInstance& shape, int c0[3], int c1[3]) const {
    Vec3 lo, hi;
    shape.bounds(lo, hi);
    const Vec3 a = toCells(lo), b = toCells(hi);
    for (int d = 0; d < 3; ++d) {
        c0[d] = std::clamp(static_cast<int>(std::floor(a[d])), 0, n_[d]);
        c1[d] = std::clamp(static_cast<int>(std::ceil(b[d])), 0, n_[d]);
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
    const auto t0 = Clock::now();
    sortParticles();  // for the questions asked between steps
    times_.sort += std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    ++frame_;
}

void LiquidSolver::substep(float dt) {
    auto t0 = Clock::now();
    auto lap = [&](double& stage) {
        const auto now = Clock::now();
        stage += std::chrono::duration<double, std::milli>(now - t0).count();
        t0 = now;
    };
    sortParticles();
    lap(times_.sort);
    emit();
    lap(times_.emit);
    toGrid();
    // The velocity the particles brought, carried out past the water -- the
    // FLIP update subtracts it, so it has to be there wherever a particle
    // may look.
    for (int a = 0; a < 3; ++a) {
        const float* w = weight_[a].data();
        uint8_t* valid = valid_[a].data();
        std::fill(valid_[a].begin(), valid_[a].end(), uint8_t{0});
        forEachCounted(*faces_[a], [&](int i, int j, int k, size_t f) {
            valid[f] = w[f] > 1e-6f && open(a, i, j, k) > 0.0f;
        });
    }
    lap(times_.toGrid);
    extrapolate(kExtrapolation);
    for (int a = 0; a < 3; ++a) std::copy(vel_[a].values().begin(), vel_[a].values().end(), old_[a].data());
    lap(times_.extrapolate);
    addForces(dt);
    lap(times_.forces);
    project(dt);
    lap(times_.project);
    extrapolate(kExtrapolation);
    lap(times_.extrapolate);
    toParticles(dt);
    lap(times_.toParticles);
    advect(dt);
    lap(times_.advect);
    time_ += dt;
    ++substepCount_;
}

void LiquidSolver::sortParticles() {
    const int nx = n_[0], ny = n_[1], nz = n_[2];
    const size_t n = position_.size();
    const int tn[3] = {(nx + Tiles::kSide - 1) / Tiles::kSide, (ny + Tiles::kSide - 1) / Tiles::kSide,
                       (nz + Tiles::kSide - 1) / Tiles::kSide};
    auto number = [&](int a, int b, int c) {
        return static_cast<size_t>(a) +
               static_cast<size_t>(tn[0]) * (static_cast<size_t>(b) + static_cast<size_t>(tn[1]) * static_cast<size_t>(c));
    };
    // Each particle's cell: its tile, and where in the tile it is.
    constexpr uint32_t kLocal = Tiles::kCells - 1;
    std::vector<uint32_t> where(n);
    pg::parallelFor(n, 8192, [&](size_t begin, size_t end) {
        for (size_t p = begin; p < end; ++p) {
            const Vec3 g = toCells(position_[p]);
            const int i = std::clamp(static_cast<int>(std::floor(g.x)), 0, nx - 1);
            const int j = std::clamp(static_cast<int>(std::floor(g.y)), 0, ny - 1);
            const int k = std::clamp(static_cast<int>(std::floor(g.z)), 0, nz - 1);
            where[p] = static_cast<uint32_t>(number(i >> Tiles::kLog, j >> Tiles::kLog, k >> Tiles::kLog) << 9) |
                       static_cast<uint32_t>(SparseGrid::local(i, j, k));
        }
    });
    // The tiles to keep: those with water in them and those of the sources
    // about to pour, and every tile round them -- or, not sparse, every one.
    std::shared_ptr<const Tiles> tiles = tiles_;
    if (!scene_.solver.sparse) {
        if (!tiles_->all()) tiles = std::make_shared<const Tiles>(nx, ny, nz);
    } else {
        std::vector<uint8_t> busy(tiles_->tileCount(), 0);
        for (size_t p = 0; p < n; ++p) busy[where[p] >> 9] = 1;
        for (size_t s = 0; s < scene_.sources.size(); ++s) {
            const WaterSource& src = scene_.sources[s];
            if (!src.activeAt(time_) || (src.mode == WaterMode::Fill && filled_[s])) continue;
            int c0[3], c1[3];
            boxCells(src.instance(), c0, c1);
            if (c0[0] >= c1[0] || c0[1] >= c1[1] || c0[2] >= c1[2]) continue;
            for (int z = c0[2] >> Tiles::kLog; z <= (c1[2] - 1) >> Tiles::kLog; ++z) {
                for (int y = c0[1] >> Tiles::kLog; y <= (c1[1] - 1) >> Tiles::kLog; ++y) {
                    for (int x = c0[0] >> Tiles::kLog; x <= (c1[0] - 1) >> Tiles::kLog; ++x) busy[number(x, y, z)] = 1;
                }
            }
        }
        std::vector<uint8_t> state(busy.size(), Tiles::Off);
        for (int c = 0; c < tn[2]; ++c) {
            for (int b = 0; b < tn[1]; ++b) {
                for (int a = 0; a < tn[0]; ++a) {
                    if (!busy[number(a, b, c)]) continue;
                    for (int z = std::max(c - 1, 0); z <= std::min(c + 1, tn[2] - 1); ++z) {
                        for (int y = std::max(b - 1, 0); y <= std::min(b + 1, tn[1] - 1); ++y) {
                            for (int x = std::max(a - 1, 0); x <= std::min(a + 1, tn[0] - 1); ++x) {
                                state[number(x, y, z)] = Tiles::Whole;
                            }
                        }
                    }
                }
            }
        }
        if (state != tiles_->states()) tiles = std::make_shared<const Tiles>(nx, ny, nz, std::move(state), -1);
    }
    if (tiles != tiles_) retile(std::move(tiles));

    // A stable counting sort, in the order of the cells of the whole grid --
    // x fastest, then y, then z -- whatever the tiles: the particles of a
    // cell keep their order.
    const Tiles& t = *tiles_;
    const size_t kept = t.stored().size() * Tiles::kCells;
    std::vector<uint32_t> index(n);
    pg::parallelFor(n, 8192, [&](size_t begin, size_t end) {
        for (size_t p = begin; p < end; ++p) {
            index[p] = static_cast<uint32_t>(t.slot(where[p] >> 9)) * Tiles::kCells + (where[p] & kLocal);
        }
    });
    cellCount_.assign(kept, 0u);
    for (size_t p = 0; p < n; ++p) ++cellCount_[index[p]];
    // The kept cells of layer k in the grid's order: row by row, a row tile
    // by tile along x.
    auto walk = [&](int k, const auto& f) {
        const int tk = k >> Tiles::kLog;
        const size_t lk = static_cast<size_t>(k & (Tiles::kSide - 1));
        for (int j = 0; j < ny; ++j) {
            const size_t row = Tiles::kSide * (static_cast<size_t>(j & (Tiles::kSide - 1)) + Tiles::kSide * lk);
            for (int a = 0; a < tn[0]; ++a) {
                const int32_t s = t.slot(number(a, j >> Tiles::kLog, tk));
                if (s < 0) continue;
                const size_t first = static_cast<size_t>(s) * Tiles::kCells + row;
                for (size_t c = first; c < first + Tiles::kSide; ++c) f(c);
            }
        }
    };
    std::vector<uint32_t> layerCount(static_cast<size_t>(nz));
    pg::parallelFor(static_cast<size_t>(nz), 1, [&](size_t begin, size_t end) {
        for (size_t k = begin; k < end; ++k) {
            uint32_t sum = 0;
            walk(static_cast<int>(k), [&](size_t c) { sum += cellCount_[c]; });
            layerCount[k] = sum;
        }
    });
    layerStart_.assign(static_cast<size_t>(nz) + 1, 0u);
    for (size_t k = 0; k < static_cast<size_t>(nz); ++k) layerStart_[k + 1] = layerStart_[k] + layerCount[k];
    cellStart_.resize(kept);
    pg::parallelFor(static_cast<size_t>(nz), 1, [&](size_t begin, size_t end) {
        for (size_t k = begin; k < end; ++k) {
            uint32_t at = layerStart_[k];
            walk(static_cast<int>(k), [&](size_t c) {
                cellStart_[c] = at;
                at += cellCount_[c];
            });
        }
    });
    std::vector<uint32_t> to(n);
    {
        std::vector<uint32_t> next(cellStart_);
        for (size_t p = 0; p < n; ++p) to[p] = next[index[p]]++;
    }
    std::vector<Vec3> position(n), velocity(n);
    std::vector<float> foam(n);
    std::vector<uint32_t> id(n);
    cellPos_.resize(n);
    pg::parallelFor(n, 8192, [&](size_t begin, size_t end) {
        for (size_t p = begin; p < end; ++p) {
            position[to[p]] = position_[p];
            velocity[to[p]] = velocity_[p];
            foam[to[p]] = foam_[p];
            id[to[p]] = id_[p];
            cellPos_[to[p]] = toCells(position_[p]);
        }
    });
    position_.swap(position);
    velocity_.swap(velocity);
    foam_.swap(foam);
    id_.swap(id);
}

void LiquidSolver::emit() {
    const float h = domain_.voxel;
    bool added = false;
    for (size_t s = 0; s < scene_.sources.size(); ++s) {
        const WaterSource& src = scene_.sources[s];
        if (!src.activeAt(time_)) continue;
        if (src.mode == WaterMode::Fill) {
            if (filled_[s]) continue;
            filled_[s] = 1;
        }
        const ShapeInstance shape = src.instance();
        const Vec3 jet = shape.turn().apply(src.velocity) + src.moving;
        Vec3 lo, hi;
        shape.bounds(lo, hi);
        int c0[3], c1[3];
        boxCells(shape, c0, c1);
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
        // at a place in it the hash picks. (The cells' tiles are kept:
        // sortParticles() keeps those of a source about to pour.)
        const uint32_t seed = src.seed * 7919u + scene_.solver.seed * 31u + substepCount_ * 104729u;
        for (int k = c0[2]; k < c1[2]; ++k) {
            for (int j = c0[1]; j < c1[1]; ++j) {
                for (int i = c0[0]; i < c1[0]; ++i) {
                    if (solidCellAt(i, j, k)) continue;
                    const size_t c = phi_.index(i, j, k);
                    unsigned taken = 0;
                    for (uint32_t p = cellStart_[c]; p < cellStart_[c] + cellCount_[c]; ++p) {
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
                        id_.push_back(made_++);
                        added = true;
                    }
                }
            }
        }
    }
    if (added) sortParticles();
}

void LiquidSolver::splat(int factor, const std::shared_ptr<const Tiles>& tiles, SparseGrid& weight,
                         SparseGrid* centre, SparseGrid* radius, SparseGrid& foam) const {
    // Reaching less than a cell and a quarter, a particle writes no further
    // than the layers of cells either side of its own -- bySlabs() needs that
    // -- on this grid and on one twice as fine, whose centres sit a quarter
    // of a cell in. Those are kept: the tiles round a particle's are.
    static_assert(kReach < 1.25f, "a particle must stay within the layers next to its own");
    const int fx = n_[0] * factor, fy = n_[1] * factor, fz = n_[2] * factor;
    weight = SparseGrid(tiles);
    for (int a = 0; a < 3; ++a) centre[a] = SparseGrid(tiles);
    if (radius) *radius = SparseGrid(tiles);
    foam = SparseGrid(tiles);
    float* sums[6] = {weight.data(), centre[0].data(), centre[1].data(), centre[2].data(),
                      radius ? radius->data() : nullptr, foam.data()};
    const float scale = static_cast<float>(factor);
    const float reach = kReach * scale, reach2 = reach * reach;
    const int nx = n_[0], ny = n_[1], nz = n_[2];
    bySlabs(layerStart_, nz, [&](uint32_t p) {
        const Vec3& g = cellPos_[p];
        const Vec3 q = g * scale;  // in the cells of this grid
        // A particle with few round it -- spray, a sheet torn thin -- is
        // drawn wider, so that what it is part of stays whole.
        float r = kRadius;
        if (radius) {
            const int ci = std::clamp(static_cast<int>(g.x), 0, nx - 1), cj = std::clamp(static_cast<int>(g.y), 0, ny - 1),
                      ck = std::clamp(static_cast<int>(g.z), 0, nz - 1);
            const float many = std::min(static_cast<float>(cellCount_[phi_.index(ci, cj, ck)]), 8.0f) / 8.0f;
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
                    sums[0][c] += w;
                    sums[1][c] += w * g.x;
                    sums[2][c] += w * g.y;
                    sums[3][c] += w * g.z;
                    if (sums[4]) sums[4][c] += w * r;
                    sums[5][c] += w * white;
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
    // Each particle's velocity onto the faces round it, weighted by how near
    // they are along each axis (trilinear): the sums first, in vel_ and
    // weight_, then their ratio.
    for (int a = 0; a < 3; ++a) {
        vel_[a].fill(0.0f);
        weight_[a].fill(0.0f);
    }
    constexpr int kLast = Tiles::kSide - 1;
    bySlabs(layerStart_, n_[2], [&](uint32_t p) {
        const Vec3& g = cellPos_[p];
        const Vec3& v = velocity_[p];
        for (int a = 0; a < 3; ++a) {
            SparseGrid& sum = vel_[a];
            float* sums = sum.data();
            float* weights = weight_[a].data();
            const float px = g.x - faceOffset(a, 0), py = g.y - faceOffset(a, 1), pz = g.z - faceOffset(a, 2);
            const float bx = std::floor(px), by = std::floor(py), bz = std::floor(pz);
            const float fx = px - bx, fy = py - by, fz = pz - bz;
            const int i0 = static_cast<int>(bx), j0 = static_cast<int>(by), k0 = static_cast<int>(bz);
            const float va = v[a];
            // The eight faces round it, mostly in one tile: looked up once.
            if (i0 >= 0 && j0 >= 0 && k0 >= 0 && i0 + 1 < sum.nx() && j0 + 1 < sum.ny() && k0 + 1 < sum.nz() &&
                (i0 & kLast) != kLast && (j0 & kLast) != kLast && (k0 & kLast) != kLast) {
                const size_t base = sum.index(i0, j0, k0);
                for (int q = 0; q < 8; ++q) {
                    const float w = ((q & 1) ? fx : 1.0f - fx) * (((q >> 1) & 1) ? fy : 1.0f - fy) * ((q >> 2) ? fz : 1.0f - fz);
                    const size_t f = base + static_cast<size_t>(q & 1) + Tiles::kSide * static_cast<size_t>((q >> 1) & 1) +
                                     Tiles::kSide * Tiles::kSide * static_cast<size_t>(q >> 2);
                    weights[f] += w;
                    sums[f] += w * va;
                }
                continue;
            }
            for (int q = 0; q < 8; ++q) {
                const int i = i0 + (q & 1), j = j0 + ((q >> 1) & 1), k = k0 + (q >> 2);
                if (i < 0 || j < 0 || k < 0 || i >= sum.nx() || j >= sum.ny() || k >= sum.nz()) continue;
                const float w = ((q & 1) ? fx : 1.0f - fx) * (((q >> 1) & 1) ? fy : 1.0f - fy) * ((q >> 2) ? fz : 1.0f - fz);
                const size_t f = sum.index(i, j, k);
                weights[f] += w;
                sums[f] += w * va;
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
    splat(1, tiles_, sphereWeight_, sphereCentre_, nullptr, sphereFoam_);
    const float* weight = sphereWeight_.data();
    const float* centre[3] = {sphereCentre_[0].data(), sphereCentre_[1].data(), sphereCentre_[2].data()};
    float* phi = phi_.data();
    forEachCounted(*tiles_, [&](int i, int j, int k, size_t c) {
        const Vec3 x(static_cast<float>(i) + 0.5f, static_cast<float>(j) + 0.5f, static_cast<float>(k) + 0.5f);
        const float w = weight[c];
        const float d = sphereDistance(x, w, Vec3(centre[0][c], centre[1][c], centre[2][c]), w * kRadius);
        phi[c] = d * h;
        cells_[c] = d < 0.0f ? FreeSurfaceSolver::Liquid
                             : solidCellAt(i, j, k) ? FreeSurfaceSolver::Solid : FreeSurfaceSolver::Air;
    });
}

void LiquidSolver::addForces(float dt) {
    const float h = domain_.voxel;
    const float fall = scene_.solver.gravity * dt;
    float* vy = vel_[1].data();
    pg::parallelFor(vel_[1].size(), 16384, [&](size_t begin, size_t end) {
        for (size_t f = begin; f < end; ++f) vy[f] -= fall;
    });
    std::vector<float> crowd;  // made when a wind needs it
    // The particles round a cell: none round one whose tile is not kept.
    auto crowdAt = [&](int i, int j, int k) {
        const int64_t c = find(phi_, i, j, k);
        return c < 0 ? 0.0f : crowd[static_cast<size_t>(c)];
    };
    for (size_t f = 0; f < scene_.forces.size(); ++f) {
        const Force& force = scene_.forces[f];
        const uint32_t seed = force.seed * 7919u + scene_.solver.seed * 31u;
        if (force.kind == ForceKind::Wind) {
            // Wind drags what flies -- spray, drops torn off -- towards its
            // own speed, as air drags a droplet; the body of the water it
            // hardly moves, so a breeze over a pond leaves it level. Spray
            // is water with few particles round it.
            if (crowd.empty()) crowd = crowdOfCells();
            detail::addForce(force, seed, domain_, time_, dt, vel_, noise_[f], [&](int a, int i, int j, int k) {
                const int at = a == 0 ? i : a == 1 ? j : k;
                float outer = -kHuge, most = 0.0f;
                if (at > 0) {
                    const int bi = i - (a == 0), bj = j - (a == 1), bk = k - (a == 2);
                    outer = std::max(outer, phi_.at(bi, bj, bk));
                    most = std::max(most, crowdAt(bi, bj, bk));
                }
                if (at < n_[a]) {
                    outer = std::max(outer, phi_.at(i, j, k));
                    most = std::max(most, crowdAt(i, j, k));
                }
                if (outer <= -1.5f * h) return 0.0f;  // deep in the water
                const float spray = 1.0f - detail::smoothstep(kSprayCrowd, kBodyCrowd, most);
                return spray + (1.0f - spray) * kWindOnWater;
            });
        } else {
            detail::addForce(force, seed, domain_, time_, dt, vel_, noise_[f], [](int, int, int, int) { return 1.0f; });
        }
    }
}

std::vector<float> LiquidSolver::crowdOfCells() const {
    const size_t kept = phi_.size();
    std::vector<float> a(kept), b(kept);
    for (size_t c = 0; c < kept; ++c) a[c] = static_cast<float>(cellCount_[c]);
    // The sum over three cells along each axis in turn; a cell whose tile is
    // not kept has no particles near it.
    for (int axis = 0; axis < 3; ++axis) {
        const std::vector<float>& from = axis == 1 ? b : a;
        std::vector<float>& to = axis == 1 ? a : b;
        forEachCounted(*tiles_, [&](int i, int j, int k, size_t c) {
            const int at[3] = {i, j, k};
            auto beside = [&](int side) {
                const int64_t g = find(phi_, i + (axis == 0 ? side : 0), j + (axis == 1 ? side : 0), k + (axis == 2 ? side : 0));
                return g < 0 ? 0.0f : from[static_cast<size_t>(g)];
            };
            float sum = from[c];
            if (at[axis] > 0) sum += beside(-1);
            if (at[axis] < n_[axis] - 1) sum += beside(1);
            to[c] = sum;
        });
    }
    return b;
}

void LiquidSolver::project(float dt) {
    const float h = domain_.voxel;
    const int nx = n_[0], ny = n_[1], nz = n_[2];
    // What flows out of each cell of water, through the open part of its faces.
    float* rhs = rhs_.data();
    forEachCounted(*tiles_, [&](int i, int j, int k, size_t c) {
        if (cells_[c] != FreeSurfaceSolver::Liquid) {
            rhs[c] = 0.0f;
            return;
        }
        // Through the open part of a face the water's velocity, through the
        // rest the solid's (Batty, Bertails and Bridson).
        auto flux = [&](int a, int fi, int fj, int fk) {
            const float o = open(a, fi, fj, fk);
            const float u = o * vel_[a].at(fi, fj, fk);
            return movingSolid_ ? u + (1.0f - o) * solidVelocityAt(a, fi, fj, fk) : u;
        };
        const float out = flux(0, i + 1, j, k) - flux(0, i, j, k) + flux(1, i, j + 1, k) - flux(1, i, j, k) +
                          flux(2, i, j, k + 1) - flux(2, i, j, k);
        rhs[c] = -(h / dt) * out;
    });
    {
        // The pressure, on the whole grid for now: the kept cells spread out
        // on it, the rest air -- or a solid's -- as far from the water as the
        // particles reach, open as the solids leave them.
        std::vector<uint8_t> cells(domain_.cellCount());
        Grid phi(nx, ny, nz), b(nx, ny, nz), p(nx, ny, nz), faces[3];
        detail::forEachIn(0, nx, 0, ny, 0, nz, [&](int i, int j, int k) {
            const size_t d = phi.index(i, j, k);
            const int64_t c = find(phi_, i, j, k);
            if (c < 0) {
                cells[d] = solidCellAt(i, j, k) ? FreeSurfaceSolver::Solid : FreeSurfaceSolver::Air;
                phi.data()[d] = phi_.background();
                return;
            }
            cells[d] = cells_[static_cast<size_t>(c)];
            phi.data()[d] = phi_.data()[c];
            b.data()[d] = rhs_.data()[c];
            p.data()[d] = pressure_.data()[c];
        });
        for (int a = 0; a < 3; ++a) {
            faces[a] = Grid(nx + (a == 0), ny + (a == 1), nz + (a == 2));
            forEachCell(faces[a], [&](int i, int j, int k) { faces[a].at(i, j, k) = open(a, i, j, k); });
        }
        pressureSolver_.setSystem(cells, faces, phi);
        lastIterations_ += pressureSolver_.solve(p, b, kTolerance, kMaxIterations);
        float* pressure = pressure_.data();
        forEachCounted(*tiles_, [&](int i, int j, int k, size_t c) { pressure[c] = p.at(i, j, k); });
    }

    // The gradient of the pressure, off each face next to water. Past the
    // surface the pressure is 0 where the surface crosses the face; past an
    // open side, on the side.
    const float scale = dt / h;
    const int n[3] = {nx, ny, nz};
    const float* pressure = pressure_.data();
    const float* phi = phi_.data();
    // Water in cell (i, j, k), and where it is kept: a cell not kept is air.
    auto wet = [&](int i, int j, int k, int64_t& at) {
        at = find(phi_, i, j, k);
        return at >= 0 && cells_[static_cast<size_t>(at)] == FreeSurfaceSolver::Liquid;
    };
    for (int a = 0; a < 3; ++a) {
        float* v = vel_[a].data();
        const float* weight = weight_[a].data();
        uint8_t* valid = valid_[a].data();
        forEachCounted(*faces_[a], [&](int i, int j, int k, size_t f) {
            if (open(a, i, j, k) <= 0.0f) {  // a wall: nothing through it -- but what a moving solid carries
                v[f] = movingSolid_ ? solidVelocityAt(a, i, j, k) : 0.0f;
                valid[f] = 0;
                return;
            }
            const int at = a == 0 ? i : a == 1 ? j : k;
            const bool hasBehind = at > 0, hasAhead = at < n[a];
            const int bi = i - (a == 0), bj = j - (a == 1), bk = k - (a == 2);
            int64_t behind = -1, ahead = -1;
            const bool wetBehind = hasBehind && wet(bi, bj, bk, behind);
            const bool wetAhead = hasAhead && wet(i, j, k, ahead);
            if (!wetBehind && !wetAhead) {
                valid[f] = weight[f] > 1e-6f;  // spray: as the particles fly
                return;
            }
            float drop;  // pressure ahead - pressure behind
            if (wetBehind && wetAhead) {
                drop = pressure[ahead] - pressure[behind];
            } else if (wetBehind) {
                const float theta = hasAhead ? FreeSurfaceSolver::surfaceFraction(phi[behind], phi_.at(i, j, k)) : 0.5f;
                drop = -pressure[behind] / theta;
            } else {
                const float theta = hasBehind ? FreeSurfaceSolver::surfaceFraction(phi[ahead], phi_.at(bi, bj, bk)) : 0.5f;
                drop = pressure[ahead] / theta;
            }
            v[f] -= scale * drop;
            valid[f] = 1;
        });
    }
}

void LiquidSolver::extrapolate(int layers) {
    for (int a = 0; a < 3; ++a) {
        SparseGrid& vel = vel_[a];
        float* v = vel.data();
        std::vector<uint8_t>& valid = valid_[a];
        std::vector<uint8_t> next;
        const int fx = vel.nx(), fy = vel.ny(), fz = vel.nz();
        for (int layer = 0; layer < layers; ++layer) {
            next = valid;
            // Only faces not yet known are written, only known ones read --
            // and a face whose tile is not kept is not known.
            forEachCounted(*faces_[a], [&](int i, int j, int k, size_t f) {
                if (valid[f]) return;
                float sum = 0.0f;
                int count = 0;
                auto take = [&](int ti, int tj, int tk) {
                    const int64_t g = find(vel, ti, tj, tk);
                    if (g >= 0 && valid[static_cast<size_t>(g)]) {
                        sum += v[g];
                        ++count;
                    }
                };
                if (i > 0) take(i - 1, j, k);
                if (i < fx - 1) take(i + 1, j, k);
                if (j > 0) take(i, j - 1, k);
                if (j < fy - 1) take(i, j + 1, k);
                if (k > 0) take(i, j, k - 1);
                if (k < fz - 1) take(i, j, k + 1);
                if (count > 0) {
                    v[f] = sum / static_cast<float>(count);
                    next[f] = 1;
                }
            });
            valid.swap(next);
        }
        // Walls stay walls.
        forEachCounted(*faces_[a], [&](int i, int j, int k, size_t f) {
            if (open(a, i, j, k) <= 0.0f) v[f] = 0.0f;
        });
    }
}

Vec3 LiquidSolver::sampleVelocity(const SparseGrid* vel, const Vec3& g) const {
    return {vel[0].sample(g.x + 0.5f, g.y, g.z), vel[1].sample(g.x, g.y + 0.5f, g.z), vel[2].sample(g.x, g.y, g.z + 0.5f)};
}

Vec3 LiquidSolver::velocityAt(const Vec3& p) const { return sampleVelocity(vel_, toCells(p)); }

float WaterLevel::at(float x, float z) const {
    if (height.empty() || cell <= 0.0f) return kNone;
    // Column centres at whole numbers.
    const float gx = (x - origin.x) / cell - 0.5f, gz = (z - origin.z) / cell - 0.5f;
    if (gx < -0.5f || gz < -0.5f || gx > static_cast<float>(nx) - 0.5f || gz > static_cast<float>(nz) - 0.5f) return kNone;
    const int i0 = std::clamp(static_cast<int>(std::floor(gx)), 0, nx - 1), k0 = std::clamp(static_cast<int>(std::floor(gz)), 0, nz - 1);
    const int i1 = std::min(i0 + 1, nx - 1), k1 = std::min(k0 + 1, nz - 1);
    const float fx = std::clamp(gx - static_cast<float>(i0), 0.0f, 1.0f), fz = std::clamp(gz - static_cast<float>(k0), 0.0f, 1.0f);
    // Between the columns that have water: one without is left out, not
    // taken as a level far below.
    float sum = 0.0f, weight = 0.0f;
    const int is[2] = {i0, i1}, ks[2] = {k0, k1};
    const float wx[2] = {1.0f - fx, fx}, wz[2] = {1.0f - fz, fz};
    for (int b = 0; b < 2; ++b) {
        for (int a = 0; a < 2; ++a) {
            const float h = height[static_cast<size_t>(is[a]) + static_cast<size_t>(nx) * static_cast<size_t>(ks[b])];
            const float w = wx[a] * wz[b];
            if (h <= kNone || w <= 0.0f) continue;
            sum += h * w;
            weight += w;
        }
    }
    return weight > 1e-6f ? sum / weight : kNone;
}

WaterLevel LiquidSolver::waterLevel() const {
    const int nx = n_[0], ny = n_[1], nz = n_[2];
    const float h = domain_.voxel;
    const Vec3 o = domain_.origin();
    WaterLevel level;
    level.origin = o;
    level.cell = h;
    level.nx = nx;
    level.nz = nz;
    level.height.assign(static_cast<size_t>(nx) * static_cast<size_t>(nz), WaterLevel::kNone);
    // Covered: a column whose water has a solid on it -- a floating piece.
    std::vector<uint8_t> covered(level.height.size(), 0);
    const float* phi = phi_.data();
    pg::parallelFor(static_cast<size_t>(nz), 4, [&](size_t begin, size_t end) {
        for (size_t kk = begin; kk < end; ++kk) {
            const int k = static_cast<int>(kk);
            for (int i = 0; i < nx; ++i) {
                // Up from the floor: past what is not water -- a tile not
                // kept at once: none of its cells is -- then through the
                // water, and the solids in it, to the first air.
                int j = 0, top = -1;
                while (j < ny) {
                    const int64_t c = find(phi_, i, j, k);
                    if (c < 0) {
                        j = (j | (Tiles::kSide - 1)) + 1;
                        continue;
                    }
                    if (phi[c] < 0.0f) break;
                    ++j;
                }
                for (; j < ny; ++j) {
                    if (phi_.at(i, j, k) < 0.0f) top = j;
                    else if (!solidCellAt(i, j, k)) break;
                }
                if (top < 0) continue;
                const size_t col = static_cast<size_t>(i) + static_cast<size_t>(nx) * kk;
                if (top + 1 < ny && solidCellAt(i, top + 1, k)) {
                    covered[col] = 1;
                    continue;
                }
                // Where the surface crosses between the top cell and the one above.
                float y = static_cast<float>(top) + 0.5f;
                if (top + 1 < ny) {
                    const float a = phi_.at(i, top, k), b = phi_.at(i, top + 1, k);
                    y += std::clamp(a / (a - b), 0.0f, 1.0f);
                } else {
                    y += 0.5f;
                }
                level.height[col] = o.y + y * h;
            }
        }
    });
    // A covered column takes the level round it: passes that each give it
    // the mean of its neighbours that have one, until none is left or none
    // has any.
    std::vector<float> next;
    for (int pass = 0; pass < nx + nz; ++pass) {
        next = level.height;
        bool left = false, filled = false;
        for (int k = 0; k < nz; ++k) {
            for (int i = 0; i < nx; ++i) {
                const size_t col = static_cast<size_t>(i) + static_cast<size_t>(nx) * static_cast<size_t>(k);
                if (!covered[col] || level.height[col] > WaterLevel::kNone) continue;
                float sum = 0.0f;
                int count = 0;
                const int di[4] = {-1, 1, 0, 0}, dk[4] = {0, 0, -1, 1};
                for (int d = 0; d < 4; ++d) {
                    const int a = i + di[d], b = k + dk[d];
                    if (a < 0 || b < 0 || a >= nx || b >= nz) continue;
                    const float v = level.height[static_cast<size_t>(a) + static_cast<size_t>(nx) * static_cast<size_t>(b)];
                    if (v <= WaterLevel::kNone) continue;
                    sum += v;
                    ++count;
                }
                if (count > 0) {
                    next[col] = sum / static_cast<float>(count);
                    filled = true;
                } else {
                    left = true;
                }
            }
        }
        level.height.swap(next);
        if (!left || !filled) break;
    }
    return level;
}

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
            const bool few = cellCount_[c] < 4;
            if (few && cells_[c] != FreeSurfaceSolver::Liquid) {
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
                        const float into = dot(v - solidVelocity(x), normal);
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
        id_[kept] = id_[p];
        ++kept;
    }
    position_.resize(kept);
    velocity_.resize(kept);
    foam_.resize(kept);
    id_.resize(kept);
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
    const std::shared_ptr<const Tiles> tiles = factor == 1 ? tiles_ : finer(*tiles_);
    SparseGrid weight, centre[3], radius, white;
    splat(factor, tiles, weight, centre, &radius, white);
    const float h = domain_.voxel;
    const float inv = 1.0f / static_cast<float>(factor);
    forEachCounted(*tiles, [&](int i, int j, int k, size_t c) {
        const float w = weight.data()[c];
        if (w <= 0.0f) return;
        const Vec3 x((static_cast<float>(i) + 0.5f) * inv, (static_cast<float>(j) + 0.5f) * inv,
                     (static_cast<float>(k) + 0.5f) * inv);
        const float d =
            sphereDistance(x, w, Vec3(centre[0].data()[c], centre[1].data()[c], centre[2].data()[c]), radius.data()[c]) * h;
        distance.at(i, j, k) = std::clamp(d, -band, band);
        foam.at(i, j, k) = std::clamp(white.data()[c] / w, 0.0f, 1.0f);
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
