#pragma once
//
// Half floats (IEEE 754 binary16): what the frames keep the gas in and
// OpenEXR keeps pictures in -- 1 sign bit, 5 of exponent, 10 of mantissa.
//
#include <cmath>
#include <cstdint>
#include <cstring>

namespace pg {

/// Round to nearest even; beyond the largest half, infinity; NaN stays NaN.
inline uint16_t halfFromFloat(float value) {
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

inline float floatFromHalf(uint16_t value) {
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

}  // namespace pg
