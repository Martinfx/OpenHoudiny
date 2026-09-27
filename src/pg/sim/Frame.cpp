#include "pg/sim/Frame.h"

#include "pg/core/Parallel.h"
#include "pg/sim/Liquid.h"
#include "pg/sim/Pyro.h"
#include "pg/sim/Rain.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace pg::sim {

uint16_t toHalf(float value) {
    uint32_t x;
    std::memcpy(&x, &value, sizeof x);
    const uint16_t sign = static_cast<uint16_t>((x >> 16) & 0x8000u);
    const uint32_t a = x & 0x7FFFFFFFu;
    if (a >= 0x7F800000u) return static_cast<uint16_t>(sign | (a > 0x7F800000u ? 0x7E00u : 0x7C00u));
    if (a >= 0x477FF000u) return static_cast<uint16_t>(sign | 0x7C00u);  // rounds past 65504
    if (a < 0x38800000u) {
        // Below the smallest normal half, 2^-14: a subnormal, m * 2^-24.
        if (a < 0x33000000u) return sign;  // less than half of the smallest subnormal
        const uint32_t e = a >> 23;
        const uint32_t m = (a & 0x7FFFFFu) | 0x800000u;
        const uint32_t shift = 126u - e;
        uint32_t h = m >> shift;
        const uint32_t rest = m & ((1u << shift) - 1u), halfway = 1u << (shift - 1u);
        if (rest > halfway || (rest == halfway && (h & 1u))) ++h;
        return static_cast<uint16_t>(sign | h);
    }
    uint32_t h = (a - 0x38000000u) >> 13;  // exponent bias 127 -> 15
    const uint32_t rest = a & 0x1FFFu;
    if (rest > 0x1000u || (rest == 0x1000u && (h & 1u))) ++h;  // a carry into the exponent is right
    return static_cast<uint16_t>(sign | h);
}

float fromHalf(uint16_t value) {
    const uint32_t sign = static_cast<uint32_t>(value & 0x8000u) << 16;
    const uint32_t e = (value >> 10) & 0x1Fu, m = value & 0x3FFu;
    if (e == 0) {
        const float v = std::ldexp(static_cast<float>(m), -24);
        return sign ? -v : v;
    }
    const uint32_t x = e == 31 ? sign | 0x7F800000u | (m << 13) : sign | ((e + 112u) << 23) | (m << 13);
    float f;
    std::memcpy(&f, &x, sizeof f);
    return f;
}

float Frame::at(int channel, int i, int j, int k) const {
    const size_t cell = static_cast<size_t>(i) +
                        static_cast<size_t>(domain.cells[0]) *
                            (static_cast<size_t>(j) + static_cast<size_t>(domain.cells[1]) * static_cast<size_t>(k));
    return fromHalf(fields[3 * cell + static_cast<size_t>(channel)]);
}

Frame capture(const PyroSolver& sim) {
    Frame f;
    f.number = sim.frame();
    f.time = sim.time();
    f.domain = sim.domain();
    const size_t n = sim.density().size();
    f.fields.resize(3 * n);
    const float* smoke = sim.density().data();
    const float* heat = sim.temperature().data();
    const float* flame = sim.flame().data();
    uint16_t* out = f.fields.data();
    pg::parallelFor(n, 16384, [&](size_t begin, size_t end) {
        for (size_t c = begin; c < end; ++c) {
            out[3 * c] = toHalf(smoke[c]);
            out[3 * c + 1] = toHalf(heat[c]);
            out[3 * c + 2] = toHalf(flame[c]);
        }
    });
    return f;
}

float WaterFrame::distance(int i, int j, int k) const {
    const size_t cell = static_cast<size_t>(i) +
                        static_cast<size_t>(domain.cells[0]) *
                            (static_cast<size_t>(j) + static_cast<size_t>(domain.cells[1]) * static_cast<size_t>(k));
    return (static_cast<float>(cells[2 * cell]) / 255.0f * 2.0f - 1.0f) * band;
}

float WaterFrame::foam(int i, int j, int k) const {
    const size_t cell = static_cast<size_t>(i) +
                        static_cast<size_t>(domain.cells[0]) *
                            (static_cast<size_t>(j) + static_cast<size_t>(domain.cells[1]) * static_cast<size_t>(k));
    return static_cast<float>(cells[2 * cell + 1]) / 255.0f;
}

WaterFrame capture(const LiquidSolver& sim, bool particles) {
    WaterFrame w;
    if (particles) {
        const size_t n = sim.particleCount();
        w.positions = sim.positions();
        w.ids = sim.ids();
        w.velocities.resize(3 * n);
        w.whiteness.resize(n);
        const auto& v = sim.velocities();
        const auto& white = sim.foam();
        for (size_t i = 0; i < n; ++i) {
            for (int a = 0; a < 3; ++a) w.velocities[3 * i + static_cast<size_t>(a)] = toHalf(v[i][a]);
            w.whiteness[i] = static_cast<uint8_t>(std::lround(std::clamp(white[i], 0.0f, 1.0f) * 255.0f));
        }
    }
    const Domain& d = sim.domain();
    for (int a = 0; a < 3; ++a) w.domain.cells[a] = 2 * d.cells[a];
    w.domain.voxel = 0.5f * d.voxel;
    // Three cells of the fine grid either side of the surface: as far as a
    // ray steps at once, and enough for its normal.
    w.band = 1.5f * d.voxel;
    w.particles = sim.particleCount();
    w.litres = sim.volume();
    Grid distance, foam;
    sim.surfaceField(2, w.band, distance, foam);
    w.cells.resize(2 * distance.size());
    const float* dist = distance.data();
    const float* white = foam.data();
    uint8_t* out = w.cells.data();
    const float scale = 0.5f / w.band;
    pg::parallelFor(distance.size(), 16384, [&](size_t begin, size_t end) {
        for (size_t c = begin; c < end; ++c) {
            const float x = std::clamp(dist[c] * scale + 0.5f, 0.0f, 1.0f);
            out[2 * c] = static_cast<uint8_t>(std::lround(x * 255.0f));
            out[2 * c + 1] = static_cast<uint8_t>(std::lround(std::clamp(white[c], 0.0f, 1.0f) * 255.0f));
        }
    });
    return w;
}

RainFrame capture(const RainSolver& sim) {
    RainFrame r;
    auto pack = [](const std::vector<RainParticle>& from, std::vector<float>& to, std::vector<uint32_t>& ids) {
        to.resize(6 * from.size());
        ids.resize(from.size());
        for (size_t i = 0; i < from.size(); ++i) {
            const RainParticle& p = from[i];
            float* o = to.data() + 6 * i;
            o[0] = p.position.x, o[1] = p.position.y, o[2] = p.position.z;
            o[3] = p.velocity.x, o[4] = p.velocity.y, o[5] = p.velocity.z;
            ids[i] = p.id;
        }
    };
    pack(sim.drops(), r.drops, r.dropIds);
    pack(sim.droplets(), r.droplets, r.dropletIds);
    r.timeStep = sim.scene().rain.timeStep;
    const Ripples& w = sim.ripples();
    if (!w.empty()) {
        r.rippleOrigin = w.origin;
        r.rippleCell = w.cell;
        r.rippleCells[0] = w.nx;
        r.rippleCells[1] = w.nz;
        r.ripples.resize(w.height.size());
        for (size_t i = 0; i < w.height.size(); ++i) r.ripples[i] = toHalf(w.height[i]);
    }
    return r;
}

}  // namespace pg::sim
