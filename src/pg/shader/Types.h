#pragma once
//
// Value types of the shader graph and the conversions between them.
//
// The shader graph is its own network type -- the equivalent of a VOP or
// material network next to the geometry network. It shares no data with the
// cook engine: its nodes produce code, not geometry.
//
#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace pg::shader {

/// Port types. `Any` exists only in node definitions: it is resolved per node
/// instance to the widest numeric type connected to it (see Generator.h).
enum class Type : uint8_t { Float, Vec2, Vec3, Vec4, Sampler2D, Any };

/// Neutral (GLSL) spelling: "float", "vec3", "sampler2D", "any".
const char* typeName(Type t);
std::optional<Type> typeFromName(const std::string& name);

/// 1..4 for numeric types, 0 for samplers and Any.
int componentCount(Type t);
/// Float, Vec2, Vec3 or Vec4 for 1..4 components.
Type vectorType(int components);
bool isNumeric(Type t);

/// Can an output of type `from` feed an input of type `to`? Numeric types
/// convert into each other (see Target::convert); samplers only to samplers.
bool convertible(Type from, Type to);

/// A numeric constant of up to four components.
struct Value {
    Type type = Type::Float;
    std::array<float, 4> v{};

    static Value scalar(float x) { return Value{Type::Float, {x, 0, 0, 0}}; }
    static Value vector(float x, float y, float z) { return Value{Type::Vec3, {x, y, z, 0}}; }
    static Value vector(float x, float y, float z, float w) {
        return Value{Type::Vec4, {x, y, z, w}};
    }

    /// The same value as another numeric type: a scalar is splatted, a vector
    /// is truncated, missing components are 0 -- except w, which is 1.
    Value as(Type t) const;

    bool operator==(const Value& o) const { return type == o.type && v == o.v; }
    bool operator!=(const Value& o) const { return !(*this == o); }
};

/// Shortest text that reads back as the same float, always with a decimal
/// point or exponent ("1.0", "0.25", "1e-07"), so it is a valid float literal
/// in every shading language. Locale independent.
std::string formatFloat(float x);

/// Parses a float written by formatFloat (or by a person). Locale independent.
bool parseFloat(const std::string& text, float& out);

}  // namespace pg::shader
