#pragma once
//
// The camera of a shot: where it stands, which way it looks and how wide it
// sees -- what `pgshader sim` renders through, and what the editor's
// viewport can look through.
//
// As a camera in Houdini or Blender it looks along its own -z, its own y up
// the picture; it is turned as an object is (Shape.h: degrees about x, then
// y, then z). Its lens is a full frame camera's, 24 mm high: a lens of
// `focal` mm sees 2 atan(12 / focal) from the top of the picture to the
// bottom -- 38 mm some 35 degrees, a long lens less, a wide one more.
//
#include "pg/sim/Shape.h"

namespace pg::sim {

struct Camera {
    Vec3 position{3.0f, 1.3f, 3.8f};
    Vec3 rotation{-10.0f, 38.0f, 0.0f};  ///< degrees; this one looks at the middle of the floor
    float focal = 38.0f;                 ///< mm
    int width = 1280, height = 720;      ///< the picture, pixels
    int node = 0;

    Rotation frame() const { return Rotation::fromEuler(rotation); }
    /// Where it looks, and the picture's right and up: unit vectors.
    Vec3 forward() const { return frame().z * -1.0f; }
    Vec3 right() const { return frame().x; }
    Vec3 up() const { return frame().y; }
    /// Degrees from the top of the picture to its bottom.
    float fovY() const;
    float aspect() const { return static_cast<float>(width) / static_cast<float>(height); }

    /// The rotation of a camera that looks along `forward` with `up` up the
    /// picture (neither need be of unit length, nor square to the other); of
    /// the angles that give it, those nearest `near`.
    static Vec3 rotationFor(const Vec3& forward, const Vec3& up, const Vec3& near = Vec3());
    /// A camera at `position` that looks at `target`, level.
    static Camera lookingAt(const Vec3& position, const Vec3& target);

    /// Finite, a lens and a picture that make sense.
    Camera sanitized() const;
    bool operator==(const Camera&) const = default;
};

}  // namespace pg::sim
