#include "pg/core/ColorSpace.h"

#include <cmath>

namespace pg {
namespace color {

D33 inverse(const D33& m) {
    const double a = m[0], b = m[1], c = m[2], d = m[3], e = m[4], f = m[5], g = m[6], h = m[7], i = m[8];
    const double ca = e * i - f * h, cb = f * g - d * i, cc = d * h - e * g;
    const double k = 1.0 / (a * ca + b * cb + c * cc);
    return {ca * k, (c * h - b * i) * k, (b * f - c * e) * k,  //
            cb * k, (a * i - c * g) * k, (c * d - a * f) * k,  //
            cc * k, (b * g - a * h) * k, (a * e - b * d) * k};
}

D33 times(const D33& a, const D33& b) {
    D33 out{};
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            out[static_cast<size_t>(3 * r + c)] = a[static_cast<size_t>(3 * r)] * b[static_cast<size_t>(c)] +
                                                  a[static_cast<size_t>(3 * r + 1)] * b[static_cast<size_t>(3 + c)] +
                                                  a[static_cast<size_t>(3 * r + 2)] * b[static_cast<size_t>(6 + c)];
        }
    }
    return out;
}

std::array<double, 3> times(const D33& m, const std::array<double, 3>& v) {
    return {m[0] * v[0] + m[1] * v[1] + m[2] * v[2], m[3] * v[0] + m[4] * v[1] + m[5] * v[2],
            m[6] * v[0] + m[7] * v[1] + m[8] * v[2]};
}

D33 rgbToXyz(const Primaries& p) {
    const D33 m = {p.xy[0][0], p.xy[1][0], p.xy[2][0],  //
                   p.xy[0][1], p.xy[1][1], p.xy[2][1],  //
                   1.0 - p.xy[0][0] - p.xy[0][1], 1.0 - p.xy[1][0] - p.xy[1][1], 1.0 - p.xy[2][0] - p.xy[2][1]};
    const std::array<double, 3> white = {p.xy[3][0] / p.xy[3][1], 1.0, (1.0 - p.xy[3][0] - p.xy[3][1]) / p.xy[3][1]};
    // Each primary as bright as makes them add up to the white.
    const std::array<double, 3> gains = times(inverse(m), white);
    D33 out{};
    for (size_t r = 0; r < 3; ++r) {
        for (size_t c = 0; c < 3; ++c) out[3 * r + c] = gains[c] * m[3 * r + c];
    }
    return out;
}

namespace {

/// Bradford's chromatic adaptation from the white XYZ `from` to `to`: the
/// cone responses scaled from one white to the other.
D33 bradford(const std::array<double, 3>& from, const std::array<double, 3>& to) {
    constexpr D33 cone = {0.8951, 0.2664, -0.1614, -0.7502, 1.7135, 0.0367, 0.0389, -0.0685, 1.0296};
    const std::array<double, 3> a = times(cone, from), b = times(cone, to);
    const D33 gain = {b[0] / a[0], 0.0, 0.0, 0.0, b[1] / a[1], 0.0, 0.0, 0.0, b[2] / a[2]};
    return times(inverse(cone), times(gain, cone));
}

constexpr std::array<double, 3> kOnes = {1.0, 1.0, 1.0};

}  // namespace

D33 conversion(const Primaries& from, const Primaries& to, bool adapt) {
    const D33 src = rgbToXyz(from), dst = rgbToXyz(to);
    const bool same = from.xy[3][0] == to.xy[3][0] && from.xy[3][1] == to.xy[3][1];
    if (same || !adapt) return times(inverse(dst), src);
    return times(inverse(dst), times(bradford(times(src, kOnes), times(dst, kOnes)), src));
}

D33 toXyzD65(const Primaries& from) {
    const D33 src = rgbToXyz(from);
    const std::array<double, 3> d65 = {0.3127 / 0.3290, 1.0, (1.0 - 0.3127 - 0.3290) / 0.3290};
    return times(bradford(times(src, kOnes), d65), src);
}

}  // namespace color

namespace {

const color::Primaries& primariesOf(LinearSpace space) {
    switch (space) {
        case LinearSpace::ACEScg: return color::kAP1;
        case LinearSpace::ACES2065_1: return color::kAP0;
        case LinearSpace::Rec709: break;
    }
    return color::kRec709;
}

color::Primaries primariesOf(const Chromaticities& c) {
    color::Primaries p{};
    for (size_t i = 0; i < 4; ++i) {
        p.xy[i][0] = c[2 * i];
        p.xy[i][1] = c[2 * i + 1];
    }
    return p;
}

Mat3 mat3(const color::D33& m) {
    // glm's columns: the matrix's.
    return Mat3(static_cast<float>(m[0]), static_cast<float>(m[3]), static_cast<float>(m[6]),  //
                static_cast<float>(m[1]), static_cast<float>(m[4]), static_cast<float>(m[7]),  //
                static_cast<float>(m[2]), static_cast<float>(m[5]), static_cast<float>(m[8]));
}

}  // namespace

Chromaticities chromaticitiesOf(LinearSpace space) {
    const color::Primaries& p = primariesOf(space);
    Chromaticities out{};
    for (size_t i = 0; i < 4; ++i) {
        out[2 * i] = static_cast<float>(p.xy[i][0]);
        out[2 * i + 1] = static_cast<float>(p.xy[i][1]);
    }
    return out;
}

bool isRec709(const Chromaticities& c) {
    const Chromaticities r = chromaticitiesOf(LinearSpace::Rec709);
    for (size_t i = 0; i < 8; ++i) {
        if (!(std::abs(c[i] - r[i]) < 1e-4f)) return false;
    }
    return true;
}

Vec3 fromRec709(const Vec3& c, LinearSpace space) {
    if (space == LinearSpace::Rec709) return c;
    static const Mat3 toCg = mat3(color::conversion(color::kRec709, color::kAP1, true)),
                      toAp0 = mat3(color::conversion(color::kRec709, color::kAP0, true));
    return (space == LinearSpace::ACEScg ? toCg : toAp0) * c;
}

Vec3 toRec709(const Vec3& c, LinearSpace space) {
    if (space == LinearSpace::Rec709) return c;
    static const Mat3 fromCg = mat3(color::conversion(color::kAP1, color::kRec709, true)),
                      fromAp0 = mat3(color::conversion(color::kAP0, color::kRec709, true));
    return (space == LinearSpace::ACEScg ? fromCg : fromAp0) * c;
}

Mat3 toRec709From(const Chromaticities& c) { return mat3(color::conversion(primariesOf(c), color::kRec709, true)); }

Mat3 fromRec709To(const Chromaticities& c) { return mat3(color::conversion(color::kRec709, primariesOf(c), true)); }

}  // namespace pg
