#include "pg/sim/Camera.h"

#include "pg/io/Picture.h"
#include "pg/sim/Shared.h"
#include "pg/usd/Geom.h"

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

std::string Camera::plateFile(int frame) const {
    return plate.empty() ? std::string() : io::sequenceFile(plate, plateFrame + frame - 1);
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

bool cameraFromUsd(const std::string& file, const std::string& prim, float frame, float fps, float offset, int width,
                   int height, bool metres, Camera& out, std::string& error, std::vector<std::string>* warnings,
                   bool* varies, double* timeCode) {
    const auto stage = usd::Stage::openCached(file, error);
    if (!stage) return false;
    const usd::Stage::Prim* p = nullptr;
    if (prim.empty()) {
        const auto all = usd::cameras(*stage);
        if (all.empty()) {
            error = "no camera in " + file;
            return false;
        }
        p = all.front();
    } else {
        p = stage->find(prim);
        if (!p) {
            error = "no prim " + prim + " in " + file;
            return false;
        }
        if (p->type != "Camera") {
            error = prim + " is " + (p->type.empty() ? std::string("no camera") : "a " + p->type + ", not a camera");
            return false;
        }
    }
    usd::CameraSample s;
    const double time = usd::timeCodeAt(*stage, frame, fps, offset);
    if (timeCode) *timeCode = time;
    usd::cameraAt(*stage, *p, time, metres, s);
    // Its matrix's rows: its x, y and z in the world, then where it is.
    const usd::Matrix& w = s.world;
    const Vec3 y(static_cast<float>(w.at(1, 0)), static_cast<float>(w.at(1, 1)), static_cast<float>(w.at(1, 2)));
    const Vec3 z(static_cast<float>(w.at(2, 0)), static_cast<float>(w.at(2, 1)), static_cast<float>(w.at(2, 2)));
    out.position = Vec3(static_cast<float>(w.at(3, 0)), static_cast<float>(w.at(3, 1)), static_cast<float>(w.at(3, 2)));
    out.rotation = Camera::rotationFor(z * -1.0f, y);
    const double h = s.horizontalAperture > 0.0 ? s.horizontalAperture : 20.955;
    const double v = s.verticalAperture > 0.0 ? s.verticalAperture : 15.2908;
    out.width = std::clamp(width, 16, 8192);
    double tall = height > 0 ? height : out.width * v / h;
    if (!(tall >= 16.0)) tall = 16.0;  // NaN too
    out.height = static_cast<int>(std::lround(std::min(tall, 8192.0)));
    // Our picture is 24 mm high: as wide a view as the film back's width.
    out.focal = static_cast<float>(24.0 * out.aspect() * s.focalLength / h);
    out = out.sanitized();
    if (warnings) {
        if (s.orthographic) warnings->push_back(p->path + " is orthographic: drawn in perspective");
        if (s.horizontalApertureOffset != 0.0 || s.verticalApertureOffset != 0.0) {
            warnings->push_back(p->path + " shifts its film back (aperture offset): drawn without the shift");
        }
    }
    if (varies) *varies = usd::cameraVaries(*stage, *p);
    return true;
}

}  // namespace pg::sim
