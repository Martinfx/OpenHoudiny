#pragma once
//
// Wind in plants: trees and grass bent by it as they bend -- stiff at the
// foot, more and more up the wood (point flex: how far along it from the
// foot, a share of the plant's height) -- each point turned about its
// plant's foot, so that nothing stretches; gusts running across the scene
// along the wind, each plant wavering a little of its own, leaves and the
// tips of blades fluttering. What the Plant Wind node does to plants as
// geometry, and to the plants that points stand for (Instances.h): those
// bent ahead into a few shapes -- so many ways, so far -- each point
// standing for the nearest, tilted the rest of the way.
//
#include "pg/core/Geometry.h"

#include <cstdint>

namespace pg {

struct WindSettings {
    Vec3 direction{1.0f, 0.0f, 0.0f};  ///< the way it blows, level (y dropped), unit
    float strength = 0.25f;  ///< radians the plants' tops bow at flex 1, the gusts at their height
    float gust = 0.6f;       ///< the share of it that comes in gusts, 0 steady to 1
    float gustSpeed = 6.0f;  ///< m/s the gusts run along it
    float gustSize = 25.0f;  ///< m from one gust to the next
    float turbulence = 0.25f;  ///< how much each plant wavers of its own, across the wind too
    float flutter = 0.35f;     ///< radians leaves flap, blades' tips toss, at the wind's strength
    float flutterSpeed = 3.0f;  ///< flaps a second
    uint64_t seed = 1;
};

/// The bend at the top of a plant whose foot is at `at`, `time` seconds in:
/// toward the way it is bowed, as many radians as its length (level).
Vec3 windBend(const WindSettings& s, const Vec3& at, float time, uint64_t plant);

/// The plants of `geo` bent by the wind at `time` -- in place: P, N turned
/// with them where there is one, and v, how fast each point goes then. A plant: the points of one primitive
/// blade (a blade of grass), else of one primitive tree, else all of them;
/// its foot the point of the least flex. Points without flex stay.
void blowPlants(Geometry& geo, const WindSettings& s, float time);

/// The plant `plant` bowed by `bend` -- level, in its own frame: toward its
/// way, as many radians as its length -- each of its plants (as above) from
/// its own foot; no flutter.
Geometry bentPlant(const Geometry& plant, const Vec3& bend);

}  // namespace pg
