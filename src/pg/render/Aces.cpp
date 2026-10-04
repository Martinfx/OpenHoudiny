#include "pg/render/Aces.h"

#include "pg/core/ColorSpace.h"

#include <algorithm>
#include <cmath>
#include <limits>

// The output transforms follow OpenColorIO 2.6 (BSD-3-Clause, Copyright
// Contributors to the OpenColorIO Project) -- its ACES 1.0 ops and its ACES
// 2.0 fixed function (ops/fixedfunction/ACES2) -- and the Academy's CTL
// they come from: the same steps, constants and tables, in float as there,
// so that a picture comes out as OpenColorIO shows it.

namespace pg::render {
namespace {

// ------------------------------------------------------------------ matrices
// Built in double as OpenColorIO builds them (core/ColorSpace.h), applied
// in float: row by row, to a column.

using color::conversion;
using color::D33;
using color::kAP0;
using color::kAP1;
using color::kRec709;
using color::Primaries;
using color::rgbToXyz;
using color::toXyzD65;
using F33 = std::array<float, 9>;
using F3 = std::array<float, 3>;

/// The colour appearance model's own primaries (ACES 2.0's CAM16 variant).
constexpr Primaries kCam16{{{0.8336, 0.1735}, {2.3854, -1.4659}, {0.087, -0.125}, {0.333, 0.333}}};

D33 inverse(const D33& m) { return color::inverse(m); }
std::array<double, 3> times(const D33& m, const std::array<double, 3>& v) { return color::times(m, v); }

F33 toFloat(const D33& m) {
    F33 out{};
    for (size_t i = 0; i < 9; ++i) out[i] = static_cast<float>(m[i]);
    return out;
}

F33 inverse(const F33& m) {
    D33 d{};
    for (size_t i = 0; i < 9; ++i) d[i] = m[i];
    return toFloat(inverse(d));
}

F33 times(const F33& a, const F33& b) {
    F33 out{};
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            out[3 * r + c] = a[3 * r] * b[c] + a[3 * r + 1] * b[3 + c] + a[3 * r + 2] * b[6 + c];
        }
    }
    return out;
}

F3 times(const F33& m, const F3& v) {
    return {v[0] * m[0] + v[1] * m[1] + v[2] * m[2], v[0] * m[3] + v[1] * m[4] + v[2] * m[5],
            v[0] * m[6] + v[1] * m[7] + v[2] * m[8]};
}

F3 times(float k, const F3& v) { return {k * v[0], k * v[1], k * v[2]}; }

F3 f3(const Vec3& v) { return {v.x, v.y, v.z}; }
Vec3 vec(const F3& v) { return {v[0], v[1], v[2]}; }

float lerpf(float a, float b, float t) { return (b - a) * t + a; }

// ------------------------------------------------------------------- sRGB
// OpenColorIO's ExponentWithLinearTransform, gamma 2.4 and offset 0.055:
// the line near black meets the power where both have the same slope.

constexpr double kSrgbGamma = 2.4, kSrgbOffset = 0.055;
const double kSrgbBreak =
    std::pow(kSrgbOffset * kSrgbGamma / ((kSrgbGamma - 1.0) * (1.0 + kSrgbOffset)), kSrgbGamma);
const double kSrgbSlope =
    std::pow((kSrgbGamma - 1.0) / kSrgbOffset, kSrgbGamma - 1.0) * std::pow((1.0 + kSrgbOffset) / kSrgbGamma, kSrgbGamma);

// ------------------------------------------------------------------ ACES 1.0

namespace aces1 {

// From linear Rec. 709 to ACES2065-1, into the rendering space AP1, and
// from AP1 out to the screen: XYZ with its white D65, then Rec. 709.
const F33 k709ToAp0 = toFloat(conversion(kRec709, kAP0, true));
const F33 kAp0ToAp1 = toFloat(conversion(kAP0, kAP1, false));
const F33 kAp1ToXyz = toFloat(toXyzD65(kAP1));
const F33 kXyzTo709 = toFloat(inverse(rgbToXyz(kRec709)));
/// The RRT's global desaturation, 0.96 (calc_sat_adjust_matrix).
constexpr F33 kRrtSat = {0.970889148671f, 0.026963270632f, 0.002147580696f,  //
                         0.010889148671f, 0.986963270632f, 0.002147580696f,  //
                         0.010889148671f, 0.026963270632f, 0.962147580696f};
/// SDR video's desaturation, 0.93: 48 nits shown at 100 look more colourful.
constexpr F33 kDesat100 = {0.949056010175f, 0.047185723607f, 0.003758266219f,  //
                           0.019056010175f, 0.977185723607f, 0.003758266219f,  //
                           0.019056010175f, 0.047185723607f, 0.933758266219f};
/// AP1's luminance: Y of its RGB.
constexpr float kAp1Y[3] = {0.27222871678091454f, 0.67408176581114831f, 0.053689517407937051f};
constexpr float kCinemaWhite = 48.0f, kCinemaBlack = 0.02f;
constexpr float kDimGamma = 0.9811f;

/// The curves of the RRT and of the 48-nit output as the CTL has them:
/// quadratic B-splines in log10 over even knots, from minPoint to midPoint
/// and on to maxPoint; straight lines of `slopeLow` and `slopeHigh` beyond.
struct Spline {
    const float* low;
    const float* high;
    int knots;  ///< each half
    float minX, minY, midX, midY, maxX, maxY;  ///< log10 of the points
    float slopeLow, slopeHigh;

    float logy(float logx) const {
        auto piece = [&](const float* c, float x0, float x1) {
            const float coord = static_cast<float>(knots - 1) * (logx - x0) / (x1 - x0);
            const int j = std::clamp(static_cast<int>(coord), 0, knots - 2);
            const float t = coord - static_cast<float>(j);
            const float c0 = c[j], c1 = c[j + 1], c2 = c[j + 2];
            // Monomials (t^2, t, 1) times cf times M = {{0.5, -1, 0.5},
            // {-1, 1, 0.5}, {0.5, 0, 0}}.
            return t * t * (0.5f * c0 - c1 + 0.5f * c2) + t * (c1 - c0) + 0.5f * (c0 + c1);
        };
        if (logx <= minX) return logx * slopeLow + (minY - slopeLow * minX);
        if (logx < midX) return piece(low, minX, midX);
        if (logx < maxX) return piece(high, midX, maxX);
        return logx * slopeHigh + (maxY - slopeHigh * maxX);
    }
};

constexpr float kRrtLow[6] = {-4.0000000000f, -4.0000000000f, -3.1573765773f, -0.4852499958f, 1.8477324706f, 1.8477324706f};
constexpr float kRrtHigh[6] = {-0.7185482425f, 2.0810307172f, 3.6681241237f, 4.0000000000f, 4.0000000000f, 4.0000000000f};
constexpr float kOdtLow[10] = {-1.6989700043f, -1.6989700043f, -1.4779000000f, -1.2291000000f, -0.8648000000f,
                               -0.4480000000f, 0.0051800000f,  0.4511080334f,  0.9113744414f,  0.9113744414f};
constexpr float kOdtHigh[10] = {0.5154386965f, 0.8470437783f, 1.1358000000f, 1.3802000000f, 1.5197000000f,
                                1.5985000000f, 1.6467000000f, 1.6746091357f, 1.6878733390f, 1.6878733390f};

const Spline kRrt = {kRrtLow,
                     kRrtHigh,
                     4,
                     std::log10(0.18f * std::pow(2.0f, -15.0f)),
                     std::log10(0.0001f),
                     std::log10(0.18f),
                     std::log10(4.8f),
                     std::log10(0.18f * std::pow(2.0f, 18.0f)),
                     std::log10(10000.0f),
                     0.0f,
                     0.0f};
const Spline kOdt = {kOdtLow,
                     kOdtHigh,
                     8,
                     kRrt.logy(std::log10(0.18f * std::pow(2.0f, -6.5f))),
                     std::log10(0.02f),
                     kRrt.logy(std::log10(0.18f)),
                     std::log10(4.8f),
                     kRrt.logy(std::log10(0.18f * std::pow(2.0f, 6.5f))),
                     std::log10(48.0f),
                     0.0f,
                     0.04f};

/// Both curves, RRT then output, in log10: the light of AP1 to nits.
float toneLog(float logx) { return kOdt.logy(kRrt.logy(logx)); }

/// ... and back, by halving the way: they rise all the way, flat only below
/// the first knots -- the least that gives `logy`.
float toneLogBack(float logy) {
    float lo = -12.0f, hi = 12.0f;
    if (logy <= toneLog(lo)) return -std::numeric_limits<float>::infinity();
    for (int i = 0; i < 48; ++i) {
        const float mid = 0.5f * (lo + hi);
        (toneLog(mid) < logy ? lo : hi) = mid;
    }
    return hi;
}

float satWeight(float r, float g, float b) {
    const float hi = std::max(r, std::max(g, b)), lo = std::min(r, std::min(g, b));
    return (std::max(1e-10f, hi) - std::max(1e-10f, lo)) / std::max(1e-2f, hi);
}

float rgbToYc(float r, float g, float b) {
    const float chroma = std::sqrt(b * (b - g) + g * (g - r) + r * (r - b));
    return (b + g + r + 1.75f * chroma) / 3.0f;
}

float sigmoidShaper(float sat) {
    const float x = (sat - 0.4f) * 5.0f;
    const float sign = std::copysign(1.0f, x);
    const float t = std::max(0.0f, 1.0f - 0.5f * sign * x);
    return (1.0f + sign * (1.0f - t * t)) * 0.5f;
}

constexpr float kGlowGain = 0.05f, kGlowMid = 0.08f;

/// The glow: shadows of saturated colours a little lighter.
F3 glow(const F3& c, bool back) {
    const float yc = rgbToYc(c[0], c[1], c[2]);
    const float gain = kGlowGain * sigmoidShaper(satWeight(c[0], c[1], c[2]));
    float out;
    if (yc >= kGlowMid * 2.0f) {
        out = 0.0f;
    } else if (!back) {
        out = yc <= kGlowMid * 2.0f / 3.0f ? gain : gain * (kGlowMid / yc - 0.5f);
    } else {
        out = yc <= (1.0f + gain) * kGlowMid * 2.0f / 3.0f ? -gain / (1.0f + gain)
                                                            : gain * (kGlowMid / yc - 0.5f) / (gain * 0.5f - 1.0f);
    }
    return times(1.0f + out, c);
}

/// How much of the red modifier's window a colour's hue is in: a cubic
/// B-spline 135 degrees across round red.
float hueWeight(float r, float g, float b) {
    const float a = 2.0f * r - (g + b);
    const float bb = 1.7320508075688772f * (g - b);
    const float hue = std::atan2(bb, a);
    const float coord = hue * 1.6976527263135504f + 2.0f;
    const int j = static_cast<int>(coord);
    static constexpr float kM[4][4] = {
        {0.25f, 0.00f, 0.00f, 0.00f}, {-0.75f, 0.75f, 0.75f, 0.25f}, {0.75f, -1.50f, 0.00f, 1.00f}, {-0.25f, 0.75f, -0.75f, 0.25f}};
    if (j < 0 || j >= 4) return 0.0f;
    const float t = coord - static_cast<float>(j);
    const float* k = kM[j];
    return k[3] + t * (k[2] + t * (k[1] + t * k[0]));
}

constexpr float kRedOneMinusScale = 1.0f - 0.82f, kRedPivot = 0.03f;

/// The red modifier: bright reds less red, so that they do not go magenta.
F3 redMod(F3 c, bool back) {
    const float fH = hueWeight(c[0], c[1], c[2]);
    if (!(fH > 0.0f)) return c;
    if (!back) {
        c[0] = c[0] + fH * satWeight(c[0], c[1], c[2]) * (kRedPivot - c[0]) * kRedOneMinusScale;
        return c;
    }
    const float least = c[1] < c[2] ? c[1] : c[2];
    const float a = fH * kRedOneMinusScale - 1.0f;
    const float b = c[0] - fH * (kRedPivot + least) * kRedOneMinusScale;
    const float k = fH * kRedPivot * least * kRedOneMinusScale;
    c[0] = (-b - std::sqrt(b * b - 4.0f * a * k)) / (2.0f * a);
    return c;
}

/// A cinema's dark surround to a living room's dim one: Y to the power
/// 0.9811, the colour kept.
F3 darkToDim(const F3& c, bool back) {
    const float y = std::max(1e-10f, kAp1Y[0] * c[0] + kAp1Y[1] * c[1] + kAp1Y[2] * c[2]);
    const float gamma = back ? 1.0f / kDimGamma : kDimGamma;
    return times(std::pow(y, gamma - 1.0f), c);
}

F3 forward(const F3& linear709) {
    F3 c = times(k709ToAp0, linear709);
    c = redMod(glow(c, false), false);
    for (float& v : c) v = std::max(v, 0.0f);
    c = times(kAp0ToAp1, c);
    for (float& v : c) v = std::max(v, 0.0f);
    c = times(kRrtSat, c);
    for (float& v : c) {
        const float nits = std::pow(10.0f, toneLog(std::log10(std::max(v, std::numeric_limits<float>::min()))));
        v = (nits - kCinemaBlack) / (kCinemaWhite - kCinemaBlack);
    }
    c = times(kDesat100, darkToDim(c, false));
    return times(kXyzTo709, times(kAp1ToXyz, c));
}

F3 back(const F3& screen709) {
    static const F33 from709 = inverse(kXyzTo709), fromXyz = inverse(kAp1ToXyz), undesat = inverse(kDesat100),
                     unsat = inverse(kRrtSat), fromAp1 = inverse(kAp0ToAp1), toLinear = inverse(k709ToAp0);
    F3 c = darkToDim(times(undesat, times(fromXyz, times(from709, screen709))), true);
    for (float& v : c) {
        const float nits = v * (kCinemaWhite - kCinemaBlack) + kCinemaBlack;
        v = nits <= kCinemaBlack ? 0.0f : std::pow(10.0f, toneLogBack(std::log10(nits)));
    }
    c = times(fromAp1, times(unsat, c));
    return times(toLinear, glow(redMod(c, true), true));
}

/// back(), then Newton's method on the light against forward(): the red
/// modifier's inverse is only near -- it weighs the hue it gives, not the
/// one it was given -- and a saturated red or magenta would come back up to
/// two steps of 255 off (as from OpenColorIO's). Light the clamps hold --
/// a channel that would need to be negative -- is left as back() has it.
F3 backExactly(const F3& screen709) {
    F3 light = back(screen709);
    auto miss = [&](const F3& l) {
        const F3 s = forward(l);
        return F3{s[0] - screen709[0], s[1] - screen709[1], s[2] - screen709[2]};
    };
    auto size = [](const F3& v) { return std::max(std::abs(v[0]), std::max(std::abs(v[1]), std::abs(v[2]))); };
    F3 off = miss(light);
    for (int step = 0; step < 4 && size(off) > 1e-6f; ++step) {
        if (light[0] <= 0.0f || light[1] <= 0.0f || light[2] <= 0.0f) break;
        D33 jacobian{};
        for (int c = 0; c < 3; ++c) {
            F3 moved = light;
            const float h = 1e-3f * light[static_cast<size_t>(c)];
            moved[static_cast<size_t>(c)] += h;
            const F3 m = miss(moved);
            for (int r = 0; r < 3; ++r) {
                jacobian[static_cast<size_t>(3 * r + c)] = (m[static_cast<size_t>(r)] - off[static_cast<size_t>(r)]) / h;
            }
        }
        const double det = jacobian[0] * (jacobian[4] * jacobian[8] - jacobian[5] * jacobian[7]) -
                           jacobian[1] * (jacobian[3] * jacobian[8] - jacobian[5] * jacobian[6]) +
                           jacobian[2] * (jacobian[3] * jacobian[7] - jacobian[4] * jacobian[6]);
        if (!(std::abs(det) > 1e-12)) break;
        const std::array<double, 3> d = times(inverse(jacobian), std::array<double, 3>{off[0], off[1], off[2]});
        const F3 next = {light[0] - static_cast<float>(d[0]), light[1] - static_cast<float>(d[1]),
                         light[2] - static_cast<float>(d[2])};
        const F3 nextOff = miss(next);
        if (!(size(nextOff) < size(off))) break;
        light = next;
        off = nextOff;
    }
    return light;
}

}  // namespace aces1

// ------------------------------------------------------------------ ACES 2.0

namespace aces2 {

constexpr float kPi = 3.14159265358979f;
constexpr float kHueLimit = 360.0f;

float toRadians(float degrees) { return kPi * degrees / 180.0f; }
/// From radians in (-pi, pi], as atan2 gives them, to degrees in [0, 360).
float fromRadians(float radians) {
    const float y = 180.0f * radians / kPi;
    return y < 0.0f ? y + kHueLimit : y;
}

/// A table by hue: 360 entries, one below and two above to wrap round.
constexpr unsigned kNominal = 360, kTotal = kNominal + 3;
constexpr unsigned kFirst = 1, kUpperWrap = kFirst + kNominal, kLast = kUpperWrap - 1;
using Table1 = std::array<float, kTotal>;
using Table3 = std::array<F3, kTotal>;

// The colour appearance model's viewing conditions.
constexpr float kReference = 100.0f;  ///< nits of the reference white
constexpr float kLA = 100.0f, kYb = 20.0f;
constexpr float kSurround[3] = {0.9f, 0.59f, 0.9f};  ///< dim
constexpr float kJScale = 100.0f;
constexpr float kNlOffset = 0.2713f * 100.0f, kNlScale = 4.0f * 100.0f;

// Chroma compression.
constexpr float kChromaCompress = 2.4f, kChromaCompressFact = 3.3f;
constexpr float kChromaExpand = 1.3f, kChromaExpandFact = 0.69f, kChromaExpandThr = 0.5f;

// Gamut compression.
constexpr float kSmoothCusps = 0.12f, kSmoothM = 0.27f, kCuspMidBlend = 1.3f, kFocusGainBlend = 0.3f;
constexpr float kFocusDistance = 1.35f, kFocusDistanceScaling = 1.75f, kCompressionThreshold = 0.75f;

// Building the tables.
constexpr float kGammaMinimum = 0.0f, kGammaMaximum = 5.0f, kGammaSearchStep = 0.4f, kGammaAccuracy = 1e-5f;
constexpr int kCuspCorners = 6, kTotalCorners = kCuspCorners + 2, kMaxSortedCorners = 2 * kCuspCorners;
constexpr float kReachCuspTolerance = 1e-3f, kDisplayCuspTolerance = 1e-7f;

struct JMhParams {
    F33 rgbToCam16c, cam16cToRgb, coneToAab, aabToCone;
    float flN, cz, invCz, awJ, invAwJ;
};

struct ToneScale {
    float n, nR, g, t1, cT, s2, u2, m2, forwardLimit, inverseLimit, logPeak;
};

struct Shared {
    float limitJMax, modelGammaInv;
    Table1 reachM;
};

struct Resolved {
    float limitJMax, modelGammaInv, reachMaxM;
};

struct Chroma {
    float sat, satThr, compr, scale;
};

struct HueGamut {
    float gammaBottomInv;
    float cuspJ, cuspM;
    float gammaTopInv, focusJ, analyticalThreshold;
};

struct Gamut {
    float midJ, focusDist, lowerHullGammaInv;
    int searchRange[2];
    Table1 hues;
    Table3 cusps;  ///< J, M and the upper hull's inverse gamma of each hue of `hues`
};

F33 rgbToXyzF(const Primaries& p) { return toFloat(rgbToXyz(p)); }
F33 xyzToRgbF(const Primaries& p) { return toFloat(inverse(rgbToXyz(p))); }
F33 diag(float a, float b, float c) { return {a, 0.0f, 0.0f, 0.0f, b, 0.0f, 0.0f, 0.0f, c}; }

float compressFwd(float rc) {
    const float f = std::pow(rc, 0.42f);
    return f / (kNlOffset + f);
}
float compressInv(float ra) {
    const float lim = std::min(ra, 0.99f);
    const float f = (kNlOffset * lim) / (1.0f - lim);
    return std::pow(f, 1.0f / 0.42f);
}
float compressFwdSigned(float v) { return std::copysign(compressFwd(std::abs(v)), v); }
float compressInvSigned(float v) { return std::copysign(compressInv(std::abs(v)), v); }

float aToJ(float a, float cz) { return kJScale * std::pow(a, cz); }
float jToA(float j, float invCz) { return std::pow(j * (1.0f / kJScale), invCz); }

float aToY(float a, const JMhParams& p) { return compressInv(p.awJ * a) / p.flN; }
float jToY(float absJ, const JMhParams& p) { return aToY(jToA(absJ, p.invCz), p); }
float yToJAbs(float absY, const JMhParams& p) { return aToJ(compressFwd(absY * p.flN) * p.invAwJ, p.cz); }
float yToJ(float y, const JMhParams& p) { return std::copysign(yToJAbs(std::abs(y), p), y); }

F3 rgbToAab(const F3& rgb, const JMhParams& p) {
    const F3 m = times(p.rgbToCam16c, rgb);
    const F3 a = {compressFwdSigned(m[0]), compressFwdSigned(m[1]), compressFwdSigned(m[2])};
    return times(p.coneToAab, a);
}

F3 aabToJMh(const F3& aab, const JMhParams& p) {
    if (aab[0] <= 0.0f) return {0.0f, 0.0f, 0.0f};
    const float j = aToJ(aab[0], p.cz);
    const float m = std::sqrt(aab[1] * aab[1] + aab[2] * aab[2]);
    return {j, m, fromRadians(std::atan2(aab[2], aab[1]))};
}

F3 rgbToJMh(const F3& rgb, const JMhParams& p) { return aabToJMh(rgbToAab(rgb, p), p); }

F3 jmhToAab(const F3& jmh, float cosH, float sinH, const JMhParams& p) {
    return {jToA(jmh[0], p.invCz), jmh[1] * cosH, jmh[1] * sinH};
}

F3 jmhToAab(const F3& jmh, const JMhParams& p) {
    const float h = toRadians(jmh[2]);
    return jmhToAab(jmh, std::cos(h), std::sin(h), p);
}

F3 aabToRgb(const F3& aab, const JMhParams& p) {
    const F3 a = times(p.aabToCone, aab);
    const F3 m = {compressInvSigned(a[0]), compressInvSigned(a[1]), compressInvSigned(a[2])};
    return times(p.cam16cToRgb, m);
}

F3 jmhToRgb(const F3& jmh, const JMhParams& p) { return aabToRgb(jmhToAab(jmh, p), p); }

float modelGamma() { return kSurround[1] * (1.48f + std::sqrt(kYb / kReference)); }

JMhParams jmhParams(const Primaries& prims) {
    const F33 base = {2.0f, 1.0f, 1.0f / 20.0f, 1.0f, -12.0f / 11.0f, 1.0f / 11.0f, 1.0f / 9.0f, 1.0f / 9.0f, -2.0f / 9.0f};
    const F33 m16 = xyzToRgbF(kCam16);
    const F33 rgbToXyz = rgbToXyzF(prims);
    const F3 xyzW = times(rgbToXyz, F3{kReference, kReference, kReference});
    const float yW = xyzW[1];
    const F3 rgbW = times(m16, xyzW);

    constexpr float k = 1.0f / (5.0f * kLA + 1.0f);
    constexpr float k4 = k * k * k * k;
    const float fl = 0.2f * k4 * (5.0f * kLA) + 0.1f * std::pow(1.0f - k4, 2.0f) * std::pow(5.0f * kLA, 1.0f / 3.0f);
    const float flN = fl / kReference;
    const float cz = modelGamma();

    const F3 dRgb = {flN * yW / rgbW[0], flN * yW / rgbW[1], flN * yW / rgbW[2]};
    const F3 rgbWc = {dRgb[0] * rgbW[0], dRgb[1] * rgbW[1], dRgb[2] * rgbW[2]};
    const F3 rgbAw = {compressFwdSigned(rgbWc[0]), compressFwdSigned(rgbWc[1]), compressFwdSigned(rgbWc[2])};

    const F33 coneToAab = times(diag(kNlScale, kNlScale, kNlScale), base);
    const float aW = coneToAab[0] * rgbAw[0] + coneToAab[1] * rgbAw[1] + coneToAab[2] * rgbAw[2];
    const float awJ = compressFwd(fl);

    const F33 rgbToCam16 = times(times(xyzToRgbF(kCam16), rgbToXyz), diag(kReference, kReference, kReference));
    const F33 rgbToCam16c = times(diag(dRgb[0], dRgb[1], dRgb[2]), rgbToCam16);
    const float ab = 43.0f * kSurround[2];
    const F33 coneToAabN = {coneToAab[0] / aW, coneToAab[1] / aW, coneToAab[2] / aW,  //
                            coneToAab[3] * ab, coneToAab[4] * ab, coneToAab[5] * ab,  //
                            coneToAab[6] * ab, coneToAab[7] * ab, coneToAab[8] * ab};
    return {rgbToCam16c, inverse(rgbToCam16c), coneToAabN, inverse(coneToAabN), flN, cz, 1.0f / cz, awJ, 1.0f / awJ};
}

/// Michaelis-Menten in Y with a toe: the tonescale (Siragusano, Hellwig).
float tonescaleY(float y, const ToneScale& t, bool back) {
    if (back) {
        const float z = std::max(0.0f, std::min(t.inverseLimit, y / kReference));
        const float f = (z + std::sqrt(z * (4.0f * t.t1 + z))) / 2.0f;
        return t.s2 / (std::pow(t.m2 / f, 1.0f / t.g) - 1.0f);
    }
    const float f = t.m2 * std::pow(y / (y + t.s2), t.g);
    return std::max(0.0f, f * f / (f + t.t1)) * t.nR;
}

float tonescaleJ(float j, const JMhParams& p, const ToneScale& t, bool back) {
    const float y = jToY(std::abs(j), p);
    return std::copysign(yToJAbs(tonescaleY(y, t, back), p), j);
}

float tonescaleAToJ(float a, const JMhParams& p, const ToneScale& t) {
    return std::copysign(yToJAbs(tonescaleY(aToY(a, p), t, false), p), a);
}

float chromaCompressNorm(float cosH, float sinH, float scale) {
    const float cos2 = 2.0f * cosH * cosH - 1.0f;
    const float sin2 = 2.0f * cosH * sinH;
    const float cos3 = 4.0f * cosH * cosH * cosH - 3.0f * cosH;
    const float sin3 = 3.0f * sinH - 4.0f * sinH * sinH * sinH;
    const float m = 11.34072f * cosH + 16.46899f * cos2 + 7.88380f * cos3 + 14.66441f * sinH + -6.37224f * sin2 +
                    9.19364f * sin3 + 77.12896f;
    return m * scale;
}

float toeFwd(float x, float limit, float k1In, float k2In) {
    if (x > limit) return x;
    const float k2 = std::max(k2In, 0.001f);
    const float k1 = std::sqrt(k1In * k1In + k2 * k2);
    const float k3 = (limit + k1) / (limit + k2);
    const float minusB = k3 * x - k1;
    const float minusAc = k2 * k3 * x;
    return 0.5f * (minusB + std::sqrt(minusB * minusB + 4.0f * minusAc));
}

float toeInv(float x, float limit, float k1In, float k2In) {
    if (x > limit) return x;
    const float k2 = std::max(k2In, 0.001f);
    const float k1 = std::sqrt(k1In * k1In + k2 * k2);
    const float k3 = (limit + k1) / (limit + k2);
    return (x * x + k1 * x) / (k3 * (x + k2));
}

F3 chromaCompressFwd(const F3& jmh, float jTs, float mNorm, const Resolved& r, const Chroma& c) {
    const float j = jmh[0], m = jmh[1];
    float mCp = m;
    if (m != 0.0f) {
        const float nJ = jTs / r.limitJMax;
        const float snJ = std::max(0.0f, 1.0f - nJ);
        const float limit = std::pow(nJ, r.modelGammaInv) * r.reachMaxM / mNorm;
        mCp = m * std::pow(jTs / j, r.modelGammaInv);
        mCp = mCp / mNorm;
        mCp = limit - toeFwd(limit - mCp, limit - 0.001f, snJ * c.sat, std::sqrt(nJ * nJ + c.satThr));
        mCp = toeFwd(mCp, limit, nJ * c.compr, snJ);
        mCp = mCp * mNorm;
    }
    return {jTs, mCp, jmh[2]};
}

F3 chromaCompressInv(const F3& jmh, float j, float mNorm, const Resolved& r, const Chroma& c) {
    const float jTs = jmh[0], mCp = jmh[1];
    float m = mCp;
    if (mCp != 0.0f) {
        const float nJ = jTs / r.limitJMax;
        const float snJ = std::max(0.0f, 1.0f - nJ);
        const float limit = std::pow(nJ, r.modelGammaInv) * r.reachMaxM / mNorm;
        m = mCp / mNorm;
        m = toeInv(m, limit, nJ * c.compr, snJ);
        m = limit - toeInv(limit - m, limit - 0.001f, snJ * c.sat, std::sqrt(nJ * nJ + c.satThr));
        m = m * mNorm;
        m = m * std::pow(jTs / j, -r.modelGammaInv);
    }
    return {j, m, jmh[2]};
}

ToneScale toneScale(float peak) {
    const float n = peak, nR = 100.0f, g = 1.15f, c = 0.18f, cD = 10.013f, wG = 0.14f, t1 = 0.04f;
    const float rHitMin = 128.0f, rHitMax = 896.0f;
    const float rHit = rHitMin + (rHitMax - rHitMin) * (std::log(n / nR) / std::log(10000.0f / 100.0f));
    const float m0 = n / nR;
    const float m1 = 0.5f * (m0 + std::sqrt(m0 * (m0 + 4.0f * t1)));
    const float u = std::pow((rHit / m1) / ((rHit / m1) + 1.0f), g);
    const float m = m1 / u;
    const float wI = std::log(n / 100.0f) / std::log(2.0f);
    const float cT = cD / nR * (1.0f + wI * wG);
    const float gIp = 0.5f * (cT + std::sqrt(cT * (cT + 4.0f * t1)));
    const float gIpp2 = -(m1 * std::pow(gIp / m, 1.0f / g)) / (std::pow(gIp / m, 1.0f / g) - 1.0f);
    const float w2 = c / gIpp2;
    const float s2 = w2 * m1 * kReference;
    const float u2 = std::pow((rHit / m1) / ((rHit / m1) + w2), g);
    const float m2 = m1 / u2;
    return {n, nR, g, t1, cT, s2, u2, m2, 8.0f * rHit, n / (u2 * nR), std::log10(n / nR)};
}

// The hue tables.

unsigned uniformPosition(float wrappedHue) { return kFirst + static_cast<unsigned>(wrappedHue); }

unsigned hueInterval(float h, const Table1& hues, const int range[2]) {
    unsigned i = uniformPosition(h);
    unsigned lo = static_cast<unsigned>(std::max(0, static_cast<int>(i) + range[0]));
    unsigned hi = static_cast<unsigned>(std::min(static_cast<int>(kUpperWrap), static_cast<int>(i) + range[1]));
    while (lo + 1 < hi) {
        if (h > hues[i]) {
            lo = i;
        } else {
            hi = i;
        }
        i = (lo + hi) / 2;
    }
    return std::max(1u, hi);
}

float reachMFromTable(float h, const Table1& table) {
    const unsigned base = static_cast<unsigned>(h);
    const float t = h - static_cast<float>(base);
    const unsigned lo = base + kFirst;
    return lerpf(table[lo], table[lo + 1], t);
}

/// R, Y, G, C, B, M: so that the hues go round in order.
F3 cubeCorner(unsigned corner) {
    return {static_cast<float>(((corner + 1) % kCuspCorners) < 3), static_cast<float>(((corner + 5) % kCuspCorners) < 3),
            static_cast<float>(((corner + 3) % kCuspCorners) < 3)};
}

using Corners = std::array<F3, kTotalCorners>;

/// The corners in a cycle, the lowest hue at [1], [0] and [7] wrapped round.
void cycle(const std::array<F3, kCuspCorners>& jmh, const std::array<F3, kCuspCorners>* rgb, Corners& jmhOut,
           Corners* rgbOut) {
    unsigned least = 0;
    for (unsigned i = 0; i < kCuspCorners; ++i) {
        if (jmh[i][2] < jmh[least][2]) least = i;
    }
    for (unsigned i = 0; i < kCuspCorners; ++i) {
        jmhOut[i + 1] = jmh[(i + least) % kCuspCorners];
        if (rgb) (*rgbOut)[i + 1] = (*rgb)[(i + least) % kCuspCorners];
    }
    jmhOut[0] = jmhOut[kCuspCorners];
    jmhOut[kCuspCorners + 1] = jmhOut[1];
    if (rgb) {
        (*rgbOut)[0] = (*rgbOut)[kCuspCorners];
        (*rgbOut)[kCuspCorners + 1] = (*rgbOut)[1];
    }
    jmhOut[0][2] -= kHueLimit;
    jmhOut[kCuspCorners + 1][2] += kHueLimit;
}

void limitingCorners(Corners& rgbCorners, Corners& jmhCorners, const JMhParams& p, float peak) {
    std::array<F3, kCuspCorners> rgb, jmh;
    for (unsigned i = 0; i < kCuspCorners; ++i) {
        rgb[i] = times(peak / kReference, cubeCorner(i));
        jmh[i] = rgbToJMh(rgb[i], p);
    }
    cycle(jmh, &rgb, jmhCorners, &rgbCorners);
}

void reachCorners(Corners& jmhCorners, const JMhParams& p, float limitJ, float maximumSource) {
    std::array<F3, kCuspCorners> jmh;
    const float limitA = jToA(limitJ, p.invCz);
    for (unsigned i = 0; i < kCuspCorners; ++i) {
        const F3 corner = cubeCorner(i);
        float lower = 0.0f, upper = maximumSource;
        while ((upper - lower) > kReachCuspTolerance) {
            const float test = (lower + upper) / 2.0f;
            const float a = rgbToAab(times(test, corner), p)[0];
            if (a < limitA) {
                lower = test;
            } else {
                upper = test;
            }
            if (a == limitA) break;
        }
        jmh[i] = rgbToJMh(times(upper, corner), p);
    }
    cycle(jmh, nullptr, jmhCorners, nullptr);
}

unsigned sortedCubeHues(std::array<float, kMaxSortedCorners>& sorted, const Corners& reach, const Corners& display) {
    unsigned idx = 0, r = 1, d = 1;
    while (r < kCuspCorners + 1 || d < kCuspCorners + 1) {
        const float rh = reach[r][2], dh = display[d][2];
        if (rh == dh) {
            sorted[idx] = rh;
            ++r;
            ++d;
        } else if (rh < dh) {
            sorted[idx] = rh;
            ++r;
        } else {
            sorted[idx] = dh;
            ++d;
        }
        ++idx;
    }
    return idx;
}

void hueSamples(unsigned samples, float lower, float upper, Table1& table, unsigned base) {
    const float delta = (upper - lower) / static_cast<float>(samples);
    for (unsigned i = 0; i < samples; ++i) table[base + i] = lower + static_cast<float>(i) * delta;
}

void hueTable(Table1& table, const std::array<float, kMaxSortedCorners>& sorted, unsigned unique) {
    const float idealSpacing = static_cast<float>(kNominal) / kHueLimit;
    std::array<unsigned, 2 * kCuspCorners + 2> counts{};
    unsigned lastIdx = std::numeric_limits<unsigned>::max();
    unsigned minIndex = sorted[0] == 0.0f ? 0 : 1;
    for (unsigned h = 0; h < unique; ++h) {
        unsigned nominal = std::min(
            std::max(static_cast<unsigned>(std::round(sorted[h] * idealSpacing)), minIndex), kNominal - 1);
        if (lastIdx == nominal) {
            if (h > 1 && counts[h - 2] != (counts[h - 1] - 1)) {
                counts[h - 1] = counts[h - 1] - 1;
            } else {
                nominal = nominal + 1;
            }
        }
        counts[h] = std::min(nominal, kNominal - 1u);
        lastIdx = minIndex = nominal;
    }
    unsigned total = 0, i = 0;
    hueSamples(counts[i], 0.0f, sorted[i], table, total + 1);
    total += counts[i];
    for (++i; i != unique; ++i) {
        const unsigned samples = counts[i] - counts[i - 1];
        hueSamples(samples, sorted[i - 1], sorted[i], table, total + 1);
        total += samples;
    }
    hueSamples(kNominal - total, sorted[i - 1], kHueLimit, table, total + 1);
    table[0] = table[kLast] - kHueLimit;
    table[kUpperWrap] = table[kFirst] + kHueLimit;
    table[kUpperWrap + 1] = table[kFirst + 1] + kHueLimit;
}

/// The cusp of the limiting gamut at `hue`: along the edge of the cube
/// between the two corners round it, halving the way.
std::array<float, 2> displayCusp(float hue, const Corners& rgbCorners, const Corners& jmhCorners, const JMhParams& p,
                                 std::array<float, 2>& previous) {
    unsigned upper = 1;
    for (unsigned i = upper; i != kTotalCorners; ++i) {
        if (jmhCorners[i][2] > hue) {
            upper = i;
            break;
        }
    }
    const unsigned lower = upper - 1;
    if (jmhCorners[lower][2] == hue) return {jmhCorners[lower][0], jmhCorners[lower][1]};
    const F3 lo = rgbCorners[lower], hi = rgbCorners[upper];
    auto at = [&](float t) {
        return F3{lerpf(lo[0], hi[0], t), lerpf(lo[1], hi[1], t), lerpf(lo[2], hi[2], t)};
    };
    float lowerT = (static_cast<float>(upper) == previous[0]) ? previous[1] : 0.0f;
    float upperT = 1.0f;
    while ((upperT - lowerT) > kDisplayCuspTolerance) {
        const float t = (lowerT + upperT) / 2.0f;
        const F3 jmh = rgbToJMh(at(t), p);
        if (jmh[2] < jmhCorners[lower][2]) {
            upperT = t;
        } else if (jmh[2] >= jmhCorners[upper][2]) {
            lowerT = t;
        } else if (jmh[2] > hue) {
            upperT = t;
        } else {
            lowerT = t;
        }
    }
    const float t = (lowerT + upperT) / 2.0f;
    const F3 jmh = rgbToJMh(at(t), p);
    previous[0] = static_cast<float>(upper);
    previous[1] = t;
    return {jmh[0], jmh[1]};
}

Table3 cuspTable(const Table1& hues, const Corners& rgbCorners, const Corners& jmhCorners, const JMhParams& p) {
    std::array<float, 2> previous = {0.0f, 0.0f};
    Table3 out{};
    for (unsigned i = kFirst; i != kUpperWrap; ++i) {
        const float hue = hues[i];
        const std::array<float, 2> jm = displayCusp(hue, rgbCorners, jmhCorners, p, previous);
        out[i] = {jm[0], jm[1] * (1.0f + kSmoothM * kSmoothCusps), hue};
    }
    out[0] = {out[kLast][0], out[kLast][1], hues[0]};
    out[kUpperWrap] = {out[kFirst][0], out[kFirst][1], hues[kUpperWrap]};
    out[kUpperWrap + 1] = {out[kFirst + 1][0], out[kFirst + 1][1], hues[kUpperWrap + 1]};
    return out;
}

bool anyBelowZero(const F3& rgb) { return rgb[0] < 0.0f || rgb[1] < 0.0f || rgb[2] < 0.0f; }

Table1 reachMTable(const JMhParams& p, float limitJMax) {
    Table1 table{};
    for (unsigned i = 0; i < kNominal; ++i) {
        const float hue = static_cast<float>(i);
        constexpr float searchRange = 50.0f, searchMaximum = 1300.0f;
        float low = 0.0f, high = low + searchRange;
        bool outside = false;
        while (!outside && high < searchMaximum) {
            outside = anyBelowZero(jmhToRgb({limitJMax, high, hue}, p));
            if (!outside) {
                low = high;
                high = high + searchRange;
            }
        }
        while (high - low > 1e-2f) {
            const float m = (high + low) / 2.0f;
            if (anyBelowZero(jmhToRgb({limitJMax, m, hue}, p))) {
                high = m;
            } else {
                low = m;
            }
        }
        table[i + kFirst] = high;
    }
    table[0] = table[kLast];
    table[kUpperWrap] = table[kFirst];
    table[kUpperWrap + 1] = table[kFirst + 1];
    return table;
}

float focusGain(float j, float analyticalThreshold, float limitJMax, float focusDist) {
    float gain = limitJMax * focusDist;
    if (j > analyticalThreshold) {
        float adjustment = std::log10((limitJMax - analyticalThreshold) / std::max(0.0001f, limitJMax - j));
        adjustment = adjustment * adjustment + 1.0f;
        gain = gain * adjustment;
    }
    return gain;
}

float solveJIntersect(float j, float m, float focusJ, float maxJ, float slopeGain) {
    const float mScaled = m / slopeGain;
    const float a = mScaled / focusJ;
    if (j < focusJ) {
        const float b = 1.0f - mScaled;
        const float c = -j;
        const float root = std::sqrt(b * b - 4.0f * a * c);
        return -2.0f * c / (b + root);
    }
    const float b = -(1.0f + mScaled + maxJ * a);
    const float c = maxJ * mScaled + j;
    const float root = std::sqrt(b * b - 4.0f * a * c);
    return -2.0f * c / (b - root);
}

float sminScaled(float a, float b, float scaleReference) {
    const float s = kSmoothCusps * scaleReference;
    const float h = std::max(s - std::abs(a - b), 0.0f) / s;
    return std::min(a, b) - h * h * h * s * (1.0f / 6.0f);
}

float compressionSlope(float intersectJ, float focusJ, float limitJMax, float slopeGain) {
    const float scaler = (intersectJ < focusJ) ? intersectJ : (limitJMax - intersectJ);
    return scaler * (intersectJ - focusJ) / (focusJ * slopeGain);
}

float lineBoundaryM(float jAxisIntersect, float slope, float invGamma, float jMax, float mMax, float jReference) {
    const float normalised = jAxisIntersect / jReference;
    const float shifted = jReference * std::pow(normalised, invGamma);
    return shifted * mMax / (jMax - slope * mMax);
}

float gamutBoundaryM(float cuspJ, float cuspM, float jMax, float gammaTopInv, float gammaBottomInv,
                     float jIntersectSource, float slope, float jIntersectCusp) {
    const float lower = lineBoundaryM(jIntersectSource, slope, gammaBottomInv, cuspJ, cuspM, jIntersectCusp);
    const float upper =
        lineBoundaryM(jMax - jIntersectSource, -slope, gammaTopInv, jMax - cuspJ, cuspM, jMax - jIntersectCusp);
    return sminScaled(lower, upper, cuspM);
}

float reinhard(float scale, float nd, bool back) {
    if (back) return nd >= 1.0f ? scale : scale * -(nd / (nd - 1.0f));
    return scale * nd / (1.0f + nd);
}

float remapM(float m, float gamutM, float reachM, bool back) {
    const float ratio = gamutM / reachM;
    const float proportion = std::max(ratio, kCompressionThreshold);
    const float threshold = proportion * gamutM;
    if (m <= threshold || proportion >= 1.0f) return m;
    const float mOffset = m - threshold;
    const float gamutOffset = gamutM - threshold;
    const float reachOffset = reachM - threshold;
    const float scale = reachOffset / ((reachOffset / gamutOffset) - 1.0f);
    return threshold + reinhard(scale, mOffset / scale, back);
}

F3 compressGamut(const F3& jmh, float jx, const Resolved& r, const Gamut& g, const HueGamut& hg, bool back) {
    const float j = jmh[0], m = jmh[1], h = jmh[2];
    const float slopeGain = focusGain(jx, hg.analyticalThreshold, r.limitJMax, g.focusDist);
    const float jIntersectSource = solveJIntersect(j, m, hg.focusJ, r.limitJMax, slopeGain);
    const float slope = compressionSlope(jIntersectSource, hg.focusJ, r.limitJMax, slopeGain);
    const float jIntersectCusp = solveJIntersect(hg.cuspJ, hg.cuspM, hg.focusJ, r.limitJMax, slopeGain);
    const float gamutM = gamutBoundaryM(hg.cuspJ, hg.cuspM, r.limitJMax, hg.gammaTopInv, hg.gammaBottomInv,
                                        jIntersectSource, slope, jIntersectCusp);
    if (gamutM <= 0.0f) return {j, 0.0f, h};
    const float reachM = lineBoundaryM(jIntersectSource, slope, r.modelGammaInv, r.limitJMax, r.reachMaxM, r.limitJMax);
    const float remapped = remapM(m, gamutM, reachM, back);
    return {jIntersectSource + remapped * slope, remapped, h};
}

float focusJOf(float cuspJ, float midJ, float limitJMax) {
    return lerpf(cuspJ, midJ, std::min(1.0f, kCuspMidBlend - (cuspJ / limitJMax)));
}

HueGamut hueGamut(float hue, const Resolved& r, const Gamut& g) {
    HueGamut hg{};
    hg.gammaBottomInv = g.lowerHullGammaInv;
    const unsigned hi = hueInterval(hue, g.hues, g.searchRange);
    const float t = (hue - g.hues[hi - 1]) / (g.hues[hi] - g.hues[hi - 1]);
    const F3& a = g.cusps[hi - 1];
    const F3& b = g.cusps[hi];
    hg.cuspJ = lerpf(a[0], b[0], t);
    hg.cuspM = lerpf(a[1], b[1], t);
    hg.gammaTopInv = lerpf(a[2], b[2], t);
    hg.focusJ = focusJOf(hg.cuspJ, g.midJ, r.limitJMax);
    hg.analyticalThreshold = lerpf(hg.cuspJ, r.limitJMax, kFocusGainBlend);
    return hg;
}

F3 gamutCompress(const F3& jmh, const Resolved& r, const Gamut& g, bool back) {
    const float j = jmh[0], m = jmh[1], h = jmh[2];
    if (j <= 0.0f) return {0.0f, 0.0f, h};
    if (m <= 0.0f || j > r.limitJMax) return {j, 0.0f, h};
    const HueGamut hg = hueGamut(h, r, g);
    if (!back) return compressGamut(jmh, j, r, g, hg, false);
    float jx = j;
    if (jx > hg.analyticalThreshold) jx = compressGamut(jmh, jx, r, g, hg, true)[0];
    return compressGamut(jmh, jx, r, g, hg, true);
}

// The upper hull's gamma, per hue: the least for which lines from five
// points between the cusp and the top meet the hull outside the gamut.

struct GammaTest {
    F3 jmh;
    float jIntersectSource, slope, jIntersectCusp;
};

bool gammaFits(float cuspJ, float cuspM, const std::array<GammaTest, 5>& data, float topGammaInv, float peak,
               float limitJMax, float lowerHullGammaInv, const JMhParams& limit) {
    const float luminanceLimit = peak / kReference;
    for (const GammaTest& d : data) {
        const float m = gamutBoundaryM(cuspJ, cuspM, limitJMax, topGammaInv, lowerHullGammaInv, d.jIntersectSource,
                                       d.slope, d.jIntersectCusp);
        const float j = d.jIntersectSource + d.slope * m;
        const F3 rgb = jmhToRgb({j, m, d.jmh[2]}, limit);
        const bool outside = rgb[0] > luminanceLimit || rgb[1] > luminanceLimit || rgb[2] > luminanceLimit;
        if (!outside) return false;
    }
    return true;
}

void upperHullGamma(const Table1& hues, Table3& cusps, float peak, float limitJMax, float midJ, float focusDist,
                    float lowerHullGammaInv, const JMhParams& limit) {
    constexpr float positions[5] = {0.01f, 0.1f, 0.5f, 0.8f, 0.99f};
    for (unsigned i = kFirst; i != kUpperWrap; ++i) {
        const float hue = hues[i];
        const float cuspJ = cusps[i][0], cuspM = cusps[i][1];
        const float threshold = lerpf(cuspJ, limitJMax, kFocusGainBlend);
        const float focusJ = focusJOf(cuspJ, midJ, limitJMax);
        std::array<GammaTest, 5> data;
        for (int k = 0; k < 5; ++k) {
            const float testJ = lerpf(cuspJ, limitJMax, positions[k]);
            const float gain = focusGain(testJ, threshold, limitJMax, focusDist);
            const float source = solveJIntersect(testJ, cuspM, focusJ, limitJMax, gain);
            data[static_cast<size_t>(k)] = {{testJ, cuspM, hue},
                                            source,
                                            compressionSlope(source, focusJ, limitJMax, gain),
                                            solveJIntersect(cuspJ, cuspM, focusJ, limitJMax, gain)};
        }
        auto fits = [&](float gamma) {
            return gammaFits(cuspJ, cuspM, data, 1.0f / gamma, peak, limitJMax, lowerHullGammaInv, limit);
        };
        float low = kGammaMinimum, high = low + kGammaSearchStep;
        bool outside = false;
        while (!outside && high < kGammaMaximum) {
            if (!fits(high)) {
                low = high;
                high = high + kGammaSearchStep;
            } else {
                outside = true;
            }
        }
        while ((high - low) > kGammaAccuracy) {
            const float test = (high + low) / 2.0f;
            if (fits(test)) {
                high = test;
            } else {
                low = test;
            }
        }
        cusps[i][2] = 1.0f / high;
    }
    cusps[0][2] = cusps[kLast][2];
    cusps[kUpperWrap][2] = cusps[kFirst][2];
    cusps[kUpperWrap + 1][2] = cusps[kFirst + 1][2];
}

/// SDR 100 nits, Rec. 709 the limiting gamut -- what the view shows on an
/// sRGB screen.
struct Transform {
    JMhParams in, out;
    ToneScale tone;
    Shared shared;
    Chroma chroma;
    Gamut gamut;

    Transform() {
        constexpr float peak = 100.0f;
        in = jmhParams(kAP0);
        out = jmhParams(kRec709);
        tone = toneScale(peak);
        const JMhParams reach = jmhParams(kAP1);
        shared.limitJMax = yToJ(peak, in);
        shared.modelGammaInv = 1.0f / modelGamma();
        shared.reachM = reachMTable(reach, shared.limitJMax);
        chroma.compr = kChromaCompress + (kChromaCompress * kChromaCompressFact) * tone.logPeak;
        chroma.sat = std::max(0.2f, kChromaExpand - (kChromaExpand * kChromaExpandFact) * tone.logPeak);
        chroma.satThr = kChromaExpandThr / tone.n;
        chroma.scale = std::pow(0.03379f * peak, 0.30596f) - 0.45135f;

        gamut.midJ = yToJ(tone.cT * kReference, in);
        gamut.focusDist = kFocusDistance + kFocusDistance * kFocusDistanceScaling * tone.logPeak;
        gamut.lowerHullGammaInv = 1.0f / (1.14f + 0.07f * tone.logPeak);
        Corners reachJmh, limitRgb, limitJmh;
        reachCorners(reachJmh, reach, shared.limitJMax, tone.forwardLimit);
        limitingCorners(limitRgb, limitJmh, out, peak);
        std::array<float, kMaxSortedCorners> sorted{};
        const unsigned unique = sortedCubeHues(sorted, reachJmh, limitJmh);
        hueTable(gamut.hues, sorted, unique);
        gamut.cusps = cuspTable(gamut.hues, limitRgb, limitJmh, out);
        // How far from even the hues are: where the search for one starts.
        gamut.searchRange[0] = 0;
        gamut.searchRange[1] = 1;
        for (unsigned i = kFirst; i != kUpperWrap; ++i) {
            const int delta = static_cast<int>(i) - static_cast<int>(uniformPosition(gamut.cusps[i][2]));
            gamut.searchRange[0] = std::min(gamut.searchRange[0], delta);
            gamut.searchRange[1] = std::max(gamut.searchRange[1], delta + 1);
        }
        upperHullGamma(gamut.hues, gamut.cusps, peak, shared.limitJMax, gamut.midJ, gamut.focusDist,
                       gamut.lowerHullGammaInv, out);
    }

    Resolved resolved(float hue) const {
        return {shared.limitJMax, shared.modelGammaInv, reachMFromTable(hue, shared.reachM)};
    }

    /// AP0 light to Rec. 709 display light, 0 to 1 at 100 nits.
    F3 forward(const F3& ap0) const {
        const F3 aab = rgbToAab(ap0, in);
        const F3 jmh = aabToJMh(aab, in);
        const Resolved r = resolved(jmh[2]);
        const float h = toRadians(jmh[2]);
        const float cosH = std::cos(h), sinH = std::sin(h);
        const float mNorm = chromaCompressNorm(cosH, sinH, chroma.scale);
        const float jTs = tonescaleAToJ(aab[0], in, tone);
        const F3 toned = chromaCompressFwd(jmh, jTs, mNorm, r, chroma);
        const F3 compressed = gamutCompress(toned, r, gamut, false);
        return aabToRgb(jmhToAab(compressed, cosH, sinH, out), out);
    }

    F3 back(const F3& display) const {
        const F3 compressed = rgbToJMh(display, out);
        const Resolved r = resolved(compressed[2]);
        const float h = toRadians(compressed[2]);
        const float cosH = std::cos(h), sinH = std::sin(h);
        const float mNorm = chromaCompressNorm(cosH, sinH, chroma.scale);
        const F3 toned = gamutCompress(compressed, r, gamut, true);
        const float j = tonescaleJ(toned[0], in, tone, true);
        const F3 jmh = chromaCompressInv(toned, j, mNorm, r, chroma);
        return aabToRgb(jmhToAab(jmh, cosH, sinH, in), in);
    }
};

const Transform& transform() {
    static const Transform t;
    return t;
}

const F33 k709ToAp0 = toFloat(conversion(kRec709, kAP0, true));
const F33 kAp0ToAp1 = toFloat(conversion(kAP0, kAP1, false));
const F33 kAp1ToAp0 = toFloat(conversion(kAP1, kAP0, false));
/// The upper bound of AP1 light the transform takes in at 100 nits.
constexpr float kUpperBound = 8.0f * 128.0f;

F3 forward(const F3& linear709) {
    F3 c = times(kAp0ToAp1, times(k709ToAp0, linear709));
    for (float& v : c) v = std::clamp(v, 0.0f, kUpperBound);
    return transform().forward(times(kAp1ToAp0, c));
}

F3 back(const F3& display709) {
    static const F33 toLinear = inverse(k709ToAp0);
    return times(toLinear, transform().back(display709));
}

}  // namespace aces2

}  // namespace

float srgbEncoded(float linear) {
    const double l = linear;
    if (l <= kSrgbBreak) return static_cast<float>(l * kSrgbSlope);
    return static_cast<float>((1.0 + kSrgbOffset) * std::pow(l, 1.0 / kSrgbGamma) - kSrgbOffset);
}

float srgbDecoded(float encoded) {
    const double e = encoded;
    if (e <= kSrgbBreak * kSrgbSlope) return static_cast<float>(e / kSrgbSlope);
    return static_cast<float>(std::pow((e + kSrgbOffset) / (1.0 + kSrgbOffset), kSrgbGamma));
}

Vec3 acesShown(const Vec3& linear, AcesOutput output) {
    const F3 c = output == AcesOutput::V1 ? aces1::forward(f3(linear)) : aces2::forward(f3(linear));
    // What has no colour the model can take -- light of no lightness at all
    // round a hue -- is black.
    auto shown = [](float v) { return v > 0.0f ? std::min(srgbEncoded(std::min(v, 1.0f)), 1.0f) : 0.0f; };
    return {shown(c[0]), shown(c[1]), shown(c[2])};
}

Vec3 acesUnshown(const Vec3& display, AcesOutput output) {
    const F3 screen = {srgbDecoded(std::clamp(display.x, 0.0f, 1.0f)), srgbDecoded(std::clamp(display.y, 0.0f, 1.0f)),
                       srgbDecoded(std::clamp(display.z, 0.0f, 1.0f))};
    const F3 c = output == AcesOutput::V1 ? aces1::backExactly(screen) : aces2::back(screen);
    return glm::max(vec(c), Vec3(0.0f));
}

}  // namespace pg::render
