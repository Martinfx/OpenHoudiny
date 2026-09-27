#include "pg/sim/Camera.h"

#include "pg/sim/Shared.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace pg::sim {

namespace {
constexpr float kPi = 3.14159265358979323846f;
constexpr float kHuge = std::numeric_limits<float>::max();
}  // namespace

float Camera::fovY() const { return 2.0f * std::atan(12.0f / focal) * 180.0f / kPi; }

Vec3 Camera::rotationFor(const Vec3& forward, const Vec3& up, const Vec3& near) {
    Vec3 f = normalize(forward);
    if (length(f) < 0.5f) f = Vec3(0.0f, 0.0f, -1.0f);
    Vec3 r = normalize(cross(f, up));
    // Looking straight along `up`: the top of the picture away from +z.
    if (length(r) < 0.5f) r = normalize(cross(f, Vec3(0.0f, 0.0f, -1.0f)));
    if (length(r) < 0.5f) r = Vec3(1.0f, 0.0f, 0.0f);
    Rotation frame;
    frame.x = r;
    frame.y = cross(r, f);
    frame.z = f * -1.0f;
    return frame.toEuler(near);
}

Camera Camera::lookingAt(const Vec3& position, const Vec3& target) {
    Camera c;
    c.position = position;
    c.rotation = rotationFor(target - position, Vec3(0.0f, 1.0f, 0.0f));
    return c;
}

Camera Camera::sanitized() const {
    const Camera d;
    Camera c = *this;
    c.position = detail::fix(c.position, -kHuge, kHuge, d.position);
    c.rotation = detail::fix(c.rotation, -kHuge, kHuge, Vec3());
    c.focal = detail::fix(c.focal, 1.0f, 5000.0f, d.focal);
    c.width = std::clamp(c.width, 16, 8192);
    c.height = std::clamp(c.height, 16, 8192);
    return c;
}

}  // namespace pg::sim
