#include "pg/core/Ecosystem.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace pg {
namespace {

uint64_t mix(uint64_t a, uint64_t b) {
    uint64_t z = a * 0x9E3779B97F4A7C15ull ^ (b + 0x632BE59BD9B4E019ull) * 0xD6E8FEB86659FD93ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
float unitOf(uint64_t bits) { return static_cast<float>(bits >> 40) / static_cast<float>(1u << 24); }

/// The places in cells of `size` metres, level (x, z): which are round a point.
struct Cells {
    float size = 1.0f;
    std::unordered_map<uint64_t, std::vector<uint32_t>> of;

    static uint64_t key(int x, int z) {
        return (static_cast<uint64_t>(static_cast<uint32_t>(x)) << 32) | static_cast<uint32_t>(z);
    }
    void add(const Vec3& p, uint32_t i) {
        of[key(static_cast<int>(std::floor(p.x / size)), static_cast<int>(std::floor(p.z / size)))].push_back(i);
    }
    /// Each index within `r` of `p`'s cell neighbourhood (the caller checks).
    template <class F>
    void near(const Vec3& p, float r, F&& f) const {
        const int x0 = static_cast<int>(std::floor((p.x - r) / size)), x1 = static_cast<int>(std::floor((p.x + r) / size));
        const int z0 = static_cast<int>(std::floor((p.z - r) / size)), z1 = static_cast<int>(std::floor((p.z + r) / size));
        for (int x = x0; x <= x1; ++x) {
            for (int z = z0; z <= z1; ++z) {
                const auto it = of.find(key(x, z));
                if (it == of.end()) continue;
                for (const uint32_t i : it->second) f(i);
            }
        }
    }
};

float level2(const Vec3& a, const Vec3& b) {
    const float dx = a.x - b.x, dz = a.z - b.z;
    return dx * dx + dz * dz;
}

}  // namespace

std::vector<EcoPlant> growEcosystem(std::span<const Vec3> places, std::span<const float> wet, const EcosystemSettings& s) {
    std::vector<EcoPlant> plants;
    const size_t n = places.size();
    if (n == 0 || s.species.empty()) return plants;
    const size_t kinds = s.species.size();
    auto wetAt = [&](uint32_t i) { return i < wet.size() ? std::clamp(wet[i], 0.0f, 1.0f) : 0.5f; };
    float widest = 0.5f;
    for (const Species& sp : s.species) widest = std::max({widest, sp.crown, sp.seeding});

    // The ground's area: its places' box, level.
    Vec3 lo(1e30f), hi(-1e30f);
    for (const Vec3& p : places) lo = glm::min(lo, p), hi = glm::max(hi, p);
    const float area = std::max((hi.x - lo.x) * (hi.z - lo.z), 1.0f);
    Cells placeCells;
    placeCells.size = std::max(widest * 0.5f, 0.5f);
    for (uint32_t i = 0; i < n; ++i) placeCells.add(places[i], i);

    // Where each species does well, 0 to 1.
    auto fit = [&](int species, uint32_t place) {
        const Species& sp = s.species[static_cast<size_t>(species)];
        const float off = (wetAt(place) - sp.moisture) / std::max(sp.tolerance, 1e-3f);
        return std::exp(-off * off);
    };
    // Which species comes up at `place`, by `u` in [0, 1): as likely as its
    // share times how well it does there.
    auto pick = [&](uint32_t place, float u) {
        float total = 0.0f;
        for (size_t k = 0; k < kinds; ++k) total += std::max(s.species[k].share, 0.0f) * fit(static_cast<int>(k), place);
        if (total <= 0.0f) return -1;
        float acc = 0.0f;
        for (size_t k = 0; k < kinds; ++k) {
            acc += std::max(s.species[k].share, 0.0f) * fit(static_cast<int>(k), place) / total;
            if (u < acc) return static_cast<int>(k);
        }
        return static_cast<int>(kinds) - 1;
    };

    // The first: as many as Start says, anywhere, of the kind that does well there.
    std::vector<uint8_t> taken(n, 0);
    {
        const size_t count = std::min(n, static_cast<size_t>(std::lround(std::max(s.start, 0.0f) * area)));
        for (size_t j = 0; j < count; ++j) {
            const uint32_t place = static_cast<uint32_t>(mix(s.seed, j) % n);
            if (taken[place]) continue;
            const int species = pick(place, unitOf(mix(s.seed ^ 0x51ull, j)));
            if (species < 0) continue;
            taken[place] = 1;
            EcoPlant p;
            p.place = place;
            p.species = species;
            p.age = s.species[static_cast<size_t>(species)].growth * unitOf(mix(s.seed ^ 0x52ull, j));
            plants.push_back(p);
        }
    }

    auto crownOf = [&](const EcoPlant& p) {
        const Species& sp = s.species[static_cast<size_t>(p.species)];
        return sp.crown * (0.15f + 0.85f * p.size);
    };
    const int years = std::clamp(s.years, 0, 2000);
    for (int year = 0; year < years; ++year) {
        const uint64_t yearSeed = mix(s.seed, 1000 + static_cast<uint64_t>(year));
        // Older, larger.
        for (EcoPlant& p : plants) {
            p.age += 1.0f;
            const Species& sp = s.species[static_cast<size_t>(p.species)];
            p.size = std::min(1.0f, p.age / std::max(sp.growth, 1.0f));
        }
        // Who shades whom: where two crowns meet, the smaller suffers as much
        // as they overlap and it does not bear shade; and each where the
        // ground does not suit it.
        Cells cells;
        cells.size = std::max(widest, 0.5f);
        for (uint32_t i = 0; i < plants.size(); ++i) cells.add(places[plants[i].place], i);
        std::vector<float> stress(plants.size(), 0.0f);
        for (uint32_t i = 0; i < plants.size(); ++i) {
            const EcoPlant& a = plants[i];
            const Vec3& pa = places[a.place];
            const float ra = crownOf(a);
            cells.near(pa, 2.0f * widest, [&](uint32_t j) {
                if (j <= i) return;
                const EcoPlant& b = plants[j];
                const float rb = crownOf(b);
                const float d = std::sqrt(level2(pa, places[b.place]));
                const float overlap = (ra + rb - d) / std::max(std::min(ra, rb), 1e-3f);
                if (overlap <= 0.0f) return;
                const bool aSmaller = ra < rb || (ra == rb && i > j);
                const uint32_t under = aSmaller ? i : j;
                const Species& sp = s.species[static_cast<size_t>(plants[under].species)];
                // Shade bearers suffer less, but crowding tells on them too.
                stress[under] += std::min(overlap, 1.0f) * (1.0f - 0.75f * std::clamp(sp.shade, 0.0f, 1.0f));
            });
        }
        std::vector<EcoPlant> alive;
        alive.reserve(plants.size());
        for (uint32_t i = 0; i < plants.size(); ++i) {
            EcoPlant p = plants[i];
            const Species& sp = s.species[static_cast<size_t>(p.species)];
            p.vigour = std::min(1.0f, p.vigour + 0.1f - 0.6f * stress[i] - 0.15f * (1.0f - fit(p.species, p.place)));
            const uint64_t own = mix(yearSeed, p.place);
            const float lifespan = sp.life * (0.8f + 0.4f * unitOf(mix(own, 1)));
            if (p.vigour <= 0.0f || p.age > lifespan) {
                taken[p.place] = 0;
                continue;  // fallen
            }
            alive.push_back(p);
        }
        plants.swap(alive);
        cells.of.clear();
        for (uint32_t i = 0; i < plants.size(); ++i) cells.add(places[plants[i].place], i);
        // The grown ones seed round them: where a place is free and no crown
        // is over it.
        const size_t parents = plants.size();
        for (size_t i = 0; i < parents; ++i) {
            const EcoPlant parent = plants[i];
            const Species& sp = s.species[static_cast<size_t>(parent.species)];
            if (parent.size < 0.5f) continue;
            const uint64_t own = mix(yearSeed ^ 0xA5ull, parent.place);
            const float many = std::max(sp.seeds, 0.0f);
            const int count = static_cast<int>(many) + (unitOf(mix(own, 2)) < many - std::floor(many) ? 1 : 0);
            for (int k = 0; k < count; ++k) {
                const uint64_t seedBits = mix(own, 10 + static_cast<uint64_t>(k));
                const float angle = 6.2831853f * unitOf(mix(seedBits, 1));
                const float dist = sp.seeding * std::sqrt(unitOf(mix(seedBits, 2)));
                const Vec3 to = places[parent.place] + Vec3(std::cos(angle) * dist, 0.0f, std::sin(angle) * dist);
                // The nearest free place to where it fell.
                uint32_t best = UINT32_MAX;
                float bestD = placeCells.size * placeCells.size;
                placeCells.near(to, placeCells.size, [&](uint32_t q) {
                    const float d2 = level2(places[q], to);
                    if (!taken[q] && d2 < bestD) bestD = d2, best = q;
                });
                if (best == UINT32_MAX) continue;
                // Not where it does badly, and as often not under a crown.
                if (unitOf(mix(seedBits, 3)) > fit(parent.species, best)) continue;
                bool shaded = false;
                cells.near(places[best], 2.0f * widest, [&](uint32_t j) {
                    if (j < plants.size() && std::sqrt(level2(places[plants[j].place], places[best])) < crownOf(plants[j])) {
                        shaded = true;
                    }
                });
                if (shaded && unitOf(mix(seedBits, 4)) > std::clamp(sp.shade, 0.0f, 1.0f)) continue;
                taken[best] = 1;
                EcoPlant young;
                young.place = best;
                young.species = parent.species;
                plants.push_back(young);
            }
        }
    }
    std::sort(plants.begin(), plants.end(), [](const EcoPlant& a, const EcoPlant& b) { return a.place < b.place; });
    return plants;
}

}  // namespace pg
