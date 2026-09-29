#include "pg/sim/Poisson.h"

#include "pg/core/Parallel.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace pg::sim {
namespace {

constexpr int kPreSmooth = 2;
constexpr int kPostSmooth = 2;
constexpr int kLast = Tiles::kSide - 1;
constexpr size_t kRow = Tiles::kSide, kSlab = Tiles::kSide * Tiles::kSide;

// The neighbour sums are the inner loop of the whole solver: inlined, or the
// call costs as much as the work.
#if defined(__GNUC__) || defined(__clang__)
#define PG_HOT_INLINE inline __attribute__((always_inline))
#elif defined(_MSC_VER)
#define PG_HOT_INLINE __forceinline
#else
#define PG_HOT_INLINE inline
#endif

/// Without solids: the sum of the neighbours inside the grid -- those not
/// counted hold 0 -- and the diagonal: 6, one more for every open side of
/// the box the cell touches (its ghost there is -p), one less for every wall
/// (the face drops out). A neighbour in the same tile is next to it in
/// memory; one in the next tile is looked up.
PG_HOT_INLINE void neighbours(const SparseGrid& p, int i, int j, int k, size_t c, const bool closed[6], float& sum,
                              float& diagonal) {
    const float* d = p.data();
    const int x = i & kLast, y = j & kLast, z = k & kLast;
    sum = 0.0f;
    diagonal = 6.0f;
    if (i > 0) sum += x > 0 ? d[c - 1] : p.at(i - 1, j, k); else diagonal += closed[0] ? -1.0f : 1.0f;
    if (i < p.nx() - 1) sum += x < kLast ? d[c + 1] : p.at(i + 1, j, k); else diagonal += closed[1] ? -1.0f : 1.0f;
    if (j > 0) sum += y > 0 ? d[c - kRow] : p.at(i, j - 1, k); else diagonal += closed[2] ? -1.0f : 1.0f;
    if (j < p.ny() - 1) sum += y < kLast ? d[c + kRow] : p.at(i, j + 1, k); else diagonal += closed[3] ? -1.0f : 1.0f;
    if (k > 0) sum += z > 0 ? d[c - kSlab] : p.at(i, j, k - 1); else diagonal += closed[4] ? -1.0f : 1.0f;
    if (k < p.nz() - 1) sum += z < kLast ? d[c + kSlab] : p.at(i, j, k + 1); else diagonal += closed[5] ? -1.0f : 1.0f;
}

/// With solids, on the finest level: the neighbours inside the grid behind
/// open faces -- what weightedSum() adds, 1 x or 0 x each.
PG_HOT_INLINE float openSum(const SparseGrid& p, uint8_t open, int i, int j, int k, size_t c) {
    const float* d = p.data();
    const int x = i & kLast, y = j & kLast, z = k & kLast;
    float sum = 0.0f;
    if (i > 0 && (open & 1)) sum += x > 0 ? d[c - 1] : p.at(i - 1, j, k);
    if (i < p.nx() - 1 && (open & 2)) sum += x < kLast ? d[c + 1] : p.at(i + 1, j, k);
    if (j > 0 && (open & 4)) sum += y > 0 ? d[c - kRow] : p.at(i, j - 1, k);
    if (j < p.ny() - 1 && (open & 8)) sum += y < kLast ? d[c + kRow] : p.at(i, j + 1, k);
    if (k > 0 && (open & 16)) sum += z > 0 ? d[c - kSlab] : p.at(i, j, k - 1);
    if (k < p.nz() - 1 && (open & 32)) sum += z < kLast ? d[c + kSlab] : p.at(i, j, k + 1);
    return sum;
}

/// With solids: the neighbours inside the grid, each weighted by the face
/// between. (The ghosts beyond the sides are in the diagonal.)
PG_HOT_INLINE float weightedSum(const SparseGrid& p, const SparseGrid* a, int i, int j, int k, size_t c) {
    const float* d = p.data();
    const int x = i & kLast, y = j & kLast, z = k & kLast;
    float sum = 0.0f;
    if (i > 0) sum += a[0].at(i, j, k) * (x > 0 ? d[c - 1] : p.at(i - 1, j, k));
    if (i < p.nx() - 1) sum += a[0].at(i + 1, j, k) * (x < kLast ? d[c + 1] : p.at(i + 1, j, k));
    if (j > 0) sum += a[1].at(i, j, k) * (y > 0 ? d[c - kRow] : p.at(i, j - 1, k));
    if (j < p.ny() - 1) sum += a[1].at(i, j + 1, k) * (y < kLast ? d[c + kRow] : p.at(i, j + 1, k));
    if (k > 0) sum += a[2].at(i, j, k) * (z > 0 ? d[c - kSlab] : p.at(i, j, k - 1));
    if (k < p.nz() - 1) sum += a[2].at(i, j, k + 1) * (z < kLast ? d[c + kSlab] : p.at(i, j, k + 1));
    return sum;
}

/// Which stored cells of `tiles` count: those inside the grid, in tiles kept
/// whole.
std::vector<uint8_t> counted(const Tiles& tiles) {
    std::vector<uint8_t> on(tiles.stored().size() * Tiles::kCells, 0);
    forEachCounted(tiles, [&](int, int, int, size_t c) { on[c] = 1; });
    return on;
}

/// The diagonal from the face coefficients: each face once, a face on a side
/// of the box twice (the ghost holds -p, twice the drop to the face).
SparseGrid diagonals(const SparseGrid* a, const std::shared_ptr<const Tiles>& tiles, const std::vector<uint8_t>& on) {
    SparseGrid d(tiles);
    const int nx = tiles->nx(), ny = tiles->ny(), nz = tiles->nz();
    forEachCounted(*tiles, [&](int i, int j, int k, size_t c) {
        if (!on[c]) return;
        float s = a[0].at(i, j, k) * (i == 0 ? 2.0f : 1.0f) + a[0].at(i + 1, j, k) * (i == nx - 1 ? 2.0f : 1.0f);
        s += a[1].at(i, j, k) * (j == 0 ? 2.0f : 1.0f) + a[1].at(i, j + 1, k) * (j == ny - 1 ? 2.0f : 1.0f);
        s += a[2].at(i, j, k) * (k == 0 ? 2.0f : 1.0f) + a[2].at(i, j, k + 1) * (k == nz - 1 ? 2.0f : 1.0f);
        d.data()[c] = s;
    });
    return d;
}

/// Fine to coarse: each coarse cell that counts gets the mean of its eight
/// children.
void restrictTo(const SparseGrid& fine, SparseGrid& coarse, const std::vector<uint8_t>& on) {
    forEachCounted(coarse.tiles(), [&](int i, int j, int k, size_t c) {
        if (!on[c]) return;
        float sum = 0.0f;
        for (int q = 0; q < 8; ++q) sum += fine.at(2 * i + (q & 1), 2 * j + ((q >> 1) & 1), 2 * k + (q >> 2));
        coarse.data()[c] = sum * 0.125f;
    });
}

bool canHalve(int nx, int ny, int nz) { return nx % 2 == 0 && ny % 2 == 0 && nz % 2 == 0 && std::min({nx, ny, nz}) >= 4; }

/// The tiles of the level below `fine`: a coarse tile covers two fine ones
/// along each axis, and is kept when either is.
std::shared_ptr<const Tiles> coarser(const Tiles& fine) {
    const int cx = fine.nx() / 2, cy = fine.ny() / 2, cz = fine.nz() / 2;
    const int tx = (cx + kLast) / Tiles::kSide, ty = (cy + kLast) / Tiles::kSide, tz = (cz + kLast) / Tiles::kSide;
    std::vector<uint8_t> state(static_cast<size_t>(tx) * static_cast<size_t>(ty) * static_cast<size_t>(tz), Tiles::Off);
    for (const uint32_t t : fine.stored()) {
        int i, j, k;
        fine.corner(t, i, j, k);
        const size_t n = static_cast<size_t>(i / 16) +
                         static_cast<size_t>(tx) * (static_cast<size_t>(j / 16) + static_cast<size_t>(ty) * static_cast<size_t>(k / 16));
        state[n] = Tiles::Whole;
    }
    return std::make_shared<const Tiles>(cx, cy, cz, std::move(state), -1);
}

}  // namespace

void PoissonSolver::setBoundary(const PoissonBoundary& boundary) {
    std::copy(boundary.closed, boundary.closed + 6, closed_);
    solid_ = SparseGrid();
    solids_ = false;
    if (boundary.solid && boundary.solid->shared()) {
        const SparseGrid& s = *boundary.solid;
        const float* v = s.data();
        solids_ = std::any_of(v, v + s.size(), [](float x) { return x > 0.5f; });
        if (solids_) solid_ = s;
    }
    dirty_ = true;
}

void PoissonSolver::build(const SparseGrid& fine, float h) {
    if (!dirty_ && tiles_ == fine.shared() && h_ == h) return;
    dirty_ = false;
    tiles_ = fine.shared();
    dims_[0] = fine.nx();
    dims_[1] = fine.ny();
    dims_[2] = fine.nz();
    h_ = h;
    fineResidual_ = SparseGrid(tiles_);
    fineOn_ = counted(*tiles_);
    const bool solids = solids_ && solid_.nx() == fine.nx() && solid_.ny() == fine.ny() && solid_.nz() == fine.nz() &&
                        solid_.tiles() == fine.tiles();

    // The finest faces: blocked by a solid on either side, or a wall. Each
    // cell's six in a byte; the diagonal as diagonals() makes it.
    fineOp_ = Operator{};
    if (solids) {
        const int n[3] = {fine.nx(), fine.ny(), fine.nz()};
        auto open = [&](int a, int i, int j, int k) {
            const int f = a == 0 ? i : a == 1 ? j : k;  // face f: between cells f-1 and f
            const int bi = i - (a == 0), bj = j - (a == 1), bk = k - (a == 2);
            const bool behind = f > 0 && solid_.at(bi, bj, bk) > 0.5f;
            const bool ahead = f < n[a] && solid_.at(i, j, k) > 0.5f;
            const bool wall = (f == 0 && closed_[2 * a]) || (f == n[a] && closed_[2 * a + 1]);
            return !(behind || ahead || wall);
        };
        fineOp_.open.assign(fine.size(), 0);
        fineOp_.diagonal = SparseGrid(tiles_);
        forEachCounted(*tiles_, [&](int i, int j, int k, size_t c) {
            if (!fineOn_[c]) return;
            uint8_t bits = 0;
            float a[6];
            for (int axis = 0; axis < 3; ++axis) {
                const bool below = open(axis, i, j, k);
                const bool above = open(axis, i + (axis == 0), j + (axis == 1), k + (axis == 2));
                bits |= static_cast<uint8_t>((below ? 1 : 0) << (2 * axis));
                bits |= static_cast<uint8_t>((above ? 1 : 0) << (2 * axis + 1));
                a[2 * axis] = below ? 1.0f : 0.0f;
                a[2 * axis + 1] = above ? 1.0f : 0.0f;
            }
            fineOp_.open[c] = bits;
            float s = a[0] * (i == 0 ? 2.0f : 1.0f) + a[1] * (i == n[0] - 1 ? 2.0f : 1.0f);
            s += a[2] * (j == 0 ? 2.0f : 1.0f) + a[3] * (j == n[1] - 1 ? 2.0f : 1.0f);
            s += a[4] * (k == 0 ? 2.0f : 1.0f) + a[5] * (k == n[2] - 1 ? 2.0f : 1.0f);
            fineOp_.diagonal.data()[c] = s;
        });
    }

    coarse_.clear();
    coarse_.reserve(16);  // pointers into it stay valid below
    const SparseGrid* g = &fine;
    const Counts* on = &fineOn_;
    const Operator* op = &fineOp_;
    float levelH = h;
    while (canHalve(g->nx(), g->ny(), g->nz())) {
        levelH *= 2.0f;
        Level level;
        const std::shared_ptr<const Tiles> tiles = coarser(g->tiles());
        level.p = SparseGrid(tiles);
        level.b = level.p;
        level.r = level.p;
        level.h = levelH;
        // A coarse cell counts when all eight of its cells do: half in the
        // still air, it would carry p = 0 half a coarse cell past where the
        // finer level has it, and the corrections it sends up would miss --
        // a solve that crawls, or blows up. (With every tile kept, every
        // cell counts, as on a dense grid.)
        level.on.assign(level.p.size(), 0);
        const SparseGrid& finer = *g;
        const Counts& finerOn = *on;
        forEachCounted(*tiles, [&](int i, int j, int k, size_t c) {
            for (int q = 0; q < 8; ++q) {
                const int fi = 2 * i + (q & 1), fj = 2 * j + ((q >> 1) & 1), fk = 2 * k + (q >> 2);
                if (!finer.stored(fi, fj, fk) || !finerOn[finer.index(fi, fj, fk)]) return;
            }
            level.on[c] = 1;
        });
        if (solids) {
            // A coarse face is as open as the four fine faces it covers.
            for (int a = 0; a < 3; ++a) {
                SparseGrid& faces = level.op.a[a];
                faces = SparseGrid(Tiles::faces(*tiles, a));
                const Operator& finerOp = *op;
                forEachCounted(faces.tiles(), [&](int i, int j, int k, size_t c) {
                    float sum = 0.0f;
                    for (int q = 0; q < 4; ++q) {
                        const int u = q & 1, w = q >> 1;  // the two directions across the face
                        const int fi = a == 0 ? 2 * i : 2 * i + u;
                        const int fj = a == 1 ? 2 * j : 2 * j + (a == 0 ? u : w);
                        const int fk = a == 2 ? 2 * k : 2 * k + w;
                        sum += faceOf(finer, finerOp, a, fi, fj, fk);
                    }
                    faces.data()[c] = 0.25f * sum;
                });
            }
            level.op.diagonal = diagonals(level.op.a, tiles, level.on);
        }
        coarse_.push_back(std::move(level));
        g = &coarse_.back().p;
        on = &coarse_.back().on;
        op = &coarse_.back().op;
    }
}

void PoissonSolver::relax(SparseGrid& p, const SparseGrid& b, const Counts& on, const Operator& op, float h,
                          int sweeps, float omega) const {
    const float h2 = h * h;
    const bool weighted = op.diagonal.shared() != nullptr;
    const bool bits = !op.open.empty();
    float* v = p.data();
    const float* rhs = b.data();
    for (int s = 0; s < sweeps; ++s) {
        for (int colour = 0; colour < 2; ++colour) {
            forEachCounted(p.tiles(), [&](int i, int j, int k, size_t c) {
                if (((i + j + k + colour) & 1) != 0 || !on[c]) return;
                if (weighted) {
                    const float d = op.diagonal.data()[c];
                    if (d <= 0.0f) {  // walled in on every side: nothing flows, nothing to solve
                        v[c] = 0.0f;
                        return;
                    }
                    const float sum = bits ? openSum(p, op.open[c], i, j, k, c) : weightedSum(p, op.a, i, j, k, c);
                    v[c] += omega * ((sum - h2 * rhs[c]) / d - v[c]);
                } else {
                    float sum, diagonal;
                    neighbours(p, i, j, k, c, closed_, sum, diagonal);
                    v[c] += omega * ((sum - h2 * rhs[c]) / diagonal - v[c]);
                }
            });
        }
    }
}

void PoissonSolver::computeResidual(const SparseGrid& p, const SparseGrid& b, const Counts& on, const Operator& op,
                                    float h, SparseGrid& r) const {
    const float invH2 = 1.0f / (h * h);
    const bool weighted = op.diagonal.shared() != nullptr;
    const bool bits = !op.open.empty();
    forEachCounted(p.tiles(), [&](int i, int j, int k, size_t c) {
        if (!on[c]) return;
        if (weighted) {
            const float d = op.diagonal.data()[c];
            const float sum = bits ? openSum(p, op.open[c], i, j, k, c) : weightedSum(p, op.a, i, j, k, c);
            r.data()[c] = d <= 0.0f ? 0.0f : b.data()[c] - (sum - d * p.data()[c]) * invH2;
        } else {
            float sum, diagonal;
            neighbours(p, i, j, k, c, closed_, sum, diagonal);
            r.data()[c] = b.data()[c] - (sum - diagonal * p.data()[c]) * invH2;
        }
    });
}

/// Coarse to fine, added: trilinear, each fine cell between its coarse cell
/// (weight 3/4 per axis) and the next coarse cell towards it (1/4). Past a side
/// of the box the ghost mirrors the cell: negated at an open side, as in the
/// equation, as it is at a wall. The four coarse rows round the fine row are
/// blended first, then along x -- in the order the dense solver took.
void PoissonSolver::prolongAdd(const SparseGrid& coarse, SparseGrid& fine, const Counts& on) const {
    const float mirror[6] = {closed_[0] ? 1.0f : -1.0f, closed_[1] ? 1.0f : -1.0f, closed_[2] ? 1.0f : -1.0f,
                             closed_[3] ? 1.0f : -1.0f, closed_[4] ? 1.0f : -1.0f, closed_[5] ? 1.0f : -1.0f};
    const int cnx = coarse.nx(), cny = coarse.ny(), cnz = coarse.nz();
    forEachCounted(fine.tiles(), [&](int i, int j, int k, size_t c) {
        if (!on[c]) return;
        const int cj = j >> 1, ck = k >> 1;
        int oj = (j & 1) ? cj + 1 : cj - 1, ok = (k & 1) ? ck + 1 : ck - 1;
        float sy = 1.0f, sz = 1.0f;
        if (oj < 0 || oj >= cny) {
            sy = mirror[oj < 0 ? 2 : 3];
            oj = cj;
        }
        if (ok < 0 || ok >= cnz) {
            sz = mirror[ok < 0 ? 4 : 5];
            ok = ck;
        }
        const float w00 = 0.5625f, w10 = 0.1875f * sy, w01 = 0.1875f * sz, w11 = 0.0625f * sy * sz;
        auto line = [&](int x) {
            return w00 * coarse.at(x, cj, ck) + w10 * coarse.at(x, oj, ck) + w01 * coarse.at(x, cj, ok) +
                   w11 * coarse.at(x, oj, ok);
        };
        const int ci = i >> 1;
        int oi = (i & 1) ? ci + 1 : ci - 1;
        float sx = 1.0f;
        if (oi < 0 || oi >= cnx) {
            sx = mirror[oi < 0 ? 0 : 1];
            oi = ci;
        }
        fine.data()[c] += 0.75f * line(ci) + 0.25f * sx * line(oi);
    });
}

void PoissonSolver::solve(SparseGrid& p, const SparseGrid& b, float h, int cycles) {
    if (!p.shared() || p.size() == 0) return;
    build(p, h);
    for (int c = 0; c < cycles; ++c) vcycle(p, b, fineResidual_, fineOn_, fineOp_, h, 0);
}

void PoissonSolver::vcycle(SparseGrid& p, const SparseGrid& b, SparseGrid& r, const Counts& on, const Operator& op,
                           float h, size_t next) {
    if (next == coarse_.size()) {
        // The coarsest grid: a few cells a side, solved by over-relaxed sweeps.
        // The factor is the optimum for Jacobi's rate on this box.
        const double pi = 3.14159265358979323846;
        const double rate = (std::cos(pi / p.nx()) + std::cos(pi / p.ny()) + std::cos(pi / p.nz())) / 3.0;
        const float omega = static_cast<float>(2.0 / (1.0 + std::sqrt(1.0 - rate * rate)));
        relax(p, b, on, op, h, 2 * std::max({p.nx(), p.ny(), p.nz()}), omega);
        return;
    }
    relax(p, b, on, op, h, kPreSmooth, 1.0f);
    computeResidual(p, b, on, op, h, r);
    Level& coarse = coarse_[next];
    restrictTo(r, coarse.b, coarse.on);
    coarse.p.fill(0.0f);
    vcycle(coarse.p, coarse.b, coarse.r, coarse.on, coarse.op, coarse.h, next + 1);
    prolongAdd(coarse.p, p, on);
    relax(p, b, on, op, h, kPostSmooth, 1.0f);
}

double PoissonSolver::residual(const SparseGrid& p, const SparseGrid& b, float h) {
    if (!p.shared() || p.size() == 0) return 0.0;
    build(p, h);
    SparseGrid r(p.shared());
    computeResidual(p, b, fineOn_, fineOp_, h, r);
    double sum = 0.0;
    size_t cells = 0;
    for (size_t c = 0; c < r.size(); ++c) {
        if (!fineOn_[c]) continue;
        sum += std::fabs(r.data()[c]);
        ++cells;
    }
    return cells ? sum / static_cast<double>(cells) : 0.0;
}

float PoissonSolver::faceOf(const SparseGrid& p, const Operator& op, int axis, int i, int j, int k) const {
    const int n = axis == 0 ? p.nx() : axis == 1 ? p.ny() : p.nz();
    const int f = axis == 0 ? i : axis == 1 ? j : k;  // face f: between cells f-1 and f
    if (op.open.empty()) {
        const SparseGrid& a = op.a[axis];
        if (a.shared() && a.stored(i, j, k) && a.has(i, j, k)) return a.at(i, j, k);
    } else {
        // The cell above the face has it as its face below; else the cell
        // below, as its face above.
        if (f < n && p.stored(i, j, k)) return (op.open[p.index(i, j, k)] >> (2 * axis) & 1) ? 1.0f : 0.0f;
        const int bi = i - (axis == 0), bj = j - (axis == 1), bk = k - (axis == 2);
        if (f > 0 && p.stored(bi, bj, bk)) return (op.open[p.index(bi, bj, bk)] >> (2 * axis + 1) & 1) ? 1.0f : 0.0f;
    }
    // Between cells that do not count: the still air, as open as the sides
    // of the box -- a coarse cell half in it meets p = 0 there, as its
    // cells do, not a wall. (Walled in, a patch of cells has no pressure
    // that solves it, and the sweeps blow up.)
    const bool wall = (f == 0 && closed_[2 * axis]) || (f == n && closed_[2 * axis + 1]);
    return wall ? 0.0f : 1.0f;
}

float PoissonSolver::faceOpen(int axis, int i, int j, int k) const {
    if (!fineOp_.open.empty()) return faceOf(fineResidual_, fineOp_, axis, i, j, k);
    const int f = axis == 0 ? i : axis == 1 ? j : k;
    const int n = dims_[axis];
    if (f == 0) return closed_[2 * axis] ? 0.0f : 1.0f;
    if (f == n) return closed_[2 * axis + 1] ? 0.0f : 1.0f;
    return 1.0f;
}

}  // namespace pg::sim
