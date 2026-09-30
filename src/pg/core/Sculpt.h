#pragma once
//
// Sculpting: a surface pushed out and pulled in, smoothed, grabbed and
// moved, flattened -- by the dabs of a brush, in the order they were made,
// each on the surface as the dabs before it left it. The dabs are places,
// not point numbers: the same strokes on a finer mesh make the same shape,
// finer.
//
// The Sculpt node keeps its dabs as text and makes its geometry of them;
// the viewport's brush writes them (pg/nodes, tools/prototype). A Sculptor
// in the node puts on only the dabs a stroke has added since it cooked.
//
#include "pg/core/Geometry.h"
#include "pg/core/Soft.h"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace pg {

/// What a sculpting brush did at one place.
struct SculptDab {
    enum class Tool : uint8_t {
        Push,     ///< out along `normal` -- in, with a strength below 0
        Smooth,   ///< each point towards the middle of its neighbours
        Grab,     ///< by `move`
        Flatten,  ///< onto the plane through `at` square to `normal`
    };
    Tool tool = Tool::Push;
    Vec3 at;                ///< its middle, on the surface as it was then
    Vec3 normal;            ///< Push, Flatten: the surface's normal there, of unit length
    Vec3 move;              ///< Grab: how far it took what it held
    float radius = 0.0f;    ///< m
    float strength = 0.0f;  ///< Push: out a fifth of the radius at its middle for 1; Smooth, Flatten: the share, 0 to 1
};

/// How far a Push dab of strength 1 moves the point at its middle, in radii.
inline constexpr float kSculptPush = 0.2f;

/// The dabs a Sculpt node keeps, as text: "p x y z nx ny nz radius
/// strength" (push), "s x y z radius strength" (smooth), "g x y z mx my mz
/// radius" (grab), "f x y z nx ny nz radius strength" (flatten), separated
/// by ';'. What does not read so is left out.
std::vector<SculptDab> parseSculpt(std::string_view text);
/// One dab as that text.
std::string sculptText(const SculptDab& dab);

/// The dabs on the points of `geo`, one after another: each on the points
/// nearer its middle than its radius where they are after the dabs before
/// it, less towards its edge as `shape` says. Smoothing keeps an open
/// border on its line and its corners where they are; the ends of a line
/// stay. Where the geometry has point normals N, they are found again from
/// the faces.
void sculpt(Geometry& geo, std::span<const SculptDab> dabs, Falloff shape = Falloff::Smooth);

/// Who each point is smoothed towards, of a geometry as it came (Sculpt.cpp).
struct SculptSmoothing;

/// Sculpting that goes on from where it got to, as a brush needs it: each
/// move of the mouse adds a dab or two to the thousands before. Given the
/// geometry it had and the dabs it had with more after them, it puts on
/// only the new ones; given them all but the last -- a grab that moves on,
/// a dab taken back -- it goes on from before that one; anything else,
/// from the start. What it gives is what `sculpt` makes of all the dabs,
/// to the bit. Not for more than one thread at once.
class Sculptor {
public:
    /// `source` sculpted by `dabs`.
    GeometryPtr cook(const GeometryPtr& source, std::span<const SculptDab> dabs, Falloff shape);
    /// How many dabs the last cook had put on already, and did not again.
    size_t reused() const { return reused_; }

private:
    GeometryPtr source_;
    Falloff shape_ = Falloff::Smooth;
    std::vector<SculptDab> dabs_;  ///< the last cook's
    GeometryPtr before_;           ///< ... the geometry after all of them but the last; null: not kept
    GeometryPtr after_;            ///< ... after all of them: what it gave
    bool movedBefore_ = false;     ///< some dab had moved some point by then
    bool movedAfter_ = false;
    std::shared_ptr<const SculptSmoothing> smoothing_;  ///< of source_, once a Smooth dab came
    size_t reused_ = 0;
};

}  // namespace pg
