#pragma once
//
// The water of a frame as a surface a renderer renders -- what Houdini's
// Particle Fluid Surface makes of a FLIP simulation: a closed mesh of quads
// round the water, turned outward (volumeToMesh over the frame's distance to
// the surface, on its grid twice as fine as the solver's), with
//
//   N     the normals, smooth
//   v     how fast the water goes there (the frame's flow): what a renderer
//         blurs it by as it moves; none from a frame cached without it
//   foam  how white it is there, 0 to 1
//
// Closed where the solver's domain ends: against the floor and the walls of
// a tank too, as a renderer needs a closed body of water to bend light
// through it. The rain's ripples, when there are some, raise and tilt its
// top.
//
#include "pg/core/Geometry.h"
#include "pg/sim/Frame.h"

namespace pg::sim {

/// Empty when the frame has no water.
std::shared_ptr<Geometry> waterMesh(const WaterFrame& water, const RainFrame* ripples = nullptr);

}  // namespace pg::sim
