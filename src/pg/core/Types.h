#pragma once
//
// Small value types shared by the whole core.
//
// The vectors are GLM's (OpenGL Mathematics): Vec3 is glm::vec3, and so on,
// with GLM's operators and its dot, cross and length -- a vector here is one
// wherever GLM is spoken. pgmath (CMakeLists.txt) sets how every file sees
// them: at zero when made without values, x, y, z and w their parts alone,
// three floats one after another -- as the attributes store them.
//
// `pg` (procedural geometry) is a placeholder namespace. The real name is a
// Phase 0 deliverable -- see ROADMAP.md 0.1.
//
#ifndef GLM_FORCE_CTOR_INIT
#define GLM_FORCE_CTOR_INIT
#endif
#ifndef GLM_FORCE_XYZW_ONLY
#define GLM_FORCE_XYZW_ONLY
#endif
#include <glm/geometric.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <cmath>
#include <cstdint>
#include <string>

// Every file the same: GLM included before this header, without pgmath's
// settings, would make vectors that differ from these.
static_assert(GLM_CONFIG_CTOR_INIT != GLM_CTOR_INIT_DISABLE, "GLM without GLM_FORCE_CTOR_INIT: link pgmath");
static_assert(GLM_CONFIG_XYZW_ONLY == GLM_ENABLE, "GLM without GLM_FORCE_XYZW_ONLY: link pgmath");

namespace pg {

// --- vectors ---------------------------------------------------------------

using Vec2 = glm::vec2;
using Vec3 = glm::vec3;
using Vec4 = glm::vec4;

static_assert(sizeof(Vec2) == 8 && sizeof(Vec3) == 12 && sizeof(Vec4) == 16, "vectors are floats, packed");

using glm::cross;  ///< right-handed: cross(x, y) == z
using glm::dot;
using glm::length;

/// Unit vector along `v`; a zero vector stays zero (glm::normalize would
/// divide by its length).
inline Vec3 normalize(const Vec3& v) {
    const float len = length(v);
    return len > 0.0f ? v * (1.0f / len) : v;
}

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
