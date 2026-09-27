#include "pg/sim/Poisson.h"

#include "pg/core/Parallel.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace pg::sim {
namespace {

constexpr int kPreSmooth = 2;
constexpr int kPostSmooth = 2;

// The neighbour sums are the inner loop of the whole solver: inlined, or the
// call costs as much as the work.
#if defined(__GNUC__) || defined(__clang__)
#define PG_HOT_INLINE inline __attribute__((always_inline))
#elif defined(_MSC_VER)
#define PG_HOT_INLINE __forceinline
#else
#define PG_HOT_INLINE inline
#endif

/// Rows of cells in chunks of some 8k cells: a coarse level fits in one,
/// which runs on the calling thread. The chunks decide where cells are
/// computed, never how, so any split gives the same bits.
template <class F>
void forEachRow(const Grid& g, const F& f) {
    const size_t rows = static_cast<size_t>(g.ny()) * static_cast<size_t>(g.nz());
    const size_t grain = std::max<size_t>(1, 8192 / static_cast<size_t>(std::max(1, g.nx())));
    pg::parallelFor(rows, grain, [&](size_t begin, size_t end) {
        for (size_t r = begin; r < end; ++r) {
            f(static_cast<int>(r % static_cast<size_t>(g.ny())), static_cast<int>(r / static_cast<size_t>(g.ny())));
        }
    });
}

/// Without solids: the sum of the neighbours inside the grid, and the
/// diagonal -- 6, one more for every open side of the box the cell touches
/// (its ghost there is -p), one less for every wall (the face drops out).
PG_HOT_INLINE void neighbours(const Grid& p, int i, int j, int k, const bool closed[6], float& sum, float& diagonal) {
    const float* d = p.data();
    const size_t c = p.index(i, j, k);
    const size_t sy = static_cast<size_t>(p.nx()), sz = sy * static_cast<size_t>(p.ny());
    sum = 0.0f;
    diagonal = 6.0f;
    if (i > 0) sum += d[c - 1]; else diagonal += closed[0] ? -1.0f : 1.0f;
    if (i < p.nx() - 1) sum += d[c + 1]; else diagonal += closed[1] ? -1.0f : 1.0f;
    if (j > 0) sum += d[c - sy]; else diagonal += closed[2] ? -1.0f : 1.0f;
    if (j < p.ny() - 1) sum += d[c + sy]; else diagonal += closed[3] ? -1.0f : 1.0f;
    if (k > 0) sum += d[c - sz]; else diagonal += closed[4] ? -1.0f : 1.0f;
    if (k < p.nz() - 1) sum += d[c + sz]; else diagonal += closed[5] ? -1.0f : 1.0f;
}

/// With solids: the neighbours inside the grid, each weighted by the face
/// between. (The ghosts beyond the sides are in the diagonal.)
PG_HOT_INLINE float weightedSum(const Grid& p, const Grid* a, int i, int j, int k) {
    const float* d = p.data();
    const size_t c = p.index(i, j, k);
    const size_t sy = static_cast<size_t>(p.nx()), sz = sy * static_cast<size_t>(p.ny());
    float sum = 0.0f;
    if (i > 0) sum += a[0].at(i, j, k) * d[c - 1];
    if (i < p.nx() - 1) sum += a[0].at(i + 1, j, k) * d[c + 1];
    if (j > 0) sum += a[1].at(i, j, k) * d[c - sy];
    if (j < p.ny() - 1) sum += a[1].at(i, j + 1, k) * d[c + sy];
    if (k > 0) sum += a[2].at(i, j, k) * d[c - sz];
    if (k < p.nz() - 1) sum += a[2].at(i, j, k + 1) * d[c + sz];
    return sum;
}

/// The diagonal from the face coefficients: each face once, a face on a side
/// of the box twice (the ghost holds -p, twice the drop to the face).
Grid diagonals(const Grid* a, int nx, int ny, int nz) {
    Grid d(nx, ny, nz);
    forEachRow(d, [&](int j, int k) {
        for (int i = 0; i < nx; ++i) {
            float s = a[0].at(i, j, k) * (i == 0 ? 2.0f : 1.0f) + a[0].at(i + 1, j, k) * (i == nx - 1 ? 2.0f : 1.0f);
            s += a[1].at(i, j, k) * (j == 0 ? 2.0f : 1.0f) + a[1].at(i, j + 1, k) * (j == ny - 1 ? 2.0f : 1.0f);
            s += a[2].at(i, j, k) * (k == 0 ? 2.0f : 1.0f) + a[2].at(i, j, k + 1) * (k == nz - 1 ? 2.0f : 1.0f);
            d.at(i, j, k) = s;
        }
    });
    return d;
}

/// Fine to coarse: each coarse cell gets the mean of its eight children.
void restrictTo(const Grid& fine, Grid& coarse) {
    forEachRow(coarse, [&](int j, int k) {
        for (int i = 0; i < coarse.nx(); ++i) {
            float sum = 0.0f;
            for (int c = 0; c < 8; ++c) sum += fine.at(2 * i + (c & 1), 2 * j + ((c >> 1) & 1), 2 * k + (c >> 2));
            coarse.at(i, j, k) = sum * 0.125f;
        }
    });
}

bool canHalve(const Grid& g) {
    return g.nx() % 2 == 0 && g.ny() % 2 == 0 && g.nz() % 2 == 0 && std::min({g.nx(), g.ny(), g.nz()}) >= 4;
}

}  // namespace

void PoissonSolver::setBoundary(const PoissonBoundary& boundary) {
    std::copy(boundary.closed, boundary.closed + 6, closed_);
    solid_ = Grid();
    if (boundary.solid) {
        const std::vector<float>& v = boundary.solid->values();
        if (std::any_of(v.begin(), v.end(), [](float s) { return s > 0.5f; })) solid_ = *boundary.solid;
    }
    dirty_ = true;
}

void PoissonSolver::build(const Grid& fine, float h) {
    if (!dirty_ && dims_[0] == fine.nx() && dims_[1] == fine.ny() && dims_[2] == fine.nz() && h_ == h) return;
    dirty_ = false;
    dims_[0] = fine.nx();
    dims_[1] = fine.ny();
    dims_[2] = fine.nz();
    h_ = h;
    fineResidual_ = Grid(fine.nx(), fine.ny(), fine.nz());
    const bool solids = solid_.size() == fine.size() && !solid_.values().empty();

    // The finest faces: blocked by a solid on either side, or a wall.
    fineOp_ = Operator{};
    if (solids) {
        const int n[3] = {fine.nx(), fine.ny(), fine.nz()};
        for (int a = 0; a < 3; ++a) {
            Grid& faces = fineOp_.a[a];
            faces = Grid(n[0] + (a == 0), n[1] + (a == 1), n[2] + (a == 2));
            forEachRow(faces, [&](int j, int k) {
                for (int i = 0; i < faces.nx(); ++i) {
                    const int f = a == 0 ? i : a == 1 ? j : k;  // face f: between cells f-1 and f
                    const int bi = i - (a == 0), bj = j - (a == 1), bk = k - (a == 2);
                    const bool behind = f > 0 && solid_.at(bi, bj, bk) > 0.5f;
                    const bool ahead = f < n[a] && solid_.at(i, j, k) > 0.5f;
                    const bool wall = (f == 0 && closed_[2 * a]) || (f == n[a] && closed_[2 * a + 1]);
                    faces.at(i, j, k) = behind || ahead || wall ? 0.0f : 1.0f;
                }
            });
        }
        fineOp_.diagonal = diagonals(fineOp_.a, n[0], n[1], n[2]);
    }

    coarse_.clear();
    coarse_.reserve(16);  // pointers into it stay valid below
    const Grid* g = &fine;
    const Operator* op = &fineOp_;
    float levelH = h;
    while (canHalve(*g)) {
        levelH *= 2.0f;
        Level level;
        const int cx = g->nx() / 2, cy = g->ny() / 2, cz = g->nz() / 2;
        level.p = Grid(cx, cy, cz);
        level.b = level.p;
        level.r = level.p;
        level.h = levelH;
        if (solids) {
            // A coarse face is as open as the four fine faces it covers.
            for (int a = 0; a < 3; ++a) {
                Grid& faces = level.op.a[a];
                faces = Grid(cx + (a == 0), cy + (a == 1), cz + (a == 2));
                const Grid& finer = op->a[a];
                forEachRow(faces, [&](int j, int k) {
                    for (int i = 0; i < faces.nx(); ++i) {
                        float sum = 0.0f;
                        for (int q = 0; q < 4; ++q) {
                            const int u = q & 1, w = q >> 1;  // the two directions across the face
                            const int fi = a == 0 ? 2 * i : 2 * i + u;
                            const int fj = a == 1 ? 2 * j : 2 * j + (a == 0 ? u : w);
                            const int fk = a == 2 ? 2 * k : 2 * k + w;
                            sum += finer.at(fi, fj, fk);
                        }
                        faces.at(i, j, k) = 0.25f * sum;
                    }
                });
            }
            level.op.diagonal = diagonals(level.op.a, cx, cy, cz);
        }
        coarse_.push_back(std::move(level));
        g = &coarse_.back().p;
        op = &coarse_.back().op;
    }
}

void PoissonSolver::relax(Grid& p, const Grid& b, const Operator& op, float h, int sweeps, float omega) const {
    const float h2 = h * h;
    const bool weighted = !op.diagonal.values().empty();
    for (int s = 0; s < sweeps; ++s) {
        for (int colour = 0; colour < 2; ++colour) {
            forEachRow(p, [&](int j, int k) {
                const int first = (colour + j + k) & 1;
                if (weighted) {
                    for (int i = first; i < p.nx(); i += 2) {
                        float& v = p.at(i, j, k);
                        const float d = op.diagonal.at(i, j, k);
                        if (d <= 0.0f) {  // walled in on every side: nothing flows, nothing to solve
                            v = 0.0f;
                            continue;
                        }
                        v += omega * ((weightedSum(p, op.a, i, j, k) - h2 * b.at(i, j, k)) / d - v);
                    }
                } else {
                    for (int i = first; i < p.nx(); i += 2) {
                        float sum, diagonal;
                        neighbours(p, i, j, k, closed_, sum, diagonal);
                        float& v = p.at(i, j, k);
                        v += omega * ((sum - h2 * b.at(i, j, k)) / diagonal - v);
                    }
                }
            });
        }
    }
}

void PoissonSolver::computeResidual(const Grid& p, const Grid& b, const Operator& op, float h, Grid& r) const {
    const float invH2 = 1.0f / (h * h);
    const bool weighted = !op.diagonal.values().empty();
    forEachRow(p, [&](int j, int k) {
        for (int i = 0; i < p.nx(); ++i) {
            if (weighted) {
                const float d = op.diagonal.at(i, j, k);
                r.at(i, j, k) =
                    d <= 0.0f ? 0.0f : b.at(i, j, k) - (weightedSum(p, op.a, i, j, k) - d * p.at(i, j, k)) * invH2;
            } else {
                float sum, diagonal;
                neighbours(p, i, j, k, closed_, sum, diagonal);
                r.at(i, j, k) = b.at(i, j, k) - (sum - diagonal * p.at(i, j, k)) * invH2;
            }
        }
    });
}

/// Coarse to fine, added: trilinear, each fine cell between its coarse cell
/// (weight 3/4 per axis) and the next coarse cell towards it (1/4). Past a side
/// of the box the ghost mirrors the cell: negated at an open side, as in the
/// equation, as it is at a wall.
///
/// Separable: for each fine row, the four coarse rows around it are blended
/// into one line first, then each fine cell takes two values of that line --
/// half the reads of eight corners, and no bounds checks inside the row.
void PoissonSolver::prolongAdd(const Grid& coarse, Grid& fine) const {
    const float mirror[6] = {closed_[0] ? 1.0f : -1.0f, closed_[1] ? 1.0f : -1.0f, closed_[2] ? 1.0f : -1.0f,
                             closed_[3] ? 1.0f : -1.0f, closed_[4] ? 1.0f : -1.0f, closed_[5] ? 1.0f : -1.0f};
    const int cnx = coarse.nx(), cny = coarse.ny(), cnz = coarse.nz();
    const size_t rows = static_cast<size_t>(fine.ny()) * static_cast<size_t>(fine.nz());
    const size_t grain = std::max<size_t>(1, 8192 / static_cast<size_t>(std::max(1, fine.nx())));
    pg::parallelFor(rows, grain, [&](size_t begin, size_t end) {
        std::vector<float> line(static_cast<size_t>(cnx));
        for (size_t r = begin; r < end; ++r) {
            const int j = static_cast<int>(r % static_cast<size_t>(fine.ny()));
            const int k = static_cast<int>(r / static_cast<size_t>(fine.ny()));
            // The coarse rows: this one, and the next one towards the fine
            // row along y and along z -- or its mirror past a side.
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
            const float* r00 = coarse.data() + coarse.index(0, cj, ck);
            const float* r10 = coarse.data() + coarse.index(0, oj, ck);
            const float* r01 = coarse.data() + coarse.index(0, cj, ok);
            const float* r11 = coarse.data() + coarse.index(0, oj, ok);
            const float w00 = 0.5625f, w10 = 0.1875f * sy, w01 = 0.1875f * sz, w11 = 0.0625f * sy * sz;
            for (int i = 0; i < cnx; ++i) {
                line[static_cast<size_t>(i)] = w00 * r00[i] + w10 * r10[i] + w01 * r01[i] + w11 * r11[i];
            }
            float* out = fine.data() + fine.index(0, j, k);
            const int nx = fine.nx();
            for (int i = 0; i < nx; ++i) {
                const int ci = i >> 1;
                int oi = (i & 1) ? ci + 1 : ci - 1;
                float sx = 1.0f;
                if (oi < 0 || oi >= cnx) {
                    sx = mirror[oi < 0 ? 0 : 1];
                    oi = ci;
                }
                out[i] += 0.75f * line[static_cast<size_t>(ci)] + 0.25f * sx * line[static_cast<size_t>(oi)];
            }
        }
    });
}

void PoissonSolver::solve(Grid& p, const Grid& b, float h, int cycles) {
    build(p, h);
    for (int c = 0; c < cycles; ++c) vcycle(p, b, fineResidual_, fineOp_, h, 0);
}

void PoissonSolver::vcycle(Grid& p, const Grid& b, Grid& r, const Operator& op, float h, size_t next) {
    if (next == coarse_.size()) {
        // The coarsest grid: a few cells a side, solved by over-relaxed sweeps.
        // The factor is the optimum for Jacobi's rate on this box.
        const double pi = 3.14159265358979323846;
        const double rate = (std::cos(pi / p.nx()) + std::cos(pi / p.ny()) + std::cos(pi / p.nz())) / 3.0;
        const float omega = static_cast<float>(2.0 / (1.0 + std::sqrt(1.0 - rate * rate)));
        relax(p, b, op, h, 2 * std::max({p.nx(), p.ny(), p.nz()}), omega);
        return;
    }
    relax(p, b, op, h, kPreSmooth, 1.0f);
    computeResidual(p, b, op, h, r);
    Level& coarse = coarse_[next];
    restrictTo(r, coarse.b);
    coarse.p.fill(0.0f);
    vcycle(coarse.p, coarse.b, coarse.r, coarse.op, coarse.h, next + 1);
    prolongAdd(coarse.p, p);
    relax(p, b, op, h, kPostSmooth, 1.0f);
}

double PoissonSolver::residual(const Grid& p, const Grid& b, float h) {
    build(p, h);
    Grid r(p.nx(), p.ny(), p.nz());
    computeResidual(p, b, fineOp_, h, r);
    double sum = 0.0;
    for (const float v : r.values()) sum += std::fabs(v);
    return sum / static_cast<double>(p.size());
}

float PoissonSolver::faceOpen(int axis, int i, int j, int k) const {
    const Grid& a = fineOp_.a[axis];
    if (!a.values().empty()) return a.at(i, j, k);
    const int f = axis == 0 ? i : axis == 1 ? j : k;
    const int n = dims_[axis];
    if (f == 0) return closed_[2 * axis] ? 0.0f : 1.0f;
    if (f == n) return closed_[2 * axis + 1] ? 0.0f : 1.0f;
    return 1.0f;
}

}  // namespace pg::sim
