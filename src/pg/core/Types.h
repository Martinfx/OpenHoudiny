#pragma once
//
// Small value types shared by the whole core.
//
// `pg` (procedural geometry) is a placeholder namespace. The real name is a
// Phase 0 deliverable -- see ROADMAP.md 0.1.
//
#include <cmath>
#include <cstdint>
#include <string>

namespace pg {

// --- vectors ---------------------------------------------------------------

struct Vec2 {
    float x = 0, y = 0;
    constexpr Vec2() = default;
    constexpr Vec2(float a, float b) : x(a), y(b) {}
};

struct Vec3 {
    float x = 0, y = 0, z = 0;
    constexpr Vec3() = default;
    constexpr Vec3(float a, float b, float c) : x(a), y(b), z(c) {}
    explicit constexpr Vec3(float s) : x(s), y(s), z(s) {}

    constexpr float operator[](int i) const { return i == 0 ? x : (i == 1 ? y : z); }
    constexpr float& operator[](int i) { return i == 0 ? x : (i == 1 ? y : z); }

    constexpr Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    constexpr Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    constexpr Vec3 operator-() const { return {-x, -y, -z}; }
    constexpr Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
    constexpr Vec3 operator*(const Vec3& o) const { return {x * o.x, y * o.y, z * o.z}; }
    constexpr Vec3& operator+=(const Vec3& o) { x += o.x; y += o.y; z += o.z; return *this; }
    constexpr bool operator==(const Vec3& o) const { return x == o.x && y == o.y && z == o.z; }
};

inline float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float length(const Vec3& v) { return std::sqrt(dot(v, v)); }
/// Right-handed: cross(x, y) == z.
inline Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
/// Unit vector along `v`; a zero vector stays zero.
inline Vec3 normalize(const Vec3& v) {
    const float len = length(v);
    return len > 0.0f ? v * (1.0f / len) : v;
}

struct Vec4 {
    float x = 0, y = 0, z = 0, w = 0;
    constexpr Vec4() = default;
    constexpr Vec4(float a, float b, float c, float d) : x(a), y(b), z(c), w(d) {}
};

// --- 4x4 matrix, row-major, row-vector convention (p * M) -------------------

struct Mat4 {
    float m[4][4]{};

    static Mat4 identity();
    static Mat4 translate(const Vec3& t);
    static Mat4 scale(const Vec3& s);
    /// Euler XYZ in degrees.
    static Mat4 rotate(const Vec3& degrees);

    Mat4 operator*(const Mat4& o) const;
    Vec3 transformPoint(const Vec3& p) const;
    Vec3 transformDirection(const Vec3& v) const;
};

// --- attribute types -------------------------------------------------------

enum class AttrType : uint8_t {
    Int,     ///< int32
    Float,   ///< float32
    Vec2,    ///< 2 x float32
    Vec3,    ///< 3 x float32
    Vec4,    ///< 4 x float32
    String,  ///< int32 index into the array's shared string table
};

/// Bytes occupied by one element of `t`.
constexpr size_t attrSize(AttrType t) {
    switch (t) {
        case AttrType::Int:    return 4;
        case AttrType::Float:  return 4;
        case AttrType::Vec2:   return 8;
        case AttrType::Vec3:   return 12;
        case AttrType::Vec4:   return 16;
        case AttrType::String: return 4;
    }
    return 0;
}

const char* attrTypeName(AttrType t);

/// The four attribute classes. Every attribute belongs to exactly one.
enum class AttrClass : uint8_t {
    Detail,     ///< one value for the whole geometry
    Point,      ///< one value per point
    Vertex,     ///< one value per primitive corner
    Primitive,  ///< one value per primitive
};

const char* attrClassName(AttrClass c);

/// Maps a C++ type to its AttrType. Used to typecheck handle access.
template <class T> struct AttrTypeOf;
template <> struct AttrTypeOf<int32_t> { static constexpr AttrType value = AttrType::Int; };
template <> struct AttrTypeOf<float>   { static constexpr AttrType value = AttrType::Float; };
template <> struct AttrTypeOf<Vec2>    { static constexpr AttrType value = AttrType::Vec2; };
template <> struct AttrTypeOf<Vec3>    { static constexpr AttrType value = AttrType::Vec3; };
template <> struct AttrTypeOf<Vec4>    { static constexpr AttrType value = AttrType::Vec4; };

}  // namespace pg
