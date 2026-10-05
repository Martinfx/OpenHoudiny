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
// Or the plants as springs (SwayingPlants): each stem -- a trunk, a branch,
// a blade -- a damped spring bending from its base toward where the wind
// would bow it, carried by the stem it grows from and flung by it: it lags
// behind a gust, swings past and back at its own pace, the trunk slow, the
// twigs quick. Stepped through time from a start, the same however the
// frames are asked for.
//
#include "pg/core/Geometry.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <tuple>
#include <vector>

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

/// How plants sway as springs (Plant Wind's Dynamics).
struct SwaySettings {
    /// Hz a stem 10 m long sways at; a shorter one quicker, as
    /// (10 m / its length)^0.6 -- a 6 m trunk 0.68 Hz, a 30 cm twig
    /// 4 Hz -- and 12 Hz at most.
    float frequency = 0.5f;
    float damping = 0.12f;  ///< a share of critical damping, below 1: low, they swing on; high, they settle
    /// How far each branch bends of its own in a steady wind: 1 as far as
    /// the bow (blowPlants) turns the plant along it, 0 not at all -- only
    /// carried and flung by its parent.
    float branches = 1.0f;
    float start = 0.0f;  ///< s: when the swaying begins; before it the plants stand still, bent as the wind is then

    bool operator==(const SwaySettings&) const = default;
};

/// The wind as it is at a moment -- what Plant Wind's parameters say then.
using WindAt = std::function<WindSettings(float time)>;

/// Plants as damped springs (SwaySettings), kept from one frame to the
/// next: each stem's bend -- the rotation, axis times radians, it is turned
/// by from its base beside what carries it -- and how fast that changes,
/// stepped 1/120 s at a time from Start. Each step solves each stem's spring
/// exactly for the push it has over the step: where the wind would hold it,
/// and how the stem it grows from swings it -- turned and shifted under it.
/// A stem bends as a cantilever, s^2 of its bend s along it. States are
/// kept along the way, so a frame asked again is stepped to from the
/// nearest before it -- the same steps, the same answer, in any order and
/// on any number of threads.
class SwayingPlants {
public:
    /// The stems of `geo`'s plants (as blowPlants finds them): by primitive
    /// tree, stem and parent -- Tree's mesh and skeleton, the leaves with
    /// the stem they grow on, a skeleton's loose leaf points with the
    /// nearest stem -- else each blade, else each plant one stem. Forgets
    /// any state.
    void build(const Geometry& geo);
    /// The points of `geo` standing for plants with flex (Instances.h):
    /// each one stem, as tall as its plant (its prototype times pscale).
    void buildInstances(const Geometry& geo);
    void clear();
    size_t stemCount() const { return stems_.size(); }

    /// `geo` -- what build was given, as it was -- bent as its stems are at
    /// `time`: P, N turned with them where there is one, v how fast each
    /// point goes then; leaves flapping and the tips of blades tossing as
    /// blowPlants has them. False, unfinished, when `interrupt` is set.
    bool blow(Geometry& geo, const WindAt& windAt, const SwaySettings& d, float time,
              const std::atomic<bool>* interrupt = nullptr);
    /// Each point's bend at `time` (buildInstances): level, the world's
    /// way, radians at the plant's top -- for bowInstances; 0 for a point
    /// standing for no plant. False when `interrupt` is set.
    bool bends(const WindAt& windAt, const SwaySettings& d, float time, std::vector<Vec3>& out,
               const std::atomic<bool>* interrupt = nullptr);
    /// Each stem's bend as last asked for (blow, bends), in the order the
    /// stems were found: a trunk before its branches.
    std::span<const Vec3> stemBends() const { return now_; }
    /// Each stem's parent (-1 none) and Hz, in that order.
    std::span<const int32_t> stemParents() const { return parentOf_; }
    float stemFrequency(size_t stem, const SwaySettings& d) const;

private:
    struct Stem {
        int32_t parent = -1;   // the stem it grows from, before it; -1 none
        float attach = 0.0f;   // where on it, a share of its length
        Vec3 base, dir;        // where it grows from, the way it goes: at rest
        float length = 1.0f;   // m
        float share = 1.0f;    // its bend in a steady wind, a share of the wind's bow
        bool branch = false;   // a branch: its share as SwaySettings::branches says
        bool blade = false;    // a blade of grass: its tip tosses
        uint64_t key = 0;      // its own number: how it wavers
    };
    struct State {
        std::vector<Vec3> bend, rate;
    };
    void finish(std::vector<Stem>& stems, std::vector<int32_t>& plantOf);
    void reset(const WindSettings& w, const SwaySettings& d);
    /// One step of `h` seconds from `t0` of all the stems, in place.
    void step(State& state, float t0, float h, const WindSettings& w, const SwaySettings& d) const;
    bool reach(const WindAt& windAt, const SwaySettings& d, float time, const std::atomic<bool>* interrupt);
    /// The bends at `time` (reach first): stepped on from the state there.
    void at(const WindAt& windAt, const SwaySettings& d, float time, std::vector<Vec3>& out);
    /// How each stem wavers, for the wind's seed.
    void waver(uint64_t seed);
    void deform(const Geometry& geo, std::span<const Vec3> bend, const WindSettings& w, float time,
                std::vector<Vec3>& P, std::vector<Vec3>* N) const;

    std::vector<Stem> stems_;           // plant by plant, each its stems parents first
    std::vector<uint32_t> plantStart_;  // the first stem of each plant, and the end
    std::vector<int32_t> parentOf_;
    std::vector<int32_t> stemOf_;       // each point's stem; -1 it stays
    std::vector<float> along_;          // how far along it, a share
    std::vector<float> flexOf_;         // its flex: how far the tip of a blade tosses
    // The steps: how far they have gone, the state there, states kept.
    SwaySettings settings_;
    float dt_ = 1.0f / 120.0f;
    int64_t steps_ = -1;
    State state_;
    std::map<int64_t, State> kept_;
    int64_t keepEvery_ = 120;
    // Per stem, for the settings stepped with: how fast it sways
    // (radians a second), a whole step's fade and swing, how it wavers.
    struct Spring {
        float omega = 1.0f, fade = 1.0f, cos = 1.0f, sin = 0.0f;
        float a = 0.0f, b = 0.0f, rate = 1.0f;
    };
    std::vector<Spring> springs_;
    uint64_t seed_ = 0;
    // Each step's leftovers, per stem: what its children are swung by.
    mutable std::vector<Vec3> spin_, swing_, shove_;
    std::vector<Vec3> now_;
};

/// The points of `geo` that stand for plants with flex (Instances.h), each
/// bowed by `bends[p]` -- level, the world's way, radians at the plant's
/// top -- standing for its plant bent ahead into the nearest of `ways` x
/// `steps` shapes up to `most` radians (in the plant's own turn), tilted
/// from its foot the rest of the way: instance and orient written; the
/// shapes added to its prototypes. The other points as they are.
void bowInstances(Geometry& geo, std::span<const Vec3> bends, int ways, int steps, float most, BentShapes& shapes);

}  // namespace pg
