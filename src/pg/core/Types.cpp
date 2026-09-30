#include "pg/core/Types.h"

#include <glm/gtx/euler_angles.hpp>

namespace pg {

const char* attrTypeName(AttrType t) {
    switch (t) {
        case AttrType::Int:    return "int";
        case AttrType::Float:  return "float";
        case AttrType::Vec2:   return "vec2";
        case AttrType::Vec3:   return "vec3";
        case AttrType::Vec4:   return "vec4";
        case AttrType::String: return "string";
    }
    return "?";
}

const char* attrClassName(AttrClass c) {
    switch (c) {
        case AttrClass::Detail:    return "detail";
        case AttrClass::Point:     return "point";
        case AttrClass::Vertex:    return "vertex";
        case AttrClass::Primitive: return "primitive";
    }
    return "?";
}

Mat4 translation(const Vec3& t) {
    Mat4 m(1.0f);
    m[3] = Vec4(t, 1.0f);
    return m;
}

Mat4 scaling(const Vec3& s) {
    Mat4 m(1.0f);
    m[0][0] = s.x;
    m[1][1] = s.y;
    m[2][2] = s.z;
    return m;
}

Mat4 rotationXYZ(const Vec3& degrees) {
    const Vec3 a = glm::radians(degrees);
    return glm::eulerAngleZYX(a.z, a.y, a.x);
}

}  // namespace pg
