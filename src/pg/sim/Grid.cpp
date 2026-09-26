#include "pg/sim/Grid.h"

#include <algorithm>
#include <cmath>

namespace pg::sim {

Grid::Grid(int nx, int ny, int nz, float value)
    : nx_(nx), ny_(ny), nz_(nz),
      data_(static_cast<size_t>(nx) * static_cast<size_t>(ny) * static_cast<size_t>(nz), value) {}

float Grid::clamped(int i, int j, int k) const {
    return at(std::clamp(i, 0, nx_ - 1), std::clamp(j, 0, ny_ - 1), std::clamp(k, 0, nz_ - 1));
}

namespace {

/// Where a trilinear lookup reads: the lower corner and the strides to the
/// seven others (0 where the grid ends), and the weights along each axis.
struct Lookup {
    size_t base, dx, dy, dz;
    float tx, ty, tz;
};

inline Lookup locate(float x, float y, float z, int nx, int ny, int nz) {
    // Cell centres sit at i + 0.5: shift so that they are the integers. Written
    // so that a NaN lands on 0 instead of in an int conversion.
    const float fx = std::max(0.0f, std::min(x - 0.5f, static_cast<float>(nx - 1)));
    const float fy = std::max(0.0f, std::min(y - 0.5f, static_cast<float>(ny - 1)));
    const float fz = std::max(0.0f, std::min(z - 0.5f, static_cast<float>(nz - 1)));
    const int i = static_cast<int>(fx), j = static_cast<int>(fy), k = static_cast<int>(fz);
    const size_t sy = static_cast<size_t>(nx), sz = sy * static_cast<size_t>(ny);
    return {static_cast<size_t>(i) + sy * static_cast<size_t>(j) + sz * static_cast<size_t>(k),
            i + 1 < nx ? 1u : 0u,
            j + 1 < ny ? sy : 0u,
            k + 1 < nz ? sz : 0u,
            fx - static_cast<float>(i),
            fy - static_cast<float>(j),
            fz - static_cast<float>(k)};
}

inline bool outside(float x, float y, float z, int nx, int ny, int nz) {
    return x < 0.0f || y < 0.0f || z < 0.0f || x > static_cast<float>(nx) || y > static_cast<float>(ny) ||
           z > static_cast<float>(nz);
}

}  // namespace

float Grid::sample(float x, float y, float z, bool zeroOutside) const {
    if (zeroOutside && outside(x, y, z, nx_, ny_, nz_)) return 0.0f;
    const Lookup l = locate(x, y, z, nx_, ny_, nz_);
    const float* c = data_.data() + l.base;
    const float x00 = c[0] + (c[l.dx] - c[0]) * l.tx;
    const float x10 = c[l.dy] + (c[l.dy + l.dx] - c[l.dy]) * l.tx;
    const float x01 = c[l.dz] + (c[l.dz + l.dx] - c[l.dz]) * l.tx;
    const float x11 = c[l.dz + l.dy] + (c[l.dz + l.dy + l.dx] - c[l.dz + l.dy]) * l.tx;
    const float y0 = x00 + (x10 - x00) * l.ty, y1 = x01 + (x11 - x01) * l.ty;
    return y0 + (y1 - y0) * l.tz;
}

float Grid::sample(float x, float y, float z, bool zeroOutside, float& lo, float& hi) const {
    if (zeroOutside && outside(x, y, z, nx_, ny_, nz_)) {
        lo = hi = 0.0f;
        return 0.0f;
    }
    const Lookup l = locate(x, y, z, nx_, ny_, nz_);
    const float* c = data_.data() + l.base;
    const float c000 = c[0], c100 = c[l.dx], c010 = c[l.dy], c110 = c[l.dy + l.dx];
    const float c001 = c[l.dz], c101 = c[l.dz + l.dx], c011 = c[l.dz + l.dy], c111 = c[l.dz + l.dy + l.dx];
    lo = std::min({c000, c100, c010, c110, c001, c101, c011, c111});
    hi = std::max({c000, c100, c010, c110, c001, c101, c011, c111});
    const float x00 = c000 + (c100 - c000) * l.tx, x10 = c010 + (c110 - c010) * l.tx;
    const float x01 = c001 + (c101 - c001) * l.tx, x11 = c011 + (c111 - c011) * l.tx;
    const float y0 = x00 + (x10 - x00) * l.ty, y1 = x01 + (x11 - x01) * l.ty;
    return y0 + (y1 - y0) * l.tz;
}

void Grid::fill(float value) { std::fill(data_.begin(), data_.end(), value); }

float Grid::max() const {
    float m = data_.empty() ? 0.0f : data_[0];
    for (float v : data_) m = std::max(m, v);
    return m;
}

double Grid::sum() const {
    double s = 0.0;
    for (float v : data_) s += v;
    return s;
}

}  // namespace pg::sim
