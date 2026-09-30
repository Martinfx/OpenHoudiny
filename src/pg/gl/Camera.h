#pragma once
//
// The cameras of the previews: an orbit around a point. Its matrices are
// GLM's: glm::lookAt along its view, glm::perspective for its lens.
//
#include "pg/core/Types.h"

namespace pg::gl {

/// Camera orbiting a point: the origin, unless the target says otherwise.
struct Orbit {
    float yaw = 30.0f;    ///< degrees around the vertical axis
    float pitch = 18.0f;  ///< degrees above the horizon
    float distance = 3.4f;
    Vec3 target{0.0f, 0.0f, 0.0f};
    /// Degrees the camera is turned about where it looks: 0 keeps the
    /// horizon level.
    float roll = 0.0f;
    float fovY = 35.0f;   ///< degrees from the top of the picture to its bottom

    /// Where the camera is.
    Vec3 eye() const;
    /// Where it looks, and the picture's right and up: unit vectors, square
    /// to each other.
    void axes(Vec3& forward, Vec3& right, Vec3& up) const;
    /// World to the camera: glm::lookAt from the eye along where it looks.
    Mat4 view() const;
    /// ... and on to the picture, `aspect` wide to 1 high (OpenGL's clip
    /// space, -1 to 1): glm::perspective times the view.
    Mat4 viewProjection(float aspect, float zNear, float zFar) const;
};

}  // namespace pg::gl
