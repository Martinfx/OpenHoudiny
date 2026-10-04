#pragma once
//
// Plants lighter for far away -- levels of detail, as SpeedTree's LODs thin
// a tree: fewer leaves and blades, each of those kept grown so that the
// foliage covers as much as before, and the twigs gone. What the viewport
// draws an instance of a tree or a clump of grass by when it is small on
// the screen (sim/Display.h: DisplayInstances, placementsByDetail).
//
#include "pg/core/Geometry.h"

namespace pg {

/// A lighter copy of the plant `plant`: `keep` (0 to 1) of its leaves and
/// blades -- the faces that let light through (primitive translucency above
/// 0), a blade all the faces of one primitive `blade` -- picked evenly
/// along them, each kept one grown to cover as much as those dropped: a
/// leaf 1/sqrt(keep) as big about its base (its first corner), a blade
/// 1/keep as wide (each row of its points -- those as far along it, point
/// flex -- spread from its middle); below `keep` 0.5 the twigs too -- stems
/// of primitive level 2 and up. The points only what is left uses; the
/// rest of the attributes as they were. `keep` 1 or more: the plant as it
/// is.
Geometry plantDetail(const Geometry& plant, float keep);

}  // namespace pg
