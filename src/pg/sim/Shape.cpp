#include "pg/sim/Shape.h"

#include <algorithm>
#include <cmath>

namespace pg::sim {
namespace {

constexpr float kRadians = 0.01745329251994329577f;  // per degree
constexpr float kDegrees = 57.2957795130823208768f;  // per radian

float smoothstep(float edge0, float edge1, float x) {
    const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

/// `angle` plus the multiple of 360 that brings it nearest `near`.
float unwrap(float angle, float near) { return angle + 360.0f * std::round((near - angle) / 360.0f); }

Vec3 unwrap(const Vec3& a, const Vec3& near) {
    return {unwrap(a.x, near.x), unwrap(a.y, near.y), unwrap(a.z, near.z)};
}

float distance2(const Vec3& a, const Vec3& b) {
    const Vec3 d = a - b;
    return dot(d, d);
}

/// The smallest root of a t^2 + 2 b t + c = 0 at or past tMin, if any.
bool firstRoot(float a, float b, float c, float tMin, float& t) {
    if (std::fabs(a) < 1e-12f) {
        if (std::fabs(b) < 1e-12f) return false;
        t = -c / (2.0f * b);
        return t >= tMin;
    }
    const float disc = b * b - a * c;
    if (disc < 0.0f) return false;
    const float s = std::sqrt(disc);
    float t0 = (-b - s) / a, t1 = (-b + s) / a;
    if (t0 > t1) std::swap(t0, t1);
    if (t0 >= tMin) {
        t = t0;
        return true;
    }
    if (t1 >= tMin) {
        t = t1;
        return true;
    }
    return false;
}

}  // namespace

const char* shapeName(Shape shape) {
    switch (shape) {
        case Shape::Sphere: return "sphere";
        case Shape::Box: return "box";
        case Shape::Cylinder: return "cylinder";
        case Shape::Cone: return "cone";
        case Shape::Torus: return "torus";
    }
    return "?";
}

// --- rotations ---------------------------------------------------------------------

Rotation Rotation::then(const Rotation& first) const {
    Rotation r;
    r.x = apply(first.x);
    r.y = apply(first.y);
    r.z = apply(first.z);
    return r;
}

Rotation Rotation::fromEuler(const Vec3& degrees) {
    const float a = degrees.x * kRadians, b = degrees.y * kRadians, c = degrees.z * kRadians;
    const float ca = std::cos(a), sa = std::sin(a), cb = std::cos(b), sb = std::sin(b), cc = std::cos(c),
                sc = std::sin(c);
    // Rz Ry Rx, column by column. With all angles 0 it is exactly the
    // identity, so an unturned shape costs no rounding.
    Rotation r;
    r.x = Vec3(cc * cb, sc * cb, -sb);
    r.y = Vec3(cc * sb * sa - sc * ca, sc * sb * sa + cc * ca, cb * sa);
    r.z = Vec3(cc * sb * ca + sc * sa, sc * sb * ca - cc * sa, cb * ca);
    return r;
}

Vec3 Rotation::toEuler(const Vec3& near) const {
    // Row i, column j of Rz Ry Rx is component i of axis j: -sin b is x.z,
    // cos b sin a is y.z, cos b cos a is z.z, sin c cos b is x.y, cos c cos b is x.x.
    const float sb = std::clamp(-x.z, -1.0f, 1.0f);
    if (std::fabs(sb) < 0.99999f) {
        const float a = std::atan2(y.z, z.z) * kDegrees;
        const float b = std::asin(sb) * kDegrees;
        const float c = std::atan2(x.y, x.x) * kDegrees;
        // The same rotation, the other way round: a + 180, 180 - b, c + 180.
        const Vec3 one = unwrap(Vec3(a, b, c), near);
        const Vec3 other = unwrap(Vec3(a + 180.0f, 180.0f - b, c + 180.0f), near);
        return distance2(one, near) <= distance2(other, near) ? one : other;
    }
    // Turned a quarter about y: only a - c (b = 90) or a + c (b = -90) is
    // determined. Keep c where it was.
    const float c = near.z;
    if (sb > 0.0f) {
        const float a = std::atan2(y.x, y.y) * kDegrees + c;
        return unwrap(Vec3(a, 90.0f, c), near);
    }
    const float a = std::atan2(-y.x, y.y) * kDegrees - c;
    return unwrap(Vec3(a, -90.0f, c), near);
}

Rotation Rotation::about(const Vec3& axis, float degrees) {
    const Vec3 n = normalize(axis);
    if (length(n) < 0.5f) return Rotation();
    const float th = degrees * kRadians;
    const float c = std::cos(th), s = std::sin(th), t = 1.0f - c;
    Rotation r;
    r.x = Vec3(t * n.x * n.x + c, t * n.x * n.y + s * n.z, t * n.x * n.z - s * n.y);
    r.y = Vec3(t * n.x * n.y - s * n.z, t * n.y * n.y + c, t * n.y * n.z + s * n.x);
    r.z = Vec3(t * n.x * n.z + s * n.y, t * n.y * n.z - s * n.x, t * n.z * n.z + c);
    return r;
}

// --- shapes ------------------------------------------------------------------------

ShapeInstance::ShapeInstance(Shape shape, const Vec3& center, const Vec3& rotationDegrees, const Vec3& size)
    : shape_(shape), center_(center), turn_(Rotation::fromEuler(rotationDegrees)) {
    half_ = Vec3(std::max(size.x, 1e-4f) * 0.5f, std::max(size.y, 1e-4f) * 0.5f, std::max(size.z, 1e-4f) * 0.5f);
    if (shape_ == Shape::Torus) {
        // The tube at most half the outer radius: a ring keeps its hole.
        tube_ = std::min(half_.y, 0.5f * half_.x);
        ring_ = half_.x - tube_;
    }
}

float ShapeInstance::localDistance(const Vec3& q) const {
    const Vec3& h = half_;
    switch (shape_) {
        case Shape::Sphere: {
            // Inigo Quilez's estimate for an ellipsoid; exact for a ball.
            const Vec3 k(q.x / h.x, q.y / h.y, q.z / h.z);
            const float k0 = length(k);
            const float k1 = length(Vec3(k.x / h.x, k.y / h.y, k.z / h.z));
            if (k1 < 1e-12f) return -std::min({h.x, h.y, h.z});
            return k0 * (k0 - 1.0f) / k1;
        }
        case Shape::Box: {
            const Vec3 d(std::fabs(q.x) - h.x, std::fabs(q.y) - h.y, std::fabs(q.z) - h.z);
            const float outside = length(Vec3(std::max(d.x, 0.0f), std::max(d.y, 0.0f), std::max(d.z, 0.0f)));
            return outside + std::min(std::max({d.x, d.y, d.z}), 0.0f);
        }
        case Shape::Cylinder: {
            const float r = std::min(h.x, h.z);
            const float across = std::sqrt((q.x / h.x) * (q.x / h.x) + (q.z / h.z) * (q.z / h.z));
            const float dr = (across - 1.0f) * r, dy = std::fabs(q.y) - h.y;
            return std::min(std::max(dr, dy), 0.0f) + std::hypot(std::max(dr, 0.0f), std::max(dy, 0.0f));
        }
        case Shape::Cone: {
            // Made round -- base radius r -- then Quilez's capped cone with a
            // top radius of 0: base at -h.y, apex at +h.y.
            const float r = std::min(h.x, h.z);
            const float px = q.x * r / h.x, pz = q.z * r / h.z;
            const float qx = std::sqrt(px * px + pz * pz), qy = q.y;
            const float hh = h.y;
            const float cax = qx - std::min(qx, qy < 0.0f ? r : 0.0f), cay = std::fabs(qy) - hh;
            const float k2x = -r, k2y = 2.0f * hh;  // from the base rim to the apex
            const float along = std::clamp(((0.0f - qx) * k2x + (hh - qy) * k2y) / (k2x * k2x + k2y * k2y), 0.0f, 1.0f);
            const float cbx = qx + k2x * along, cby = qy - hh + k2y * along;
            const float s = cbx < 0.0f && cay < 0.0f ? -1.0f : 1.0f;
            return s * std::sqrt(std::min(cax * cax + cay * cay, cbx * cbx + cby * cby));
        }
        case Shape::Torus: {
            const float z = q.z * h.x / h.z;
            const float a = std::sqrt(q.x * q.x + z * z) - ring_;
            return std::sqrt(a * a + q.y * q.y) - tube_;
        }
    }
    return 1.0f;
}

float ShapeInstance::distance(const Vec3& p) const { return localDistance(toLocal(p)); }

bool ShapeInstance::contains(const Vec3& p) const { return distance(p) < 0.0f; }

Vec3 ShapeInstance::normal(const Vec3& p) const {
    const Vec3 q = toLocal(p);
    const float e = std::max(1e-4f, 1e-3f * std::min({half_.x, half_.y, half_.z}));
    const Vec3 g(localDistance(q + Vec3(e, 0, 0)) - localDistance(q - Vec3(e, 0, 0)),
                 localDistance(q + Vec3(0, e, 0)) - localDistance(q - Vec3(0, e, 0)),
                 localDistance(q + Vec3(0, 0, e)) - localDistance(q - Vec3(0, 0, e)));
    const Vec3 n = normalize(turn_.apply(g));
    return length(n) > 0.5f ? n : turn_.y;
}

float ShapeInstance::falloff(const Vec3& p) const {
    const Vec3 q = toLocal(p);
    const Vec3& h = half_;
    switch (shape_) {
        case Shape::Sphere: {
            const float rho = length(Vec3(q.x / h.x, q.y / h.y, q.z / h.z));
            return rho >= 1.0f ? 0.0f : 1.0f - smoothstep(0.6f, 1.0f, rho);
        }
        case Shape::Box: {
            float w = 1.0f;
            for (int a = 0; a < 3; ++a) {
                const float t = std::fabs(q[a]) / h[a];
                if (t >= 1.0f) return 0.0f;
                w *= 1.0f - smoothstep(0.75f, 1.0f, t);
            }
            return w;
        }
        case Shape::Cylinder:
        case Shape::Cone: {
            const float ty = std::fabs(q.y) / h.y;
            if (ty >= 1.0f) return 0.0f;
            // A cone's cross-section shrinks from the base (1) to the apex (0).
            const float room = shape_ == Shape::Cone ? 0.5f * (1.0f - q.y / h.y) : 1.0f;
            if (room <= 1e-6f) return 0.0f;
            const float rho = std::sqrt((q.x / h.x) * (q.x / h.x) + (q.z / h.z) * (q.z / h.z)) / room;
            if (rho >= 1.0f) return 0.0f;
            return (1.0f - smoothstep(0.6f, 1.0f, rho)) * (1.0f - smoothstep(0.75f, 1.0f, ty));
        }
        case Shape::Torus: {
            const float z = q.z * h.x / h.z;
            const float a = std::sqrt(q.x * q.x + z * z) - ring_;
            const float d = std::sqrt(a * a + q.y * q.y) / std::max(tube_, 1e-6f);
            return d >= 1.0f ? 0.0f : 1.0f - smoothstep(0.6f, 1.0f, d);
        }
    }
    return 0.0f;
}

bool ShapeInstance::intersect(const Vec3& origin, const Vec3& dir, float tMin, float& tHit, Vec3& normalOut) const {
    // The ray in the shape's own axes; t stays the same.
    const Vec3 lo = toLocal(origin), ld = turn_.inverse(dir);
    const Vec3& h = half_;
    if (shape_ == Shape::Torus) {
        // Sphere tracing the exact distance of the round torus in a space
        // where z is squeezed to x's scale; t is the ray's own parameter.
        const float k = h.x / h.z;
        const Vec3 o(lo.x, lo.y, lo.z * k), d(ld.x, ld.y, ld.z * k);
        const float speed = length(d);
        if (speed < 1e-12f) return false;
        // Start where the ray enters the box round the torus.
        const Vec3 box(h.x, tube_, h.x);
        float t0 = tMin, t1 = 1e30f;
        for (int a = 0; a < 3; ++a) {
            if (std::fabs(d[a]) < 1e-12f) {
                if (std::fabs(o[a]) > box[a]) return false;
                continue;
            }
            float ta = (-box[a] - o[a]) / d[a], tb = (box[a] - o[a]) / d[a];
            if (ta > tb) std::swap(ta, tb);
            t0 = std::max(t0, ta);
            t1 = std::min(t1, tb);
        }
        if (t0 > t1) return false;
        auto sdf = [&](float t) {
            const Vec3 p = o + d * t;
            const float a = std::sqrt(p.x * p.x + p.z * p.z) - ring_;
            return std::sqrt(a * a + p.y * p.y) - tube_;
        };
        float t = t0;
        const bool startInside = sdf(t) < 0.0f;
        const float eps = 1e-5f * std::max(h.x, 1e-3f);
        for (int i = 0; i < 160 && t <= t1; ++i) {
            const float s = sdf(t);
            if (startInside ? s >= -eps : s <= eps) {
                tHit = t;
                normalOut = normal(origin + dir * t);
                return true;
            }
            t += std::max(std::fabs(s), eps) / speed;
        }
        return false;
    }

    // The unit space: the shape fills [-1, 1] along each of its axes.
    const Vec3 o(lo.x / h.x, lo.y / h.y, lo.z / h.z), d(ld.x / h.x, ld.y / h.y, ld.z / h.z);
    float best = 1e30f;
    Vec3 unitNormal;
    auto consider = [&](float t, const Vec3& n) {
        if (t >= tMin && t < best) {
            best = t;
            unitNormal = n;
        }
    };
    switch (shape_) {
        case Shape::Sphere: {
            float t = 0.0f;
            if (firstRoot(dot(d, d), dot(o, d), dot(o, o) - 1.0f, tMin, t)) consider(t, o + d * t);
            break;
        }
        case Shape::Box: {
            float tNear = -1e30f, tFar = 1e30f;
            int nearAxis = 0, farAxis = 0;
            for (int a = 0; a < 3; ++a) {
                if (std::fabs(d[a]) < 1e-12f) {
                    if (std::fabs(o[a]) > 1.0f) return false;
                    continue;
                }
                float ta = (-1.0f - o[a]) / d[a], tb = (1.0f - o[a]) / d[a];
                if (ta > tb) std::swap(ta, tb);
                if (ta > tNear) {
                    tNear = ta;
                    nearAxis = a;
                }
                if (tb < tFar) {
                    tFar = tb;
                    farAxis = a;
                }
            }
            if (tNear > tFar) return false;
            for (const auto& [t, axis] : {std::pair<float, int>{tNear, nearAxis}, {tFar, farAxis}}) {
                Vec3 n;
                n[axis] = (o + d * t)[axis] > 0.0f ? 1.0f : -1.0f;
                consider(t, n);
            }
            break;
        }
        case Shape::Cylinder: {
            float t = 0.0f;
            // The side: x^2 + z^2 = 1, both roots, within |y| <= 1.
            const float a = d.x * d.x + d.z * d.z, b = o.x * d.x + o.z * d.z, c = o.x * o.x + o.z * o.z - 1.0f;
            const float disc = b * b - a * c;
            if (a > 1e-12f && disc >= 0.0f) {
                for (const float s : {-1.0f, 1.0f}) {
                    t = (-b + s * std::sqrt(disc)) / a;
                    const Vec3 p = o + d * t;
                    if (std::fabs(p.y) <= 1.0f) consider(t, Vec3(p.x, 0.0f, p.z));
                }
            }
            // The two caps.
            if (std::fabs(d.y) > 1e-12f) {
                for (const float y : {-1.0f, 1.0f}) {
                    t = (y - o.y) / d.y;
                    const Vec3 p = o + d * t;
                    if (p.x * p.x + p.z * p.z <= 1.0f) consider(t, Vec3(0.0f, y, 0.0f));
                }
            }
            break;
        }
        case Shape::Cone: {
            // x^2 + z^2 = k^2 (1 - y)^2 with k = 1/2: base radius 1 at y = -1,
            // the apex at y = 1.
            const float k2 = 0.25f;
            const float w = 1.0f - o.y, dw = -d.y;
            const float a = d.x * d.x + d.z * d.z - k2 * dw * dw;
            const float b = o.x * d.x + o.z * d.z - k2 * w * dw;
            const float c = o.x * o.x + o.z * o.z - k2 * w * w;
            if (std::fabs(a) > 1e-12f) {
                const float disc = b * b - a * c;
                if (disc >= 0.0f) {
                    for (const float s : {-1.0f, 1.0f}) {
                        const float t = (-b + s * std::sqrt(disc)) / a;
                        const Vec3 p = o + d * t;
                        if (p.y >= -1.0f && p.y <= 1.0f) consider(t, Vec3(p.x, k2 * (1.0f - p.y), p.z));
                    }
                }
            } else if (std::fabs(b) > 1e-12f) {
                const float t = -c / (2.0f * b);
                const Vec3 p = o + d * t;
                if (p.y >= -1.0f && p.y <= 1.0f) consider(t, Vec3(p.x, k2 * (1.0f - p.y), p.z));
            }
            if (std::fabs(d.y) > 1e-12f) {
                const float t = (-1.0f - o.y) / d.y;
                const Vec3 p = o + d * t;
                if (p.x * p.x + p.z * p.z <= 1.0f) consider(t, Vec3(0.0f, -1.0f, 0.0f));
            }
            break;
        }
        case Shape::Torus: break;
    }
    if (best >= 1e29f) return false;
    tHit = best;
    // A normal in unit space goes back through the inverse transpose: divide
    // by the half sizes, then turn.
    normalOut = normalize(turn_.apply(Vec3(unitNormal.x / h.x, unitNormal.y / h.y, unitNormal.z / h.z)));
    return true;
}

void ShapeInstance::bounds(Vec3& lo, Vec3& hi) const {
    lo = Vec3(1e30f);
    hi = Vec3(-1e30f);
    const Vec3 h(half_.x, shape_ == Shape::Torus ? tube_ : half_.y, half_.z);
    for (int c = 0; c < 8; ++c) {
        const Vec3 corner((c & 1) ? h.x : -h.x, (c & 2) ? h.y : -h.y, (c & 4) ? h.z : -h.z);
        const Vec3 p = toWorld(corner);
        for (int a = 0; a < 3; ++a) {
            lo[a] = std::min(lo[a], p[a]);
            hi[a] = std::max(hi[a], p[a]);
        }
    }
}

}  // namespace pg::sim
