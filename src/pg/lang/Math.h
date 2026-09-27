#pragma once
//
// Arithmetic on every value type of the language, for the interpreter and
// the builtins: componentwise on vectors, division by zero is zero, ints
// wrap rather than overflow.
//
#include "pg/lang/Ast.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace pg::lang {

// --- component access ------------------------------------------------------------------------

inline float comp(const Vec2& v, int i) { return i == 0 ? v.x : v.y; }
inline float comp(const Vec3& v, int i) { return v[i]; }
inline float comp(const Vec4& v, int i) { return i == 0 ? v.x : i == 1 ? v.y : i == 2 ? v.z : v.w; }
inline float& compRef(Vec2& v, int i) { return i == 0 ? v.x : v.y; }
inline float& compRef(Vec3& v, int i) { return v[i]; }
inline float& compRef(Vec4& v, int i) { return i == 0 ? v.x : i == 1 ? v.y : i == 2 ? v.z : v.w; }

template <class V> constexpr int widthOf() { return 1; }
template <> constexpr int widthOf<Vec2>() { return 2; }
template <> constexpr int widthOf<Vec3>() { return 3; }
template <> constexpr int widthOf<Vec4>() { return 4; }

template <class V, class F> V mapv(const V& a, F f) {
    V r;
    for (int i = 0; i < widthOf<V>(); ++i) compRef(r, i) = f(comp(a, i));
    return r;
}
template <class V, class F> V zipv(const V& a, const V& b, F f) {
    V r;
    for (int i = 0; i < widthOf<V>(); ++i) compRef(r, i) = f(comp(a, i), comp(b, i));
    return r;
}

// --- safe scalars ----------------------------------------------------------------------------

inline int32_t wrapAdd(int32_t a, int32_t b) { return static_cast<int32_t>(static_cast<uint32_t>(a) + static_cast<uint32_t>(b)); }
inline int32_t wrapSub(int32_t a, int32_t b) { return static_cast<int32_t>(static_cast<uint32_t>(a) - static_cast<uint32_t>(b)); }
inline int32_t wrapMul(int32_t a, int32_t b) { return static_cast<int32_t>(static_cast<uint32_t>(a) * static_cast<uint32_t>(b)); }
inline int32_t safeMod(int32_t a, int32_t b) { return (b == 0 || (a == INT32_MIN && b == -1)) ? 0 : a % b; }
inline float safeDiv(float a, float b) { return b == 0.0f ? 0.0f : a / b; }
inline float safeFmod(float a, float b) { return b == 0.0f ? 0.0f : std::fmod(a, b); }
/// A float cut to an int, as C does -- but defined for every float.
inline int32_t toInt(double v) {
    if (!std::isfinite(v)) return 0;
    return static_cast<int32_t>(std::clamp(std::trunc(v), -2147483648.0, 2147483647.0));
}

// --- matrices ----------------------------------------------------------------------------------

inline Mat3 addm(const Mat3& a, const Mat3& b, float s) {
    Mat3 r;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r.m[i][j] = a.m[i][j] + s * b.m[i][j];
    return r;
}
inline Mat4 addm(const Mat4& a, const Mat4& b, float s) {
    Mat4 r;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) r.m[i][j] = a.m[i][j] + s * b.m[i][j];
    return r;
}
inline Mat3 scalem(const Mat3& a, float s) {
    Mat3 r;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r.m[i][j] = a.m[i][j] * s;
    return r;
}
inline Mat4 scalem(const Mat4& a, float s) {
    Mat4 r;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) r.m[i][j] = a.m[i][j] * s;
    return r;
}
inline Mat3 diag3(float s) {
    Mat3 r;
    r.m[0][0] = r.m[1][1] = r.m[2][2] = s;
    return r;
}
inline Mat4 diag4(float s) {
    Mat4 r;
    r.m[0][0] = r.m[1][1] = r.m[2][2] = r.m[3][3] = s;
    return r;
}
inline bool sameMat(const Mat4& a, const Mat4& b) { return std::memcmp(a.m, b.m, sizeof a.m) == 0; }
inline Vec4 mul(const Vec4& v, const Mat4& m) {
    Vec4 r;
    for (int j = 0; j < 4; ++j) {
        compRef(r, j) = v.x * m.m[0][j] + v.y * m.m[1][j] + v.z * m.m[2][j] + v.w * m.m[3][j];
    }
    return r;
}

// --- the generic operators ---------------------------------------------------------------------

template <class T> inline T opAdd(const T& a, const T& b) {
    if constexpr (std::is_same_v<T, int32_t>) return wrapAdd(a, b);
    else if constexpr (std::is_same_v<T, float>) return a + b;
    else if constexpr (std::is_same_v<T, Mat3> || std::is_same_v<T, Mat4>) return addm(a, b, 1.0f);
    else if constexpr (std::is_same_v<T, std::string>) return a + b;
    else return zipv(a, b, [](float x, float y) { return x + y; });
}
template <class T> inline T opSub(const T& a, const T& b) {
    if constexpr (std::is_same_v<T, int32_t>) return wrapSub(a, b);
    else if constexpr (std::is_same_v<T, float>) return a - b;
    else if constexpr (std::is_same_v<T, Mat3> || std::is_same_v<T, Mat4>) return addm(a, b, -1.0f);
    else return zipv(a, b, [](float x, float y) { return x - y; });
}
template <class T> inline T opMul(const T& a, const T& b) {
    if constexpr (std::is_same_v<T, int32_t>) return wrapMul(a, b);
    else if constexpr (std::is_same_v<T, float>) return a * b;
    else return zipv(a, b, [](float x, float y) { return x * y; });
}
template <class T> inline T opDiv(const T& a, const T& b) {
    if constexpr (std::is_same_v<T, int32_t>) return b == 0 ? 0 : (a == INT32_MIN && b == -1 ? a : a / b);
    else if constexpr (std::is_same_v<T, float>) return safeDiv(a, b);
    else return zipv(a, b, safeDiv);
}
template <class T> inline T opMod(const T& a, const T& b) {
    if constexpr (std::is_same_v<T, int32_t>) return safeMod(a, b);
    else if constexpr (std::is_same_v<T, float>) return safeFmod(a, b);
    else return zipv(a, b, safeFmod);
}
template <class T> inline T opNeg(const T& a) {
    if constexpr (std::is_same_v<T, int32_t>) return wrapSub(0, a);
    else if constexpr (std::is_same_v<T, float>) return -a;
    else if constexpr (std::is_same_v<T, Mat3> || std::is_same_v<T, Mat4>) return scalem(a, -1.0f);
    else return mapv(a, [](float x) { return -x; });
}

template <class T> struct IsArray : std::false_type {};
template <class E> struct IsArray<std::vector<E>> : std::true_type {};

template <class T> inline bool opEq(const T& a, const T& b) {
    if constexpr (IsArray<T>::value) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) {
            if (!opEq(a[i], b[i])) return false;
        }
        return true;
    } else if constexpr (std::is_same_v<T, Vec2>) return a.x == b.x && a.y == b.y;
    else if constexpr (std::is_same_v<T, Vec4>) return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w;
    else if constexpr (std::is_same_v<T, Mat4>) return sameMat(a, b);
    else return a == b;
}

}  // namespace pg::lang
