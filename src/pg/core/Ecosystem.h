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
// Or the light by height (EcosystemSettings::byHeight), as forest gap
// models have it (Botkin's JABOWA, Pacala's SORTIE): each crown an
// ellipsoid of leaves at the height its plant has grown to, the light of
// an overcast sky dimmed through the crowns above (CanopyLight); a plant
// grows as fast as its light lets it, the more the less light it needs,
// and withers in too little; a seedling comes up as likely as the light
// on the ground suits it. A tall crown shades the short ones under it
// whoever is the larger: shade bearers wait under the canopy and shoot up
// where one falls; shrubs live under trees, an understorey.
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
    // By height (EcosystemSettings::byHeight):
    float height = 12.0f;  ///< m it stands, grown
    float depth = 0.6f;    ///< how deep its crown is, a share of its height
    float density = 4.0f;  ///< leaf area over the ground its crown covers: how deep its shade
};

struct EcosystemSettings {
    std::vector<Species> species;
    int years = 80;
    float start = 0.02f;  ///< plants a square metre at the start
    uint64_t seed = 1;
    /// The light by height, through the crowns above (CanopyLight); else
    /// crowns meeting in plan, the smaller suffering.
    bool byHeight = false;
};

struct EcoPlant {
    uint32_t place = 0;  ///< which of the places it stands on
    int species = 0;
    float age = 0.0f;    ///< years
    float size = 0.0f;   ///< how grown, 0 to 1
    float vigour = 1.0f;
    float light = 1.0f;  ///< by height: the share of the sky's light at its top, its last year
};

/// The community on `places` -- level enough to stand on, each `wet` (0 to
/// 1; empty: 0.5 everywhere) -- after `s.years`: the plants alive, in the
/// order of their places.
std::vector<EcoPlant> growEcosystem(std::span<const Vec3> places, std::span<const float> wet, const EcosystemSettings& s);

/// How tall a plant stands, and how wide its crown spreads (its radius),
/// grown as far as it is: a seedling 0.15 of its kind's.
float plantHeight(const EcoPlant& p, const Species& sp);
float crownRadius(const EcoPlant& p, const Species& sp);

/// The light under the crowns of a community: each plant's crown an
/// ellipsoid of leaves -- its crown's radius across, from (1 - depth) of
/// its height to its top -- holding density times the ground it covers of
/// leaves, laid evenly in cells of a grid. The light of an overcast sky
/// (as bright as 1 + 2 cos of how far from the zenith) comes from the
/// zenith and two rings of eight ways, each dimmed by the leaves it passes
/// as leaves do (Beer and Lambert: e^(-0.5 L), L the leaf area it meets per
/// square metre of it). Asked from any number of threads.
class CanopyLight {
public:
    CanopyLight(std::span<const Vec3> places, std::span<const EcoPlant> plants, const std::vector<Species>& species);

    /// The share of the sky's light at `p`, 0 to 1.
    float at(const Vec3& p) const;
    /// ... just above the top of plant `i`'s crown: out of its own leaves.
    float atTop(size_t i) const;
    float cell() const { return cell_; }

private:
    float leaves(const Vec3& p) const;  // leaf area a cubic metre at `p`

    std::vector<Vec3> tops_;  // above each crown
    Vec3 lo_;
    float cell_ = 1.0f;
    int n_[3] = {0, 0, 0};        // cells along x, y, z
    std::vector<float> density_;  // x fastest, then z, then y
};

}  // namespace pg
