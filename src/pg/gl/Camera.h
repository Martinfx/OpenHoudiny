#pragma once
//
// The cameras of the previews: an orbit around a point. Its matrices are
// GLM's: glm::lookAt along its view, glm::perspective for its lens.
//
#include "pg/core/Types.h"

#include <algorithm>

namespace pg::gl {

/// How near and how far the previews' cameras clip, as a rule.
inline constexpr float kNearClip = 0.02f, kFarClip = 500.0f;

/// The planes a camera clips at, when what it draws reaches as far as
/// `farthest` from it: kNearClip and kFarClip -- both farther in the same
/// ratio past kFarClip, so that a big scene is not cut off and its depth is
/// as fine as a small one's.
inline void clipPlanes(float farthest, float& zNear, float& zFar) {
    zFar = std::max(kFarClip, 1.05f * farthest);
    zNear = kNearClip * (zFar / kFarClip);
}

/// The corner of the box lo..hi farthest from `eye`, how far it is; 0 for
/// no box (lo above hi).
inline float farthestCorner(const Vec3& eye, const Vec3& lo, const Vec3& hi) {
    if (!(lo.x <= hi.x && lo.y <= hi.y && lo.z <= hi.z)) return 0.0f;
    const Vec3 far(std::max(eye.x - lo.x, hi.x - eye.x), std::max(eye.y - lo.y, hi.y - eye.y),
                   std::max(eye.z - lo.z, hi.z - eye.z));
    return length(far);
}

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
