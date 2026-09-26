#include "pg/sim/Poisson.h"

#include "pg/core/Parallel.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace pg::sim {
namespace {

constexpr int kPreSmooth = 2;
constexpr int kPostSmooth = 2;

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

/// Sum of the neighbours inside the grid, and the diagonal: 6, plus one for
/// every face of the box the cell touches (its ghost neighbour is -p).
inline void neighbours(const Grid& p, int i, int j, int k, float& sum, float& diagonal) {
    const float* d = p.data();
    const size_t c = p.index(i, j, k);
    const size_t sy = static_cast<size_t>(p.nx()), sz = sy * static_cast<size_t>(p.ny());
    sum = 0.0f;
    diagonal = 6.0f;
    if (i > 0) sum += d[c - 1]; else diagonal += 1.0f;
    if (i < p.nx() - 1) sum += d[c + 1]; else diagonal += 1.0f;
    if (j > 0) sum += d[c - sy]; else diagonal += 1.0f;
    if (j < p.ny() - 1) sum += d[c + sy]; else diagonal += 1.0f;
    if (k > 0) sum += d[c - sz]; else diagonal += 1.0f;
    if (k < p.nz() - 1) sum += d[c + sz]; else diagonal += 1.0f;
}

/// Red-black Gauss-Seidel sweeps, over-relaxed by omega (1 = plain).
void relax(Grid& p, const Grid& b, float h, int sweeps, float omega) {
    const float h2 = h * h;
    for (int s = 0; s < sweeps; ++s) {
        for (int colour = 0; colour < 2; ++colour) {
            forEachRow(p, [&](int j, int k) {
                for (int i = (colour + j + k) & 1; i < p.nx(); i += 2) {
                    float sum, diagonal;
                    neighbours(p, i, j, k, sum, diagonal);
                    float& v = p.at(i, j, k);
                    v += omega * ((sum - h2 * b.at(i, j, k)) / diagonal - v);
                }
            });
        }
    }
}

void computeResidual(const Grid& p, const Grid& b, float h, Grid& r) {
    const float invH2 = 1.0f / (h * h);
    forEachRow(p, [&](int j, int k) {
        for (int i = 0; i < p.nx(); ++i) {
            float sum, diagonal;
            neighbours(p, i, j, k, sum, diagonal);
            r.at(i, j, k) = b.at(i, j, k) - (sum - diagonal * p.at(i, j, k)) * invH2;
        }
    });
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

/// Coarse to fine, added: trilinear, each fine cell between its coarse cell
/// (weight 3/4 per axis) and the next coarse cell towards it (1/4). Past a
/// face of the box the ghost is the cell mirrored and negated, as in the
/// equation.
void prolongAdd(const Grid& coarse, Grid& fine) {
    forEachRow(fine, [&](int j, int k) {
        const int cj = j >> 1, ck = k >> 1;
        const int oj = (j & 1) ? cj + 1 : cj - 1, ok = (k & 1) ? ck + 1 : ck - 1;
        for (int i = 0; i < fine.nx(); ++i) {
            const int ci = i >> 1, oi = (i & 1) ? ci + 1 : ci - 1;
            float value = 0.0f;
            for (int c = 0; c < 8; ++c) {
                int x = (c & 1) ? oi : ci, y = (c & 2) ? oj : cj, z = (c & 4) ? ok : ck;
                const float w = ((c & 1) ? 0.25f : 0.75f) * ((c & 2) ? 0.25f : 0.75f) * ((c & 4) ? 0.25f : 0.75f);
                float sign = 1.0f;
                if (x < 0 || x >= coarse.nx()) { x = ci; sign = -sign; }
                if (y < 0 || y >= coarse.ny()) { y = cj; sign = -sign; }
                if (z < 0 || z >= coarse.nz()) { z = ck; sign = -sign; }
                value += w * sign * coarse.at(x, y, z);
            }
            fine.at(i, j, k) += value;
        }
    });
}

bool canHalve(const Grid& g) {
    return g.nx() % 2 == 0 && g.ny() % 2 == 0 && g.nz() % 2 == 0 && std::min({g.nx(), g.ny(), g.nz()}) >= 4;
}

}  // namespace

void PoissonSolver::build(const Grid& fine, float h) {
    const bool same = fineResidual_.nx() == fine.nx() && fineResidual_.ny() == fine.ny() &&
                      fineResidual_.nz() == fine.nz() && (coarse_.empty() || coarse_[0].h == 2.0f * h);
    if (same) return;
    fineResidual_ = Grid(fine.nx(), fine.ny(), fine.nz());
    coarse_.clear();
    const Grid* g = &fine;
    float levelH = h;
    while (canHalve(*g)) {
        levelH *= 2.0f;
        Level level;
        level.p = Grid(g->nx() / 2, g->ny() / 2, g->nz() / 2);
        level.b = level.p;
        level.r = level.p;
        level.h = levelH;
        coarse_.push_back(std::move(level));
        g = &coarse_.back().p;
    }
}

void PoissonSolver::solve(Grid& p, const Grid& b, float h, int cycles) {
    build(p, h);
    for (int c = 0; c < cycles; ++c) vcycle(p, b, fineResidual_, h, 0);
}

void PoissonSolver::vcycle(Grid& p, const Grid& b, Grid& r, float h, size_t next) {
    if (next == coarse_.size()) {
        // The coarsest grid: a few cells a side, solved by over-relaxed sweeps.
        // The factor is the optimum for Jacobi's rate on this box.
        const double pi = 3.14159265358979323846;
        const double rate = (std::cos(pi / p.nx()) + std::cos(pi / p.ny()) + std::cos(pi / p.nz())) / 3.0;
        const float omega = static_cast<float>(2.0 / (1.0 + std::sqrt(1.0 - rate * rate)));
        relax(p, b, h, 2 * std::max({p.nx(), p.ny(), p.nz()}), omega);
        return;
    }
    relax(p, b, h, kPreSmooth, 1.0f);
    computeResidual(p, b, h, r);
    Level& coarse = coarse_[next];
    restrictTo(r, coarse.b);
    coarse.p.fill(0.0f);
    vcycle(coarse.p, coarse.b, coarse.r, coarse.h, next + 1);
    prolongAdd(coarse.p, p);
    relax(p, b, h, kPostSmooth, 1.0f);
}

double PoissonSolver::residual(const Grid& p, const Grid& b, float h) {
    const float invH2 = 1.0f / (h * h);
    double sum = 0.0;
    for (int k = 0; k < p.nz(); ++k) {
        for (int j = 0; j < p.ny(); ++j) {
            for (int i = 0; i < p.nx(); ++i) {
                float s, diagonal;
                neighbours(p, i, j, k, s, diagonal);
                sum += std::fabs(b.at(i, j, k) - (s - diagonal * p.at(i, j, k)) * invH2);
            }
        }
    }
    return sum / static_cast<double>(p.size());
}

}  // namespace pg::sim
