#pragma once
//
// A plant community as it grows over years (after Deussen et al., Realistic
// Modeling and Rendering of Plant Ecosystems, 1998, and Lane and
// Prusinkiewicz's multiset L-systems): plants of a few species start where
// the ground lets them, grow their crowns year by year, shade each other
// where the crowns meet -- the smaller suffering, the more the less it
// stands shade -- and wither where the ground is too wet or too dry for
// them; they die of age, and the grown ones seed round themselves. What is
// left after so many years: clumps of a kind where it does well, a few old
// giants with their young round them, gaps where one fell.
//
// Plants stand only where the ground offers a place -- the points given,
// scattered over a terrain as densely as plants could stand -- each with how
// wet it is there. The same settings and seed, the same community, on any
// number of threads.
//
#include "pg/core/Types.h"

#include <cstdint>
#include <span>
#include <vector>

namespace pg {

struct Species {
    float share = 1.0f;        ///< how many of it start, against the others
    float crown = 3.0f;        ///< m, how wide its crown spreads, grown (radius)
    float growth = 25.0f;      ///< years to grow to it
    float life = 150.0f;       ///< years it lives, give or take a fifth
    float shade = 0.3f;        ///< how well it bears another's shade, 0 not at all, 1 fully
    float moisture = 0.5f;     ///< how wet it likes the ground, 0 dry to 1 wet
    float tolerance = 0.35f;   ///< how far from that it does well
    float seeding = 8.0f;      ///< m, how far its seeds fall round it
    float seeds = 0.6f;        ///< seedlings a grown plant brings up a year, where they may
};

struct EcosystemSettings {
    std::vector<Species> species;
    int years = 80;
    float start = 0.02f;  ///< plants a square metre at the start
    uint64_t seed = 1;
};

struct EcoPlant {
    uint32_t place = 0;  ///< which of the places it stands on
    int species = 0;
    float age = 0.0f;    ///< years
    float size = 0.0f;   ///< how grown, 0 to 1
    float vigour = 1.0f;
};

/// The community on `places` -- level enough to stand on, each `wet` (0 to
/// 1; empty: 0.5 everywhere) -- after `s.years`: the plants alive, in the
/// order of their places.
std::vector<EcoPlant> growEcosystem(std::span<const Vec3> places, std::span<const float> wet, const EcosystemSettings& s);

}  // namespace pg
