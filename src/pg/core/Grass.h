#pragma once
//
// Grass as it grows: a clump of blades from one root -- each a strip
// narrowing to its tip, leaning out from the clump's middle and bowing over
// under its weight, the more the higher up, turned a little about itself;
// green from dark at the root to light at the tip, a blade here and there
// dry. A meadow is a few such clumps on many points (pg/core/Instances.h).
//
// A clump is the same for the same settings and seed.
//
#include "pg/core/Geometry.h"

#include <cstdint>

namespace pg {

struct GrassSettings {
    int blades = 16;                 ///< in a clump
    float height = 0.4f;             ///< m, how long a blade is
    float heightVariation = 0.4f;    ///< how much the blades differ in length, 0 to 1
    float width = 0.006f;            ///< m, how wide a blade is at its root
    float bend = 0.55f;              ///< how far they bow over, 0 upright to 1 lying flat at the tip
    float lean = 30.0f;              ///< degrees the blades lean out from the clump's middle, at the most
    float spread = 0.08f;            ///< m, how far from the middle their roots are, at the most
    int segments = 4;                ///< pieces along a blade, 1 to 16
    Vec3 rootColor{0.08f, 0.14f, 0.03f};
    Vec3 tipColor{0.25f, 0.4f, 0.08f};
    float dry = 0.1f;                ///< the share of blades that are dry, 0 to 1
    Vec3 dryColor{0.45f, 0.38f, 0.15f};
    float variation = 0.2f;          ///< how much the blades differ in shade, 0 to 1
};

/// A clump of grass at the origin, growing up +y, of `seed`: each blade a
/// strip of quads and a triangle at its tip -- point Cd, and flex: how far
/// along the blade, 0 at the root to 1 at the tip, what wind bends it by;
/// primitive blade.
Geometry growGrassClump(const GrassSettings& s, uint64_t seed);

}  // namespace pg
