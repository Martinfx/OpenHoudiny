#include "Gizmo.h"

#include "Theme.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace pg::editor {
namespace {

constexpr float kPi = 3.14159265358979323846f;

const ImU32 kAxisColor[3] = {IM_COL32(232, 88, 88, 255), IM_COL32(122, 212, 98, 255), IM_COL32(92, 142, 244, 255)};
const ImU32 kHot = IM_COL32(255, 213, 79, 255);
const ImU32 kCenter = IM_COL32(236, 236, 240, 255);

float lengthOf(ImVec2 v) { return std::sqrt(v.x * v.x + v.y * v.y); }
ImVec2 sub(ImVec2 a, ImVec2 b) { return ImVec2(a.x - b.x, a.y - b.y); }
ImVec2 add(ImVec2 a, ImVec2 b) { return ImVec2(a.x + b.x, a.y + b.y); }
ImVec2 scaled(ImVec2 a, float s) { return ImVec2(a.x * s, a.y * s); }

float toSegment(ImVec2 p, ImVec2 a, ImVec2 b) { return pixelsToSegment(p, a, b); }

/// Inside the convex quad a b c d (either winding)?
bool inQuad(ImVec2 p, const ImVec2 q[4]) {
    float sign = 0.0f;
    for (int i = 0; i < 4; ++i) {
        const ImVec2 a = q[i], b = q[(i + 1) % 4];
        const float cross = (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
        if (cross == 0.0f) continue;
        if (sign == 0.0f) sign = cross;
        else if ((cross > 0.0f) != (sign > 0.0f)) return false;
    }
    return true;
}

/// Where along the line through `p` in direction `a` it comes closest to
/// the ray o + t d: the parameter s of p + s a. False when they are parallel.
bool closestAlong(const Vec3& p, const Vec3& a, const Vec3& o, const Vec3& d, float& s) {
    const Vec3 w = p - o;
    const float aa = dot(a, a), ab = dot(a, d), dd = dot(d, d), aw = dot(a, w), dw = dot(d, w);
    const float den = aa * dd - ab * ab;
    if (std::fabs(den) < 1e-6f * aa * dd) return false;
    s = (ab * dw - dd * aw) / den;
    return true;
}

/// Where the ray meets the plane through p with normal n.
bool onPlane(const Vec3& p, const Vec3& n, const Vec3& o, const Vec3& d, Vec3& out) {
    const float den = dot(n, d);
    if (std::fabs(den) < 1e-5f) return false;
    const float t = dot(n, p - o) / den;
    if (t < 0.0f) return false;
    out = o + d * t;
    return true;
}

float roundTo(float v, float step) { return step > 0.0f ? std::round(v / step) * step : v; }

int axisOf(Handle h) {
    switch (h) {
        case Handle::AxisX: case Handle::PlaneYZ: return 0;
        case Handle::AxisY: case Handle::PlaneXZ: return 1;
        case Handle::AxisZ: case Handle::PlaneXY: return 2;
        default: return -1;
    }
}

bool isAxis(Handle h) { return h == Handle::AxisX || h == Handle::AxisY || h == Handle::AxisZ; }
bool isPlane(Handle h) { return h == Handle::PlaneYZ || h == Handle::PlaneXZ || h == Handle::PlaneXY; }

}  // namespace

// --- the camera ----------------------------------------------------------------------

float pixelsToSegment(ImVec2 p, ImVec2 a, ImVec2 b, float* along) {
    const ImVec2 ab = sub(b, a), ap = sub(p, a);
    const float len2 = ab.x * ab.x + ab.y * ab.y;
    const float t = len2 > 1e-6f ? std::clamp((ap.x * ab.x + ap.y * ab.y) / len2, 0.0f, 1.0f) : 0.0f;
    if (along) *along = t;
    return lengthOf(sub(p, add(a, scaled(ab, t))));
}

ViewCamera ViewCamera::of(const gl::Orbit& orbit, ImVec2 lo, ImVec2 size) {
    ViewCamera c;
    float e[3], f[3], r[3], u[3];
    orbit.eye(e);
    orbit.axes(f, r, u);
    c.eye = Vec3(e[0], e[1], e[2]);
    c.forward = Vec3(f[0], f[1], f[2]);
    c.right = Vec3(r[0], r[1], r[2]);
    c.up = Vec3(u[0], u[1], u[2]);
    c.tanHalfFov = std::tan(orbit.fovY * kPi / 360.0f);
    c.aspect = size.x / std::max(size.y, 1.0f);
    c.lo = lo;
    c.size = size;
    return c;
}

void ViewCamera::ray(ImVec2 screen, Vec3& origin, Vec3& dir) const {
    const float x = (screen.x - lo.x) / std::max(size.x, 1.0f) * 2.0f - 1.0f;
    const float y = 1.0f - (screen.y - lo.y) / std::max(size.y, 1.0f) * 2.0f;
    origin = eye;
    dir = normalize(forward + right * (x * tanHalfFov * aspect) + up * (y * tanHalfFov));
}

bool ViewCamera::toScreen(const Vec3& p, ImVec2& out) const {
    const Vec3 v = p - eye;
    const float z = dot(v, forward);
    if (z <= 1e-4f) return false;
    const float x = dot(v, right) / (z * tanHalfFov * aspect);
    const float y = dot(v, up) / (z * tanHalfFov);
    out = ImVec2(lo.x + (x + 1.0f) * 0.5f * size.x, lo.y + (1.0f - y) * 0.5f * size.y);
    return true;
}

float ViewCamera::pixel(const Vec3& p) const {
    const float z = std::max(dot(p - eye, forward), 1e-3f);
    return 2.0f * z * tanHalfFov / std::max(size.y, 1.0f);
}

// --- the gizmo --------------------------------------------------------------------------

float Gizmo::screenLength() { return theme::px(92.0f); }

Handle Gizmo::pick(const ViewCamera& cam, GizmoMode mode, const Vec3& pivot, const sim::Rotation& frame,
                   ImVec2 mouse) const {
    ImVec2 c;
    if (mode == GizmoMode::Select || !cam.toScreen(pivot, c)) return Handle::None;
    const float length = cam.pixel(pivot) * screenLength();
    const float reach = theme::px(8.0f);
    if (mode == GizmoMode::Rotate) {
        Handle best = Handle::None;
        float bestDistance = reach;
        const float viewRing = screenLength() * 1.12f;
        const float toView = std::fabs(lengthOf(sub(mouse, c)) - viewRing);
        if (toView < bestDistance) {
            best = Handle::View;
            bestDistance = toView;
        }
        const Vec3 toEye = normalize(cam.eye - pivot);
        for (int i = 0; i < 3; ++i) {
            const Vec3 u = frame.axis((i + 1) % 3), v = frame.axis((i + 2) % 3);
            ImVec2 previous;
            bool havePrevious = false;
            for (int s = 0; s <= 64; ++s) {
                const float a = 2.0f * kPi * static_cast<float>(s) / 64.0f;
                const Vec3 r = u * std::cos(a) + v * std::sin(a);
                ImVec2 q;
                const bool front = dot(r, toEye) >= -0.05f;
                if (!cam.toScreen(pivot + r * (0.9f * length), q)) {
                    havePrevious = false;
                    continue;
                }
                if (havePrevious && front) {
                    const float dist = toSegment(mouse, previous, q);
                    if (dist < bestDistance) {
                        bestDistance = dist;
                        best = i == 0 ? Handle::AxisX : i == 1 ? Handle::AxisY : Handle::AxisZ;
                    }
                }
                previous = q;
                havePrevious = true;
            }
        }
        return best;
    }

    // Move and scale: the middle first, then the axes, then (move) the planes.
    if (lengthOf(sub(mouse, c)) < theme::px(mode == GizmoMode::Scale ? 9.0f : 8.0f)) return Handle::Center;
    Handle best = Handle::None;
    float bestDistance = reach;
    for (int i = 0; i < 3; ++i) {
        ImVec2 end;
        if (!cam.toScreen(pivot + frame.axis(i) * length, end)) continue;
        const ImVec2 along = sub(end, c);
        const float screen = lengthOf(along);
        if (screen < theme::px(12.0f)) continue;  // pointing at the eye: nothing to drag
        // The arrow head, or the cube, sticks out past the end.
        const ImVec2 tip = add(end, scaled(along, theme::px(12.0f) / screen));
        const float dist = toSegment(mouse, add(c, scaled(along, theme::px(14.0f) / screen)), tip);
        if (dist < bestDistance) {
            bestDistance = dist;
            best = i == 0 ? Handle::AxisX : i == 1 ? Handle::AxisY : Handle::AxisZ;
        }
    }
    if (best != Handle::None || mode != GizmoMode::Move) return best;
    for (int i = 0; i < 3; ++i) {
        const Vec3 u = frame.axis((i + 1) % 3), v = frame.axis((i + 2) % 3);
        const Vec3 corners[4] = {u * 0.22f + v * 0.22f, u * 0.48f + v * 0.22f, u * 0.48f + v * 0.48f, u * 0.22f + v * 0.48f};
        ImVec2 q[4];
        bool visible = true;
        for (int k = 0; k < 4; ++k) visible = visible && cam.toScreen(pivot + corners[k] * length, q[k]);
        if (visible && inQuad(mouse, q)) return i == 0 ? Handle::PlaneYZ : i == 1 ? Handle::PlaneXZ : Handle::PlaneXY;
    }
    return Handle::None;
}

void Gizmo::begin(const ViewCamera& cam, GizmoMode mode, const Vec3& pivot, const sim::Rotation& frame, ImVec2 mouse) {
    drag_ = GizmoDrag();
    drag_.mode = mode;
    drag_.handle = hovered_;
    pivot_ = pivot;
    frame_ = frame;
    mouse0_ = mouse;
    worldLength_ = cam.pixel(pivot) * screenLength();
    angle_ = 0.0f;
    Vec3 o, d;
    cam.ray(mouse, o, d);
    const int a = axisOf(hovered_);
    if (isAxis(hovered_)) {
        if (!closestAlong(pivot, frame.axis(a), o, d, along0_)) along0_ = 0.0f;
    } else if (isPlane(hovered_) || (hovered_ == Handle::Center && mode == GizmoMode::Move)) {
        planeNormal_ = isPlane(hovered_) ? frame.axis(a) : cam.forward;
        if (!onPlane(pivot, planeNormal_, o, d, plane0_)) plane0_ = pivot;
    }
    ImVec2 c;
    if (cam.toScreen(pivot, c)) lastAngle_ = std::atan2(mouse.y - c.y, mouse.x - c.x);
}

void Gizmo::follow(const ViewCamera& cam, ImVec2 mouse, bool snap, const Snap& steps) {
    Vec3 o, d;
    cam.ray(mouse, o, d);
    const Handle h = drag_.handle;
    const int a = axisOf(h);
    switch (drag_.mode) {
        case GizmoMode::Select: break;
        case GizmoMode::Move: {
            if (isAxis(h)) {
                float s = 0.0f;
                if (!closestAlong(pivot_, frame_.axis(a), o, d, s)) break;
                // No further than the camera could still see: a ray almost
                // along the axis would throw it to infinity.
                float delta = std::clamp(s - along0_, -200.0f * worldLength_, 200.0f * worldLength_);
                if (snap) delta = roundTo(delta, steps.move);
                drag_.move = frame_.axis(a) * delta;
            } else {
                Vec3 x;
                if (!onPlane(pivot_, planeNormal_, o, d, x)) break;
                Vec3 m = x - plane0_;
                if (length(m) > 200.0f * worldLength_) m = normalize(m) * (200.0f * worldLength_);
                if (snap) {
                    // Rounded along the gizmo's own axes.
                    const Vec3 local(roundTo(dot(m, frame_.x), steps.move), roundTo(dot(m, frame_.y), steps.move),
                                     roundTo(dot(m, frame_.z), steps.move));
                    m = frame_.apply(local);
                    if (isPlane(h)) m = m - planeNormal_ * dot(m, planeNormal_);
                }
                drag_.move = m;
            }
            break;
        }
        case GizmoMode::Rotate: {
            ImVec2 c;
            if (!cam.toScreen(pivot_, c)) break;
            const float now = std::atan2(mouse.y - c.y, mouse.x - c.x);
            float step = now - lastAngle_;
            while (step > kPi) step -= 2.0f * kPi;
            while (step < -kPi) step += 2.0f * kPi;
            angle_ += step;
            lastAngle_ = now;
            // Screen y points down: turning clockwise on screen is turning
            // anticlockwise about an axis that points at the eye.
            const Vec3 toEye = normalize(cam.eye - pivot_);
            const Vec3 axis = h == Handle::View ? toEye : frame_.axis(a);
            float degrees = -angle_ * 180.0f / kPi * (dot(axis, toEye) >= 0.0f ? 1.0f : -1.0f);
            if (snap) degrees = roundTo(degrees, steps.degrees);
            drag_.axis = axis;
            drag_.degrees = degrees;
            break;
        }
        case GizmoMode::Scale: {
            float factor = 1.0f;
            if (isAxis(h)) {
                float s = 0.0f;
                if (!closestAlong(pivot_, frame_.axis(a), o, d, s) || std::fabs(along0_) < 1e-6f) break;
                factor = s / along0_;
            } else {
                factor = std::exp((mouse.x - mouse0_.x) / theme::px(100.0f));
            }
            factor = std::clamp(factor, 0.01f, 100.0f);
            if (snap) factor = std::max(steps.scale, roundTo(factor, steps.scale));
            drag_.scale = Vec3(1.0f);
            if (isAxis(h)) drag_.scale[a] = factor;
            else drag_.scale = Vec3(factor);
            break;
        }
    }
}

void Gizmo::cancel() {
    dragging_ = false;
    ended_ = false;
    hovered_ = Handle::None;
    drag_ = GizmoDrag();
}

bool Gizmo::update(ImDrawList* draw, const ViewCamera& cam, GizmoMode mode, const Vec3& pivot,
                   const sim::Rotation& frame, bool free, bool snap, const Snap& steps) {
    began_ = ended_ = false;
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const bool inside = mouse.x >= cam.lo.x && mouse.y >= cam.lo.y && mouse.x < cam.lo.x + cam.size.x &&
                        mouse.y < cam.lo.y + cam.size.y;
    if (dragging_) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            follow(cam, mouse, snap, steps);
        } else {
            dragging_ = false;
            ended_ = true;
        }
    } else {
        hovered_ = free && inside && mode != GizmoMode::Select ? pick(cam, mode, pivot, frame, mouse) : Handle::None;
        if (hovered_ != Handle::None && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            begin(cam, mode, pivot, frame, mouse);
            dragging_ = true;
            began_ = true;
            follow(cam, mouse, snap, steps);
        }
    }
    if (mode != GizmoMode::Select) paint(draw, cam, mode, pivot, frame);
    return dragging_ || ended_ || hovered_ != Handle::None;
}

void Gizmo::paint(ImDrawList* d, const ViewCamera& cam, GizmoMode mode, const Vec3& pivot,
                  const sim::Rotation& frame) const {
    ImVec2 c;
    if (!cam.toScreen(pivot, c)) return;
    const Handle hot = dragging_ ? drag_.handle : hovered_;
    const float length = cam.pixel(pivot) * screenLength();
    const float w = theme::px(2.2f);
    auto colour = [&](int axis, Handle h) { return hot == h ? kHot : kAxisColor[axis]; };
    const Handle axes[3] = {Handle::AxisX, Handle::AxisY, Handle::AxisZ};
    const Handle planes[3] = {Handle::PlaneYZ, Handle::PlaneXZ, Handle::PlaneXY};
    // Farthest first: the axis towards the eye is drawn on top.
    int order[3] = {0, 1, 2};
    std::sort(order, order + 3, [&](int a, int b) { return dot(frame.axis(a), cam.forward) > dot(frame.axis(b), cam.forward); });

    if (mode == GizmoMode::Rotate) {
        const Vec3 toEye = normalize(cam.eye - pivot);
        // The ring round the view.
        const float viewRing = screenLength() * 1.12f;
        d->AddCircle(c, viewRing, hot == Handle::View ? kHot : IM_COL32(210, 212, 220, 200), 72,
                     hot == Handle::View ? w * 1.4f : w * 0.9f);
        for (const int i : order) {
            const Vec3 u = frame.axis((i + 1) % 3), v = frame.axis((i + 2) % 3);
            const ImU32 col = colour(i, axes[i]);
            ImVec2 previous;
            bool havePrevious = false;
            for (int s = 0; s <= 64; ++s) {
                const float a = 2.0f * kPi * static_cast<float>(s) / 64.0f;
                const Vec3 r = u * std::cos(a) + v * std::sin(a);
                ImVec2 q;
                if (!cam.toScreen(pivot + r * (0.9f * length), q)) {
                    havePrevious = false;
                    continue;
                }
                if (havePrevious) {
                    const bool front = dot(r, toEye) >= -0.05f;
                    d->AddLine(previous, q, front ? col : theme::fade(col, 0.28f),
                               front ? (hot == axes[i] ? w * 1.5f : w) : w * 0.7f);
                }
                previous = q;
                havePrevious = true;
            }
        }
        // While turning: the angle swept, and how far.
        if (dragging_) {
            const float r = screenLength() * 0.55f;
            const float start = lastAngle_ - angle_;
            const int n = std::max(2, static_cast<int>(std::fabs(angle_) / (2.0f * kPi) * 64.0f) + 2);
            d->PathLineTo(c);
            for (int s = 0; s <= n; ++s) {
                const float a = start + angle_ * static_cast<float>(s) / static_cast<float>(n);
                d->PathLineTo(ImVec2(c.x + std::cos(a) * r, c.y + std::sin(a) * r));
            }
            d->PathFillConcave(IM_COL32(255, 213, 79, 60));
            d->AddLine(c, ImVec2(c.x + std::cos(start) * r, c.y + std::sin(start) * r), IM_COL32(255, 213, 79, 160), w * 0.8f);
            d->AddLine(c, ImVec2(c.x + std::cos(lastAngle_) * r, c.y + std::sin(lastAngle_) * r), kHot, w);
        }
        d->AddCircleFilled(c, theme::px(3.0f), kCenter);
    } else {
        // The planes (move), faint squares between two axes.
        if (mode == GizmoMode::Move) {
            for (const int i : order) {
                const Vec3 u = frame.axis((i + 1) % 3), v = frame.axis((i + 2) % 3);
                const Vec3 corners[4] = {u * 0.22f + v * 0.22f, u * 0.48f + v * 0.22f, u * 0.48f + v * 0.48f,
                                         u * 0.22f + v * 0.48f};
                ImVec2 q[4];
                bool visible = true;
                for (int k = 0; k < 4; ++k) visible = visible && cam.toScreen(pivot + corners[k] * length, q[k]);
                if (!visible) continue;
                // Seen edge on, a plane is no use: fade it out.
                const float facing = std::fabs(dot(frame.axis(i), cam.forward));
                if (facing < 0.15f) continue;
                const ImU32 col = hot == planes[i] ? kHot : kAxisColor[i];
                d->AddQuadFilled(q[0], q[1], q[2], q[3], theme::fade(col, hot == planes[i] ? 0.55f : 0.3f));
                d->AddQuad(q[0], q[1], q[2], q[3], theme::fade(col, 0.8f), w * 0.6f);
            }
        }
        // While moving along an axis: the line it slides on.
        if (dragging_ && mode == GizmoMode::Move && isAxis(drag_.handle)) {
            const Vec3 a = frame_.axis(axisOf(drag_.handle));
            ImVec2 p0, p1;
            if (cam.toScreen(pivot_ - a * (50.0f * length), p0) && cam.toScreen(pivot_ + a * (50.0f * length), p1)) {
                d->AddLine(p0, p1, theme::fade(kAxisColor[axisOf(drag_.handle)], 0.5f), w * 0.6f);
            }
        }
        for (const int i : order) {
            // A scaled axis grows as it is dragged.
            const float stretch = dragging_ && mode == GizmoMode::Scale ? std::clamp(drag_.scale[i], 0.1f, 4.0f) : 1.0f;
            ImVec2 end;
            if (!cam.toScreen(pivot + frame.axis(i) * (length * stretch), end)) continue;
            const ImVec2 along = sub(end, c);
            const float screen = lengthOf(along);
            if (screen < theme::px(4.0f)) continue;
            const ImVec2 dir = scaled(along, 1.0f / screen), across(-dir.y, dir.x);
            const float fade = std::clamp((screen - theme::px(6.0f)) / theme::px(20.0f), 0.0f, 1.0f);
            const ImU32 col = theme::fade(colour(i, axes[i]), fade);
            const float thick = hot == axes[i] ? w * 1.5f : w;
            if (mode == GizmoMode::Move) {
                d->AddLine(add(c, scaled(dir, theme::px(10.0f))), end, col, thick);
                const ImVec2 tip = add(end, scaled(dir, theme::px(14.0f)));
                const ImVec2 left = add(end, scaled(across, theme::px(5.5f))), right = sub(end, scaled(across, theme::px(5.5f)));
                d->AddTriangleFilled(tip, left, right, col);
            } else {
                d->AddLine(add(c, scaled(dir, theme::px(10.0f))), end, col, thick);
                const float half = theme::px(hot == axes[i] ? 6.0f : 5.0f);
                d->AddRectFilled(ImVec2(end.x - half, end.y - half), ImVec2(end.x + half, end.y + half), col, theme::px(1.5f));
            }
        }
        // The middle: a dot to move in the screen's plane, a cube to scale all.
        const ImU32 mid = hot == Handle::Center ? kHot : kCenter;
        if (mode == GizmoMode::Move) {
            d->AddCircle(c, theme::px(7.0f), mid, 24, w * 0.9f);
            d->AddCircleFilled(c, theme::px(3.0f), mid);
        } else {
            const float half = theme::px(hot == Handle::Center ? 7.5f : 6.5f);
            d->AddRectFilled(ImVec2(c.x - half, c.y - half), ImVec2(c.x + half, c.y + half), theme::fade(mid, 0.9f), theme::px(1.5f));
        }
    }

    // What the drag has done, by the mouse.
    if (dragging_) {
        char text[64] = "";
        switch (drag_.mode) {
            case GizmoMode::Move: {
                const Vec3& m = drag_.move;
                if (isAxis(drag_.handle)) std::snprintf(text, sizeof text, "%+.3f m", static_cast<double>(dot(m, frame_.axis(axisOf(drag_.handle)))));
                else std::snprintf(text, sizeof text, "%+.3f  %+.3f  %+.3f m", static_cast<double>(m.x), static_cast<double>(m.y), static_cast<double>(m.z));
                break;
            }
            case GizmoMode::Rotate: std::snprintf(text, sizeof text, "%+.1f\xc2\xb0", static_cast<double>(drag_.degrees)); break;
            case GizmoMode::Scale: {
                const int a = axisOf(drag_.handle);
                std::snprintf(text, sizeof text, "\xc3\x97%.2f", static_cast<double>(a >= 0 ? drag_.scale[a] : drag_.scale.x));
                break;
            }
            case GizmoMode::Select: break;
        }
        const ImVec2 m = ImGui::GetIO().MousePos;
        const ImVec2 at(m.x + theme::px(16.0f), m.y + theme::px(10.0f));
        const ImVec2 size = ImGui::CalcTextSize(text);
        d->AddRectFilled(ImVec2(at.x - theme::px(5.0f), at.y - theme::px(3.0f)),
                         ImVec2(at.x + size.x + theme::px(5.0f), at.y + size.y + theme::px(3.0f)), IM_COL32(20, 21, 24, 220),
                         theme::px(4.0f));
        d->AddText(at, kHot, text);
    }
}

}  // namespace pg::editor
