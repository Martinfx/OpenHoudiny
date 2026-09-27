#pragma once
//
// The viewport's transform gizmo: the handles that move, turn and size what
// is selected, as in any 3D program.
//
//   move     an arrow along each axis, a square for each plane between two
//            axes, and a dot in the middle that moves in the plane of the
//            screen;
//   rotate   a ring about each axis, and one round the view;
//   scale    each axis ending in a cube, and a cube in the middle for all
//            three at once.
//
// X is red, Y green, Z blue; the handle under the mouse, and the one being
// dragged, turn yellow. The gizmo is the same size on screen at any
// distance. It is drawn over the viewport's image with ImGui's draw list; the
// math of a drag -- where the mouse ray meets an axis or a plane -- is done
// in the world.
//
// The gizmo does not know what it moves. A drag reports how far it has gone
// since it began (GizmoDrag); the caller applies that to the values it had
// then, so rounding never builds up and Escape puts everything back.
//
#include "pg/core/Types.h"
#include "pg/gl/Camera.h"
#include "pg/sim/Shape.h"

#include "imgui.h"

namespace pg::editor {

/// The viewport's camera as the mouse sees it.
struct ViewCamera {
    Vec3 eye, forward{0.0f, 0.0f, -1.0f}, right{1.0f, 0.0f, 0.0f}, up{0.0f, 1.0f, 0.0f};
    float tanHalfFov = 0.3f;  ///< vertical
    float aspect = 1.0f;
    ImVec2 lo, size;          ///< the viewport's rectangle on screen

    /// What `orbit` sees -- its axes, roll and field of view -- in the
    /// rectangle `lo`, `size` of the screen.
    static ViewCamera of(const gl::Orbit& orbit, ImVec2 lo, ImVec2 size);
    /// The ray under a point of the screen: from the eye, of unit length.
    void ray(ImVec2 screen, Vec3& origin, Vec3& dir) const;
    /// Where a world point lands on screen; false when it is behind the eye.
    bool toScreen(const Vec3& p, ImVec2& out) const;
    /// World units a pixel covers at the distance of p.
    float pixel(const Vec3& p) const;
    /// Distance from the eye along the view direction.
    float depth(const Vec3& p) const { return dot(p - eye, forward); }
};

/// Pixels from p to the segment a-b, and where along it (0 at a, 1 at b)
/// the nearest point is.
float pixelsToSegment(ImVec2 p, ImVec2 a, ImVec2 b, float* along = nullptr);

enum class GizmoMode { Select, Move, Rotate, Scale };

/// A part of the gizmo.
enum class Handle { None, AxisX, AxisY, AxisZ, PlaneYZ, PlaneXZ, PlaneXY, Center, View };

/// What a drag has done since it began.
struct GizmoDrag {
    GizmoMode mode = GizmoMode::Move;
    Handle handle = Handle::None;
    Vec3 move;                       ///< Move: world units
    Vec3 axis{0.0f, 1.0f, 0.0f};     ///< Rotate: the world axis turned about
    float degrees = 0.0f;            ///< Rotate: how far, right-handed about `axis`
    Vec3 scale{1.0f, 1.0f, 1.0f};    ///< Scale: factors along the gizmo's own axes
};

/// Steps the gizmo snaps to while snapping is on (or Ctrl held).
struct Snap {
    float move = 0.05f;    ///< m
    float degrees = 15.0f;
    float scale = 0.1f;    ///< of the size it had
};

class Gizmo {
public:
    /// Draws the gizmo at `pivot`, its axes those of `frame`, into `draw`, and
    /// follows the mouse: a handle lights up under it, and a left press on one
    /// starts a drag -- unless the mouse is not `free` (over a toolbar, a
    /// popup). `snap` rounds what a drag does to the steps of `steps`.
    /// Returns true while the mouse is the gizmo's -- over a handle, or
    /// dragging one: the viewport then leaves the mouse alone.
    bool update(ImDrawList* draw, const ViewCamera& cam, GizmoMode mode, const Vec3& pivot,
                const sim::Rotation& frame, bool free, bool snap, const Snap& steps = Snap());

    bool dragging() const { return dragging_; }
    /// True on the frame a drag began and the frame it ended (a release).
    bool began() const { return began_; }
    bool ended() const { return ended_; }
    /// The drag so far: while dragging(), and on the frame it ended.
    const GizmoDrag& drag() const { return drag_; }
    /// Stops the drag; the caller puts back what it had moved.
    void cancel();
    Handle hovered() const { return hovered_; }

    /// The size on screen, pixels from the pivot to the end of an axis.
    static float screenLength();

private:
    Handle pick(const ViewCamera& cam, GizmoMode mode, const Vec3& pivot, const sim::Rotation& frame, ImVec2 mouse) const;
    void begin(const ViewCamera& cam, GizmoMode mode, const Vec3& pivot, const sim::Rotation& frame, ImVec2 mouse);
    void follow(const ViewCamera& cam, ImVec2 mouse, bool snap, const Snap& steps);
    void paint(ImDrawList* draw, const ViewCamera& cam, GizmoMode mode, const Vec3& pivot, const sim::Rotation& frame) const;

    Handle hovered_ = Handle::None;
    bool dragging_ = false, began_ = false, ended_ = false;
    GizmoDrag drag_;
    // How the drag began.
    Vec3 pivot_;
    sim::Rotation frame_;
    ImVec2 mouse0_;
    float along0_ = 0.0f;      // where the mouse ray met the axis, along it
    Vec3 plane0_;              // ... or the plane
    Vec3 planeNormal_;
    float angle_ = 0.0f;       // screen angle round the pivot, turned so far (radians)
    float lastAngle_ = 0.0f;
    float worldLength_ = 1.0f; // the axes' length in the world when it began
};

}  // namespace pg::editor
