#include "pg/sim/FreeSurface.h"

#include "pg/core/Parallel.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace pg::sim {
namespace {

/// Cells a chunk of work takes at least. Handing a chunk to another thread
/// costs about as much as a few thousand cells of these loops: a coarse grid
/// is one chunk, run on the calling thread.
constexpr size_t kChunkCells = 16384;

/// Rows of cells in chunks of whole rows. The split decides where a cell is
/// computed, never how.
template <class F>
void forEachRow(int nx, int ny, int nz, const F& f) {
    const size_t rows = static_cast<size_t>(ny) * static_cast<size_t>(nz);
    const size_t grain = std::max<size_t>(1, kChunkCells / static_cast<size_t>(std::max(1, nx)));
    pg::parallelFor(rows, grain, [&](size_t begin, size_t end) {
        for (size_t r = begin; r < end; ++r) {
            f(static_cast<int>(r % static_cast<size_t>(ny)), static_cast<int>(r / static_cast<size_t>(ny)));
        }
    });
}

/// Of a liquid cell's links (FreeSurfaceSolver::links_): bit 2a + side for
/// the face behind (side 0) or ahead (1) along axis a when it leads on into
/// the liquid -- an open face between two liquid cells, one with a weight;
/// kToAir when a face leads to air or out of an open side; kReached once a
/// flood got to the cell.
constexpr uint8_t kToAir = 64, kReached = 128;

bool canHalve(const int n[3]) {
    return n[0] % 2 == 0 && n[1] % 2 == 0 && n[2] % 2 == 0 && std::min({n[0], n[1], n[2]}) >= 4;
}

}  // namespace

template <class F>
void FreeSurfaceSolver::forActiveRows(const F& f) const {
    const size_t nx = static_cast<size_t>(levels_.front().n[0]);
    const size_t grain = std::max<size_t>(1, kChunkCells / nx);
    pg::parallelFor(activeRows_.size(), grain, [&](size_t begin, size_t end) {
        for (size_t r = begin; r < end; ++r) {
            const size_t first = static_cast<size_t>(activeRows_[r]) * nx;
            f(first, first + nx);
        }
    });
}

template <class Map>
double FreeSurfaceSolver::sumActive(const Map& map) const {
    const size_t nx = static_cast<size_t>(levels_.front().n[0]);
    const size_t grain = std::max<size_t>(1, kChunkCells / nx);
    return pg::parallelReduce(
        activeRows_.size(), grain, 0.0,
        [&](size_t begin, size_t end) {
            double s = 0.0;
            for (size_t r = begin; r < end; ++r) {
                const size_t first = static_cast<size_t>(activeRows_[r]) * nx;
                s += map(first, first + nx);
            }
            return s;
        },
        [](double a, double b) { return a + b; });
}

float FreeSurfaceSolver::surfaceFraction(float phiLiquid, float phiAir) {
    if (!(phiLiquid < 0.0f)) return kMinTheta;  // the surface is at the liquid cell's centre
    if (!(phiAir > 0.0f)) return 1.0f;          // no crossing seen: at the air cell's centre
    return std::clamp(phiLiquid / (phiLiquid - phiAir), kMinTheta, 1.0f);
}

float FreeSurfaceSolver::airWeight(float open, float phiLiquid, float phiAir) const {
    return open / surfaceFraction(phiLiquid, phiAir);
}

void FreeSurfaceSolver::build(Level& L, const Grid* phi, std::vector<uint8_t>* links) const {
    const int nx = L.n[0], ny = L.n[1], nz = L.n[2];
    const int n[3] = {nx, ny, nz};
    auto at = [&](int i, int j, int k) {
        return static_cast<size_t>(i) + static_cast<size_t>(nx) * (static_cast<size_t>(j) + static_cast<size_t>(ny) * static_cast<size_t>(k));
    };
    const uint8_t* cells = L.cells.data();
    // Between two liquid cells, a face weighs as much as it is open.
    for (int a = 0; a < 3; ++a) {
        Grid& w = L.weight[a];
        const Grid& open = L.open[a];
        w = Grid(nx + (a == 0), ny + (a == 1), nz + (a == 2));
        forEachRow(w.nx(), w.ny(), w.nz(), [&](int j, int k) {
            for (int i = 0; i < w.nx(); ++i) {
                const int f = a == 0 ? i : a == 1 ? j : k;  // face f: between cells f-1 and f
                if (f == 0 || f == n[a]) continue;
                const bool behind = cells[at(i - (a == 0), j - (a == 1), k - (a == 2))] == Liquid;
                const bool ahead = cells[at(i, j, k)] == Liquid;
                if (behind && ahead) w.at(i, j, k) = open.at(i, j, k);
            }
        });
    }
    // The diagonal: every open face of a liquid cell, towards liquid as it
    // weighs, towards air as far as the surface is. With `links`, which of
    // those faces lead on into the liquid, which to air.
    L.diagonal = Grid(nx, ny, nz);
    if (links) links->resize(L.cells.size());
    forEachRow(nx, ny, nz, [&](int j, int k) {
        for (int i = 0; i < nx; ++i) {
            const size_t c = at(i, j, k);
            if (cells[c] != Liquid) {
                if (links) (*links)[c] = 0;
                continue;
            }
            float sum = 0.0f;
            uint8_t bits = 0;
            const int here[3] = {i, j, k};
            for (int a = 0; a < 3; ++a) {
                for (int side = 0; side < 2; ++side) {
                    // The face on this side, and the cell beyond it.
                    int fi = i, fj = j, fk = k;
                    if (side == 1) (a == 0 ? fi : a == 1 ? fj : fk) += 1;
                    const float open = L.open[a].at(fi, fj, fk);
                    if (open <= 0.0f) continue;
                    const int beyond = here[a] + (side == 0 ? -1 : 1);
                    if (beyond < 0 || beyond >= n[a]) {
                        sum += 2.0f * open;  // an open side: p = 0 on it, half a cell away
                        bits |= kToAir;
                        continue;
                    }
                    const size_t nb = at(i + (a == 0 ? beyond - i : 0), j + (a == 1 ? beyond - j : 0),
                                         k + (a == 2 ? beyond - k : 0));
                    if (cells[nb] == Liquid) {
                        sum += open;
                        bits |= static_cast<uint8_t>(1u << (2 * a + side));
                    } else {
                        const float theta = phi ? surfaceFraction(phi->data()[c], phi->data()[nb]) : 1.0f;
                        sum += open / theta;
                        bits |= kToAir;
                    }
                }
            }
            L.diagonal.data()[c] = sum;
            if (links) (*links)[c] = bits;
        }
    });
    // A liquid cell with no open face takes no part.
    for (size_t c = 0; c < L.cells.size(); ++c) {
        if (L.cells[c] == Liquid && L.diagonal.data()[c] <= 0.0f) L.cells[c] = Solid;
    }
    // The rows with liquid in them: most of a grid is often air.
    L.rows.assign(static_cast<size_t>(ny) * static_cast<size_t>(nz), 0);
    for (size_t row = 0; row < L.rows.size(); ++row) {
        const uint8_t* first = cells + row * static_cast<size_t>(nx);
        L.rows[row] = std::find(first, first + nx, static_cast<uint8_t>(Liquid)) != first + nx;
    }
    L.x = Grid(nx, ny, nz);
    L.b = Grid(nx, ny, nz);
    L.r = Grid(nx, ny, nz);
}

void FreeSurfaceSolver::coarsen(const Level& fine, Level& coarse) const {
    for (int a = 0; a < 3; ++a) coarse.n[a] = fine.n[a] / 2;
    const int nx = coarse.n[0], ny = coarse.n[1], nz = coarse.n[2];
    const int fx = fine.n[0], fy = fine.n[1];
    coarse.cells.assign(static_cast<size_t>(nx) * static_cast<size_t>(ny) * static_cast<size_t>(nz), Air);
    forEachRow(nx, ny, nz, [&](int j, int k) {
        for (int i = 0; i < nx; ++i) {
            bool liquid = false, solid = true;
            for (int c = 0; c < 8; ++c) {
                const size_t child = static_cast<size_t>(2 * i + (c & 1)) +
                                     static_cast<size_t>(fx) * (static_cast<size_t>(2 * j + ((c >> 1) & 1)) +
                                                                static_cast<size_t>(fy) * static_cast<size_t>(2 * k + (c >> 2)));
                const uint8_t t = fine.cells[child];
                liquid = liquid || t == Liquid;
                solid = solid && t == Solid;
            }
            coarse.cells[static_cast<size_t>(i) + static_cast<size_t>(nx) * (static_cast<size_t>(j) + static_cast<size_t>(ny) * static_cast<size_t>(k))] =
                liquid ? Liquid : solid ? Solid : Air;
        }
    });
    // A coarse face is as open as the four fine faces it covers.
    for (int a = 0; a < 3; ++a) {
        Grid& faces = coarse.open[a];
        faces = Grid(nx + (a == 0), ny + (a == 1), nz + (a == 2));
        const Grid& finer = fine.open[a];
        forEachRow(faces.nx(), faces.ny(), faces.nz(), [&](int j, int k) {
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
    build(coarse, nullptr, nullptr);
}

void FreeSurfaceSolver::setSystem(const std::vector<uint8_t>& cells, const Grid open[3], const Grid& phi) {
    levels_.clear();
    levels_.reserve(8);
    Level fine;
    fine.n[0] = phi.nx();
    fine.n[1] = phi.ny();
    fine.n[2] = phi.nz();
    fine.cells = cells;
    for (int a = 0; a < 3; ++a) fine.open[a] = open[a];
    build(fine, &phi, &links_);
    unknowns_ = static_cast<size_t>(std::count(fine.cells.begin(), fine.cells.end(), static_cast<uint8_t>(Liquid)));
    levels_.push_back(std::move(fine));
    while (canHalve(levels_.back().n)) {
        Level coarse;
        coarsen(levels_.back(), coarse);
        levels_.push_back(std::move(coarse));
    }
    const Level& f = levels_.front();
    for (Grid* g : {&r_, &rOld_, &z_, &d_, &q_}) *g = Grid(f.n[0], f.n[1], f.n[2]);
    activeRows_.clear();
    for (size_t row = 0; row < f.rows.size(); ++row) {
        if (f.rows[row]) activeRows_.push_back(static_cast<uint32_t>(row));
    }
    findPockets();
    b_ = pockets() > 0 ? Grid(f.n[0], f.n[1], f.n[2]) : Grid();
}

void FreeSurfaceSolver::findPockets() {
    pocketCells_.clear();
    pocketStart_.clear();
    const Level& L = levels_.front();
    const size_t stride[3] = {1, static_cast<size_t>(L.n[0]), static_cast<size_t>(L.n[0]) * static_cast<size_t>(L.n[1])};
    const uint8_t* cells = L.cells.data();
    // From every cell next to air, through the liquid: breadth first, in the
    // order of the cells, the same on any number of threads.
    reached_.clear();
    auto flood = [&](size_t from) {
        for (size_t q = from; q < reached_.size(); ++q) {
            const size_t c = reached_[q];
            const uint8_t bits = links_[c];
            for (int a = 0; a < 3; ++a) {
                for (int side = 0; side < 2; ++side) {
                    if (!(bits & (1u << (2 * a + side)))) continue;
                    const size_t next = side == 0 ? c - stride[a] : c + stride[a];
                    if (links_[next] & kReached) continue;
                    links_[next] |= kReached;
                    reached_.push_back(static_cast<uint32_t>(next));
                }
            }
        }
    };
    for (const uint32_t row : activeRows_) {
        const size_t first = static_cast<size_t>(row) * stride[1];
        for (size_t c = first; c < first + stride[1]; ++c) {
            if (cells[c] != Liquid || !(links_[c] & kToAir)) continue;
            links_[c] |= kReached;
            reached_.push_back(static_cast<uint32_t>(c));
        }
    }
    flood(0);
    if (reached_.size() == unknowns_) return;  // no pocket: the usual case
    // What the air does not reach: pockets, each flooded from its first cell.
    for (const uint32_t row : activeRows_) {
        const size_t first = static_cast<size_t>(row) * stride[1];
        for (size_t c = first; c < first + stride[1]; ++c) {
            if (cells[c] != Liquid || (links_[c] & kReached)) continue;
            const size_t from = reached_.size();
            links_[c] |= kReached;
            reached_.push_back(static_cast<uint32_t>(c));
            flood(from);
            pocketStart_.push_back(static_cast<uint32_t>(pocketCells_.size()));
            pocketCells_.insert(pocketCells_.end(), reached_.begin() + static_cast<std::ptrdiff_t>(from), reached_.end());
        }
    }
    pocketStart_.push_back(static_cast<uint32_t>(pocketCells_.size()));
}

void FreeSurfaceSolver::applyOn(const Level& L, const Grid& x, Grid& out) const {
    const int nx = L.n[0], ny = L.n[1], nz = L.n[2];
    const size_t sy = static_cast<size_t>(nx), sz = sy * static_cast<size_t>(ny);
    const float* v = x.data();
    forEachRow(nx, ny, nz, [&](int j, int k) {
        if (!L.rows[static_cast<size_t>(j) + static_cast<size_t>(ny) * static_cast<size_t>(k)]) {
            std::fill_n(out.data() + x.index(0, j, k), nx, 0.0f);
            return;
        }
        for (int i = 0; i < nx; ++i) {
            const size_t c = x.index(i, j, k);
            if (L.cells[c] != Liquid) {
                out.data()[c] = 0.0f;
                continue;
            }
            float s = L.diagonal.data()[c] * v[c];
            if (i > 0) s -= L.weight[0].at(i, j, k) * v[c - 1];
            if (i < nx - 1) s -= L.weight[0].at(i + 1, j, k) * v[c + 1];
            if (j > 0) s -= L.weight[1].at(i, j, k) * v[c - sy];
            if (j < ny - 1) s -= L.weight[1].at(i, j + 1, k) * v[c + sy];
            if (k > 0) s -= L.weight[2].at(i, j, k) * v[c - sz];
            if (k < nz - 1) s -= L.weight[2].at(i, j, k + 1) * v[c + sz];
            out.data()[c] = s;
        }
    });
}

void FreeSurfaceSolver::apply(const Grid& p, Grid& out) const {
    if (levels_.empty()) return;
    applyOn(levels_.front(), p, out);
}

void FreeSurfaceSolver::residualOn(const Level& L, const Grid& x, const Grid& b, Grid& r) const {
    applyOn(L, x, r);
    const float* bb = b.data();
    float* rr = r.data();
    const uint8_t* cells = L.cells.data();
    pg::parallelFor(r.size(), kChunkCells, [&](size_t begin, size_t end) {
        for (size_t c = begin; c < end; ++c) rr[c] = cells[c] == Liquid ? bb[c] - rr[c] : 0.0f;
    });
}

void FreeSurfaceSolver::relax(const Level& L, Grid& x, const Grid& b, int colour, float omega) const {
    const int nx = L.n[0], ny = L.n[1], nz = L.n[2];
    const size_t sy = static_cast<size_t>(nx), sz = sy * static_cast<size_t>(ny);
    float* v = x.data();
    forEachRow(nx, ny, nz, [&](int j, int k) {
        if (!L.rows[static_cast<size_t>(j) + static_cast<size_t>(ny) * static_cast<size_t>(k)]) return;
        const int first = (colour + j + k) & 1;
        for (int i = first; i < nx; i += 2) {
            const size_t c = x.index(i, j, k);
            if (L.cells[c] != Liquid) continue;
            float s = b.data()[c];
            if (i > 0) s += L.weight[0].at(i, j, k) * v[c - 1];
            if (i < nx - 1) s += L.weight[0].at(i + 1, j, k) * v[c + 1];
            if (j > 0) s += L.weight[1].at(i, j, k) * v[c - sy];
            if (j < ny - 1) s += L.weight[1].at(i, j + 1, k) * v[c + sy];
            if (k > 0) s += L.weight[2].at(i, j, k) * v[c - sz];
            if (k < nz - 1) s += L.weight[2].at(i, j, k + 1) * v[c + sz];
            v[c] += omega * (s / L.diagonal.data()[c] - v[c]);
        }
    });
}

/// Fine to coarse: the coarse grid's cells are twice as wide, so its equation
/// -- multiplied by its h^2 -- takes four times the mean of the eight
/// residuals: half their sum.
void FreeSurfaceSolver::restrictTo(const Level& fine, const Grid& r, Level& coarse) const {
    const int nx = coarse.n[0], ny = coarse.n[1], nz = coarse.n[2];
    forEachRow(nx, ny, nz, [&](int j, int k) {
        if (!coarse.rows[static_cast<size_t>(j) + static_cast<size_t>(ny) * static_cast<size_t>(k)]) {
            std::fill_n(coarse.b.data() + coarse.b.index(0, j, k), nx, 0.0f);
            return;
        }
        for (int i = 0; i < nx; ++i) {
            const size_t c = coarse.b.index(i, j, k);
            if (coarse.cells[c] != Liquid) {
                coarse.b.data()[c] = 0.0f;
                continue;
            }
            float sum = 0.0f;
            for (int q = 0; q < 8; ++q) sum += r.at(2 * i + (q & 1), 2 * j + ((q >> 1) & 1), 2 * k + (q >> 2));
            coarse.b.data()[c] = 0.5f * sum;
        }
    });
    (void)fine;
}

/// Coarse to fine, added to the liquid cells: trilinear, each fine cell
/// between its coarse cell (3/4 along each axis) and the next one towards it
/// (1/4). A neighbour of air counts 0 -- the pressure is 0 there; one of solid,
/// or past a closed side, counts as the cell itself -- nothing flows there,
/// the correction goes on level.
void FreeSurfaceSolver::prolongAdd(const Level& coarse, const Level& fine, Grid& x) const {
    const int cn[3] = {coarse.n[0], coarse.n[1], coarse.n[2]};
    forEachRow(fine.n[0], fine.n[1], fine.n[2], [&](int j, int k) {
        if (!fine.rows[static_cast<size_t>(j) + static_cast<size_t>(fine.n[1]) * static_cast<size_t>(k)]) return;
        for (int i = 0; i < fine.n[0]; ++i) {
            const size_t f = x.index(i, j, k);
            if (fine.cells[f] != Liquid) continue;
            const int p[3] = {i >> 1, j >> 1, k >> 1};
            const int o[3] = {(i & 1) ? 1 : -1, (j & 1) ? 1 : -1, (k & 1) ? 1 : -1};
            const float own = coarse.x.at(p[0], p[1], p[2]);
            float sum = 0.0f;
            for (int q = 0; q < 8; ++q) {
                int c[3];
                float w = 1.0f;
                bool outside = false, openSide = false;
                for (int a = 0; a < 3; ++a) {
                    const bool step = (q >> a) & 1;
                    c[a] = p[a] + (step ? o[a] : 0);
                    w *= step ? 0.25f : 0.75f;
                    if (c[a] < 0 || c[a] >= cn[a]) {
                        outside = true;
                        // The side's face next to the parent: a wall, or open.
                        int fi = p[0], fj = p[1], fk = p[2];
                        (a == 0 ? fi : a == 1 ? fj : fk) = c[a] < 0 ? 0 : cn[a];
                        if (coarse.open[a].at(fi, fj, fk) >= 0.5f) openSide = true;
                    }
                }
                float value;
                if (outside) {
                    value = openSide ? 0.0f : own;
                } else {
                    const size_t cc = coarse.x.index(c[0], c[1], c[2]);
                    const uint8_t t = coarse.cells[cc];
                    value = t == Liquid ? coarse.x.data()[cc] : t == Solid ? own : 0.0f;
                }
                sum += w * value;
            }
            x.data()[f] += sum;
        }
    });
}

void FreeSurfaceSolver::vcycle(size_t level) {
    Level& L = levels_[level];
    if (level + 1 == levels_.size()) {
        // The coarsest grid: a few cells a side, over-relaxed sweeps.
        const double pi = 3.14159265358979323846;
        const double rate = (std::cos(pi / L.n[0]) + std::cos(pi / L.n[1]) + std::cos(pi / L.n[2])) / 3.0;
        const float omega = static_cast<float>(2.0 / (1.0 + std::sqrt(1.0 - rate * rate)));
        const int sweeps = 2 * std::max({L.n[0], L.n[1], L.n[2]});
        for (int s = 0; s < sweeps; ++s) {
            relax(L, L.x, L.b, 0, omega);
            relax(L, L.x, L.b, 1, omega);
        }
        return;
    }
    relax(L, L.x, L.b, 0, 1.0f);
    relax(L, L.x, L.b, 1, 1.0f);
    residualOn(L, L.x, L.b, L.r);
    Level& C = levels_[level + 1];
    restrictTo(L, L.r, C);
    C.x.fill(0.0f);
    vcycle(level + 1);
    prolongAdd(C, L, L.x);
    relax(L, L.x, L.b, 1, 1.0f);
    relax(L, L.x, L.b, 0, 1.0f);
}

void FreeSurfaceSolver::precondition(const Grid& r, Grid& z) {
    // Only the rows with liquid: the rest of the finest grid's x stays 0, of
    // b and z unread.
    Level& L = levels_.front();
    forActiveRows([&](size_t begin, size_t end) {
        std::copy(r.data() + begin, r.data() + end, L.b.data() + begin);
        std::fill(L.x.data() + begin, L.x.data() + end, 0.0f);
    });
    vcycle(0);
    forActiveRows([&](size_t begin, size_t end) { std::copy(L.x.data() + begin, L.x.data() + end, z.data() + begin); });
}

int FreeSurfaceSolver::solve(Grid& p, const Grid& rhs, float tolerance, int maxIterations) {
    residual_ = 0.0;
    if (levels_.empty()) return 0;
    const uint8_t* cells = levels_.front().cells.data();
    // Nothing outside the liquid.
    for (size_t c = 0; c < p.size(); ++c) {
        if (cells[c] != Liquid) p.data()[c] = 0.0f;
    }
    const size_t pockets = this->pockets();
    if (pockets == 0) return iterate(p, rhs, tolerance, maxIterations);
    // In each pocket the right-hand side less its mean, which sums to 0; the
    // mean of its pressure as it came, the level it keeps.
    forActiveRows([&](size_t begin, size_t end) { std::copy(rhs.data() + begin, rhs.data() + end, b_.data() + begin); });
    std::vector<double> level(pockets);
    for (size_t q = 0; q < pockets; ++q) {
        const uint32_t* first = pocketCells_.data() + pocketStart_[q];
        const size_t count = pocketStart_[q + 1] - pocketStart_[q];
        double inflow = 0.0, sum = 0.0;
        for (size_t c = 0; c < count; ++c) {
            inflow += rhs.data()[first[c]];
            sum += p.data()[first[c]];
        }
        const float mean = static_cast<float>(inflow / static_cast<double>(count));
        for (size_t c = 0; c < count; ++c) b_.data()[first[c]] -= mean;
        level[q] = sum / static_cast<double>(count);
    }
    const int iterations = iterate(p, b_, tolerance, maxIterations);
    for (size_t q = 0; q < pockets; ++q) {
        const uint32_t* first = pocketCells_.data() + pocketStart_[q];
        const size_t count = pocketStart_[q + 1] - pocketStart_[q];
        double sum = 0.0;
        for (size_t c = 0; c < count; ++c) sum += p.data()[first[c]];
        const float shift = static_cast<float>(level[q] - sum / static_cast<double>(count));
        for (size_t c = 0; c < count; ++c) p.data()[first[c]] += shift;
    }
    return iterations;
}

int FreeSurfaceSolver::iterate(Grid& p, const Grid& rhs, float tolerance, int maxIterations) {
    const Level& L = levels_.front();
    const uint8_t* cells = L.cells.data();
    // Every vector below is read in the rows with liquid alone: those rows of
    // r, z, d and q are what the iterations are about; elsewhere z and d stay
    // 0 (as set up), which is what the operator reads next to the liquid.
    auto largest = [&](const Grid& g) {
        const size_t nx = static_cast<size_t>(L.n[0]);
        return pg::parallelReduce(
            activeRows_.size(), std::max<size_t>(1, kChunkCells / nx), 0.0,
            [&](size_t begin, size_t end) {
                double m = 0.0;
                for (size_t r = begin; r < end; ++r) {
                    const size_t first = static_cast<size_t>(activeRows_[r]) * nx;
                    for (size_t c = first; c < first + nx; ++c) {
                        if (cells[c] == Liquid) m = std::max(m, static_cast<double>(std::fabs(g.data()[c])));
                    }
                }
                return m;
            },
            [](double a, double b) { return std::max(a, b); });
    };
    auto dot = [&](const Grid& a, const Grid& b) {
        return sumActive([&](size_t begin, size_t end) {
            double s = 0.0;
            for (size_t c = begin; c < end; ++c) s += static_cast<double>(a.data()[c]) * static_cast<double>(b.data()[c]);
            return s;
        });
    };
    residualOn(L, p, rhs, r_);
    const double bNorm = largest(rhs);
    if (bNorm <= 0.0) {
        p.fill(0.0f);
        return 0;
    }
    double rNorm = largest(r_);
    residual_ = rNorm / bNorm;
    if (rNorm <= tolerance * bNorm) return 0;

    precondition(r_, z_);
    double rz = dot(r_, z_);
    if (!(rz > 0.0)) {  // the V-cycle went astray: plain steepest descent this once
        forActiveRows([&](size_t begin, size_t end) { std::copy(r_.data() + begin, r_.data() + end, z_.data() + begin); });
        rz = dot(r_, z_);
    }
    forActiveRows([&](size_t begin, size_t end) { std::copy(z_.data() + begin, z_.data() + end, d_.data() + begin); });
    for (int it = 1; it <= maxIterations; ++it) {
        applyOn(L, d_, q_);
        const double dq = dot(d_, q_);
        if (!(dq > 0.0)) return it;
        const float alpha = static_cast<float>(rz / dq);
        forActiveRows([&](size_t begin, size_t end) {
            float* pp = p.data();
            float* rr = r_.data();
            float* old = rOld_.data();
            const float* dd = d_.data();
            const float* qq = q_.data();
            for (size_t c = begin; c < end; ++c) {
                old[c] = rr[c];
                pp[c] += alpha * dd[c];
                rr[c] -= alpha * qq[c];
            }
        });
        rNorm = largest(r_);
        residual_ = rNorm / bNorm;
        if (rNorm <= tolerance * bNorm) return it;
        precondition(r_, z_);
        // Polak-Ribiere: what is new in z since the last step.
        double rzNew = 0.0, zOld = 0.0;
        {
            const size_t nx = static_cast<size_t>(L.n[0]);
            struct Pair {
                double a = 0.0, b = 0.0;
            };
            const Pair both = pg::parallelReduce(
                activeRows_.size(), std::max<size_t>(1, kChunkCells / nx), Pair{},
                [&](size_t begin, size_t end) {
                    Pair s;
                    for (size_t r = begin; r < end; ++r) {
                        const size_t first = static_cast<size_t>(activeRows_[r]) * nx;
                        for (size_t c = first; c < first + nx; ++c) {
                            const double z = z_.data()[c];
                            s.a += z * static_cast<double>(r_.data()[c]);
                            s.b += z * static_cast<double>(rOld_.data()[c]);
                        }
                    }
                    return s;
                },
                [](Pair x, Pair y) { return Pair{x.a + y.a, x.b + y.b}; });
            rzNew = both.a;
            zOld = both.b;
        }
        if (!(rzNew > 0.0)) {
            forActiveRows([&](size_t begin, size_t end) { std::copy(r_.data() + begin, r_.data() + end, z_.data() + begin); });
            rzNew = dot(z_, r_);
            zOld = dot(z_, rOld_);
        }
        const float beta = static_cast<float>(std::max(0.0, (rzNew - zOld) / rz));
        forActiveRows([&](size_t begin, size_t end) {
            float* d = d_.data();
            const float* zz = z_.data();
            for (size_t c = begin; c < end; ++c) d[c] = zz[c] + beta * d[c];
        });
        rz = rzNew;
    }
    return maxIterations;
}

}  // namespace pg::sim
