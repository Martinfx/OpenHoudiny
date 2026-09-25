#include "pg/shader/Types.h"

#include <charconv>
#include <locale>
#include <sstream>

namespace pg::shader {

const char* typeName(Type t) {
    switch (t) {
        case Type::Float:     return "float";
        case Type::Vec2:      return "vec2";
        case Type::Vec3:      return "vec3";
        case Type::Vec4:      return "vec4";
        case Type::Sampler2D: return "sampler2D";
        case Type::Any:       return "any";
    }
    return "?";
}

std::optional<Type> typeFromName(const std::string& name) {
    for (Type t : {Type::Float, Type::Vec2, Type::Vec3, Type::Vec4, Type::Sampler2D, Type::Any}) {
        if (name == typeName(t)) return t;
    }
    return std::nullopt;
}

int componentCount(Type t) {
    switch (t) {
        case Type::Float: return 1;
        case Type::Vec2:  return 2;
        case Type::Vec3:  return 3;
        case Type::Vec4:  return 4;
        default:          return 0;
    }
}

Type vectorType(int components) {
    switch (components) {
        case 2:  return Type::Vec2;
        case 3:  return Type::Vec3;
        case 4:  return Type::Vec4;
        default: return Type::Float;
    }
}

bool isNumeric(Type t) { return componentCount(t) > 0; }

bool convertible(Type from, Type to) {
    if (from == Type::Any || to == Type::Any) return from != Type::Sampler2D && to != Type::Sampler2D;
    if (from == Type::Sampler2D || to == Type::Sampler2D) return from == to;
    return true;
}

Value Value::as(Type t) const {
    if (t == type || !isNumeric(t)) return *this;
    Value out;
    out.type = t;
    const int from = componentCount(type), to = componentCount(t);
    for (int i = 0; i < to; ++i) {
        if (from == 1) out.v[i] = v[0];               // splat
        else if (i < from) out.v[i] = v[i];           // keep / truncate
        else out.v[i] = i == 3 ? 1.0f : 0.0f;         // pad, w = 1
    }
    return out;
}

std::string formatFloat(float x) {
    char buf[32];
    const auto res = std::to_chars(buf, buf + sizeof(buf), x);
    std::string s(buf, res.ptr);
    if (s.find_first_of(".en") == std::string::npos) s += ".0";  // "1" -> "1.0"
    return s;
}

bool parseFloat(const std::string& text, float& out) {
    std::istringstream in(text);
    in.imbue(std::locale::classic());
    float v = 0.0f;
    in >> v;
    if (in.fail()) return false;
    in >> std::ws;
    if (!in.eof()) return false;  // trailing garbage
    out = v;
    return true;
}

}  // namespace pg::shader
