#include "pg/render/Ocio.h"

#include "pg/core/ColorSpace.h"
#include "pg/core/Parallel.h"
#include "pg/io/Yaml.h"
#include "pg/render/Aces.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <sstream>

namespace pg::render {

using io::YamlNode;

/// One step of a processor, as OpenColorIO's CPU renderers do it: in
/// float, on red, green and blue (alpha taken as 1).
struct OcioProcessor::Op {
    virtual ~Op() = default;
    virtual void apply(float* c) const = 0;
    /// The step back; null where there is none.
    virtual std::shared_ptr<const Op> inverse() const = 0;
    /// What it is: said where it does not go back.
    virtual std::string name() const = 0;
    /// A matrix: its 3 x 3 and offset, in double. False for other steps.
    virtual bool matrix(double* /*m*/, double* /*o*/) const { return false; }
};

namespace {

using Op = OcioProcessor::Op;
using OpPtr = std::shared_ptr<const Op>;
using color::D33;
using color::Primaries;

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool same(const std::string& a, const std::string& b) { return a.size() == b.size() && lower(a) == lower(b); }

// --- the steps ----------------------------------------------------------------------------

/// out = M in + offset.
struct MatrixOp final : Op {
    double m[9], o[3];
    float fm[9], fo[3];

    MatrixOp(const double* m9, const double* o3) {
        for (int k = 0; k < 9; ++k) fm[k] = static_cast<float>(m[k] = m9[k]);
        for (int k = 0; k < 3; ++k) fo[k] = static_cast<float>(o[k] = o3 ? o3[k] : 0.0);
    }
    void apply(float* c) const override {
        const float r = c[0], g = c[1], b = c[2];
        c[0] = fm[0] * r + fm[1] * g + fm[2] * b + fo[0];
        c[1] = fm[3] * r + fm[4] * g + fm[5] * b + fo[1];
        c[2] = fm[6] * r + fm[7] * g + fm[8] * b + fo[2];
    }
    OpPtr inverse() const override {
        const D33 a{m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8]};
        const double det = a[0] * (a[4] * a[8] - a[5] * a[7]) - a[1] * (a[3] * a[8] - a[5] * a[6]) +
                           a[2] * (a[3] * a[7] - a[4] * a[6]);
        if (!(std::abs(det) > 1e-30)) return nullptr;
        const D33 inv = color::inverse(a);
        const std::array<double, 3> back = color::times(inv, std::array<double, 3>{o[0], o[1], o[2]});
        const double off[3] = {-back[0], -back[1], -back[2]};
        return std::make_shared<MatrixOp>(inv.data(), off);
    }
    std::string name() const override { return "a matrix"; }
    bool matrix(double* m9, double* o3) const override {
        std::copy(m, m + 9, m9);
        std::copy(o, o + 3, o3);
        return true;
    }
};

OpPtr matrixOp(const D33& m) { return std::make_shared<MatrixOp>(m.data(), nullptr); }

/// What a power does below 0: holds at 0; mirrors; lets it pass.
enum class Negative { Clamp, Mirror, Pass };

/// out = in ^ g, each channel (OpenColorIO's basic gamma).
struct PowerOp final : Op {
    float g[3];
    Negative style;
    PowerOp(const double* gamma, Negative s) : style(s) {
        for (int k = 0; k < 3; ++k) g[k] = static_cast<float>(gamma[k]);
    }
    void apply(float* c) const override {
        for (int k = 0; k < 3; ++k) {
            const float v = c[k];
            if (v >= 0.0f) {
                c[k] = std::pow(v, g[k]);
            } else if (style == Negative::Mirror) {
                c[k] = -std::pow(-v, g[k]);
            } else if (style == Negative::Clamp) {
                c[k] = std::pow(0.0f, g[k]);
            }
        }
    }
    OpPtr inverse() const override {
        const double back[3] = {1.0 / g[0], 1.0 / g[1], 1.0 / g[2]};
        return std::make_shared<PowerOp>(back, style);
    }
    std::string name() const override { return "an exponent"; }
};

/// A power with a straight line near black: OpenColorIO's moncurve. Forward
/// takes an encoding to light (as sRGB's EOTF); back, light to it.
struct MonCurveOp final : Op {
    double gamma[3], offset[3];
    bool forward, mirror;
    float scale_[3], off_[3], g_[3], brk_[3], slope_[3];

    MonCurveOp(const double* gm, const double* of, bool fwd, bool mir) : forward(fwd), mirror(mir) {
        constexpr double kEps = 1e-6;
        for (int k = 0; k < 3; ++k) {
            gamma[k] = gm[k];
            offset[k] = of[k];
            const double g = std::max(gm[k], 1.0 + kEps), o = std::max(of[k], kEps);
            if (forward) {
                g_[k] = static_cast<float>(g);
                off_[k] = static_cast<float>(o / (1.0 + o));
                brk_[k] = static_cast<float>(o / (g - 1.0));
                slope_[k] = static_cast<float>((g - 1.0) / o * std::pow(o * g / ((g - 1.0) * (1.0 + o)), g));
                scale_[k] = static_cast<float>(1.0 / (1.0 + o));
            } else {
                g_[k] = static_cast<float>(1.0 / g);
                off_[k] = static_cast<float>(o);
                brk_[k] = static_cast<float>(std::pow(o * g / ((g - 1.0) * (1.0 + o)), g));
                slope_[k] = static_cast<float>(std::pow((g - 1.0) / o, g - 1.0) * std::pow((1.0 + o) / g, g));
                scale_[k] = static_cast<float>(1.0 + o);
            }
        }
    }
    float curve(float x, int k) const {
        if (forward) return x <= brk_[k] ? x * slope_[k] : std::pow(x * scale_[k] + off_[k], g_[k]);
        return x <= brk_[k] ? x * slope_[k] : std::pow(x, g_[k]) * scale_[k] - off_[k];
    }
    void apply(float* c) const override {
        for (int k = 0; k < 3; ++k) {
            if (mirror) c[k] = std::copysign(curve(std::fabs(c[k]), k), c[k]);
            else c[k] = curve(c[k], k);
        }
    }
    OpPtr inverse() const override { return std::make_shared<MonCurveOp>(gamma, offset, !forward, mirror); }
    std::string name() const override { return "an exponent with a linear part"; }
};

/// OpenColorIO's log: out = logSlope log_base(linSlope in + linOffset) +
/// logOffset (lin to log); with a camera's break, a straight line below it.
struct LogOp final : Op {
    double base;
    double logSlope[3], logOffset[3], linSlope[3], linOffset[3];
    bool camera = false;
    double linBreak[3] = {0, 0, 0};
    bool hasLinearSlope = false;
    double linearSlopeGiven[3] = {0, 0, 0};
    bool toLog;  ///< forward: lin to log

    float kLog_[3], kInv_[3], logBreak_[3], linSlope_[3], linOffsetF_[3];

    void prepare() {
        const float log2Base = std::log2(static_cast<float>(base));
        for (int k = 0; k < 3; ++k) {
            kLog_[k] = static_cast<float>(logSlope[k] / std::log2(base));
            kInv_[k] = log2Base / static_cast<float>(logSlope[k]);
            if (camera) {
                linSlope_[k] = hasLinearSlope ? static_cast<float>(linearSlopeGiven[k])
                                              : static_cast<float>(logSlope[k] * linSlope[k] /
                                                                   ((linSlope[k] * linBreak[k] + linOffset[k]) * std::log(base)));
                float lb = std::log2(static_cast<float>(linSlope[k] * linBreak[k] + linOffset[k]));
                lb *= static_cast<float>(logSlope[k]) / std::log2(static_cast<float>(base));
                lb += static_cast<float>(logOffset[k]);
                logBreak_[k] = lb;
                linOffsetF_[k] = lb - linSlope_[k] * static_cast<float>(linBreak[k]);
            }
        }
    }
    void apply(float* c) const override {
        constexpr float kMin = std::numeric_limits<float>::min();
        for (int k = 0; k < 3; ++k) {
            const float v = c[k];
            if (toLog) {
                if (camera && v < static_cast<float>(linBreak[k])) {
                    c[k] = linSlope_[k] * v + linOffsetF_[k];
                    continue;
                }
                const float x = std::max(kMin, v * static_cast<float>(linSlope[k]) + static_cast<float>(linOffset[k]));
                c[k] = std::log2(x) * kLog_[k] + static_cast<float>(logOffset[k]);
            } else {
                if (camera && v < logBreak_[k]) {
                    c[k] = (v - linOffsetF_[k]) / linSlope_[k];
                    continue;
                }
                const float e = std::exp2((v - static_cast<float>(logOffset[k])) * kInv_[k]);
                c[k] = (e - static_cast<float>(linOffset[k])) / static_cast<float>(linSlope[k]);
            }
        }
    }
    OpPtr inverse() const override {
        auto back = std::make_shared<LogOp>(*this);
        back->toLog = !toLog;
        return back;
    }
    std::string name() const override { return "a log"; }
};

std::shared_ptr<LogOp> logOp(double base, const double* logSlope, const double* logOffset, const double* linSlope,
                             const double* linOffset, bool toLog) {
    auto op = std::make_shared<LogOp>();
    op->base = base;
    for (int k = 0; k < 3; ++k) {
        op->logSlope[k] = logSlope[k];
        op->logOffset[k] = logOffset[k];
        op->linSlope[k] = linSlope[k];
        op->linOffset[k] = linOffset[k];
    }
    op->toLog = toLog;
    op->prepare();
    return op;
}

/// The ASC CDL, as OpenColorIO's renderers: slope, offset, power, saturation
/// -- clamped to 0..1 in the ASC style, not in noClamp.
struct CdlOp final : Op {
    double slope[3], offset[3], power[3], sat;
    bool clamp, forward;
    CdlOp(const double* s, const double* o, const double* p, double sa, bool c, bool f) : sat(sa), clamp(c), forward(f) {
        for (int k = 0; k < 3; ++k) {
            slope[k] = s[k];
            offset[k] = o[k];
            power[k] = p[k];
        }
    }
    static float clamp01(float v) { return std::isnan(v) ? 0.0f : std::clamp(v, 0.0f, 1.0f); }
    void powerOf(float* c, const float* p) const {
        for (int k = 0; k < 3; ++k) {
            if (clamp) c[k] = std::pow(clamp01(c[k]), p[k]);
            else c[k] = std::isnan(c[k]) ? 0.0f : c[k] < 0.0f ? c[k] : std::pow(c[k], p[k]);
        }
    }
    static void saturate(float* c, float s) {
        const float luma = 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
        for (int k = 0; k < 3; ++k) c[k] = luma + s * (c[k] - luma);
    }
    void apply(float* c) const override {
        if (forward) {
            for (int k = 0; k < 3; ++k) c[k] = c[k] * static_cast<float>(slope[k]) + static_cast<float>(offset[k]);
            const float p[3] = {static_cast<float>(power[0]), static_cast<float>(power[1]), static_cast<float>(power[2])};
            powerOf(c, p);
            saturate(c, static_cast<float>(sat));
            if (clamp) {
                for (int k = 0; k < 3; ++k) c[k] = clamp01(c[k]);
            }
            return;
        }
        if (clamp) {
            for (int k = 0; k < 3; ++k) c[k] = clamp01(c[k]);
        }
        saturate(c, static_cast<float>(1.0 / sat));
        const float p[3] = {static_cast<float>(1.0 / power[0]), static_cast<float>(1.0 / power[1]),
                            static_cast<float>(1.0 / power[2])};
        powerOf(c, p);
        for (int k = 0; k < 3; ++k) c[k] = (c[k] - static_cast<float>(offset[k])) * static_cast<float>(1.0 / slope[k]);
        if (clamp) {
            for (int k = 0; k < 3; ++k) c[k] = clamp01(c[k]);
        }
    }
    OpPtr inverse() const override { return std::make_shared<CdlOp>(slope, offset, power, sat, clamp, !forward); }
    std::string name() const override { return "a CDL"; }
};

/// From [minIn, maxIn] to [minOut, maxOut]; clamped, or not. Either end may
/// be missing: then only an offset, and a clamp at the other.
struct RangeOp final : Op {
    double minIn, maxIn, minOut, maxOut;
    bool hasMin, hasMax, clamp;
    float scale_ = 1.0f, offset_ = 0.0f;
    RangeOp(double a, double b, double c, double d, bool hMin, bool hMax, bool cl)
        : minIn(a), maxIn(b), minOut(c), maxOut(d), hasMin(hMin), hasMax(hMax), clamp(cl) {
        if (hasMin && hasMax) {
            const double s = (maxOut - minOut) / (maxIn - minIn);
            scale_ = static_cast<float>(s);
            offset_ = static_cast<float>(minOut - s * minIn);
        } else if (hasMin) {
            offset_ = static_cast<float>(minOut - minIn);
        } else if (hasMax) {
            offset_ = static_cast<float>(maxOut - maxIn);
        }
    }
    void apply(float* c) const override {
        for (int k = 0; k < 3; ++k) {
            float v = c[k] * scale_ + offset_;
            if (clamp || !(hasMin && hasMax)) {
                if (hasMin) v = std::max(v, static_cast<float>(minOut));
                if (hasMax) v = std::min(v, static_cast<float>(maxOut));
            }
            c[k] = v;
        }
    }
    OpPtr inverse() const override { return std::make_shared<RangeOp>(minOut, maxOut, minIn, maxIn, hasMin, hasMax, clamp); }
    std::string name() const override { return "a range"; }
};

/// A 1D table: each channel through its column, linearly between the
/// entries, held at its ends; `lo` to `hi` the input it spans.
struct Lut1D {
    int size = 0, components = 1;
    float lo[3] = {0, 0, 0}, hi[3] = {1, 1, 1};
    std::vector<float> values;  ///< `components` an entry
    float at(int k, size_t i) const { return values[i * static_cast<size_t>(components) + static_cast<size_t>(components == 1 ? 0 : k)]; }
};

/// A 1D table taken back as OpenColorIO takes it: each column made to rise
/// all the way -- negated where it falls; where it turns back, held flat --
/// and its flat ends left out, so that a value at one comes back where the
/// table leaves it.
struct Lut1DBack {
    struct Column {
        std::vector<float> v;
        size_t start = 0, end = 0;
        float sign = 1.0f;
    };
    Column columns[3];

    explicit Lut1DBack(const Lut1D& t) {
        const size_t n = static_cast<size_t>(t.size);
        for (int k = 0; k < (t.components == 1 ? 1 : 3); ++k) {
            Column& c = columns[k];
            c.v.resize(n);
            for (size_t i = 0; i < n; ++i) c.v[i] = t.at(k, i);
            const bool rising = c.v[0] < c.v[n - 1];
            float previous = c.v[0];
            for (size_t i = 1; i < n; ++i) {
                if (rising != (c.v[i] > previous)) c.v[i] = previous;
                else previous = c.v[i];
            }
            c.end = n - 1;
            while (c.end > 0 && c.v[c.end - 1] == c.v[n - 1]) --c.end;
            while (c.start < c.end && c.v[c.start + 1] == c.v[0]) ++c.start;
            c.sign = rising ? 1.0f : -1.0f;
            if (!rising) {
                for (float& x : c.v) x = -x;
            }
        }
        if (t.components == 1) columns[1] = columns[2] = columns[0];
    }

    /// Where on the table, 0 to 1, `y` lies (OpenColorIO's FindLutInv).
    float find(int k, float y) const {
        const Column& c = columns[k];
        const float* start = c.v.data() + c.start;
        const float* end = c.v.data() + c.end;
        const float cv = std::min(std::max(y * c.sign, *start), *end);
        const float* low = std::lower_bound(start, end, cv);
        if (low > start) --low;
        const float* high = low < end ? low + 1 : low;
        const float delta = *high > *low ? (cv - *low) / (*high - *low) : 0.0f;
        const float index = static_cast<float>(low - start) + static_cast<float>(c.start) + delta;
        return index / static_cast<float>(c.v.size() - 1);
    }
};

struct Lut1DOp final : Op {
    std::shared_ptr<const Lut1D> lut;
    std::shared_ptr<const Lut1DBack> back;  ///< going back: what it finds the way with
    explicit Lut1DOp(std::shared_ptr<const Lut1D> l, std::shared_ptr<const Lut1DBack> b = nullptr)
        : lut(std::move(l)), back(std::move(b)) {}
    void apply(float* c) const override {
        const Lut1D& t = *lut;
        const int n = t.size;
        for (int k = 0; k < 3; ++k) {
            if (back) {
                c[k] = t.lo[k] + back->find(k, c[k]) * (t.hi[k] - t.lo[k]);
                continue;
            }
            const float x = (c[k] - t.lo[k]) / (t.hi[k] - t.lo[k]) * static_cast<float>(n - 1);
            const float f = std::isnan(x) ? 0.0f : std::clamp(x, 0.0f, static_cast<float>(n - 1));
            const int i = std::min(static_cast<int>(f), n - 2);
            const float w = f - static_cast<float>(i);
            const float a = t.at(k, static_cast<size_t>(i)), b = t.at(k, static_cast<size_t>(i + 1));
            c[k] = a + (b - a) * w;
        }
    }
    OpPtr inverse() const override {
        if (back) return std::make_shared<Lut1DOp>(lut);
        return std::make_shared<Lut1DOp>(lut, std::make_shared<Lut1DBack>(*lut));
    }
    std::string name() const override { return "a 1D table"; }
};

/// A 3D table over its domain, red fastest in `values`.
struct Lut3D {
    int size = 0;
    float lo[3] = {0, 0, 0}, hi[3] = {1, 1, 1};
    std::vector<float> values;  ///< rgb at (r, g, b): index (b * n + g) * n + r
    const float* at(int r, int g, int b) const {
        return &values[3 * ((static_cast<size_t>(b) * static_cast<size_t>(size) + static_cast<size_t>(g)) * static_cast<size_t>(size) +
                            static_cast<size_t>(r))];
    }
};

/// What a 3D table gives at `c`: linearly in each axis, or in the
/// tetrahedron of the cell the point is in (by the order of its fractions).
void lookUp(const Lut3D& t, bool tetrahedral, float* c) {
    const int n = t.size;
    float f[3];
    int i0[3], i1[3];
    for (int k = 0; k < 3; ++k) {
        const float x = (c[k] - t.lo[k]) / (t.hi[k] - t.lo[k]) * static_cast<float>(n - 1);
        const float v = std::isnan(x) ? 0.0f : std::clamp(x, 0.0f, static_cast<float>(n - 1));
        i0[k] = std::min(static_cast<int>(v), n - 1);
        i1[k] = std::min(i0[k] + 1, n - 1);
        f[k] = v - static_cast<float>(i0[k]);
    }
    float out[3];
    if (!tetrahedral) {
        for (int k = 0; k < 3; ++k) {
            const float c000 = t.at(i0[0], i0[1], i0[2])[k], c100 = t.at(i1[0], i0[1], i0[2])[k];
            const float c010 = t.at(i0[0], i1[1], i0[2])[k], c110 = t.at(i1[0], i1[1], i0[2])[k];
            const float c001 = t.at(i0[0], i0[1], i1[2])[k], c101 = t.at(i1[0], i0[1], i1[2])[k];
            const float c011 = t.at(i0[0], i1[1], i1[2])[k], c111 = t.at(i1[0], i1[1], i1[2])[k];
            const float a = c000 + (c100 - c000) * f[0], b = c010 + (c110 - c010) * f[0];
            const float d = c001 + (c101 - c001) * f[0], e = c011 + (c111 - c011) * f[0];
            const float g0 = a + (b - a) * f[1], g1 = d + (e - d) * f[1];
            out[k] = g0 + (g1 - g0) * f[2];
        }
    } else {
        const float fr = f[0], fg = f[1], fb = f[2];
        const float* n000 = t.at(i0[0], i0[1], i0[2]);
        const float* n111 = t.at(i1[0], i1[1], i1[2]);
        for (int k = 0; k < 3; ++k) {
            float v;
            if (fr > fg) {
                if (fg > fb) {
                    v = (1 - fr) * n000[k] + (fr - fg) * t.at(i1[0], i0[1], i0[2])[k] + (fg - fb) * t.at(i1[0], i1[1], i0[2])[k] +
                        fb * n111[k];
                } else if (fr > fb) {
                    v = (1 - fr) * n000[k] + (fr - fb) * t.at(i1[0], i0[1], i0[2])[k] + (fb - fg) * t.at(i1[0], i0[1], i1[2])[k] +
                        fg * n111[k];
                } else {
                    v = (1 - fb) * n000[k] + (fb - fr) * t.at(i0[0], i0[1], i1[2])[k] + (fr - fg) * t.at(i1[0], i0[1], i1[2])[k] +
                        fg * n111[k];
                }
            } else {
                if (fb > fg) {
                    v = (1 - fb) * n000[k] + (fb - fg) * t.at(i0[0], i0[1], i1[2])[k] + (fg - fr) * t.at(i0[0], i1[1], i1[2])[k] +
                        fr * n111[k];
                } else if (fb > fr) {
                    v = (1 - fg) * n000[k] + (fg - fb) * t.at(i0[0], i1[1], i0[2])[k] + (fb - fr) * t.at(i0[0], i1[1], i1[2])[k] +
                        fr * n111[k];
                } else {
                    v = (1 - fg) * n000[k] + (fg - fr) * t.at(i0[0], i1[1], i0[2])[k] + (fr - fb) * t.at(i1[0], i1[1], i0[2])[k] +
                        fb * n111[k];
                }
            }
            out[k] = v;
        }
    }
    c[0] = out[0];
    c[1] = out[1];
    c[2] = out[2];
}

/// Where in its input a 3D table gives a value, as OpenColorIO's exact
/// inverse finds it: the table grown by a cell each way -- the values on
/// its outside four times as far from 1/2 as those they are grown from --,
/// then the tetrahedron of a cell, as tetrahedral interpolation cuts it,
/// that holds the value (0 to 1 each); found by the cells whose values'
/// bounds hold it, binned over the unit cube.
class Lut3DSearch {
public:
    explicit Lut3DSearch(const Lut3D& t) : n_(t.size), m_(t.size + 2) {
        const size_t m = static_cast<size_t>(m_);
        grid_.resize(3 * m * m * m);
        for (int b = 0; b < m_; ++b) {
            for (int g = 0; g < m_; ++g) {
                for (int r = 0; r < m_; ++r) {
                    const int sr = std::clamp(r - 1, 0, n_ - 1), sg = std::clamp(g - 1, 0, n_ - 1), sb = std::clamp(b - 1, 0, n_ - 1);
                    const bool outside = r == 0 || g == 0 || b == 0 || r == m_ - 1 || g == m_ - 1 || b == m_ - 1;
                    const float* v = t.at(sr, sg, sb);
                    float* out = &grid_[3 * index(r, g, b)];
                    for (int k = 0; k < 3; ++k) out[k] = outside ? (v[k] - 0.5f) * 4.0f + 0.5f : v[k];
                }
            }
        }
        // The cells binned by the bounds of their values within the unit cube.
        const int cells = m_ - 1;
        std::vector<std::array<int, 6>> spans;
        spans.reserve(static_cast<size_t>(cells) * static_cast<size_t>(cells) * static_cast<size_t>(cells));
        std::vector<uint32_t> counts(static_cast<size_t>(kBins) * kBins * kBins + 1, 0);
        for (int b = 0; b < cells; ++b) {
            for (int g = 0; g < cells; ++g) {
                for (int r = 0; r < cells; ++r) {
                    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
                    for (int corner = 0; corner < 8; ++corner) {
                        const float* v = &grid_[3 * index(r + (corner & 1), g + ((corner >> 1) & 1), b + ((corner >> 2) & 1))];
                        for (int k = 0; k < 3; ++k) {
                            lo[k] = std::min(lo[k], v[k]);
                            hi[k] = std::max(hi[k], v[k]);
                        }
                    }
                    std::array<int, 6> span{};
                    bool empty = false;
                    for (int k = 0; k < 3; ++k) {
                        if (!(hi[k] >= 0.0f && lo[k] <= 1.0f)) empty = true;
                        span[k] = bin(lo[k]);
                        span[3 + k] = bin(hi[k]);
                    }
                    if (empty) span[0] = -1;
                    spans.push_back(span);
                    if (empty) continue;
                    for (int z = span[2]; z <= span[5]; ++z) {
                        for (int y = span[1]; y <= span[4]; ++y) {
                            for (int x = span[0]; x <= span[3]; ++x) ++counts[binIndex(x, y, z) + 1];
                        }
                    }
                }
            }
        }
        for (size_t i = 1; i < counts.size(); ++i) counts[i] += counts[i - 1];
        cells_.resize(counts.back());
        std::vector<uint32_t> fill(counts.begin(), counts.end() - 1);
        for (size_t c = 0; c < spans.size(); ++c) {
            const auto& span = spans[c];
            if (span[0] < 0) continue;
            for (int z = span[2]; z <= span[5]; ++z) {
                for (int y = span[1]; y <= span[4]; ++y) {
                    for (int x = span[0]; x <= span[3]; ++x) cells_[fill[binIndex(x, y, z)]++] = static_cast<uint32_t>(c);
                }
            }
        }
        start_ = std::move(counts);
    }

    /// The input, 0 to 1 each, the table gives `rgb` at -- held to the unit
    /// cube --; 0 where no tetrahedron holds it.
    void find(const float* rgb, float* out) const {
        const double p[3] = {std::clamp(static_cast<double>(rgb[0]), 0.0, 1.0), std::clamp(static_cast<double>(rgb[1]), 0.0, 1.0),
                             std::clamp(static_cast<double>(rgb[2]), 0.0, 1.0)};
        double result[3] = {0.0, 0.0, 0.0};
        const size_t at = binIndex(bin(static_cast<float>(p[0])), bin(static_cast<float>(p[1])), bin(static_cast<float>(p[2])));
        const int cells = m_ - 1;
        for (uint32_t i = start_[at]; i < start_[at + 1]; ++i) {
            const int c = static_cast<int>(cells_[i]);
            const int r = c % cells, g = (c / cells) % cells, b = c / (cells * cells);
            if (inCell(r, g, b, p, result)) break;
        }
        const float top = static_cast<float>(n_ - 1);
        for (int k = 0; k < 3; ++k) out[k] = std::clamp(static_cast<float>(result[k]) - 1.0f, 0.0f, top) / top;
    }

private:
    static constexpr int kBins = 32;

    size_t index(int r, int g, int b) const {
        return (static_cast<size_t>(b) * static_cast<size_t>(m_) + static_cast<size_t>(g)) * static_cast<size_t>(m_) +
               static_cast<size_t>(r);
    }
    static int bin(float v) { return std::clamp(static_cast<int>(std::floor(v * kBins)), 0, kBins - 1); }
    static size_t binIndex(int x, int y, int z) {
        return (static_cast<size_t>(z) * kBins + static_cast<size_t>(y)) * kBins + static_cast<size_t>(x);
    }

    /// Whether one of the cell's six tetrahedra holds `p`; where, in index
    /// units of the grown table, into `out`.
    bool inCell(int r, int g, int b, const double* p, double* out) const {
        static constexpr int kOrders[6][3] = {{0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}};
        const int base[3] = {r, g, b};
        const auto value = [&](const int* corner, int k) {
            return static_cast<double>(grid_[3 * index(corner[0], corner[1], corner[2]) + static_cast<size_t>(k)]);
        };
        for (const auto& order : kOrders) {
            // The corners: the cell's, then a step along each axis in turn.
            int v[4][3];
            for (int k = 0; k < 3; ++k) v[0][k] = base[k];
            for (int s = 0; s < 3; ++s) {
                for (int k = 0; k < 3; ++k) v[s + 1][k] = v[s][k];
                v[s + 1][order[s]] += 1;
            }
            double m[3][3], d[3];
            for (int k = 0; k < 3; ++k) {
                const double origin = value(v[0], k);
                d[k] = p[k] - origin;
                for (int s = 0; s < 3; ++s) m[k][s] = value(v[s + 1], k) - origin;
            }
            const double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                               m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
            if (std::abs(det) < 1e-18) continue;
            double w[3];
            for (int s = 0; s < 3; ++s) {
                double a[3][3];
                for (int row = 0; row < 3; ++row) {
                    for (int col = 0; col < 3; ++col) a[row][col] = col == s ? d[row] : m[row][col];
                }
                w[s] = (a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
                        a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0])) /
                       det;
            }
            constexpr double kTolerance = 1e-9;
            if (w[0] < -kTolerance || w[1] < -kTolerance || w[2] < -kTolerance || w[0] + w[1] + w[2] > 1.0 + kTolerance) continue;
            for (int k = 0; k < 3; ++k) {
                out[k] = v[0][k] + w[0] * (v[1][k] - v[0][k]) + w[1] * (v[2][k] - v[0][k]) + w[2] * (v[3][k] - v[0][k]);
            }
            return true;
        }
        return false;
    }

    int n_, m_;
    std::vector<float> grid_;
    std::vector<uint32_t> start_, cells_;
};

/// The size of the table a 3D table goes back by: OpenColorIO's.
constexpr int kLut3DBackSize = 48;

struct Lut3DOp final : Op {
    std::shared_ptr<const Lut3D> lut;
    bool tetrahedral;
    std::shared_ptr<const Lut3D> forward;  ///< for a table that takes one back: that one
    bool forwardTetrahedral = false;
    Lut3DOp(std::shared_ptr<const Lut3D> l, bool tet) : lut(std::move(l)), tetrahedral(tet) {}
    void apply(float* c) const override { lookUp(*lut, tetrahedral, c); }
    /// Back as OpenColorIO's processors take it by default: a table of the
    /// exact inverse at 48 x 48 x 48 points of the unit cube, linearly
    /// between them.
    OpPtr inverse() const override {
        if (forward) {
            auto op = std::make_shared<Lut3DOp>(forward, forwardTetrahedral);
            return op;
        }
        const Lut3DSearch search(*lut);
        auto back = std::make_shared<Lut3D>();
        const int n = kLut3DBackSize;
        back->size = n;
        back->values.resize(3 * static_cast<size_t>(n) * static_cast<size_t>(n) * static_cast<size_t>(n));
        parallelFor(static_cast<size_t>(n), 1, [&](size_t begin, size_t end) {
            for (size_t b = begin; b < end; ++b) {
                for (int g = 0; g < n; ++g) {
                    for (int r = 0; r < n; ++r) {
                        const float at[3] = {static_cast<float>(r) / static_cast<float>(n - 1),
                                             static_cast<float>(g) / static_cast<float>(n - 1),
                                             static_cast<float>(b) / static_cast<float>(n - 1)};
                        float t[3];
                        search.find(at, t);
                        float* out = &back->values[3 * ((b * static_cast<size_t>(n) + static_cast<size_t>(g)) * static_cast<size_t>(n) +
                                                         static_cast<size_t>(r))];
                        for (int k = 0; k < 3; ++k) out[k] = lut->lo[k] + t[k] * (lut->hi[k] - lut->lo[k]);
                    }
                }
            }
        });
        auto op = std::make_shared<Lut3DOp>(back, false);
        op->forward = lut;
        op->forwardTetrahedral = tetrahedral;
        return op;
    }
    std::string name() const override { return "a 3D table"; }
};

/// A step of its own: a function and its inverse (null: none).
struct FunctionOp final : Op {
    std::function<Vec3(const Vec3&)> there, back;
    std::string what;
    FunctionOp(std::function<Vec3(const Vec3&)> t, std::function<Vec3(const Vec3&)> b, std::string w)
        : there(std::move(t)), back(std::move(b)), what(std::move(w)) {}
    void apply(float* c) const override {
        const Vec3 v = there(Vec3(c[0], c[1], c[2]));
        c[0] = v.x;
        c[1] = v.y;
        c[2] = v.z;
    }
    OpPtr inverse() const override { return back ? std::make_shared<FunctionOp>(back, there, what) : nullptr; }
    std::string name() const override { return what; }
};

// --- ACES's reference gamut compression (1.3) ---------------------------------------------

struct GamutCompression {
    float lim[3] = {1.147f, 1.264f, 1.312f}, thr[3] = {0.815f, 0.803f, 0.880f}, power = 1.2f, scale[3];
    GamutCompression() {
        for (int k = 0; k < 3; ++k) {
            scale[k] = (lim[k] - thr[k]) / std::pow(std::pow((1.0f - thr[k]) / (lim[k] - thr[k]), -power) - 1.0f, 1.0f / power);
        }
    }
    float compress(float d, int k) const {
        const float nd = (d - thr[k]) / scale[k];
        const float p = std::pow(nd, power);
        return thr[k] + scale[k] * nd / std::pow(1.0f + p, 1.0f / power);
    }
    float uncompress(float d, int k) const {
        if (d >= thr[k] + scale[k]) return d;
        const float nd = (d - thr[k]) / scale[k];
        const float p = std::pow(nd, power);
        return thr[k] + scale[k] * std::pow(-(p / (p - 1.0f)), 1.0f / power);
    }
    Vec3 apply(const Vec3& c, bool back) const {
        const float ach = std::max(c.x, std::max(c.y, c.z));
        Vec3 out;
        for (int k = 0; k < 3; ++k) {
            if (ach == 0.0f) {
                out[k] = 0.0f;
                continue;
            }
            const float dist = (ach - c[k]) / std::fabs(ach);
            if (dist < thr[k]) {
                out[k] = c[k];
                continue;
            }
            const float d = back ? uncompress(dist, k) : compress(dist, k);
            out[k] = ach - d * std::fabs(ach);
        }
        return out;
    }
};

// --- tables in files ------------------------------------------------------------------------

/// Each line's words, comments (#) left out, to `fn` -- views into `text`,
/// read without copying: a table of a hundred thousand lines quickly.
/// `fn` false: no more.
template <class Fn>
void eachLine(std::string_view text, Fn fn) {
    std::vector<std::string_view> words;
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string_view::npos) end = text.size();
        std::string_view line = text.substr(at, end - at);
        at = end + 1;
        if (const size_t hash = line.find('#'); hash != std::string_view::npos) line = line.substr(0, hash);
        words.clear();
        for (size_t i = 0; i < line.size();) {
            while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
            size_t j = i;
            while (j < line.size() && !std::isspace(static_cast<unsigned char>(line[j]))) ++j;
            if (j > i) words.push_back(line.substr(i, j - i));
            i = j;
        }
        if (!words.empty() && !fn(words)) return;
    }
}

bool number(std::string_view s, double& out) {
    char buffer[64];
    if (s.empty() || s.size() >= sizeof buffer) return false;
    std::copy(s.begin(), s.end(), buffer);
    buffer[s.size()] = '\0';
    char* end = nullptr;
    out = std::strtod(buffer, &end);
    return end == buffer + s.size();
}

int whole(std::string_view s) {
    double v = 0.0;
    return number(s, v) && std::abs(v) < 1e9 ? static_cast<int>(v) : 0;
}

/// Sony Imageworks' 1D table: "From lo hi", "Length n", "Components c",
/// then { values }.
bool readSpi1d(const std::string& text, OcioProcessor& out, std::string& error) {
    auto lut = std::make_shared<Lut1D>();
    double from[2] = {0.0, 1.0};
    int length = 0;
    bool in = false, bad = false;
    eachLine(text, [&](const std::vector<std::string_view>& w) {
        if (!in) {
            if (w[0] == "From" && w.size() >= 3) {
                number(w[1], from[0]);
                number(w[2], from[1]);
            } else if (w[0] == "Length" && w.size() >= 2) {
                length = whole(w[1]);
            } else if (w[0] == "Components" && w.size() >= 2) {
                lut->components = whole(w[1]);
            } else if (w[0] == "{") {
                in = true;
            }
            return true;
        }
        if (w[0] == "}") return false;
        for (const std::string_view s : w) {
            double v = 0.0;
            if (!number(s, v)) {
                error = "a number in the table is not one: " + std::string(s);
                bad = true;
                return false;
            }
            lut->values.push_back(static_cast<float>(v));
        }
        return true;
    });
    if (bad) return false;
    if (lut->components != 1 && lut->components != 3) lut->components = 1;
    lut->size = length;
    if (length < 2 || lut->values.size() != static_cast<size_t>(length) * static_cast<size_t>(lut->components)) {
        error = "the table has not Length entries";
        return false;
    }
    for (int k = 0; k < 3; ++k) {
        lut->lo[k] = static_cast<float>(from[0]);
        lut->hi[k] = static_cast<float>(from[1]);
    }
    out.add(std::make_shared<Lut1DOp>(lut));
    return true;
}

/// Sony Imageworks' 3D table: "SPILUT 1.0", "3 3", "n n n", then "i j k r g b".
bool readSpi3d(const std::string& text, OcioProcessor& out, bool tetrahedral, std::string& error) {
    auto lut = std::make_shared<Lut3D>();
    int line = 0, n = 0;
    size_t count = 0;
    bool bad = false;
    eachLine(text, [&](const std::vector<std::string_view>& w) {
        if (++line < 3) return true;
        if (line == 3) {
            n = w.size() >= 3 ? whole(w[0]) : 0;
            if (n < 2 || whole(w[1]) != n || whole(w[2]) != n) {
                error = "a .spi3d table must be a cube";
                bad = true;
                return false;
            }
            lut->size = n;
            lut->values.assign(3 * static_cast<size_t>(n) * static_cast<size_t>(n) * static_cast<size_t>(n), 0.0f);
            return true;
        }
        if (w.size() < 6) return true;
        const int i = whole(w[0]), j = whole(w[1]), k = whole(w[2]);
        if (i < 0 || j < 0 || k < 0 || i >= n || j >= n || k >= n) return true;
        float* v = &lut->values[3 * ((static_cast<size_t>(k) * static_cast<size_t>(n) + static_cast<size_t>(j)) * static_cast<size_t>(n) +
                                     static_cast<size_t>(i))];
        for (int c = 0; c < 3; ++c) {
            double x = 0.0;
            number(w[static_cast<size_t>(3 + c)], x);
            v[c] = static_cast<float>(x);
        }
        ++count;
        return true;
    });
    if (bad) return false;
    if (n < 2) {
        error = "not a .spi3d table";
        return false;
    }
    if (count != static_cast<size_t>(n) * static_cast<size_t>(n) * static_cast<size_t>(n)) {
        error = "the .spi3d table has not n^3 entries";
        return false;
    }
    out.add(std::make_shared<Lut3DOp>(lut, tetrahedral));
    return true;
}

/// A 3 x 4 matrix: three rows of three and an offset in 0..65535.
bool readSpimtx(const std::string& text, OcioProcessor& out, std::string& error) {
    std::vector<double> v;
    eachLine(text, [&](const std::vector<std::string_view>& w) {
        for (const std::string_view s : w) {
            double x = 0.0;
            if (number(s, x)) v.push_back(x);
        }
        return true;
    });
    if (v.size() != 12) {
        error = "a .spimtx has 12 numbers";
        return false;
    }
    const double m[9] = {v[0], v[1], v[2], v[4], v[5], v[6], v[8], v[9], v[10]};
    const double o[3] = {v[3] / 65535.0, v[7] / 65535.0, v[11] / 65535.0};
    out.add(std::make_shared<MatrixOp>(m, o));
    return true;
}

/// Resolve's and Iridas' .cube: LUT_1D_SIZE or LUT_3D_SIZE, the domain, then
/// rows of three, red fastest.
bool readCube(const std::string& text, OcioProcessor& out, bool tetrahedral, std::string& error) {
    int size1 = 0, size3 = 0;
    double lo[3] = {0, 0, 0}, hi[3] = {1, 1, 1};
    std::vector<float> values;
    eachLine(text, [&](const std::vector<std::string_view>& w) {
        const std::string_view key = w[0];
        if (key == "TITLE") return true;
        if (key == "LUT_1D_SIZE" && w.size() >= 2) {
            size1 = whole(w[1]);
        } else if (key == "LUT_3D_SIZE" && w.size() >= 2) {
            size3 = whole(w[1]);
            values.reserve(3 * static_cast<size_t>(std::clamp(size3, 0, 256)) * static_cast<size_t>(std::clamp(size3, 0, 256)) *
                           static_cast<size_t>(std::clamp(size3, 0, 256)));
        } else if (key == "DOMAIN_MIN" && w.size() >= 4) {
            for (int k = 0; k < 3; ++k) number(w[static_cast<size_t>(k + 1)], lo[k]);
        } else if (key == "DOMAIN_MAX" && w.size() >= 4) {
            for (int k = 0; k < 3; ++k) number(w[static_cast<size_t>(k + 1)], hi[k]);
        } else if ((key == "LUT_1D_INPUT_RANGE" || key == "LUT_3D_INPUT_RANGE") && w.size() >= 3) {
            double a = 0.0, b = 1.0;
            number(w[1], a);
            number(w[2], b);
            for (int k = 0; k < 3; ++k) lo[k] = a, hi[k] = b;
        } else if (w.size() == 3) {
            double r = 0, g = 0, b = 0;
            if (number(w[0], r) && number(w[1], g) && number(w[2], b)) {
                values.push_back(static_cast<float>(r));
                values.push_back(static_cast<float>(g));
                values.push_back(static_cast<float>(b));
            }
        }
        return true;
    });
    if (size3 >= 2) {
        if (values.size() != 3 * static_cast<size_t>(size3) * static_cast<size_t>(size3) * static_cast<size_t>(size3)) {
            error = "the .cube has not LUT_3D_SIZE^3 rows";
            return false;
        }
        auto lut = std::make_shared<Lut3D>();
        lut->size = size3;
        for (int k = 0; k < 3; ++k) {
            lut->lo[k] = static_cast<float>(lo[k]);
            lut->hi[k] = static_cast<float>(hi[k]);
        }
        lut->values = std::move(values);
        out.add(std::make_shared<Lut3DOp>(lut, tetrahedral));
        return true;
    }
    if (size1 >= 2) {
        if (values.size() != 3 * static_cast<size_t>(size1)) {
            error = "the .cube has not LUT_1D_SIZE rows";
            return false;
        }
        auto lut = std::make_shared<Lut1D>();
        lut->size = size1;
        lut->components = 3;
        for (int k = 0; k < 3; ++k) {
            lut->lo[k] = static_cast<float>(lo[k]);
            lut->hi[k] = static_cast<float>(hi[k]);
        }
        lut->values = std::move(values);
        out.add(std::make_shared<Lut1DOp>(lut));
        return true;
    }
    error = "a .cube with no LUT_1D_SIZE nor LUT_3D_SIZE";
    return false;
}

// --- the built-in transforms ----------------------------------------------------------------

constexpr Primaries kP3D65{{{0.680, 0.320}, {0.265, 0.690}, {0.150, 0.060}, {0.3127, 0.3290}}};
constexpr Primaries kP3Dci{{{0.680, 0.320}, {0.265, 0.690}, {0.150, 0.060}, {0.314, 0.351}}};
constexpr Primaries kP3D60{{{0.680, 0.320}, {0.265, 0.690}, {0.150, 0.060}, {0.32168, 0.33767}}};
constexpr Primaries kRec2020{{{0.708, 0.292}, {0.170, 0.797}, {0.131, 0.046}, {0.3127, 0.3290}}};

/// CIE XYZ (D65) to RGB of `p`: with Bradford's adaptation to its white where `adapt`.
D33 fromXyzD65(const Primaries& p, bool adapt) {
    return adapt ? color::inverse(color::toXyzD65(p)) : color::inverse(color::rgbToXyz(p));
}

/// A display's encoding: XYZ to its primaries, then its curve.
void display(OcioProcessor& out, const Primaries& p, bool adapt, double gamma, bool srgb, bool mirror) {
    out.add(matrixOp(fromXyzD65(p, adapt)));
    const double g[3] = {gamma, gamma, gamma};
    if (srgb) {
        const double o[3] = {0.055, 0.055, 0.055};
        out.add(std::make_shared<MonCurveOp>(g, o, false, mirror));
    } else {
        const double inv[3] = {1.0 / gamma, 1.0 / gamma, 1.0 / gamma};
        out.add(std::make_shared<PowerOp>(inv, mirror ? Negative::Mirror : Negative::Clamp));
    }
}

/// The built-in transform `style` forward into `out`; false with why.
bool builtin(const std::string& style, OcioProcessor& out, std::string& error) {
    const std::string s = lower(style);
    const auto is = [&](const char* name) { return s == lower(name); };
    using color::kAP0;
    using color::kAP1;
    using color::kRec709;
    if (is("UTILITY - ACES-AP0_to_CIE-XYZ-D65_BFD")) return out.add(matrixOp(color::toXyzD65(kAP0))), true;
    if (is("UTILITY - ACES-AP1_to_CIE-XYZ-D65_BFD")) return out.add(matrixOp(color::toXyzD65(kAP1))), true;
    if (is("UTILITY - ACES-AP1_to_LINEAR-REC709_BFD")) return out.add(matrixOp(color::conversion(kAP1, kRec709, true))), true;
    if (is("ACEScg_to_ACES2065-1")) return out.add(matrixOp(color::conversion(kAP1, kAP0, false))), true;
    if (is("ACEScct_to_ACES2065-1")) {
        const double logSlope[3] = {1.0 / 17.52, 1.0 / 17.52, 1.0 / 17.52}, logOffset[3] = {9.72 / 17.52, 9.72 / 17.52, 9.72 / 17.52};
        const double one[3] = {1, 1, 1}, zero[3] = {0, 0, 0};
        auto op = logOp(2.0, logSlope, logOffset, one, zero, false);
        auto cam = std::make_shared<LogOp>(*op);
        cam->camera = true;
        for (double& b : cam->linBreak) b = 0.0078125;
        cam->prepare();
        out.add(cam);
        out.add(matrixOp(color::conversion(kAP1, kAP0, false)));
        return true;
    }
    if (is("ACEScc_to_ACES2065-1")) {
        // As OpenColorIO: a range, a 4096-entry table of the curve, the matrix, no light below 0.
        out.add(std::make_shared<RangeOp>(-0.36, 1.5, 0.0, 1.0, true, true, true));
        auto lut = std::make_shared<Lut1D>();
        lut->size = 4096;
        lut->values.resize(4096);
        for (int i = 0; i < 4096; ++i) {
            const double in = static_cast<double>(i) / 4095.0 * (1.5 + 0.36) - 0.36;
            const double v = in < (9.72 - 15.0) / 17.52 ? (std::pow(2.0, in * 17.52 - 9.72) - std::pow(2.0, -16.0)) * 2.0
                                                        : std::pow(2.0, in * 17.52 - 9.72);
            lut->values[static_cast<size_t>(i)] = static_cast<float>(v);
        }
        out.add(std::make_shared<Lut1DOp>(lut));
        out.add(matrixOp(color::conversion(kAP1, kAP0, false)));
        out.add(std::make_shared<RangeOp>(0.0, 0.0, 0.0, 0.0, true, false, true));
        return true;
    }
    if (is("ACES-LMT - ACES 1.3 Reference Gamut Compression")) {
        static const GamutCompression gc;
        out.add(matrixOp(color::conversion(kAP0, kAP1, false)));
        out.add(std::make_shared<FunctionOp>([](const Vec3& c) { return gc.apply(c, false); },
                                             [](const Vec3& c) { return gc.apply(c, true); }, "the gamut compression"));
        out.add(matrixOp(color::conversion(kAP1, kAP0, false)));
        return true;
    }
    if (AcesOutputXyz which; acesOutputXyzNamed(style, which)) {
        out.add(std::make_shared<FunctionOp>([which](const Vec3& c) { return acesOutputRgb(c, which); },
                                             [which](const Vec3& c) { return acesOutputRgbBack(c, which); }, style));
        out.add(matrixOp(acesOutputRgbToXyz(which)));
        return true;
    }
    struct Display {
        const char* name;
        const Primaries* p;
        bool adapt;
        double gamma;
        bool srgb, mirror;
    };
    static const Display displays[] = {
        {"DISPLAY - CIE-XYZ-D65_to_sRGB", &kRec709, false, 2.4, true, false},
        {"DISPLAY - CIE-XYZ-D65_to_sRGB - MIRROR NEGS", &kRec709, false, 2.4, true, true},
        {"DISPLAY - CIE-XYZ-D65_to_REC.1886-REC.709", &kRec709, false, 2.4, false, false},
        {"DISPLAY - CIE-XYZ-D65_to_REC.1886-REC.709 - MIRROR NEGS", &kRec709, false, 2.4, false, true},
        {"DISPLAY - CIE-XYZ-D65_to_REC.1886-REC.2020", &kRec2020, false, 2.4, false, false},
        {"DISPLAY - CIE-XYZ-D65_to_REC.1886-REC.2020 - MIRROR NEGS", &kRec2020, false, 2.4, false, true},
        {"DISPLAY - CIE-XYZ-D65_to_G2.2-REC.709", &kRec709, false, 2.2, false, false},
        {"DISPLAY - CIE-XYZ-D65_to_G2.2-REC.709 - MIRROR NEGS", &kRec709, false, 2.2, false, true},
        {"DISPLAY - CIE-XYZ-D65_to_G2.6-P3-D65", &kP3D65, false, 2.6, false, false},
        {"DISPLAY - CIE-XYZ-D65_to_G2.6-P3-D65 - MIRROR NEGS", &kP3D65, false, 2.6, false, true},
        {"DISPLAY - CIE-XYZ-D65_to_G2.6-P3-DCI-BFD", &kP3Dci, true, 2.6, false, false},
        {"DISPLAY - CIE-XYZ-D65_to_G2.6-P3-D60-BFD", &kP3D60, true, 2.6, false, false},
        {"DISPLAY - CIE-XYZ-D65_to_DisplayP3", &kP3D65, false, 2.4, true, true},
    };
    for (const Display& d : displays) {
        if (!is(d.name)) continue;
        display(out, *d.p, d.adapt, d.gamma, d.srgb, d.mirror);
        return true;
    }
    error = "the built-in transform '" + style + "' is not here (HDR, cameras' logs and the like)";
    return false;
}

}  // namespace

// --- the processor ---------------------------------------------------------------------------

Vec3 OcioProcessor::apply(const Vec3& rgb) const {
    float c[3] = {rgb.x, rgb.y, rgb.z};
    for (const auto& op : ops_) op->apply(c);
    return {c[0], c[1], c[2]};
}

void OcioProcessor::add(std::shared_ptr<const Op> op) {
    if (!op) return;
    double m2[9], o2[3];
    if (!ops_.empty() && op->matrix(m2, o2)) {
        double m1[9], o1[3];
        if (ops_.back()->matrix(m1, o1)) {
            // One after the other: M2 (M1 x + o1) + o2.
            const D33 a{m1[0], m1[1], m1[2], m1[3], m1[4], m1[5], m1[6], m1[7], m1[8]};
            const D33 b{m2[0], m2[1], m2[2], m2[3], m2[4], m2[5], m2[6], m2[7], m2[8]};
            const D33 m = color::times(b, a);
            const std::array<double, 3> t = color::times(b, std::array<double, 3>{o1[0], o1[1], o1[2]});
            const double o[3] = {t[0] + o2[0], t[1] + o2[1], t[2] + o2[2]};
            ops_.back() = std::make_shared<MatrixOp>(m.data(), o);
            return;
        }
    }
    ops_.push_back(std::move(op));
}

void OcioProcessor::append(const OcioProcessor& other) {
    for (const auto& op : other.ops_) add(op);
}

bool OcioProcessor::inverse(OcioProcessor& out, std::string& error) const {
    OcioProcessor back;
    for (auto it = ops_.rbegin(); it != ops_.rend(); ++it) {
        OpPtr inv = (*it)->inverse();
        if (!inv) {
            error = (*it)->name() + " does not go back";
            return false;
        }
        back.add(inv);
    }
    out = std::move(back);
    return true;
}

// --- the config -----------------------------------------------------------------------------

struct OcioConfig::Data {
    YamlNode root;
    int version = 1;
    std::string folder;                 ///< where the config is: its search paths start there
    std::vector<std::string> searchPaths;
    std::map<std::string, std::string> environment;
    struct Space {
        std::string name;
        std::vector<std::string> aliases;
        bool data = false, display = false;
        const YamlNode* toRef = nullptr;
        const YamlNode* fromRef = nullptr;
    };
    std::vector<Space> spaces;
    struct ViewTransform {
        std::string name;
        const YamlNode *fromScene = nullptr, *toScene = nullptr, *fromDisplay = nullptr, *toDisplay = nullptr;
    };
    std::vector<ViewTransform> viewTransforms;
    std::string defaultViewTransform;
    struct View {
        std::string name, colorspace, viewTransform, displayColorspace, looks;
    };
    struct Display {
        std::string name;
        std::vector<View> views;
    };
    std::vector<Display> displays;
    std::vector<std::string> activeDisplays, activeViews, inactiveSpaces;
    std::map<std::string, std::string> roles;  ///< lower-cased role -> colour space
    struct Look {
        std::string name, processSpace;
        const YamlNode *transform = nullptr, *inverse = nullptr;
    };
    std::vector<Look> looks;
    mutable std::mutex filesMutex;
    mutable std::map<std::string, std::string> files;  ///< tables read, by path

    const Space* space(const std::string& name) const {
        for (const Space& s : spaces) {
            if (same(s.name, name)) return &s;
        }
        for (const Space& s : spaces) {
            for (const std::string& a : s.aliases) {
                if (same(a, name)) return &s;
            }
        }
        const auto it = roles.find(lower(name));
        if (it != roles.end() && !same(it->second, name)) return space(it->second);
        return nullptr;
    }
    const ViewTransform* viewTransform(const std::string& name) const {
        for (const ViewTransform& v : viewTransforms) {
            if (same(v.name, name)) return &v;
        }
        return nullptr;
    }
    const Display* display(const std::string& name) const {
        for (const Display& d : displays) {
            if (same(d.name, name)) return &d;
        }
        return nullptr;
    }
    const Look* look(const std::string& name) const {
        for (const Look& l : looks) {
            if (same(l.name, name)) return &l;
        }
        return nullptr;
    }

    std::string expand(std::string s) const {
        // $NAME and ${NAME}: from the config's environment section.
        for (size_t at = s.find('$'); at != std::string::npos; at = s.find('$', at)) {
            size_t end = at + 1;
            std::string name;
            if (end < s.size() && s[end] == '{') {
                const size_t close = s.find('}', end);
                if (close == std::string::npos) break;
                name = s.substr(end + 1, close - end - 1);
                end = close + 1;
            } else {
                while (end < s.size() && (std::isalnum(static_cast<unsigned char>(s[end])) || s[end] == '_')) ++end;
                name = s.substr(at + 1, end - at - 1);
            }
            const auto it = environment.find(name);
            const std::string value = it != environment.end() ? it->second : std::string();
            s.replace(at, end - at, value);
            at += value.size();
        }
        return s;
    }

    /// A table's file: where it is, by the search paths.
    bool findFile(const std::string& src, std::string& path, std::string& error) const {
        namespace fs = std::filesystem;
        const std::string name = expand(src);
        std::error_code ec;
        if (fs::path(name).is_absolute()) {
            path = name;
            if (fs::is_regular_file(path, ec)) return true;
        } else {
            std::vector<std::string> dirs = searchPaths;
            if (dirs.empty()) dirs.push_back(".");
            for (const std::string& d : dirs) {
                fs::path dir = fs::path(expand(d));
                if (dir.is_relative()) dir = fs::path(folder) / dir;
                const fs::path p = (dir / name).lexically_normal();
                if (fs::is_regular_file(p, ec)) {
                    path = p.string();
                    return true;
                }
            }
        }
        error = "no file " + name + " where the config looks (search_path)";
        return false;
    }

    bool readFile(const std::string& path, std::string& text, std::string& error) const {
        std::lock_guard<std::mutex> lock(filesMutex);
        const auto it = files.find(path);
        if (it != files.end()) {
            text = it->second;
            return true;
        }
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            error = path + ": cannot read it";
            return false;
        }
        std::stringstream ss;
        ss << in.rdbuf();
        text = files[path] = ss.str();
        return true;
    }

    bool transform(const YamlNode& t, bool inverse, OcioProcessor& out, std::string& error, int depth) const;
    bool toReference(const Space& s, OcioProcessor& out, std::string& error, int depth) const;
    bool fromReference(const Space& s, OcioProcessor& out, std::string& error, int depth) const;
    bool between(const std::string& from, const std::string& to, OcioProcessor& out, std::string& error, int depth) const;
    bool bridge(bool toDisplay, OcioProcessor& out, std::string& error, int depth) const;
    bool look(const Look& l, bool back, OcioProcessor& out, std::string& error, int depth) const;
    bool looksOf(const std::string& looks, std::vector<std::pair<const Look*, bool>>& out, std::string& error) const;
    bool applyLooks(const std::string& looks, std::string& space, OcioProcessor& out, std::string& error, int depth) const;
    bool view(const std::string& from, const std::string& display, const std::string& view, const std::string& looks,
              bool inverse, OcioProcessor& out, std::string& error, int depth) const;
};

namespace {

/// Numbers of a sequence, or one scalar repeated.
bool numbersOf(const YamlNode* n, std::vector<double>& out) {
    out.clear();
    if (!n) return false;
    if (n->isScalar()) {
        double v = 0.0;
        if (!number(n->scalar, v)) return false;
        out.push_back(v);
        return true;
    }
    if (!n->isSequence()) return false;
    for (const YamlNode& i : n->items) {
        double v = 0.0;
        if (!i.isScalar() || !number(i.scalar, v)) return false;
        out.push_back(v);
    }
    return true;
}

/// Three channels of a scalar or a list of 3 or 4.
bool rgbOf(const YamlNode* n, double* rgb, double fallback) {
    std::vector<double> v;
    if (!numbersOf(n, v)) {
        rgb[0] = rgb[1] = rgb[2] = fallback;
        return n == nullptr;
    }
    if (v.size() == 1) {
        rgb[0] = rgb[1] = rgb[2] = v[0];
        return true;
    }
    if (v.size() < 3) return false;
    for (int k = 0; k < 3; ++k) rgb[k] = v[static_cast<size_t>(k)];
    return true;
}

double scalarOf(const YamlNode& t, const char* key, double fallback, bool* found = nullptr) {
    const YamlNode* n = t.find(key);
    double v = fallback;
    const bool ok = n && n->isScalar() && number(n->scalar, v);
    if (found) *found = ok;
    return ok ? v : fallback;
}

}  // namespace

bool OcioConfig::Data::transform(const YamlNode& t, bool inverse, OcioProcessor& out, std::string& error, int depth) const {
    if (depth > 64) {
        error = "transforms that go round in a circle";
        return false;
    }
    const std::string type = t.tag;
    if (lower(t.text("direction", "forward")) == "inverse") inverse = !inverse;
    // A step and its inverse, by the direction.
    const auto step = [&](const OpPtr& op) {
        if (!inverse) {
            out.add(op);
            return true;
        }
        OpPtr back = op->inverse();
        if (!back) {
            error = op->name() + " does not go back";
            return false;
        }
        out.add(back);
        return true;
    };
    if (type == "GroupTransform") {
        const YamlNode* children = t.find("children");
        if (!children || !children->isSequence()) return true;
        if (!inverse) {
            for (const YamlNode& c : children->items) {
                if (!transform(c, false, out, error, depth + 1)) return false;
            }
        } else {
            for (auto it = children->items.rbegin(); it != children->items.rend(); ++it) {
                if (!transform(*it, true, out, error, depth + 1)) return false;
            }
        }
        return true;
    }
    if (type == "MatrixTransform") {
        std::vector<double> m, o;
        numbersOf(t.find("matrix"), m);
        numbersOf(t.find("offset"), o);
        double m9[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}, o3[3] = {0, 0, 0};
        if (m.size() == 16) {
            for (int r = 0; r < 3; ++r) {
                for (int c = 0; c < 3; ++c) m9[3 * r + c] = m[static_cast<size_t>(4 * r + c)];
                o3[r] += m[static_cast<size_t>(4 * r + 3)];  // alpha is 1
            }
        } else if (!m.empty()) {
            error = "a matrix has 16 numbers";
            return false;
        }
        for (size_t k = 0; k < 3 && k < o.size(); ++k) o3[k] += o[k];
        return step(std::make_shared<MatrixOp>(m9, o3));
    }
    if (type == "ExponentTransform") {
        double g[3];
        if (!rgbOf(t.find("value"), g, 1.0)) {
            error = "an exponent's value is 1 or 4 numbers";
            return false;
        }
        const std::string style = lower(t.text("style", "basicFwd"));
        const Negative neg = style.find("mirror") != std::string::npos   ? Negative::Mirror
                             : style.find("pass") != std::string::npos ? Negative::Pass
                                                                         : Negative::Clamp;
        return step(std::make_shared<PowerOp>(g, neg));
    }
    if (type == "ExponentWithLinearTransform") {
        double g[3], o[3];
        rgbOf(t.find("gamma"), g, 1.0);
        rgbOf(t.find("offset"), o, 0.0);
        const bool mirror = lower(t.text("style", "linear")).find("mirror") != std::string::npos;
        // Forward: from the encoding to light.
        return step(std::make_shared<MonCurveOp>(g, o, true, mirror));
    }
    if (type == "LogTransform" || type == "LogAffineTransform" || type == "LogCameraTransform") {
        const double base = scalarOf(t, "base", 2.0);
        double logSlope[3], logOffset[3], linSlope[3], linOffset[3];
        rgbOf(t.find("log_side_slope"), logSlope, 1.0);
        rgbOf(t.find("log_side_offset"), logOffset, 0.0);
        rgbOf(t.find("lin_side_slope"), linSlope, 1.0);
        rgbOf(t.find("lin_side_offset"), linOffset, 0.0);
        auto op = logOp(base, logSlope, logOffset, linSlope, linOffset, true);
        if (type == "LogCameraTransform") {
            auto cam = std::make_shared<LogOp>(*op);
            cam->camera = true;
            if (!t.find("lin_side_break")) {
                error = "a camera's log needs lin_side_break";
                return false;
            }
            rgbOf(t.find("lin_side_break"), cam->linBreak, 0.0);
            if (t.find("linear_slope")) {
                cam->hasLinearSlope = true;
                rgbOf(t.find("linear_slope"), cam->linearSlopeGiven, 1.0);
            }
            cam->prepare();
            return step(cam);
        }
        return step(op);
    }
    if (type == "CDLTransform") {
        double slope[3], offset[3], power[3];
        rgbOf(t.find("slope"), slope, 1.0);
        rgbOf(t.find("offset"), offset, 0.0);
        rgbOf(t.find("power"), power, 1.0);
        const double sat = scalarOf(t, "sat", scalarOf(t, "saturation", 1.0));
        // OpenColorIO 2's own style unless it says ASC's: no clamps.
        const bool clamp = lower(t.text("style", "noclamp")) == "asc";
        return step(std::make_shared<CdlOp>(slope, offset, power, sat, clamp, true));
    }
    if (type == "RangeTransform") {
        bool a = false, b = false, c = false, d = false;
        const double minIn = scalarOf(t, "min_in_value", 0.0, &a), maxIn = scalarOf(t, "max_in_value", 1.0, &b);
        const double minOut = scalarOf(t, "min_out_value", 0.0, &c), maxOut = scalarOf(t, "max_out_value", 1.0, &d);
        const bool clamp = lower(t.text("style", "clamp")) != "noclamp";
        return step(std::make_shared<RangeOp>(minIn, maxIn, minOut, maxOut, a && c, b && d, clamp));
    }
    if (type == "AllocationTransform") {
        const std::string kind = lower(t.text("allocation", "uniform"));
        std::vector<double> vars;
        numbersOf(t.find("vars"), vars);
        double lo = kind == "lg2" ? -10.0 : 0.0, hi = kind == "lg2" ? 6.0 : 1.0;
        if (vars.size() >= 2) lo = vars[0], hi = vars[1];
        const double s = 1.0 / (hi - lo);
        const double fit[9] = {s, 0, 0, 0, s, 0, 0, 0, s}, off[3] = {-lo * s, -lo * s, -lo * s};
        OcioProcessor steps;
        if (kind == "lg2") {
            const double one[3] = {1, 1, 1}, zero[3] = {0, 0, 0};
            double linOffset[3] = {0, 0, 0};
            if (vars.size() >= 3) linOffset[0] = linOffset[1] = linOffset[2] = vars[2];
            steps.add(logOp(2.0, one, zero, one, linOffset, true));
        }
        steps.add(std::make_shared<MatrixOp>(fit, off));
        if (inverse) {
            OcioProcessor back;
            if (!steps.inverse(back, error)) return false;
            out.append(back);
        } else {
            out.append(steps);
        }
        return true;
    }
    if (type == "FileTransform") {
        const std::string src = t.text("src");
        std::string path;
        if (!findFile(src, path, error)) return false;
        std::string text;
        if (!readFile(path, text, error)) return false;
        const std::string interp = lower(t.text("interpolation", "linear"));
        const bool tetrahedral = interp == "tetrahedral" || interp == "best";
        std::string ext = lower(std::filesystem::path(path).extension().string());
        OcioProcessor steps;
        bool ok = false;
        std::string why;
        if (ext == ".spi1d") ok = readSpi1d(text, steps, why);
        else if (ext == ".spi3d") ok = readSpi3d(text, steps, tetrahedral, why);
        else if (ext == ".spimtx") ok = readSpimtx(text, steps, why);
        else if (ext == ".cube") ok = readCube(text, steps, tetrahedral, why);
        else why = "a table of a kind not read here (" + ext + ")";
        if (!ok) {
            error = path + ": " + why;
            return false;
        }
        if (inverse) {
            OcioProcessor back;
            if (!steps.inverse(back, error)) {
                error = path + ": " + error;
                return false;
            }
            out.append(back);
        } else {
            out.append(steps);
        }
        return true;
    }
    if (type == "ColorSpaceTransform") {
        const std::string src = t.text("src"), dst = t.text("dst");
        return inverse ? between(dst, src, out, error, depth + 1) : between(src, dst, out, error, depth + 1);
    }
    if (type == "LookTransform") {
        const std::string src = t.text("src"), dst = t.text("dst"), looks = t.text("looks");
        if (!inverse) {
            std::string at = src;
            if (!applyLooks(looks, at, out, error, depth + 1)) return false;
            return between(at, dst, out, error, depth + 1);
        }
        // Back: from dst to where the looks left the light, then each look
        // back, in its process space, to the one before it.
        std::vector<std::pair<const Look*, bool>> steps;
        if (!looksOf(looks, steps, error)) return false;
        std::vector<std::string> spaces;
        std::string at = src;
        for (const auto& step : steps) {
            if (!step.first->processSpace.empty()) at = step.first->processSpace;
            spaces.push_back(at);
        }
        if (!between(dst, at, out, error, depth + 1)) return false;
        for (size_t i = steps.size(); i-- > 0;) {
            if (!look(*steps[i].first, !steps[i].second, out, error, depth) ||
                !between(spaces[i], i > 0 ? spaces[i - 1] : src, out, error, depth + 1)) {
                return false;
            }
        }
        return true;
    }
    if (type == "DisplayViewTransform") {
        return view(t.text("src"), t.text("display"), t.text("view"), "", inverse, out, error, depth + 1);
    }
    if (type == "BuiltinTransform") {
        OcioProcessor steps;
        if (!builtin(t.text("style"), steps, error)) return false;
        if (inverse) {
            OcioProcessor back;
            if (!steps.inverse(back, error)) return false;
            out.append(back);
        } else {
            out.append(steps);
        }
        return true;
    }
    if (type == "FixedFunctionTransform") {
        const std::string style = lower(t.text("style"));
        if (style == "aces_gamutcomp13" || style == "aces_gamut_comp_13") {
            static const GamutCompression gc;
            return step(std::make_shared<FunctionOp>([](const Vec3& c) { return gc.apply(c, false); },
                                                     [](const Vec3& c) { return gc.apply(c, true); }, "the gamut compression"));
        }
        error = "the fixed function '" + t.text("style") + "' is not here";
        return false;
    }
    error = (type.empty() ? std::string("a transform with no type") : "the " + type) + " is not here";
    return false;
}

bool OcioConfig::Data::toReference(const Space& s, OcioProcessor& out, std::string& error, int depth) const {
    if (s.toRef) return transform(*s.toRef, false, out, error, depth + 1);
    if (s.fromRef) return transform(*s.fromRef, true, out, error, depth + 1);
    return true;  // the reference itself
}

bool OcioConfig::Data::fromReference(const Space& s, OcioProcessor& out, std::string& error, int depth) const {
    if (s.fromRef) return transform(*s.fromRef, false, out, error, depth + 1);
    if (s.toRef) return transform(*s.toRef, true, out, error, depth + 1);
    return true;
}

bool OcioConfig::Data::bridge(bool toDisplay, OcioProcessor& out, std::string& error, int depth) const {
    // The default view transform -- or the first between a scene and a
    // display there is -- bridges the scene's reference and the display's:
    // each way by the transform it has for that way, or the other's inverse.
    const ViewTransform* vt = viewTransform(defaultViewTransform);
    if (vt && !vt->fromScene && !vt->toScene) vt = nullptr;
    if (!vt) {
        for (const ViewTransform& v : viewTransforms) {
            if (v.fromScene || v.toScene) {
                vt = &v;
                break;
            }
        }
    }
    if (!vt) {
        error = "no view transform between the scene's light and a display's";
        return false;
    }
    if (toDisplay) {
        return vt->fromScene ? transform(*vt->fromScene, false, out, error, depth + 1)
                             : transform(*vt->toScene, true, out, error, depth + 1);
    }
    return vt->toScene ? transform(*vt->toScene, false, out, error, depth + 1)
                       : transform(*vt->fromScene, true, out, error, depth + 1);
}

bool OcioConfig::Data::between(const std::string& from, const std::string& to, OcioProcessor& out, std::string& error,
                               int depth) const {
    if (depth > 64) {
        error = "colour spaces that go round in a circle";
        return false;
    }
    const Space* a = space(from);
    const Space* b = space(to);
    if (!a || !b) {
        error = "no colour space " + (a ? to : from);
        return false;
    }
    if (a == b || a->data || b->data) return true;
    if (!toReference(*a, out, error, depth)) return false;
    if (a->display != b->display && !bridge(b->display, out, error, depth)) return false;
    return fromReference(*b, out, error, depth);
}

bool OcioConfig::Data::looksOf(const std::string& looks, std::vector<std::pair<const Look*, bool>>& out,
                               std::string& error) const {
    // "A, -B" or "+A -B": each forward, or - back.
    std::string word;
    for (const char c : looks + ",") {
        if (c != ',' && c != ' ' && c != ':') {
            word += c;
            continue;
        }
        if (word.empty()) continue;
        const bool back = word[0] == '-';
        const std::string name = word[0] == '+' || word[0] == '-' ? word.substr(1) : word;
        word.clear();
        const Look* l = look(name);
        if (!l) {
            error = "no look " + name;
            return false;
        }
        out.emplace_back(l, back);
    }
    return true;
}

bool OcioConfig::Data::look(const Look& l, bool back, OcioProcessor& out, std::string& error, int depth) const {
    // Each way by the transform the look has for it, or the other's inverse.
    if (!back) {
        if (l.transform) return transform(*l.transform, false, out, error, depth + 1);
        return !l.inverse || transform(*l.inverse, true, out, error, depth + 1);
    }
    if (l.inverse) return transform(*l.inverse, false, out, error, depth + 1);
    return !l.transform || transform(*l.transform, true, out, error, depth + 1);
}

bool OcioConfig::Data::applyLooks(const std::string& looks, std::string& space, OcioProcessor& out, std::string& error,
                                  int depth) const {
    std::vector<std::pair<const Look*, bool>> steps;
    if (!looksOf(looks, steps, error)) return false;
    for (const auto& [l, back] : steps) {
        if (!l->processSpace.empty()) {
            if (!between(space, l->processSpace, out, error, depth + 1)) return false;
            space = l->processSpace;
        }
        if (!look(*l, back, out, error, depth)) return false;
    }
    return true;
}

bool OcioConfig::Data::view(const std::string& from, const std::string& displayName, const std::string& viewName,
                            const std::string& looks, bool inverse, OcioProcessor& out, std::string& error,
                            int depth) const {
    const Display* disp = display(displayName);
    if (!disp) {
        error = "no display " + displayName;
        return false;
    }
    const View* v = nullptr;
    for (const auto& x : disp->views) {
        if (same(x.name, viewName)) v = &x;
    }
    if (!v) {
        std::string names;
        for (const auto& x : disp->views) names += (names.empty() ? "" : ", ") + x.name;
        error = "no view " + viewName + " of " + disp->name + " (" + names + ")";
        return false;
    }
    const Space* src = space(from);
    if (!src) {
        error = "no colour space " + from;
        return false;
    }
    // Light of a data space -- Raw, Non-Color --, or for one, is not
    // converted: looks asked for go on it as they are, else it passes.
    const Space* shown = space(v->viewTransform.empty() ? v->colorspace : v->displayColorspace);
    const bool data = src->data || !shown || shown->data;
    // The looks asked for -- in place of the view's own, as OpenColorIO's
    // viewing pipeline and Blender take them --, each in its process space:
    // where the light is after them.
    std::vector<std::pair<const Look*, bool>> steps;
    if (!looksOf(looks.empty() ? (data ? std::string() : v->looks) : looks, steps, error)) return false;
    if (data) {
        OcioProcessor p;
        for (size_t i = 0; i < steps.size(); ++i) {
            const auto& step = inverse ? steps[steps.size() - 1 - i] : steps[i];
            if (!look(*step.first, step.second != inverse, p, error, depth)) return false;
        }
        out.append(p);
        return true;
    }
    std::vector<std::string> spaces;
    std::string at = src->name;
    for (const auto& step : steps) {
        if (!step.first->processSpace.empty()) at = step.first->processSpace;
        spaces.push_back(at);
    }
    OcioProcessor p;
    if (!inverse) {
        std::string here = src->name;
        for (size_t i = 0; i < steps.size(); ++i) {
            if (!between(here, spaces[i], p, error, depth + 1) || !look(*steps[i].first, steps[i].second, p, error, depth)) {
                return false;
            }
            here = spaces[i];
        }
    }
    // The view: a colour space; or a view transform into a display's.
    if (v->viewTransform.empty()) {
        if (!(inverse ? between(v->colorspace, at, p, error, depth + 1) : between(at, v->colorspace, p, error, depth + 1))) {
            return false;
        }
    } else {
        const ViewTransform* vt = viewTransform(v->viewTransform);
        if (!vt) {
            error = "no view transform " + v->viewTransform;
            return false;
        }
        const Space* dst = space(v->displayColorspace);
        if (!dst) {
            error = "no display colour space " + v->displayColorspace;
            return false;
        }
        const Space* here = space(at);
        if (here && !here->data && !dst->data) {
            // To the reference of the light's side, across by the default
            // view transform to the side of the view transform, through it
            // -- the scene's light to a display's, or a display's to
            // another's --, to the display's space. Back: the other way, each
            // step by what it has for that way.
            const bool sceneSide = vt->fromScene || vt->toScene;
            const bool across = sceneSide == here->display;
            if (!inverse) {
                if (!toReference(*here, p, error, depth + 1)) return false;
                if (across && !bridge(!sceneSide, p, error, depth + 1)) return false;
                const YamlNode* there = sceneSide ? vt->fromScene : vt->fromDisplay;
                const YamlNode* back = sceneSide ? vt->toScene : vt->toDisplay;
                if (there ? !transform(*there, false, p, error, depth + 1) : back && !transform(*back, true, p, error, depth + 1)) {
                    return false;
                }
                if (!fromReference(*dst, p, error, depth + 1)) return false;
            } else {
                if (!toReference(*dst, p, error, depth + 1)) return false;
                const YamlNode* there = sceneSide ? vt->toScene : vt->toDisplay;
                const YamlNode* back = sceneSide ? vt->fromScene : vt->fromDisplay;
                if (there ? !transform(*there, false, p, error, depth + 1) : back && !transform(*back, true, p, error, depth + 1)) {
                    return false;
                }
                if (across && !bridge(sceneSide, p, error, depth + 1)) return false;
                if (!fromReference(*here, p, error, depth + 1)) return false;
            }
        }
    }
    if (inverse) {
        for (size_t i = steps.size(); i-- > 0;) {
            if (!look(*steps[i].first, !steps[i].second, p, error, depth)) return false;
            if (!between(spaces[i], i > 0 ? spaces[i - 1] : src->name, p, error, depth + 1)) return false;
        }
    }
    out.append(p);
    return true;
}

OcioConfig::OcioConfig() : d_(std::make_unique<Data>()) {}
OcioConfig::~OcioConfig() = default;

std::shared_ptr<const OcioConfig> OcioConfig::load(const std::string& path, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = path + ": cannot read it";
        return nullptr;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    auto config = parse(ss.str(), std::filesystem::path(path).parent_path().string(), error);
    if (!config) error = path + ": " + error;
    return config;
}

std::shared_ptr<const OcioConfig> OcioConfig::parse(std::string_view text, const std::string& folder, std::string& error) {
    auto config = std::make_shared<OcioConfig>();
    Data& d = *config->d_;
    if (!io::parseYaml(text, d.root, error)) return nullptr;
    if (!d.root.isMapping() || !d.root.find("ocio_profile_version")) {
        error = "not an OpenColorIO config: no ocio_profile_version";
        return nullptr;
    }
    d.version = std::atoi(d.root.text("ocio_profile_version", "1").c_str());
    d.folder = folder.empty() ? std::string(".") : folder;
    if (const YamlNode* env = d.root.find("environment"); env && env->isMapping()) {
        for (const auto& [k, v] : env->pairs) d.environment[k] = v.isScalar() ? v.scalar : std::string();
    }
    if (const YamlNode* sp = d.root.find("search_path")) {
        if (sp->isScalar()) {
            std::string part;
            for (const char c : sp->scalar + ":") {
                if (c == ':') {
                    if (!part.empty()) d.searchPaths.push_back(part);
                    part.clear();
                } else {
                    part += c;
                }
            }
        } else if (sp->isSequence()) {
            for (const YamlNode& i : sp->items) {
                if (i.isScalar()) d.searchPaths.push_back(i.scalar);
            }
        }
    }
    if (const YamlNode* r = d.root.find("roles"); r && r->isMapping()) {
        for (const auto& [k, v] : r->pairs) {
            if (v.isScalar()) d.roles[lower(k)] = v.scalar;
        }
    }
    const auto spaces = [&](const char* key, bool displayRef) {
        const YamlNode* list = d.root.find(key);
        if (!list || !list->isSequence()) return;
        for (const YamlNode& n : list->items) {
            if (!n.isMapping()) continue;
            Data::Space s;
            s.name = n.text("name");
            s.display = displayRef;
            if (const YamlNode* a = n.find("aliases"); a && a->isSequence()) {
                for (const YamlNode& i : a->items) {
                    if (i.isScalar()) s.aliases.push_back(i.scalar);
                }
            }
            s.data = lower(n.text("isdata", "false")) == "true";
            const auto get = [&](const char* k) -> const YamlNode* {
                const YamlNode* t = n.find(k);
                return t && !t->isNull() ? t : nullptr;
            };
            if (displayRef) {
                s.toRef = get("to_display_reference");
                s.fromRef = get("from_display_reference");
            } else {
                s.toRef = get("to_scene_reference");
                s.fromRef = get("from_scene_reference");
                if (!s.toRef) s.toRef = get("to_reference");
                if (!s.fromRef) s.fromRef = get("from_reference");
            }
            if (!s.name.empty()) d.spaces.push_back(std::move(s));
        }
    };
    spaces("colorspaces", false);
    spaces("display_colorspaces", true);
    if (const YamlNode* vts = d.root.find("view_transforms"); vts && vts->isSequence()) {
        for (const YamlNode& n : vts->items) {
            Data::ViewTransform v;
            v.name = n.text("name");
            const auto get = [&](const char* k) -> const YamlNode* {
                const YamlNode* t = n.find(k);
                return t && !t->isNull() ? t : nullptr;
            };
            v.fromScene = get("from_scene_reference");
            v.toScene = get("to_scene_reference");
            v.fromDisplay = get("from_display_reference");
            v.toDisplay = get("to_display_reference");
            if (!v.fromScene && !v.fromDisplay) v.fromScene = get("from_reference");
            if (!v.toScene && !v.toDisplay) v.toScene = get("to_reference");
            d.viewTransforms.push_back(v);
        }
    }
    d.defaultViewTransform = d.root.text("default_view_transform");
    // Views shared between displays, and each display's.
    std::map<std::string, Data::View> shared;
    const auto viewOf = [](const YamlNode& n) {
        Data::View v;
        v.name = n.text("name");
        v.colorspace = n.text("colorspace");
        v.viewTransform = n.text("view_transform");
        v.displayColorspace = n.text("display_colorspace");
        v.looks = n.text("looks", n.text("look"));
        return v;
    };
    if (const YamlNode* sv = d.root.find("shared_views"); sv && sv->isSequence()) {
        for (const YamlNode& n : sv->items) {
            if (n.isMapping()) shared[lower(n.text("name"))] = viewOf(n);
        }
    }
    if (const YamlNode* ds = d.root.find("displays"); ds && ds->isMapping()) {
        for (const auto& [name, list] : ds->pairs) {
            Data::Display disp;
            disp.name = name;
            if (list.isSequence()) {
                for (const YamlNode& n : list.items) {
                    if (n.isMapping()) {
                        disp.views.push_back(viewOf(n));
                    } else if (n.isSequence()) {
                        // !<Views> [names]: shared views, the display's
                        // colour space named after it where they say so.
                        for (const YamlNode& s : n.items) {
                            const auto it = shared.find(lower(s.scalar));
                            if (it == shared.end()) continue;
                            Data::View v = it->second;
                            if (v.displayColorspace == "<USE_DISPLAY_NAME>") v.displayColorspace = name;
                            disp.views.push_back(v);
                        }
                    }
                }
            }
            d.displays.push_back(std::move(disp));
        }
    }
    const auto list = [&](const char* key, std::vector<std::string>& out) {
        const YamlNode* n = d.root.find(key);
        if (n && n->isSequence()) {
            for (const YamlNode& i : n->items) {
                if (i.isScalar() && !i.scalar.empty()) out.push_back(i.scalar);
            }
        } else if (n && n->isScalar()) {
            std::string part;
            for (const char c : n->scalar + ",") {
                if (c == ',') {
                    const std::string_view t = part;
                    size_t a = 0, b = t.size();
                    while (a < b && t[a] == ' ') ++a;
                    while (b > a && t[b - 1] == ' ') --b;
                    if (b > a) out.emplace_back(t.substr(a, b - a));
                    part.clear();
                } else {
                    part += c;
                }
            }
        }
    };
    list("active_displays", d.activeDisplays);
    list("active_views", d.activeViews);
    list("inactive_colorspaces", d.inactiveSpaces);
    if (const YamlNode* ls = d.root.find("looks"); ls && ls->isSequence()) {
        for (const YamlNode& n : ls->items) {
            Data::Look l;
            l.name = n.text("name");
            l.processSpace = n.text("process_space");
            const YamlNode* t = n.find("transform");
            const YamlNode* i = n.find("inverse_transform");
            l.transform = t && !t->isNull() ? t : nullptr;
            l.inverse = i && !i->isNull() ? i : nullptr;
            d.looks.push_back(l);
        }
    }
    if (d.spaces.empty()) {
        error = "a config with no colour spaces";
        return nullptr;
    }
    return config;
}

std::vector<std::string> OcioConfig::colorSpaces() const {
    std::vector<std::string> out;
    for (const auto& s : d_->spaces) {
        bool inactive = false;
        for (const std::string& n : d_->inactiveSpaces) inactive = inactive || same(n, s.name);
        if (!inactive) out.push_back(s.name);
    }
    return out;
}

std::vector<std::string> OcioConfig::displays() const {
    std::vector<std::string> out;
    for (const std::string& a : d_->activeDisplays) {
        if (const auto* disp = d_->display(a)) out.push_back(disp->name);
    }
    if (out.empty()) {
        for (const auto& disp : d_->displays) out.push_back(disp.name);
    }
    return out;
}

std::vector<std::string> OcioConfig::views(const std::string& display) const {
    std::vector<std::string> out;
    const auto* disp = d_->display(display);
    if (!disp) return out;
    // In the order of active_views, where there is one; then the rest of the display's.
    if (!d_->activeViews.empty()) {
        for (const std::string& a : d_->activeViews) {
            for (const auto& v : disp->views) {
                if (same(v.name, a)) out.push_back(v.name);
            }
        }
        return out;
    }
    for (const auto& v : disp->views) out.push_back(v.name);
    return out;
}

std::string OcioConfig::display(const std::string& name) const {
    const auto* disp = d_->display(name);
    return disp ? disp->name : std::string();
}

std::string OcioConfig::defaultDisplay() const {
    const auto all = displays();
    return all.empty() ? std::string() : all.front();
}

std::string OcioConfig::defaultView(const std::string& display) const {
    const auto all = views(display);
    return all.empty() ? std::string() : all.front();
}

std::string OcioConfig::colorSpace(const std::string& name) const {
    const auto* s = d_->space(name);
    return s ? s->name : std::string();
}

std::string OcioConfig::role(const std::string& name) const {
    const auto it = d_->roles.find(lower(name));
    if (it == d_->roles.end()) return {};
    return colorSpace(it->second);
}

std::string OcioConfig::linearRec709() const {
    static const char* names[] = {"Linear Rec.709 (sRGB)", "lin_rec709_srgb", "lin_rec709", "lin_srgb", "Linear Rec.709",
                                  "Utility - Linear - sRGB", "Utility - Linear - Rec.709", "linear_srgb", "Linear BT.709",
                                  "lin_rec709_scene"};
    for (const char* n : names) {
        if (const auto* s = d_->space(n); s && !s->display && !s->data) return s->name;
    }
    return {};
}

bool OcioConfig::processor(const std::string& from, const std::string& to, OcioProcessor& out, std::string& error) const {
    OcioProcessor p;
    if (!d_->between(from, to, p, error, 0)) return false;
    out = std::move(p);
    return true;
}

bool OcioConfig::viewProcessor(const std::string& from, const std::string& display, const std::string& view,
                               const std::string& looks, OcioProcessor& out, std::string& error, bool inverse) const {
    OcioProcessor p;
    if (!d_->view(from, display, view, looks, inverse, p, error, 0)) return false;
    out = std::move(p);
    return true;
}

// --- a view of the render ---------------------------------------------------------------------

std::shared_ptr<const OcioView> OcioView::make(const std::string& config, const std::string& display,
                                               const std::string& view, const std::string& looks,
                                               const std::string& space, std::string& error) {
    // Made once for the same arguments and the same file, as it was then.
    static std::mutex mutex;
    static std::map<std::string, std::pair<std::shared_ptr<const OcioView>, std::string>> made;
    std::error_code ec;
    const auto stamp = std::filesystem::last_write_time(config, ec);
    const std::string key = config + "\n" + display + "\n" + view + "\n" + looks + "\n" + space + "\n" +
                            std::to_string(ec ? 0 : static_cast<long long>(stamp.time_since_epoch().count()));
    {
        std::lock_guard<std::mutex> lock(mutex);
        const auto it = made.find(key);
        if (it != made.end()) {
            error = it->second.second;
            return it->second.first;
        }
    }
    auto result = [&]() -> std::shared_ptr<const OcioView> {
        const auto cfg = OcioConfig::load(config, error);
        if (!cfg) return nullptr;
        auto v = std::make_shared<OcioView>();
        v->display_ = display.empty() ? cfg->defaultDisplay() : cfg->display(display);
        if (v->display_.empty()) {
            error = display.empty() ? "the config has no display" : "no display " + display + " in the config";
            return nullptr;
        }
        v->view_ = view.empty() ? cfg->defaultView(v->display_) : view;
        // The render's light: linear Rec. 709 -- as the config names it, or
        // through ACES2065-1, or as its scene_linear.
        OcioProcessor into, outOf;
        std::string from;
        if (!space.empty()) {
            from = cfg->colorSpace(space);
            if (from.empty()) {
                error = "no colour space " + space + " in the config";
                return nullptr;
            }
        } else if (!(from = cfg->linearRec709()).empty()) {
        } else if (!(from = cfg->role("aces_interchange")).empty()) {
            const color::D33 m = color::conversion(color::kRec709, color::kAP0, true);
            into.add(matrixOp(m));
            outOf.add(matrixOp(color::inverse(m)));
        } else if (!(from = cfg->role("scene_linear")).empty()) {
        } else {
            error = "the config says not which colour space the render's light is in: give its Colour Space";
            return nullptr;
        }
        v->space_ = from;
        OcioProcessor through;
        if (!cfg->viewProcessor(from, v->display_, v->view_, looks, through, error)) return nullptr;
        v->forward_ = into;
        v->forward_.append(through);
        // Back as the config takes it back -- else, as far as it goes, not.
        OcioProcessor back;
        std::string why;
        v->invertible_ = cfg->viewProcessor(from, v->display_, v->view_, looks, back, why, true);
        if (v->invertible_) {
            v->back_ = back;
            v->back_.append(outOf);
        }
        return v;
    }();
    std::lock_guard<std::mutex> lock(mutex);
    if (made.size() > 64) made.clear();
    made[key] = {result, result ? std::string() : error};
    return result;
}

Vec3 OcioView::shown(const Vec3& linear) const { return forward_.apply(linear); }

namespace {

bool finite(const Vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
float largest(const Vec3& v) { return std::max(std::abs(v.x), std::max(std::abs(v.y), std::abs(v.z))); }

/// Newton's method on light against `forward` for `target`, from `light`:
/// on the light itself, or on its log2 (none below 0 then -- what views of
/// logs take best). Each step whole, or half or a quarter of it, the first
/// that brings what it shows nearer; none does: there it stops.
Vec3 newton(const OcioProcessor& forward, Vec3 light, const Vec3& target, bool logs, int steps) {
    const auto toLight = [&](const Vec3& y) { return logs ? Vec3(std::exp2(y.x), std::exp2(y.y), std::exp2(y.z)) : y; };
    const auto miss = [&](const Vec3& y) { return forward.apply(toLight(y)) - target; };
    Vec3 y = logs ? Vec3(std::log2(std::max(light.x, 1e-12f)), std::log2(std::max(light.y, 1e-12f)),
                         std::log2(std::max(light.z, 1e-12f)))
                  : light;
    Vec3 off = miss(y);
    for (int step = 0; step < steps && finite(off) && largest(off) > 1e-6f; ++step) {
        color::D33 jacobian{};
        for (int c = 0; c < 3; ++c) {
            Vec3 moved = y;
            const float h = logs ? 1e-3f : 1e-3f * std::max(std::abs(y[c]), 1e-3f);
            moved[c] += h;
            const Vec3 m = miss(moved);
            for (int r = 0; r < 3; ++r) jacobian[static_cast<size_t>(3 * r + c)] = (m[r] - off[r]) / h;
        }
        const double det = jacobian[0] * (jacobian[4] * jacobian[8] - jacobian[5] * jacobian[7]) -
                           jacobian[1] * (jacobian[3] * jacobian[8] - jacobian[5] * jacobian[6]) +
                           jacobian[2] * (jacobian[3] * jacobian[7] - jacobian[4] * jacobian[6]);
        if (!(std::abs(det) > 1e-12)) break;
        const std::array<double, 3> d = color::times(color::inverse(jacobian), std::array<double, 3>{off.x, off.y, off.z});
        bool nearer = false;
        for (float part = 1.0f; part > 0.2f && !nearer; part *= 0.5f) {
            const Vec3 next = y - part * Vec3(static_cast<float>(d[0]), static_cast<float>(d[1]), static_cast<float>(d[2]));
            const Vec3 nextOff = miss(next);
            if (finite(next) && finite(nextOff) && largest(nextOff) < largest(off)) {
                y = next;
                off = nextOff;
                nearer = true;
            }
        }
        if (!nearer) break;
    }
    return toLight(y);
}

}  // namespace

Vec3 OcioView::unshown(const Vec3& display) const {
    const Vec3 guess = invertible_ ? back_.apply(display)
                                   : Vec3(srgbDecoded(display.x), srgbDecoded(display.y), srgbDecoded(display.z));
    if (!finite(guess)) return Vec3(0.0f);
    // Made exact by Newton's method on the light against the view, so that
    // a plate shows as itself: a step's inverse may be only near (ACES 1.0's
    // red modifier, a table's). Where that does not get there -- a view
    // that squeezes the brightest colours together, its inverse far off --
    // on the light's logarithm, then from a grey as bright: the nearest.
    const auto miss = [&](const Vec3& l) { return largest(forward_.apply(l) - display); };
    Vec3 light = newton(forward_, guess, display, false, 8);
    if (miss(light) > 1e-4f) {
        const Vec3 other = newton(forward_, glm::max(guess, Vec3(1e-6f)), display, true, 8);
        if (miss(other) < miss(light)) light = other;
    }
    if (miss(light) > 1e-4f) {
        const float grey = std::max((std::max(guess.x, 0.0f) + std::max(guess.y, 0.0f) + std::max(guess.z, 0.0f)) / 3.0f, 1e-3f);
        const Vec3 other = newton(forward_, Vec3(grey), display, true, 12);
        if (miss(other) < miss(light)) light = other;
    }
    return light;
}

}  // namespace pg::render
