// Division and the square root of 32-bit floats, rounded to the nearest (ties
// to even) as IEEE 754 has them -- as the CPU's `/` and std::sqrt round them
// -- in integer arithmetic alone, so the same bits on every device. A GPU's
// own need not be: Vulkan allows its division 2.5 units in the last place,
// its square root more. Subnormal numbers in and out, infinities and NaN as
// IEEE 754 says (a NaN's bits aside).

/// A result to its bits: `sig` has its leading 1 at bit 25 -- 24 bits, then
/// the guard and the round bit -- `sticky` whether anything was left below,
/// `e` the biased exponent of 1.xxx; rounded to the nearest, ties to even.
uint exactPack(uint sign, int e, uint sig, bool sticky) {
    if (e >= 255) return sign | 0x7f800000u;  // too big: infinity
    if (e <= 0) {
        // Below 2^-126: as a subnormal, shifted down to its place.
        const int shift = 1 - e;
        if (shift >= 26) {
            sticky = sticky || sig != 0u;
            sig = 0u;
        } else {
            sticky = sticky || (sig & ((1u << uint(shift)) - 1u)) != 0u;
            sig >>= uint(shift);
        }
        e = 0;
    }
    uint m = sig >> 2;
    const bool guard = (sig & 2u) != 0u;
    const bool rest = (sig & 1u) != 0u || sticky;
    if (guard && (rest || (m & 1u) != 0u)) m += 1u;
    // A carry out of the mantissa goes into the exponent, from a subnormal
    // to the least normal number, from the greatest to infinity.
    return sign | (e >= 1 ? (uint(e) << 23) + m - 0x800000u : m);
}

const uint kExactNaN = 0x7fc00000u;

/// a / b, rounded as the CPU rounds it.
float exactDiv(float a, float b) {
    const uint ua = floatBitsToUint(a), ub = floatBitsToUint(b);
    const uint sign = (ua ^ ub) & 0x80000000u;
    int ea = int((ua >> 23) & 0xffu), eb = int((ub >> 23) & 0xffu);
    uint ma = ua & 0x7fffffu, mb = ub & 0x7fffffu;
    if (ea == 255) {
        if (ma != 0u) return uintBitsToFloat(ua | 0x400000u);  // NaN
        if (eb == 255) return uintBitsToFloat(mb != 0u ? ub | 0x400000u : kExactNaN);
        return uintBitsToFloat(sign | 0x7f800000u);  // infinity / finite
    }
    if (eb == 255) return uintBitsToFloat(mb != 0u ? ub | 0x400000u : sign);  // finite / infinity: 0
    if (eb == 0 && mb == 0u) return uintBitsToFloat(ea == 0 && ma == 0u ? kExactNaN : sign | 0x7f800000u);
    if (ea == 0 && ma == 0u) return uintBitsToFloat(sign);
    // Both as 1.xxx with 23 bits after the point; a subnormal shifted up.
    if (ea == 0) {
        const int s = 23 - findMSB(ma);
        ma <<= uint(s);
        ea = 1 - s;
    } else {
        ma |= 0x800000u;
    }
    if (eb == 0) {
        const int s = 23 - findMSB(mb);
        mb <<= uint(s);
        eb = 1 - s;
    } else {
        mb |= 0x800000u;
    }
    int e = ea - eb + 127;
    if (ma < mb) {  // the quotient from [1, 2)
        ma <<= 1u;
        e -= 1;
    }
    // Long division: 26 bits of the quotient, and whether anything is left.
    uint q = 0u, rest = ma;
    for (int i = 0; i < 26; ++i) {
        q <<= 1u;
        if (rest >= mb) {
            rest -= mb;
            q |= 1u;
        }
        rest <<= 1u;
    }
    return uintBitsToFloat(exactPack(sign, e, q, rest != 0u));
}

/// The square root of x, rounded as the CPU rounds it.
float exactSqrt(float x) {
    const uint u = floatBitsToUint(x);
    int e = int((u >> 23) & 0xffu);
    uint m = u & 0x7fffffu;
    if (e == 255) {
        if (m != 0u) return uintBitsToFloat(u | 0x400000u);  // NaN
        return uintBitsToFloat((u & 0x80000000u) != 0u ? kExactNaN : u);  // -infinity: NaN; +infinity
    }
    if (e == 0 && m == 0u) return x;  // +0, -0
    if ((u & 0x80000000u) != 0u) return uintBitsToFloat(kExactNaN);
    if (e == 0) {
        const int s = 23 - findMSB(m);
        m <<= uint(s);
        e = 1 - s;
    } else {
        m |= 0x800000u;
    }
    // x = m 2^(p - 23); p made even, the root's exponent its half.
    int p = e - 127;
    if ((p & 1) != 0) {
        m <<= 1u;
        p -= 1;
    }
    // Digit by digit, the root of m 2^29 -- 54 bits, two at a time: 27 bits
    // of it, and whether anything is left.
    const uint hi = m >> 3u, lo = m << 29u;
    uint rest = 0u, root = 0u;
    for (int i = 26; i >= 0; --i) {
        const uint pair = 2 * i >= 32 ? (hi >> uint(2 * i - 32)) & 3u : (lo >> uint(2 * i)) & 3u;
        rest = (rest << 2u) | pair;
        const uint trial = (root << 2u) | 1u;
        root <<= 1u;
        if (rest >= trial) {
            rest -= trial;
            root |= 1u;
        }
    }
    return uintBitsToFloat(exactPack(0u, p / 2 + 127, root >> 1u, rest != 0u || (root & 1u) != 0u));
}
