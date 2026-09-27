#include "pg/gl/Camera.h"

#include <cmath>

namespace pg::gl {
namespace {
constexpr float kPi = 3.14159265358979323846f;
}  // namespace

Mat4 multiply(const Mat4& a, const Mat4& b) {
    Mat4 r{};
    for (int c = 0; c < 4; ++c) {
        for (int row = 0; row < 4; ++row) {
            float s = 0.0f;
            for (int k = 0; k < 4; ++k) s += a[k * 4 + row] * b[c * 4 + k];
            r[c * 4 + row] = s;
        }
    }
    return r;
}

Mat4 perspective(float fovyDegrees, float aspect, float zNear, float zFar) {
    const float f = 1.0f / std::tan(fovyDegrees * kPi / 360.0f);
    Mat4 m{};
    m[0] = f / aspect;
    m[5] = f;
    m[10] = (zFar + zNear) / (zNear - zFar);
    m[11] = -1.0f;
    m[14] = 2.0f * zFar * zNear / (zNear - zFar);
    return m;
}

Mat4 lookAt(const float eye[3]) {
    const float origin[3] = {0.0f, 0.0f, 0.0f};
    return lookAt(eye, origin);
}

Mat4 lookAt(const float eye[3], const float target[3]) {
    float f[3] = {target[0] - eye[0], target[1] - eye[1], target[2] - eye[2]};
    const float fl = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    for (float& v : f) v /= fl;
    float s[3] = {f[1] * 0.0f - f[2] * 1.0f, f[2] * 0.0f - f[0] * 0.0f, f[0] * 1.0f - f[1] * 0.0f};
    const float sl = std::sqrt(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
    for (float& v : s) v /= sl;
    const float u[3] = {s[1] * f[2] - s[2] * f[1], s[2] * f[0] - s[0] * f[2], s[0] * f[1] - s[1] * f[0]};
    Mat4 m{};
    m[0] = s[0]; m[4] = s[1]; m[8] = s[2];
    m[1] = u[0]; m[5] = u[1]; m[9] = u[2];
    m[2] = -f[0]; m[6] = -f[1]; m[10] = -f[2];
    m[12] = -(s[0] * eye[0] + s[1] * eye[1] + s[2] * eye[2]);
    m[13] = -(u[0] * eye[0] + u[1] * eye[1] + u[2] * eye[2]);
    m[14] = f[0] * eye[0] + f[1] * eye[1] + f[2] * eye[2];
    m[15] = 1.0f;
    return m;
}

Mat4 lookAlong(const float eye[3], const float f[3], const float u[3]) {
    const float s[3] = {f[1] * u[2] - f[2] * u[1], f[2] * u[0] - f[0] * u[2], f[0] * u[1] - f[1] * u[0]};
    Mat4 m{};
    m[0] = s[0]; m[4] = s[1]; m[8] = s[2];
    m[1] = u[0]; m[5] = u[1]; m[9] = u[2];
    m[2] = -f[0]; m[6] = -f[1]; m[10] = -f[2];
    m[12] = -(s[0] * eye[0] + s[1] * eye[1] + s[2] * eye[2]);
    m[13] = -(u[0] * eye[0] + u[1] * eye[1] + u[2] * eye[2]);
    m[14] = f[0] * eye[0] + f[1] * eye[1] + f[2] * eye[2];
    m[15] = 1.0f;
    return m;
}

Mat4 identity() {
    Mat4 m{};
    m[0] = m[5] = m[10] = m[15] = 1.0f;
    return m;
}

void Orbit::eye(float out[3]) const {
    const float y = yaw * kPi / 180.0f, p = pitch * kPi / 180.0f;
    out[0] = target[0] + distance * std::cos(p) * std::sin(y);
    out[1] = target[1] + distance * std::sin(p);
    out[2] = target[2] + distance * std::cos(p) * std::cos(y);
}

void Orbit::axes(float forward[3], float right[3], float up[3]) const {
    const float y = yaw * kPi / 180.0f, p = pitch * kPi / 180.0f;
    const float f[3] = {-std::cos(p) * std::sin(y), -std::sin(p), -std::cos(p) * std::cos(y)};
    // Level: the right square to the vertical -- or, looking straight up or
    // down, as the yaw turns it.
    float r[3] = {-f[2], 0.0f, f[0]};
    float rl = std::sqrt(r[0] * r[0] + r[2] * r[2]);
    if (rl < 1e-6f) {
        r[0] = std::cos(y);
        r[2] = -std::sin(y);
        rl = 1.0f;
    }
    r[0] /= rl;
    r[2] /= rl;
    const float u[3] = {r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0]};
    // Turned by the roll about the view.
    const float c = std::cos(roll * kPi / 180.0f), s = std::sin(roll * kPi / 180.0f);
    for (int k = 0; k < 3; ++k) {
        forward[k] = f[k];
        right[k] = r[k] * c + u[k] * s;
        up[k] = u[k] * c - r[k] * s;
    }
}

}  // namespace pg::gl
