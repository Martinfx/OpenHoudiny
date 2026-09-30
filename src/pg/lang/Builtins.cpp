// The builtins that do not touch geometry: math, noise and random numbers,
// strings, arrays, matrices and quaternions. The geometry ones are in
// BuiltinsGeo.cpp.
#include "pg/lang/Builtins.h"
#include "pg/lang/Math.h"

#include <glm/gtc/quaternion.hpp>
#include <glm/matrix.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <numeric>

namespace pg::lang {

// --- shared helpers --------------------------------------------------------------------------------

uint32_t hashBits(float x) {
    if (x == 0.0f) x = 0.0f;  // -0 is 0
    uint32_t h;
    std::memcpy(&h, &x, sizeof h);
    // A strong 32-bit mix (lowbias32).
    h ^= h >> 16;
    h *= 0x7feb352dU;
    h ^= h >> 15;
    h *= 0x846ca68bU;
    h ^= h >> 16;
    return h;
}

float hashFloat(uint32_t h) {
    h ^= h >> 16;
    h *= 0x7feb352dU;
    h ^= h >> 15;
    h *= 0x846ca68bU;
    h ^= h >> 16;
    return static_cast<float>(h >> 8) * (1.0f / 16777216.0f);  // [0, 1)
}

namespace {

inline float hashLattice(int32_t x, int32_t y, int32_t z) {
    uint32_t h = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(y) * 668265263u +
                 static_cast<uint32_t>(z) * 1274126177u;
    h ^= h >> 13;
    h *= 1274126177u;
    h ^= h >> 16;
    return static_cast<float>(h) * (1.0f / 4294967296.0f);
}

}  // namespace

float valueNoise(const Vec3& p) {
    const float fx = std::floor(p.x), fy = std::floor(p.y), fz = std::floor(p.z);
    const auto ix = static_cast<int32_t>(fx), iy = static_cast<int32_t>(fy), iz = static_cast<int32_t>(fz);
    const float tx = p.x - fx, ty = p.y - fy, tz = p.z - fz;
    const float sx = tx * tx * (3.0f - 2.0f * tx), sy = ty * ty * (3.0f - 2.0f * ty), sz = tz * tz * (3.0f - 2.0f * tz);
    auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
    const float c000 = hashLattice(ix, iy, iz), c100 = hashLattice(ix + 1, iy, iz);
    const float c010 = hashLattice(ix, iy + 1, iz), c110 = hashLattice(ix + 1, iy + 1, iz);
    const float c001 = hashLattice(ix, iy, iz + 1), c101 = hashLattice(ix + 1, iy, iz + 1);
    const float c011 = hashLattice(ix, iy + 1, iz + 1), c111 = hashLattice(ix + 1, iy + 1, iz + 1);
    return lerp(lerp(lerp(c000, c100, sx), lerp(c010, c110, sx), sy), lerp(lerp(c001, c101, sx), lerp(c011, c111, sx), sy), sz);
}

// Quaternions and matrices are GLM's. VEX's matrices are rows of the images
// of the axes, v * M; GLM's the same numbers as columns, M * v (Ast.h).

Vec4 quatFromAxisAngle(float angle, const Vec3& axis) { return vec4Of(glm::angleAxis(angle, normalize(axis))); }

Vec4 quatMul(const Vec4& a, const Vec4& b) { return vec4Of(quatOf(a) * quatOf(b)); }

Vec3 quatRotate(const Vec4& q, const Vec3& v) { return quatOf(q) * v; }

Mat3 quatToMat3(const Vec4& q0) {
    const Quat q = quatOf(q0);
    const float len = glm::length(q);
    return glm::mat3_cast(len > 0.0f ? q / len : Quat::wxyz(1.0f, 0.0f, 0.0f, 0.0f));
}

Vec4 mat3ToQuat(const Mat3& r) { return vec4Of(glm::quat_cast(glm::mat3(r))); }

Mat3 mul(const Mat3& a, const Mat3& b) { return b * a; }

Vec3 mul(const Vec3& v, const Mat3& m) { return m * v; }

Mat3 transpose(const Mat3& m) { return glm::transpose(glm::mat3(m)); }

float determinant(const Mat3& m) { return glm::determinant(glm::mat3(m)); }

Mat3 inverse(const Mat3& m) {
    // A singular matrix gives zeros.
    if (determinant(m) == 0.0f) return Mat3();
    return glm::inverse(glm::mat3(m));
}

Mat4 transpose(const Mat4& m) { return glm::transpose(glm::mat4(m)); }

float determinant(const Mat4& m) { return glm::determinant(glm::mat4(m)); }

Mat4 inverse(const Mat4& m) {
    if (determinant(m) == 0.0f) return Mat4();
    return glm::inverse(glm::mat4(m));
}

Mat3 upper(const Mat4& m) { return glm::mat3(m); }

Mat4 widen(const Mat3& m) { return glm::mat4(glm::mat3(m)); }

// --- formatting ------------------------------------------------------------------------------------

std::string format(const std::string& fmt, const std::vector<FormatArg>& args) {
    std::string out;
    size_t next = 0;
    for (size_t i = 0; i < fmt.size(); ++i) {
        if (fmt[i] != '%') {
            out.push_back(fmt[i]);
            continue;
        }
        if (i + 1 < fmt.size() && fmt[i + 1] == '%') {
            out.push_back('%');
            ++i;
            continue;
        }
        // %[flags][width][.precision]conversion
        size_t j = i + 1;
        while (j < fmt.size() && std::strchr("-+ 0#", fmt[j])) ++j;
        while (j < fmt.size() && std::isdigit(static_cast<unsigned char>(fmt[j]))) ++j;
        if (j < fmt.size() && fmt[j] == '.') {
            ++j;
            while (j < fmt.size() && std::isdigit(static_cast<unsigned char>(fmt[j]))) ++j;
        }
        while (j < fmt.size() && std::strchr("lhqL", fmt[j])) ++j;  // C's length modifiers, ignored
        if (j >= fmt.size()) {
            out += fmt.substr(i);
            break;
        }
        const char conv = fmt[j];
        std::string spec = fmt.substr(i, j - i);
        spec.erase(std::remove_if(spec.begin(), spec.end(), [](char c) { return std::strchr("lhqL", c) != nullptr; }),
                   spec.end());
        i = j;
        if (next >= args.size()) {
            out += "<missing>";
            continue;
        }
        const FormatArg& a = args[next++];
        char buf[256];
        auto number = [&](double v) {
            switch (conv) {
                case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': case 'c': {
                    const std::string s = spec + (conv == 'u' ? 'd' : conv);
                    std::snprintf(buf, sizeof buf, s.c_str(), static_cast<int>(toInt(v)));
                    return std::string(buf);
                }
                case 's': {
                    std::snprintf(buf, sizeof buf, "%g", v);
                    const std::string s = spec + 's';
                    char b2[256];
                    std::snprintf(b2, sizeof b2, s.c_str(), buf);
                    return std::string(b2);
                }
                default: {
                    const std::string s = spec + (std::strchr("fFeEgGaA", conv) ? conv : 'g');
                    std::snprintf(buf, sizeof buf, s.c_str(), v);
                    return std::string(buf);
                }
            }
        };
        if (a.type == Type::String || !a.text.empty()) {
            if (conv == 's' || !isNumeric(a.type)) {
                const std::string s = spec + 's';
                std::snprintf(buf, sizeof buf, s.c_str(), a.text.c_str());
                out += buf;
                continue;
            }
        }
        if (isVector(a.type)) {
            out += "{";
            for (int c = 0; c < width(a.type); ++c) out += (c ? ", " : "") + number(comp(a.v, c));
            out += "}";
        } else {
            out += number(a.number);
        }
    }
    return out;
}

// --- registration ----------------------------------------------------------------------------------

namespace {

template <class T> T arg(Env& e, const TNode& c, size_t i) { return ev<T>(*c.kids[i], e); }
template <class T> void put(void* out, T v) { *static_cast<T*>(out) = std::move(v); }

using F = float;

// Componentwise functions over float and the vectors.
template <float (*Fn)(float)> void registerUnary(Builtins& b, const char* name) {
    add(b, name, Type::Float, {Type::Float}, [](Env& e, const TNode& c, void* o) { put(o, Fn(arg<F>(e, c, 0))); });
    add(b, name, Type::Vec2, {Type::Vec2}, [](Env& e, const TNode& c, void* o) { put(o, mapv(arg<Vec2>(e, c, 0), Fn)); });
    add(b, name, Type::Vec3, {Type::Vec3}, [](Env& e, const TNode& c, void* o) { put(o, mapv(arg<Vec3>(e, c, 0), Fn)); });
    add(b, name, Type::Vec4, {Type::Vec4}, [](Env& e, const TNode& c, void* o) { put(o, mapv(arg<Vec4>(e, c, 0), Fn)); });
}

template <float (*Fn)(float, float)> void registerBinary(Builtins& b, const char* name) {
    add(b, name, Type::Float, {Type::Float, Type::Float},
        [](Env& e, const TNode& c, void* o) { put(o, Fn(arg<F>(e, c, 0), arg<F>(e, c, 1))); });
    add(b, name, Type::Vec2, {Type::Vec2, Type::Vec2},
        [](Env& e, const TNode& c, void* o) { put(o, zipv(arg<Vec2>(e, c, 0), arg<Vec2>(e, c, 1), Fn)); });
    add(b, name, Type::Vec3, {Type::Vec3, Type::Vec3},
        [](Env& e, const TNode& c, void* o) { put(o, zipv(arg<Vec3>(e, c, 0), arg<Vec3>(e, c, 1), Fn)); });
    add(b, name, Type::Vec4, {Type::Vec4, Type::Vec4},
        [](Env& e, const TNode& c, void* o) { put(o, zipv(arg<Vec4>(e, c, 0), arg<Vec4>(e, c, 1), Fn)); });
}

float fsin(float x) { return std::sin(x); }
float fcos(float x) { return std::cos(x); }
float ftan(float x) { return std::tan(x); }
float fasin(float x) { return std::asin(std::clamp(x, -1.0f, 1.0f)); }
float facos(float x) { return std::acos(std::clamp(x, -1.0f, 1.0f)); }
float fatan(float x) { return std::atan(x); }
float fsinh(float x) { return std::sinh(x); }
float fcosh(float x) { return std::cosh(x); }
float ftanh(float x) { return std::tanh(x); }
float fexp(float x) { return std::exp(x); }
float flog(float x) { return x > 0.0f ? std::log(x) : 0.0f; }
float flog10(float x) { return x > 0.0f ? std::log10(x) : 0.0f; }
float fsqrt(float x) { return x > 0.0f ? std::sqrt(x) : 0.0f; }
float fabs_(float x) { return std::fabs(x); }
float ffloor(float x) { return std::floor(x); }
float fceil(float x) { return std::ceil(x); }
float fround(float x) { return std::round(x); }
float frint(float x) { return std::nearbyint(x); }
float ffrac(float x) { return x - std::floor(x); }
float ftrunc(float x) { return std::trunc(x); }
float fsign(float x) { return x > 0.0f ? 1.0f : x < 0.0f ? -1.0f : 0.0f; }
float fradians(float x) { return x * 0.017453292519943295f; }
float fdegrees(float x) { return x * 57.29577951308232f; }
float fpow(float x, float y) {
    const float r = std::pow(x, y);
    return std::isfinite(r) ? r : 0.0f;
}
float fatan2(float y, float x) { return std::atan2(y, x); }
float fmin_(float x, float y) { return x < y ? x : y; }
float fmax_(float x, float y) { return x > y ? x : y; }
float fmodf_(float x, float y) { return safeFmod(x, y); }
float fstep(float edge, float x) { return x < edge ? 0.0f : 1.0f; }

float fitf(float x, float omin, float omax, float nmin, float nmax, bool clampIt) {
    const float d = omax - omin;
    float t = d == 0.0f ? 0.0f : (x - omin) / d;
    if (clampIt) t = std::clamp(t, 0.0f, 1.0f);
    return nmin + (nmax - nmin) * t;
}

float smoothf(float lo, float hi, float x) {
    if (hi == lo) return x < lo ? 0.0f : 1.0f;
    const float t = std::clamp((x - lo) / (hi - lo), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

template <class V> float lengthOf(const V& v) {
    float s = 0.0f;
    for (int i = 0; i < widthOf<V>(); ++i) s += comp(v, i) * comp(v, i);
    return std::sqrt(s);
}
template <class V> float dotOf(const V& a, const V& b) {
    float s = 0.0f;
    for (int i = 0; i < widthOf<V>(); ++i) s += comp(a, i) * comp(b, i);
    return s;
}
template <class V> V normalizeOf(const V& v) {
    const float l = lengthOf(v);
    return l > 0.0f ? mapv(v, [l](float x) { return x / l; }) : v;
}

float randOf(float seed) { return hashFloat(hashBits(seed)); }
template <class V> float randVec(const V& v) {
    uint32_t h = 0x9e3779b9u;
    for (int i = 0; i < widthOf<V>(); ++i) h = hashBits(comp(v, i) + static_cast<float>(h & 0xffff) * 1e-7f) ^ (h * 0x85ebca6bu);
    return hashFloat(h);
}
template <class V> V randSpread(float seed) {
    V r{};
    for (int i = 0; i < widthOf<V>(); ++i) compRef(r, i) = hashFloat(hashBits(seed) + 0x68e31da4u * static_cast<uint32_t>(i + 1));
    return r;
}

Vec3 noiseVec(const Vec3& p) {
    return Vec3(valueNoise(p), valueNoise(p + Vec3(31.416f, 47.853f, 12.793f)), valueNoise(p + Vec3(-71.19f, 5.37f, 93.11f)));
}

Vec3 curl(const Vec3& p) {
    const float h = 1e-3f;
    auto d = [&](int axis, int comp) {
        Vec3 o;
        o[axis] = h;
        return (noiseVec(p + o)[comp] - noiseVec(p - o)[comp]) / (2.0f * h);
    };
    return Vec3(d(1, 2) - d(2, 1), d(2, 0) - d(0, 2), d(0, 1) - d(1, 0));
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::string upperStr(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

/// Glob matching: * any run, ? any one character.
bool globMatch(const char* p, const char* s) {
    while (*p) {
        if (*p == '*') {
            ++p;
            for (;;) {
                if (globMatch(p, s)) return true;
                if (!*s) return false;
                ++s;
            }
        }
        if (!*s || (*p != '?' && *p != *s)) return false;
        ++p;
        ++s;
    }
    return !*s;
}

// --- arrays, for every element type ------------------------------------------------------------------

template <class E> void registerArray(Builtins& b) {
    using A = std::vector<E>;
    const Type AT = TypeOf<A>::value;
    const Type ET = TypeOf<E>::value;
    add(b, "len", Type::Int, {AT}, [](Env& e, const TNode& c, void* o) {
        A s;
        put(o, static_cast<int32_t>(evRef<A>(*c.kids[0], e, s).size()));
    });
    add(b, "append", Type::Void, {AT, ET}, [](Env& e, const TNode& c, void*) {
        E v = arg<E>(e, c, 1);
        refOf<A>(*c.lv, e).push_back(std::move(v));
    }, 1);
    add(b, "append", Type::Void, {AT, AT}, [](Env& e, const TNode& c, void*) {
        A more = arg<A>(e, c, 1);
        A& a = refOf<A>(*c.lv, e);
        a.insert(a.end(), more.begin(), more.end());
    }, 1);
    add(b, "push", Type::Void, {AT, ET}, [](Env& e, const TNode& c, void*) {
        E v = arg<E>(e, c, 1);
        refOf<A>(*c.lv, e).push_back(std::move(v));
    }, 1);
    add(b, "pop", ET, {AT}, [](Env& e, const TNode& c, void* o) {
        A& a = refOf<A>(*c.lv, e);
        if (a.empty()) {
            put(o, E{});
            return;
        }
        put(o, a.back());
        a.pop_back();
    }, 1);
    add(b, "insert", Type::Void, {AT, Type::Int, ET}, [](Env& e, const TNode& c, void*) {
        const int32_t i = arg<int32_t>(e, c, 1);
        E v = arg<E>(e, c, 2);
        A& a = refOf<A>(*c.lv, e);
        const size_t k = static_cast<size_t>(std::clamp<int64_t>(i, 0, static_cast<int64_t>(a.size())));
        a.insert(a.begin() + static_cast<std::ptrdiff_t>(k), std::move(v));
    }, 1);
    add(b, "removeindex", ET, {AT, Type::Int}, [](Env& e, const TNode& c, void* o) {
        int64_t i = arg<int32_t>(e, c, 1);
        A& a = refOf<A>(*c.lv, e);
        if (i < 0) i += static_cast<int64_t>(a.size());
        if (i < 0 || i >= static_cast<int64_t>(a.size())) {
            put(o, E{});
            return;
        }
        put(o, a[static_cast<size_t>(i)]);
        a.erase(a.begin() + i);
    }, 1);
    add(b, "removevalue", Type::Int, {AT, ET}, [](Env& e, const TNode& c, void* o) {
        const E v = arg<E>(e, c, 1);
        A& a = refOf<A>(*c.lv, e);
        for (size_t i = 0; i < a.size(); ++i) {
            if (opEq(a[i], v)) {
                a.erase(a.begin() + static_cast<std::ptrdiff_t>(i));
                put(o, int32_t(1));
                return;
            }
        }
        put(o, int32_t(0));
    }, 1);
    add(b, "resize", Type::Void, {AT, Type::Int}, [](Env& e, const TNode& c, void*) {
        const int32_t n = arg<int32_t>(e, c, 1);
        refOf<A>(*c.lv, e).resize(static_cast<size_t>(std::clamp(n, 0, 1 << 26)));
    }, 1);
    add(b, "find", Type::Int, {AT, ET}, [](Env& e, const TNode& c, void* o) {
        A s;
        const A& a = evRef<A>(*c.kids[0], e, s);
        const E v = arg<E>(e, c, 1);
        for (size_t i = 0; i < a.size(); ++i) {
            if (opEq(a[i], v)) {
                put(o, static_cast<int32_t>(i));
                return;
            }
        }
        put(o, int32_t(-1));
    });
    add(b, "reverse", AT, {AT}, [](Env& e, const TNode& c, void* o) {
        A a = arg<A>(e, c, 0);
        std::reverse(a.begin(), a.end());
        put(o, std::move(a));
    });
    add(b, "isvalidindex", Type::Int, {AT, Type::Int}, [](Env& e, const TNode& c, void* o) {
        A s;
        const A& a = evRef<A>(*c.kids[0], e, s);
        const int32_t i = arg<int32_t>(e, c, 1);
        put(o, int32_t(i >= 0 && static_cast<size_t>(i) < a.size() ? 1 : 0));
    });
    add(b, "slice", AT, {AT, Type::Int, Type::Int}, [](Env& e, const TNode& c, void* o) {
        A s;
        const A& a = evRef<A>(*c.kids[0], e, s);
        auto clampIndex = [&](int64_t i) {
            if (i < 0) i += static_cast<int64_t>(a.size());
            return static_cast<size_t>(std::clamp<int64_t>(i, 0, static_cast<int64_t>(a.size())));
        };
        const size_t from = clampIndex(arg<int32_t>(e, c, 1)), to = clampIndex(arg<int32_t>(e, c, 2));
        put(o, from < to ? A(a.begin() + static_cast<std::ptrdiff_t>(from), a.begin() + static_cast<std::ptrdiff_t>(to)) : A());
    });
    if constexpr (std::is_same_v<E, int32_t> || std::is_same_v<E, float> || std::is_same_v<E, std::string>) {
        add(b, "sort", AT, {AT}, [](Env& e, const TNode& c, void* o) {
            A a = arg<A>(e, c, 0);
            std::stable_sort(a.begin(), a.end());
            put(o, std::move(a));
        });
        add(b, "argsort", Type::IntArray, {AT}, [](Env& e, const TNode& c, void* o) {
            A s;
            const A& a = evRef<A>(*c.kids[0], e, s);
            IntArr idx(a.size());
            std::iota(idx.begin(), idx.end(), 0);
            std::stable_sort(idx.begin(), idx.end(), [&](int32_t x, int32_t y) { return a[static_cast<size_t>(x)] < a[static_cast<size_t>(y)]; });
            put(o, std::move(idx));
        });
    }
    if constexpr (std::is_same_v<E, int32_t> || std::is_same_v<E, float>) {
        add(b, "max", ET, {AT}, [](Env& e, const TNode& c, void* o) {
            A s;
            const A& a = evRef<A>(*c.kids[0], e, s);
            put(o, a.empty() ? E{} : *std::max_element(a.begin(), a.end()));
        });
        add(b, "min", ET, {AT}, [](Env& e, const TNode& c, void* o) {
            A s;
            const A& a = evRef<A>(*c.kids[0], e, s);
            put(o, a.empty() ? E{} : *std::min_element(a.begin(), a.end()));
        });
    }
    if constexpr (std::is_same_v<E, int32_t> || std::is_same_v<E, float> || std::is_same_v<E, Vec2> ||
                  std::is_same_v<E, Vec3> || std::is_same_v<E, Vec4>) {
        add(b, "sum", ET, {AT}, [](Env& e, const TNode& c, void* o) {
            A s;
            E total{};
            for (const E& v : evRef<A>(*c.kids[0], e, s)) total = opAdd(total, v);
            put(o, total);
        });
    }
    if constexpr (std::is_same_v<E, float> || std::is_same_v<E, Vec2> || std::is_same_v<E, Vec3> || std::is_same_v<E, Vec4>) {
        add(b, "avg", ET, {AT}, [](Env& e, const TNode& c, void* o) {
            A s;
            const A& a = evRef<A>(*c.kids[0], e, s);
            E total{};
            for (const E& v : a) total = opAdd(total, v);
            if constexpr (std::is_same_v<E, float>) put(o, a.empty() ? 0.0f : total / static_cast<float>(a.size()));
            else put(o, a.empty() ? E{} : mapv(total, [n = static_cast<float>(a.size())](float x) { return x / n; }));
        });
    }
}

}  // namespace

void add(Builtins& b, const char* name, Type ret, std::vector<Type> params, Impl fn, int refs, bool sideEffect, bool variadic) {
    Overload o;
    o.ret = ret;
    o.params = std::move(params);
    o.fn = fn;
    o.refs = refs;
    o.sideEffect = sideEffect;
    o.variadic = variadic;
    b[name].push_back(std::move(o));
}

void addMathBuiltins(Builtins& b) {
    registerUnary<fsin>(b, "sin");
    registerUnary<fcos>(b, "cos");
    registerUnary<ftan>(b, "tan");
    registerUnary<fasin>(b, "asin");
    registerUnary<facos>(b, "acos");
    registerUnary<fatan>(b, "atan");
    registerUnary<fsinh>(b, "sinh");
    registerUnary<fcosh>(b, "cosh");
    registerUnary<ftanh>(b, "tanh");
    registerUnary<fexp>(b, "exp");
    registerUnary<flog>(b, "log");
    registerUnary<flog10>(b, "log10");
    registerUnary<fsqrt>(b, "sqrt");
    registerUnary<fabs_>(b, "abs");
    registerUnary<ffloor>(b, "floor");
    registerUnary<fceil>(b, "ceil");
    registerUnary<fround>(b, "round");
    registerUnary<frint>(b, "rint");
    registerUnary<ffrac>(b, "frac");
    registerUnary<ftrunc>(b, "trunc");
    registerUnary<fsign>(b, "sign");
    registerUnary<fradians>(b, "radians");
    registerUnary<fdegrees>(b, "degrees");
    registerBinary<fpow>(b, "pow");
    registerBinary<fatan2>(b, "atan2");
    registerBinary<fmin_>(b, "min");
    registerBinary<fmax_>(b, "max");
    registerBinary<fmodf_>(b, "fmod");
    registerBinary<fstep>(b, "step");
    add(b, "abs", Type::Int, {Type::Int}, [](Env& e, const TNode& c, void* o) {
        const int32_t v = arg<int32_t>(e, c, 0);
        put(o, v < 0 ? wrapSub(0, v) : v);
    });
    add(b, "min", Type::Int, {Type::Int, Type::Int},
        [](Env& e, const TNode& c, void* o) { put(o, std::min(arg<int32_t>(e, c, 0), arg<int32_t>(e, c, 1))); });
    add(b, "max", Type::Int, {Type::Int, Type::Int},
        [](Env& e, const TNode& c, void* o) { put(o, std::max(arg<int32_t>(e, c, 0), arg<int32_t>(e, c, 1))); });
    add(b, "min", Type::Float, {Type::Float, Type::Float, Type::Float}, [](Env& e, const TNode& c, void* o) {
        float m = arg<F>(e, c, 0);
        for (size_t i = 1; i < c.kids.size(); ++i) m = std::min(m, arg<F>(e, c, i));
        put(o, m);
    }, 0, false, true);
    add(b, "max", Type::Float, {Type::Float, Type::Float, Type::Float}, [](Env& e, const TNode& c, void* o) {
        float m = arg<F>(e, c, 0);
        for (size_t i = 1; i < c.kids.size(); ++i) m = std::max(m, arg<F>(e, c, i));
        put(o, m);
    }, 0, false, true);
    add(b, "clamp", Type::Int, {Type::Int, Type::Int, Type::Int}, [](Env& e, const TNode& c, void* o) {
        const int32_t lo = arg<int32_t>(e, c, 1), hi = arg<int32_t>(e, c, 2);
        put(o, std::max(lo, std::min(arg<int32_t>(e, c, 0), hi)));
    });
    add(b, "clamp", Type::Float, {Type::Float, Type::Float, Type::Float}, [](Env& e, const TNode& c, void* o) {
        const float lo = arg<F>(e, c, 1), hi = arg<F>(e, c, 2);
        put(o, std::max(lo, std::min(arg<F>(e, c, 0), hi)));
    });
    auto clampVec = [](auto tag, Builtins& bb, Type t) {
        using V = decltype(tag);
        add(bb, "clamp", t, {t, Type::Float, Type::Float}, [](Env& e, const TNode& c, void* o) {
            const float lo = arg<F>(e, c, 1), hi = arg<F>(e, c, 2);
            put(o, mapv(arg<V>(e, c, 0), [lo, hi](float x) { return std::max(lo, std::min(x, hi)); }));
        });
        add(bb, "clamp", t, {t, t, t}, [](Env& e, const TNode& c, void* o) {
            const V lo = arg<V>(e, c, 1), hi = arg<V>(e, c, 2), v = arg<V>(e, c, 0);
            V r;
            for (int i = 0; i < widthOf<V>(); ++i) compRef(r, i) = std::max(comp(lo, i), std::min(comp(v, i), comp(hi, i)));
            put(o, r);
        });
        add(bb, "lerp", t, {t, t, Type::Float}, [](Env& e, const TNode& c, void* o) {
            const V a = arg<V>(e, c, 0), b2 = arg<V>(e, c, 1);
            const float k = arg<F>(e, c, 2);
            put(o, zipv(a, b2, [k](float x, float y) { return x + (y - x) * k; }));
        });
        add(bb, "lerp", t, {t, t, t}, [](Env& e, const TNode& c, void* o) {
            const V a = arg<V>(e, c, 0), b2 = arg<V>(e, c, 1), k = arg<V>(e, c, 2);
            V r;
            for (int i = 0; i < widthOf<V>(); ++i) compRef(r, i) = comp(a, i) + (comp(b2, i) - comp(a, i)) * comp(k, i);
            put(o, r);
        });
        add(bb, "length", Type::Float, {t}, [](Env& e, const TNode& c, void* o) { put(o, lengthOf(arg<V>(e, c, 0))); });
        add(bb, "length2", Type::Float, {t}, [](Env& e, const TNode& c, void* o) {
            const V v = arg<V>(e, c, 0);
            put(o, dotOf(v, v));
        });
        add(bb, "normalize", t, {t}, [](Env& e, const TNode& c, void* o) { put(o, normalizeOf(arg<V>(e, c, 0))); });
        add(bb, "dot", Type::Float, {t, t}, [](Env& e, const TNode& c, void* o) { put(o, dotOf(arg<V>(e, c, 0), arg<V>(e, c, 1))); });
        add(bb, "distance", Type::Float, {t, t}, [](Env& e, const TNode& c, void* o) {
            put(o, lengthOf(opSub(arg<V>(e, c, 0), arg<V>(e, c, 1))));
        });
        add(bb, "distance2", Type::Float, {t, t}, [](Env& e, const TNode& c, void* o) {
            const V d = opSub(arg<V>(e, c, 0), arg<V>(e, c, 1));
            put(o, dotOf(d, d));
        });
        add(bb, "avg", Type::Float, {t}, [](Env& e, const TNode& c, void* o) {
            const V v = arg<V>(e, c, 0);
            float s = 0.0f;
            for (int i = 0; i < widthOf<V>(); ++i) s += comp(v, i);
            put(o, s / static_cast<float>(widthOf<V>()));
        });
        add(bb, "sum", Type::Float, {t}, [](Env& e, const TNode& c, void* o) {
            const V v = arg<V>(e, c, 0);
            float s = 0.0f;
            for (int i = 0; i < widthOf<V>(); ++i) s += comp(v, i);
            put(o, s);
        });
        add(bb, "fit", t, {t, t, t, t, t}, [](Env& e, const TNode& c, void* o) {
            const V x = arg<V>(e, c, 0), a = arg<V>(e, c, 1), b2 = arg<V>(e, c, 2), n0 = arg<V>(e, c, 3), n1 = arg<V>(e, c, 4);
            V r;
            for (int i = 0; i < widthOf<V>(); ++i) compRef(r, i) = fitf(comp(x, i), comp(a, i), comp(b2, i), comp(n0, i), comp(n1, i), true);
            put(o, r);
        });
        add(bb, "fit01", t, {t, t, t}, [](Env& e, const TNode& c, void* o) {
            const V x = arg<V>(e, c, 0), n0 = arg<V>(e, c, 1), n1 = arg<V>(e, c, 2);
            V r;
            for (int i = 0; i < widthOf<V>(); ++i) compRef(r, i) = fitf(comp(x, i), 0.0f, 1.0f, comp(n0, i), comp(n1, i), true);
            put(o, r);
        });
    };
    clampVec(Vec2{}, b, Type::Vec2);
    clampVec(Vec3{}, b, Type::Vec3);
    clampVec(Vec4{}, b, Type::Vec4);
    add(b, "lerp", Type::Float, {Type::Float, Type::Float, Type::Float}, [](Env& e, const TNode& c, void* o) {
        const float a = arg<F>(e, c, 0), b2 = arg<F>(e, c, 1);
        put(o, a + (b2 - a) * arg<F>(e, c, 2));
    });
    add(b, "length", Type::Float, {Type::Float}, [](Env& e, const TNode& c, void* o) { put(o, std::fabs(arg<F>(e, c, 0))); });
    add(b, "cross", Type::Vec3, {Type::Vec3, Type::Vec3}, [](Env& e, const TNode& c, void* o) {
        put(o, cross(arg<Vec3>(e, c, 0), arg<Vec3>(e, c, 1)));
    });
    add(b, "reflect", Type::Vec3, {Type::Vec3, Type::Vec3}, [](Env& e, const TNode& c, void* o) {
        const Vec3 d = arg<Vec3>(e, c, 0), n = normalize(arg<Vec3>(e, c, 1));
        put(o, d - n * (2.0f * dot(d, n)));
    });
    add(b, "fit", Type::Float, {Type::Float, Type::Float, Type::Float, Type::Float, Type::Float}, [](Env& e, const TNode& c, void* o) {
        put(o, fitf(arg<F>(e, c, 0), arg<F>(e, c, 1), arg<F>(e, c, 2), arg<F>(e, c, 3), arg<F>(e, c, 4), true));
    });
    add(b, "efit", Type::Float, {Type::Float, Type::Float, Type::Float, Type::Float, Type::Float}, [](Env& e, const TNode& c, void* o) {
        put(o, fitf(arg<F>(e, c, 0), arg<F>(e, c, 1), arg<F>(e, c, 2), arg<F>(e, c, 3), arg<F>(e, c, 4), false));
    });
    add(b, "fit01", Type::Float, {Type::Float, Type::Float, Type::Float}, [](Env& e, const TNode& c, void* o) {
        put(o, fitf(arg<F>(e, c, 0), 0.0f, 1.0f, arg<F>(e, c, 1), arg<F>(e, c, 2), true));
    });
    add(b, "fit10", Type::Float, {Type::Float, Type::Float, Type::Float}, [](Env& e, const TNode& c, void* o) {
        put(o, fitf(arg<F>(e, c, 0), 1.0f, 0.0f, arg<F>(e, c, 1), arg<F>(e, c, 2), true));
    });
    add(b, "fit11", Type::Float, {Type::Float, Type::Float, Type::Float}, [](Env& e, const TNode& c, void* o) {
        put(o, fitf(arg<F>(e, c, 0), -1.0f, 1.0f, arg<F>(e, c, 1), arg<F>(e, c, 2), true));
    });
    add(b, "smooth", Type::Float, {Type::Float, Type::Float, Type::Float}, [](Env& e, const TNode& c, void* o) {
        put(o, smoothf(arg<F>(e, c, 0), arg<F>(e, c, 1), arg<F>(e, c, 2)));
    });
    add(b, "smoothstep", Type::Float, {Type::Float, Type::Float, Type::Float}, [](Env& e, const TNode& c, void* o) {
        put(o, smoothf(arg<F>(e, c, 0), arg<F>(e, c, 1), arg<F>(e, c, 2)));
    });
    // The prototype's first language made vectors with vec3().
    add(b, "vec3", Type::Vec3, {Type::Float, Type::Float, Type::Float}, [](Env& e, const TNode& c, void* o) {
        put(o, Vec3(arg<F>(e, c, 0), arg<F>(e, c, 1), arg<F>(e, c, 2)));
    });

    // Noise and random numbers: the same place, the same number, everywhere.
    add(b, "noise", Type::Float, {Type::Vec3}, [](Env& e, const TNode& c, void* o) { put(o, valueNoise(arg<Vec3>(e, c, 0))); });
    add(b, "noise", Type::Float, {Type::Float}, [](Env& e, const TNode& c, void* o) {
        put(o, valueNoise(Vec3(arg<F>(e, c, 0), 0.0f, 0.0f)));
    });
    add(b, "noise", Type::Float, {Type::Vec2}, [](Env& e, const TNode& c, void* o) {
        const Vec2 p = arg<Vec2>(e, c, 0);
        put(o, valueNoise(Vec3(p.x, p.y, 0.0f)));
    });
    add(b, "noise", Type::Vec3, {Type::Vec3}, [](Env& e, const TNode& c, void* o) { put(o, noiseVec(arg<Vec3>(e, c, 0))); });
    add(b, "noise", Type::Vec3, {Type::Float}, [](Env& e, const TNode& c, void* o) {
        put(o, noiseVec(Vec3(arg<F>(e, c, 0), 0.0f, 0.0f)));
    });
    add(b, "curlnoise", Type::Vec3, {Type::Vec3}, [](Env& e, const TNode& c, void* o) { put(o, curl(arg<Vec3>(e, c, 0))); });
    for (const char* name : {"rand", "random"}) {
        add(b, name, Type::Float, {Type::Float}, [](Env& e, const TNode& c, void* o) { put(o, randOf(arg<F>(e, c, 0))); });
        add(b, name, Type::Float, {Type::Vec2}, [](Env& e, const TNode& c, void* o) { put(o, randVec(arg<Vec2>(e, c, 0))); });
        add(b, name, Type::Float, {Type::Vec3}, [](Env& e, const TNode& c, void* o) { put(o, randVec(arg<Vec3>(e, c, 0))); });
        add(b, name, Type::Float, {Type::Vec4}, [](Env& e, const TNode& c, void* o) { put(o, randVec(arg<Vec4>(e, c, 0))); });
        add(b, name, Type::Vec2, {Type::Float}, [](Env& e, const TNode& c, void* o) { put(o, randSpread<Vec2>(arg<F>(e, c, 0))); });
        add(b, name, Type::Vec3, {Type::Float}, [](Env& e, const TNode& c, void* o) { put(o, randSpread<Vec3>(arg<F>(e, c, 0))); });
        add(b, name, Type::Vec4, {Type::Float}, [](Env& e, const TNode& c, void* o) { put(o, randSpread<Vec4>(arg<F>(e, c, 0))); });
    }

    // Matrices and quaternions.
    add(b, "ident", Type::Mat4, {}, [](Env&, const TNode&, void* o) { put(o, Mat4(1.0f)); });
    add(b, "ident", Type::Mat3, {}, [](Env&, const TNode&, void* o) { put(o, Mat3(1.0f)); });
    add(b, "transpose", Type::Mat3, {Type::Mat3}, [](Env& e, const TNode& c, void* o) { put(o, transpose(arg<Mat3>(e, c, 0))); });
    add(b, "transpose", Type::Mat4, {Type::Mat4}, [](Env& e, const TNode& c, void* o) { put(o, transpose(arg<Mat4>(e, c, 0))); });
    add(b, "invert", Type::Mat3, {Type::Mat3}, [](Env& e, const TNode& c, void* o) { put(o, inverse(arg<Mat3>(e, c, 0))); });
    add(b, "invert", Type::Mat4, {Type::Mat4}, [](Env& e, const TNode& c, void* o) { put(o, inverse(arg<Mat4>(e, c, 0))); });
    add(b, "determinant", Type::Float, {Type::Mat3}, [](Env& e, const TNode& c, void* o) { put(o, determinant(arg<Mat3>(e, c, 0))); });
    add(b, "determinant", Type::Float, {Type::Mat4}, [](Env& e, const TNode& c, void* o) { put(o, determinant(arg<Mat4>(e, c, 0))); });
    add(b, "rotate", Type::Void, {Type::Mat3, Type::Float, Type::Vec3}, [](Env& e, const TNode& c, void*) {
        const Mat3 r = quatToMat3(quatFromAxisAngle(arg<F>(e, c, 1), arg<Vec3>(e, c, 2)));
        Mat3& m = refOf<Mat3>(*c.lv, e);
        m = mul(m, r);
    }, 1);
    add(b, "rotate", Type::Void, {Type::Mat4, Type::Float, Type::Vec3}, [](Env& e, const TNode& c, void*) {
        const Mat4 r = widen(quatToMat3(quatFromAxisAngle(arg<F>(e, c, 1), arg<Vec3>(e, c, 2))));
        Mat4& m = refOf<Mat4>(*c.lv, e);
        m = r * m;  // m, then the turn
    }, 1);
    add(b, "scale", Type::Void, {Type::Mat3, Type::Vec3}, [](Env& e, const TNode& c, void*) {
        const Mat3 k = glm::mat3(scaling(arg<Vec3>(e, c, 1)));
        Mat3& m = refOf<Mat3>(*c.lv, e);
        m = mul(m, k);
    }, 1);
    add(b, "scale", Type::Void, {Type::Mat4, Type::Vec3}, [](Env& e, const TNode& c, void*) {
        Mat4& m = refOf<Mat4>(*c.lv, e);
        m = scaling(arg<Vec3>(e, c, 1)) * m;
    }, 1);
    add(b, "translate", Type::Void, {Type::Mat4, Type::Vec3}, [](Env& e, const TNode& c, void*) {
        Mat4& m = refOf<Mat4>(*c.lv, e);
        m = translation(arg<Vec3>(e, c, 1)) * m;
    }, 1);
    add(b, "dihedral", Type::Mat3, {Type::Vec3, Type::Vec3}, [](Env& e, const TNode& c, void* o) {
        const Vec3 a = normalize(arg<Vec3>(e, c, 0)), d = normalize(arg<Vec3>(e, c, 1));
        const Vec3 axis = cross(a, d);
        const float s = length(axis), cs = std::clamp(dot(a, d), -1.0f, 1.0f);
        if (s < 1e-7f) {
            if (cs > 0.0f) {
                put(o, Mat3(1.0f));
                return;
            }
            // Opposite: half a turn about any axis across a.
            Vec3 other = std::fabs(a.x) < 0.9f ? Vec3(1, 0, 0) : Vec3(0, 1, 0);
            put(o, quatToMat3(quatFromAxisAngle(3.14159265f, cross(a, other))));
            return;
        }
        put(o, quatToMat3(quatFromAxisAngle(std::atan2(s, cs), axis)));
    });
    add(b, "lookat", Type::Mat3, {Type::Vec3, Type::Vec3, Type::Vec3}, [](Env& e, const TNode& c, void* o) {
        const Vec3 from = arg<Vec3>(e, c, 0), to = arg<Vec3>(e, c, 1), up = arg<Vec3>(e, c, 2);
        const Vec3 z = normalize(from - to);  // looking down -z, as a camera
        Vec3 x = normalize(cross(up, z));
        if (length(x) == 0.0f) x = Vec3(1, 0, 0);
        const Vec3 y = cross(z, x);
        put(o, Mat3(glm::mat3(x, y, z)));  // VEX's rows x, y, z: GLM's columns
    });
    add(b, "quaternion", Type::Vec4, {Type::Float, Type::Vec3}, [](Env& e, const TNode& c, void* o) {
        put(o, quatFromAxisAngle(arg<F>(e, c, 0), arg<Vec3>(e, c, 1)));
    });
    add(b, "quaternion", Type::Vec4, {Type::Mat3}, [](Env& e, const TNode& c, void* o) { put(o, mat3ToQuat(arg<Mat3>(e, c, 0))); });
    add(b, "qmultiply", Type::Vec4, {Type::Vec4, Type::Vec4}, [](Env& e, const TNode& c, void* o) {
        put(o, quatMul(arg<Vec4>(e, c, 0), arg<Vec4>(e, c, 1)));
    });
    add(b, "qrotate", Type::Vec3, {Type::Vec4, Type::Vec3}, [](Env& e, const TNode& c, void* o) {
        const Vec4 q = normalizeOf(arg<Vec4>(e, c, 0));
        put(o, quatRotate(q, arg<Vec3>(e, c, 1)));
    });
    add(b, "qinvert", Type::Vec4, {Type::Vec4}, [](Env& e, const TNode& c, void* o) {
        const Quat q = quatOf(arg<Vec4>(e, c, 0));
        put(o, glm::dot(q, q) > 0.0f ? vec4Of(glm::inverse(q)) : Vec4());
    });
    add(b, "qconvert", Type::Mat3, {Type::Vec4}, [](Env& e, const TNode& c, void* o) { put(o, quatToMat3(arg<Vec4>(e, c, 0))); });
    add(b, "slerp", Type::Vec4, {Type::Vec4, Type::Vec4, Type::Float}, [](Env& e, const TNode& c, void* o) {
        // The shorter way round, at an even speed; the ends as unit quaternions.
        const Vec4 a = normalizeOf(arg<Vec4>(e, c, 0)), d = normalizeOf(arg<Vec4>(e, c, 1));
        const float t = arg<F>(e, c, 2);
        put(o, normalizeOf(vec4Of(glm::slerp(quatOf(a), quatOf(d), t))));
    });
    add(b, "eulertoquaternion", Type::Vec4, {Type::Vec3, Type::Int}, [](Env& e, const TNode& c, void* o) {
        const Vec3 r = arg<Vec3>(e, c, 0);
        const Vec4 qx = quatFromAxisAngle(r.x, Vec3(1, 0, 0)), qy = quatFromAxisAngle(r.y, Vec3(0, 1, 0)),
                   qz = quatFromAxisAngle(r.z, Vec3(0, 0, 1));
        put(o, vec4Of(quatOf(qz) * (quatOf(qy) * quatOf(qx))));  // x first, then y, then z
    });

    // Strings.
    add(b, "itoa", Type::String, {Type::Int}, [](Env& e, const TNode& c, void* o) { put(o, std::to_string(arg<int32_t>(e, c, 0))); });
    add(b, "atoi", Type::Int, {Type::String}, [](Env& e, const TNode& c, void* o) {
        put(o, static_cast<int32_t>(std::strtol(arg<std::string>(e, c, 0).c_str(), nullptr, 10)));
    });
    add(b, "atof", Type::Float, {Type::String}, [](Env& e, const TNode& c, void* o) {
        put(o, static_cast<float>(std::strtod(arg<std::string>(e, c, 0).c_str(), nullptr)));
    });
    add(b, "strlen", Type::Int, {Type::String}, [](Env& e, const TNode& c, void* o) {
        put(o, static_cast<int32_t>(arg<std::string>(e, c, 0).size()));
    });
    add(b, "len", Type::Int, {Type::String}, [](Env& e, const TNode& c, void* o) {
        put(o, static_cast<int32_t>(arg<std::string>(e, c, 0).size()));
    });
    add(b, "len", Type::Int, {Type::Vec2}, [](Env&, const TNode&, void* o) { put(o, int32_t(2)); });
    add(b, "len", Type::Int, {Type::Vec3}, [](Env&, const TNode&, void* o) { put(o, int32_t(3)); });
    add(b, "len", Type::Int, {Type::Vec4}, [](Env&, const TNode&, void* o) { put(o, int32_t(4)); });
    add(b, "concat", Type::String, {Type::String}, [](Env& e, const TNode& c, void* o) {
        std::string s;
        for (size_t i = 0; i < c.kids.size(); ++i) s += arg<std::string>(e, c, i);
        put(o, std::move(s));
    }, 0, false, true);
    add(b, "toupper", Type::String, {Type::String}, [](Env& e, const TNode& c, void* o) { put(o, upperStr(arg<std::string>(e, c, 0))); });
    add(b, "tolower", Type::String, {Type::String}, [](Env& e, const TNode& c, void* o) { put(o, lower(arg<std::string>(e, c, 0))); });
    add(b, "startswith", Type::Int, {Type::String, Type::String}, [](Env& e, const TNode& c, void* o) {
        const std::string s = arg<std::string>(e, c, 0), p = arg<std::string>(e, c, 1);
        put(o, int32_t(s.rfind(p, 0) == 0 ? 1 : 0));
    });
    add(b, "endswith", Type::Int, {Type::String, Type::String}, [](Env& e, const TNode& c, void* o) {
        const std::string s = arg<std::string>(e, c, 0), p = arg<std::string>(e, c, 1);
        put(o, int32_t(s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0 ? 1 : 0));
    });
    add(b, "find", Type::Int, {Type::String, Type::String}, [](Env& e, const TNode& c, void* o) {
        const size_t at = arg<std::string>(e, c, 0).find(arg<std::string>(e, c, 1));
        put(o, at == std::string::npos ? int32_t(-1) : static_cast<int32_t>(at));
    });
    add(b, "replace", Type::String, {Type::String, Type::String, Type::String}, [](Env& e, const TNode& c, void* o) {
        std::string s = arg<std::string>(e, c, 0);
        const std::string from = arg<std::string>(e, c, 1), to = arg<std::string>(e, c, 2);
        if (!from.empty()) {
            for (size_t at = s.find(from); at != std::string::npos; at = s.find(from, at + to.size())) s.replace(at, from.size(), to);
        }
        put(o, std::move(s));
    });
    add(b, "strip", Type::String, {Type::String}, [](Env& e, const TNode& c, void* o) {
        std::string s = arg<std::string>(e, c, 0);
        const size_t a = s.find_first_not_of(" \t\r\n");
        const size_t z = s.find_last_not_of(" \t\r\n");
        put(o, a == std::string::npos ? std::string() : s.substr(a, z - a + 1));
    });
    add(b, "split", Type::StringArray, {Type::String}, [](Env& e, const TNode& c, void* o) {
        const std::string s = arg<std::string>(e, c, 0);
        StrArr out;
        size_t i = 0;
        while (i < s.size()) {
            while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
            size_t j = i;
            while (j < s.size() && !std::isspace(static_cast<unsigned char>(s[j]))) ++j;
            if (j > i) out.push_back(s.substr(i, j - i));
            i = j;
        }
        put(o, std::move(out));
    });
    add(b, "split", Type::StringArray, {Type::String, Type::String}, [](Env& e, const TNode& c, void* o) {
        const std::string s = arg<std::string>(e, c, 0), sep = arg<std::string>(e, c, 1);
        StrArr out;
        if (sep.empty()) {
            out.push_back(s);
        } else {
            size_t start = 0;
            for (size_t at = s.find(sep); at != std::string::npos; at = s.find(sep, start)) {
                out.push_back(s.substr(start, at - start));
                start = at + sep.size();
            }
            out.push_back(s.substr(start));
        }
        put(o, std::move(out));
    });
    add(b, "join", Type::String, {Type::StringArray, Type::String}, [](Env& e, const TNode& c, void* o) {
        const StrArr a = arg<StrArr>(e, c, 0);
        const std::string sep = arg<std::string>(e, c, 1);
        std::string s;
        for (size_t i = 0; i < a.size(); ++i) s += (i ? sep : std::string()) + a[i];
        put(o, std::move(s));
    });
    add(b, "match", Type::Int, {Type::String, Type::String}, [](Env& e, const TNode& c, void* o) {
        const std::string p = arg<std::string>(e, c, 0), s = arg<std::string>(e, c, 1);
        put(o, int32_t(globMatch(p.c_str(), s.c_str()) ? 1 : 0));
    });

    registerArray<int32_t>(b);
    registerArray<float>(b);
    registerArray<Vec2>(b);
    registerArray<Vec3>(b);
    registerArray<Vec4>(b);
    registerArray<std::string>(b);
}

const Builtins& builtins() {
    static const Builtins table = [] {
        Builtins b;
        addMathBuiltins(b);
        addGeometryBuiltins(b);
        // Typed by the checker itself: here so that the parser knows them.
        for (const char* name : {"ch", "chf", "chi", "chv", "chs", "chu", "chp", "set", "array", "point", "prim",
                                 "vertex", "detail", "printf", "sprintf", "warning", "error"}) {
            b[name];
        }
        return b;
    }();
    return table;
}

}  // namespace pg::lang
