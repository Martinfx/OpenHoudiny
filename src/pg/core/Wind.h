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
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <tuple>

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

/// The plants of `geo` each bowed by what `bendOf` says for its foot and
/// its own number (as windBend's): P and N, in place; no flutter, no v.
void bowPlants(Geometry& geo, const std::function<Vec3(const Vec3& foot, uint64_t plant)>& bendOf);

/// The shapes plants are bent ahead into, kept from one cook to the next:
/// the same plants frame after frame -- what the viewport keeps on the GPU.
class BentShapes {
public:
    /// `plant` bent `way` of `ways` round it, `step` of `steps` of `most`
    /// radians: made once, then the same.
    std::shared_ptr<const Geometry> shape(const std::shared_ptr<const Geometry>& plant, int way, int ways, int step,
                                          int steps, float most);

private:
    struct Entry {
        std::shared_ptr<const Geometry> of, bent;  // `of` held: its pointer names it
    };
    std::mutex mutex_;
    std::map<std::tuple<const Geometry*, int, int, int, int, float>, Entry> made_;
};

/// The points of `geo` that stand for plants with flex (Instances.h), each
/// bowed by `bends[p]` -- level, the world's way, radians at the plant's
/// top -- standing for its plant bent ahead into the nearest of `ways` x
/// `steps` shapes up to `most` radians (in the plant's own turn), tilted
/// from its foot the rest of the way: instance and orient written; the
/// shapes added to its prototypes. The other points as they are.
void bowInstances(Geometry& geo, std::span<const Vec3> bends, int ways, int steps, float most, BentShapes& shapes);

}  // namespace pg
