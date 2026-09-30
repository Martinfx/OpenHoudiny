#include "pg/gl/Camera.h"

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>

#include <cmath>

namespace pg::gl {

Vec3 Orbit::eye() const {
    const float y = glm::radians(yaw), p = glm::radians(pitch);
    return target + Vec3(std::cos(p) * std::sin(y), std::sin(p), std::cos(p) * std::cos(y)) * distance;
}

void Orbit::axes(Vec3& forward, Vec3& right, Vec3& up) const {
    const float y = glm::radians(yaw), p = glm::radians(pitch);
    const Vec3 f(-std::cos(p) * std::sin(y), -std::sin(p), -std::cos(p) * std::cos(y));
    // Level: the right square to the vertical -- or, looking straight up or
    // down, as the yaw turns it.
    const Vec3 level(-f.z, 0.0f, f.x);
    const float rl = length(level);
    const Vec3 r = rl < 1e-6f ? Vec3(std::cos(y), 0.0f, -std::sin(y)) : level / rl;
    const Vec3 u = cross(r, f);
    // Turned by the roll about the view.
    const float a = glm::radians(roll), c = std::cos(a), s = std::sin(a);
    forward = f;
    right = r * c + u * s;
    up = u * c - r * s;
}

Mat4 Orbit::view() const {
    const Vec3 e = eye();
    Vec3 f, r, u;
    axes(f, r, u);
    return glm::lookAt(e, e + f, u);
}

Mat4 Orbit::viewProjection(float aspect, float zNear, float zFar) const {
    return glm::perspective(glm::radians(fovY), aspect, zNear, zFar) * view();
}

}  // namespace pg::gl
