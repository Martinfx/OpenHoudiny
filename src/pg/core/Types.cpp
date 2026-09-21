#include "pg/core/Types.h"

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

Mat4 Mat4::identity() {
    Mat4 r;
    for (int i = 0; i < 4; ++i) r.m[i][i] = 1.0f;
    return r;
}

Mat4 Mat4::translate(const Vec3& t) {
    Mat4 r = identity();
    r.m[3][0] = t.x;
    r.m[3][1] = t.y;
    r.m[3][2] = t.z;
    return r;
}

Mat4 Mat4::scale(const Vec3& s) {
    Mat4 r = identity();
    r.m[0][0] = s.x;
    r.m[1][1] = s.y;
    r.m[2][2] = s.z;
    return r;
}

Mat4 Mat4::rotate(const Vec3& degrees) {
    const float d2r = 3.14159265358979323846f / 180.0f;
    const float cx = std::cos(degrees.x * d2r), sx = std::sin(degrees.x * d2r);
    const float cy = std::cos(degrees.y * d2r), sy = std::sin(degrees.y * d2r);
    const float cz = std::cos(degrees.z * d2r), sz = std::sin(degrees.z * d2r);

    Mat4 rx = identity();
    rx.m[1][1] = cx;  rx.m[1][2] = sx;
    rx.m[2][1] = -sx; rx.m[2][2] = cx;

    Mat4 ry = identity();
    ry.m[0][0] = cy;  ry.m[0][2] = -sy;
    ry.m[2][0] = sy;  ry.m[2][2] = cy;

    Mat4 rz = identity();
    rz.m[0][0] = cz;  rz.m[0][1] = sz;
    rz.m[1][0] = -sz; rz.m[1][1] = cz;

    return rx * ry * rz;
}

Mat4 Mat4::operator*(const Mat4& o) const {
    Mat4 r;
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            float s = 0.0f;
            for (int k = 0; k < 4; ++k) s += m[i][k] * o.m[k][j];
            r.m[i][j] = s;
        }
    }
    return r;
}

Vec3 Mat4::transformPoint(const Vec3& p) const {
    return Vec3(p.x * m[0][0] + p.y * m[1][0] + p.z * m[2][0] + m[3][0],
                p.x * m[0][1] + p.y * m[1][1] + p.z * m[2][1] + m[3][1],
                p.x * m[0][2] + p.y * m[1][2] + p.z * m[2][2] + m[3][2]);
}

Vec3 Mat4::transformDirection(const Vec3& v) const {
    return Vec3(v.x * m[0][0] + v.y * m[1][0] + v.z * m[2][0],
                v.x * m[0][1] + v.y * m[1][1] + v.z * m[2][1],
                v.x * m[0][2] + v.y * m[1][2] + v.z * m[2][2]);
}

}  // namespace pg
