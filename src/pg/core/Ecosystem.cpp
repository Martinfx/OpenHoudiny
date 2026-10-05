#include "pg/core/Ecosystem.h"

#include "pg/core/Parallel.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace pg {
namespace {

constexpr float kPi = 3.14159265358979f;

/// A way the sky's light comes from, and its share of it.
struct SkyRay {
    Vec3 dir;
    float weight = 0.0f;
};

/// An overcast sky, as bright as 1 + 2 cos of how far from the zenith,
/// lights level ground from a band of it as the integral of (u + 2 u^2) du,
/// u the cosine: the zenith up to 25 degrees, a ring of eight ways to 55
/// (at 40), a ring to 90 (at 70), turned half a way from the first.
const std::vector<SkyRay>& skyRays() {
    static const std::vector<SkyRay> rays = [] {
        auto lit = [](float from, float to) {
            auto F = [](float u) { return 0.5f * u * u + 2.0f / 3.0f * u * u * u; };
            return F(std::cos(from * kPi / 180.0f)) - F(std::cos(to * kPi / 180.0f));
        };
        const float all = lit(0.0f, 90.0f);
        std::vector<SkyRay> out;
        out.push_back({Vec3(0.0f, 1.0f, 0.0f), lit(0.0f, 25.0f) / all});
        for (int ring = 0; ring < 2; ++ring) {
            const float zenith = (ring == 0 ? 40.0f : 70.0f) * kPi / 180.0f;
            const float weight = (ring == 0 ? lit(25.0f, 55.0f) : lit(55.0f, 90.0f)) / all / 8.0f;
            for (int k = 0; k < 8; ++k) {
                const float around = 2.0f * kPi * (static_cast<float>(k) + 0.5f * static_cast<float>(ring)) / 8.0f;
                out.push_back({Vec3(std::sin(zenith) * std::cos(around), std::cos(zenith), std::sin(zenith) * std::sin(around)),
                               weight});
            }
        }
        return out;
    }();
    return rays;
}

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

float plantHeight(const EcoPlant& p, const Species& sp) { return std::max(sp.height, 0.05f) * (0.15f + 0.85f * p.size); }

float crownRadius(const EcoPlant& p, const Species& sp) { return sp.crown * (0.15f + 0.85f * p.size); }

CanopyLight::CanopyLight(std::span<const Vec3> places, std::span<const EcoPlant> plants, const std::vector<Species>& species) {
    if (plants.empty() || species.empty()) return;
    // Cells half as wide as the narrowest crown, grown: 0.5 to 2 m.
    float narrow = 1e30f;
    for (const Species& sp : species) narrow = std::min(narrow, sp.crown);
    cell_ = std::clamp(0.5f * narrow, 0.5f, 2.0f);
    // The box round the crowns; above each its top.
    Vec3 lo(1e30f), hi(-1e30f);
    tops_.resize(plants.size());
    for (size_t i = 0; i < plants.size(); ++i) {
        const EcoPlant& p = plants[i];
        const Species& sp = species[static_cast<size_t>(p.species)];
        const Vec3& at = places[p.place];
        const float r = std::max(crownRadius(p, sp), 1e-3f), h = plantHeight(p, sp);
        lo = glm::min(lo, Vec3(at.x - r, at.y, at.z - r));
        hi = glm::max(hi, Vec3(at.x + r, at.y + h, at.z + r));
        tops_[i] = Vec3(at.x, at.y + h, at.z);
    }
    // No more than 8 million cells: coarser for a wide land.
    for (;;) {
        for (int a = 0; a < 3; ++a) n_[a] = static_cast<int>(std::ceil((hi[a] - lo[a]) / cell_)) + 1;
        if (static_cast<double>(n_[0]) * n_[1] * n_[2] <= 8e6) break;
        cell_ *= 1.25f;
    }
    lo_ = lo;
    for (Vec3& top : tops_) top.y += 0.75f * cell_;  // out of its own leaves
    density_.assign(static_cast<size_t>(n_[0]) * static_cast<size_t>(n_[1]) * static_cast<size_t>(n_[2]), 0.0f);
    const float volume = cell_ * cell_ * cell_;
    std::vector<size_t> inside;
    for (const EcoPlant& p : plants) {
        const Species& sp = species[static_cast<size_t>(p.species)];
        const Vec3& at = places[p.place];
        const float r = std::max(crownRadius(p, sp), 1e-3f), h = plantHeight(p, sp);
        const float half = std::max(0.5f * h * std::clamp(sp.depth, 0.0f, 1.0f), 1e-3f);
        const Vec3 middle(at.x, at.y + h - half, at.z);
        // Its leaves evenly in the cells whose middles are in it -- the one
        // holding its middle when it is smaller than a cell.
        inside.clear();
        auto range = [&](int a, float c, float extent, int& from, int& to) {
            from = std::max(static_cast<int>(std::floor((c - extent - lo[a]) / cell_)), 0);
            to = std::min(static_cast<int>(std::floor((c + extent - lo[a]) / cell_)), n_[a] - 1);
        };
        int x0, x1, y0, y1, z0, z1;
        range(0, middle.x, r, x0, x1);
        range(1, middle.y, half, y0, y1);
        range(2, middle.z, r, z0, z1);
        for (int j = y0; j <= y1; ++j) {
            const float dy = (lo.y + (static_cast<float>(j) + 0.5f) * cell_ - middle.y) / half;
            for (int k = z0; k <= z1; ++k) {
                const float dz = (lo.z + (static_cast<float>(k) + 0.5f) * cell_ - middle.z) / r;
                for (int i = x0; i <= x1; ++i) {
                    const float dx = (lo.x + (static_cast<float>(i) + 0.5f) * cell_ - middle.x) / r;
                    if (dx * dx + dy * dy + dz * dz <= 1.0f) {
                        inside.push_back(static_cast<size_t>(i) +
                                         static_cast<size_t>(n_[0]) * (static_cast<size_t>(k) + static_cast<size_t>(n_[2]) * static_cast<size_t>(j)));
                    }
                }
            }
        }
        if (inside.empty()) {
            const int i = std::clamp(static_cast<int>((middle.x - lo.x) / cell_), 0, n_[0] - 1);
            const int j = std::clamp(static_cast<int>((middle.y - lo.y) / cell_), 0, n_[1] - 1);
            const int k = std::clamp(static_cast<int>((middle.z - lo.z) / cell_), 0, n_[2] - 1);
            inside.push_back(static_cast<size_t>(i) +
                             static_cast<size_t>(n_[0]) * (static_cast<size_t>(k) + static_cast<size_t>(n_[2]) * static_cast<size_t>(j)));
        }
        const float area = std::max(sp.density, 0.0f) * kPi * r * r;
        const float each = area / (static_cast<float>(inside.size()) * volume);
        for (const size_t c : inside) density_[c] += each;
    }
}

float CanopyLight::leaves(const Vec3& p) const {
    const float fx = (p.x - lo_.x) / cell_, fy = (p.y - lo_.y) / cell_, fz = (p.z - lo_.z) / cell_;
    if (!(fx >= 0.0f && fy >= 0.0f && fz >= 0.0f)) return 0.0f;
    const int i = static_cast<int>(fx), j = static_cast<int>(fy), k = static_cast<int>(fz);
    if (i >= n_[0] || j >= n_[1] || k >= n_[2]) return 0.0f;
    return density_[static_cast<size_t>(i) +
                    static_cast<size_t>(n_[0]) * (static_cast<size_t>(k) + static_cast<size_t>(n_[2]) * static_cast<size_t>(j))];
}

float CanopyLight::at(const Vec3& p) const {
    if (density_.empty()) return 1.0f;
    const Vec3 hi = lo_ + Vec3(static_cast<float>(n_[0]), static_cast<float>(n_[1]), static_cast<float>(n_[2])) * cell_;
    const float step = 0.5f * cell_;
    float light = 0.0f;
    for (const SkyRay& ray : skyRays()) {
        // As far as it is in the box of the crowns, up and out.
        float exit = (hi.y - p.y) / ray.dir.y;
        for (const int a : {0, 2}) {
            if (ray.dir[a] > 1e-6f) exit = std::min(exit, (hi[a] - p[a]) / ray.dir[a]);
            if (ray.dir[a] < -1e-6f) exit = std::min(exit, (lo_[a] - p[a]) / ray.dir[a]);
        }
        float depth = 0.0f;
        for (float t = 0.5f * step; t < exit; t += step) depth += leaves(p + ray.dir * t);
        light += ray.weight * std::exp(-0.5f * depth * step);
    }
    return std::clamp(light, 0.0f, 1.0f);
}

float CanopyLight::atTop(size_t i) const { return i < tops_.size() ? at(tops_[i]) : 1.0f; }

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
            p.size = std::min(1.0f, p.age / std::max(s.species[static_cast<size_t>(species)].growth, 1.0f));
            plants.push_back(p);
        }
    }

    auto crownOf = [&](const EcoPlant& p) { return crownRadius(p, s.species[static_cast<size_t>(p.species)]); };
    // By height: the light a kind needs to grow at its pace -- the less, the
    // better it bears shade.
    auto need = [&](int species) {
        return 0.05f + 0.6f * (1.0f - std::clamp(s.species[static_cast<size_t>(species)].shade, 0.0f, 1.0f));
    };
    const int years = std::clamp(s.years, 0, 2000);
    for (int year = 0; year < years; ++year) {
        const uint64_t yearSeed = mix(s.seed, 1000 + static_cast<uint64_t>(year));
        if (s.byHeight) {
            // The light each has this year: at its top, through the crowns
            // above and round it.
            const CanopyLight canopy(places, plants, s.species);
            parallelFor(plants.size(), 32, [&](size_t b, size_t e) {
                for (size_t i = b; i < e; ++i) plants[i].light = canopy.atTop(i);
            });
        }
        // Older, larger: by height as fast as its light lets it.
        for (EcoPlant& p : plants) {
            p.age += 1.0f;
            const Species& sp = s.species[static_cast<size_t>(p.species)];
            if (s.byHeight) {
                p.size = std::min(1.0f, p.size + std::clamp(p.light / need(p.species), 0.0f, 1.0f) / std::max(sp.growth, 1.0f));
            } else {
                p.size = std::min(1.0f, p.age / std::max(sp.growth, 1.0f));
            }
        }
        // Who shades whom: where two crowns meet, the smaller suffers as much
        // as they overlap and it does not bear shade; by height, each as
        // short of the light it needs it is -- shade bearers the less. And
        // each where the ground does not suit it.
        Cells cells;
        cells.size = std::max(widest, 0.5f);
        for (uint32_t i = 0; i < plants.size(); ++i) cells.add(places[plants[i].place], i);
        std::vector<float> stress(plants.size(), 0.0f);
        for (uint32_t i = 0; i < plants.size() && s.byHeight; ++i) {
            const EcoPlant& p = plants[i];
            const float shade = std::clamp(s.species[static_cast<size_t>(p.species)].shade, 0.0f, 1.0f);
            stress[i] = 0.7f * std::max(0.0f, 1.0f - p.light / need(p.species)) * (1.0f - 0.5f * shade);
        }
        for (uint32_t i = 0; i < plants.size() && !s.byHeight; ++i) {
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
        // By height, the light on the ground with the fallen gone.
        const CanopyLight ground = s.byHeight ? CanopyLight(places, plants, s.species)
                                              : CanopyLight(places, std::span<const EcoPlant>(), s.species);
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
                // Not where it does badly, and as often not under a crown --
                // by height, as likely as the light on the ground suits it.
                if (unitOf(mix(seedBits, 3)) > fit(parent.species, best)) continue;
                if (s.byHeight) {
                    const float light = ground.at(places[best] + Vec3(0.0f, 0.5f, 0.0f));
                    if (unitOf(mix(seedBits, 4)) > std::clamp(light / need(parent.species), 0.0f, 1.0f)) continue;
                } else {
                    bool shaded = false;
                    cells.near(places[best], 2.0f * widest, [&](uint32_t j) {
                        if (j < plants.size() && std::sqrt(level2(places[plants[j].place], places[best])) < crownOf(plants[j])) {
                            shaded = true;
                        }
                    });
                    if (shaded && unitOf(mix(seedBits, 4)) > std::clamp(sp.shade, 0.0f, 1.0f)) continue;
                }
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
