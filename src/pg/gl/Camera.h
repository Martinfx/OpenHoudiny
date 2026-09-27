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
/// Looking from `eye` along the unit vector `forward`, the unit vector `up`
/// (square to it) up the picture.
Mat4 lookAlong(const float eye[3], const float forward[3], const float up[3]);
Mat4 identity();

/// Camera orbiting a point: the origin, unless the target says otherwise.
struct Orbit {
    float yaw = 30.0f;    ///< degrees around the vertical axis
    float pitch = 18.0f;  ///< degrees above the horizon
    float distance = 3.4f;
    float target[3] = {0.0f, 0.0f, 0.0f};
    /// Degrees the camera is turned about where it looks: 0 keeps the
    /// horizon level.
    float roll = 0.0f;
    float fovY = 35.0f;   ///< degrees from the top of the picture to its bottom

    /// Where the camera is.
    void eye(float out[3]) const;
    /// Where it looks, and the picture's right and up: unit vectors, square
    /// to each other.
    void axes(float forward[3], float right[3], float up[3]) const;
};

}  // namespace pg::gl
