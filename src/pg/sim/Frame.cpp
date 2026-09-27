#include "pg/sim/Frame.h"

#include "pg/core/Parallel.h"
#include "pg/sim/Pyro.h"

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

}  // namespace pg::sim
