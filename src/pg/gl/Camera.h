#pragma once
//
// The cameras of the previews: an orbit around the origin, and the matrices
// OpenGL wants for it.
//
#include <array>

namespace pg::gl {

using Mat4 = std::array<float, 16>;  // column-major, as GL expects

Mat4 multiply(const Mat4& a, const Mat4& b);
Mat4 perspective(float fovyDegrees, float aspect, float zNear, float zFar);
/// Looking from `eye` at the origin, y up.
Mat4 lookAt(const float eye[3]);
/// Looking from `eye` at `target`, y up.
Mat4 lookAt(const float eye[3], const float target[3]);
Mat4 identity();

/// Camera orbiting a point: the origin, unless the target says otherwise.
struct Orbit {
    float yaw = 30.0f;    ///< degrees around the vertical axis
    float pitch = 18.0f;  ///< degrees above the horizon
    float distance = 3.4f;
    float target[3] = {0.0f, 0.0f, 0.0f};

    /// Where the camera is.
    void eye(float out[3]) const;
};

}  // namespace pg::gl
