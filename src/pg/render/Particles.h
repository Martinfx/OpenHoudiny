#pragma once
//
// What the render makes of a frame's particles -- the grit of the broken
// pieces and the rain -- as the viewport draws them, but as things rays
// meet (Scene.h):
//
//   grit   each point a chip: of stone -- a box flattened and stretched,
//          its corners and edges broken off by planes, as a bit of a break
//          is -- or of glass, a sliver of three to five sides as thick as a
//          pane. A dozen shapes of stone, half a dozen of glass, made once;
//          which a chip is goes by its id, so that it keeps its shape as it
//          flies. As big as its pscale -- its farthest corner so far from
//          its middle -- turned by its orient, a shade of its colour of its
//          own: stones are not all alike -- flying at its velocity v. Stone
//          is broken concrete, its photographs laid on as on a chip some
//          5 cm across; glass is glass.
//   rain   each drop the streak it falls in a share of a frame
//          (Look::rainStreak) as the viewport draws it: from where it is,
//          back along how fast it goes; a spindle as thick as a drop at its
//          head (2.5 mm, a droplet of a splash half that), thinning to
//          nothing at its tail. Water (Material::Kind::Rain) of the rain's
//          colour, there as much of the time as Look::rainOpacity says.
//
#include "pg/core/Chips.h"
#include "pg/render/Scene.h"
#include "pg/sim/Frame.h"

#include <cstddef>
#include <memory>

namespace pg::render {

/// The chip of stone -- or the sliver of glass -- `shape`, a unit from its
/// middle to its farthest corner, its faces flat: made once and kept, its
/// hierarchy `engine`'s.
std::shared_ptr<const Mesh> chipMesh(size_t shape, bool glass, RayEngine engine);

/// The chips of the loose points of `geo` whose pscale is above 0 -- the
/// grit drawnPieces gives the pieces -- placed on their points in `scene`,
/// their shapes' meshes added to its meshes. How many.
size_t placeChips(const Geometry& geo, Scene& scene);

/// The streaks of `rain`'s drops and droplets as `look` draws them -- but
/// those within half a metre of `eye`, if given: just in front of the lens,
/// a drop is a faint blur, not a sharp streak. Null without any.
std::shared_ptr<const Mesh> rainMesh(const sim::RainFrame& rain, const sim::Look& look,
                                     RayEngine engine = defaultRayEngine(), const Vec3* eye = nullptr);

}  // namespace pg::render
