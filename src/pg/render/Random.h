#pragma once
//
// The numbers a path tracer's sample takes (PathTracer.h): from its pixel,
// its number and the seed alone -- a render is the same on any number of
// threads -- and in the order the sample takes them, however many it takes.
//
#include <cstdint>

namespace pg::render {

inline uint32_t hash32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

/// The numbers of one sample of one pixel: the same whatever else runs.
struct Rng {
    uint32_t state;

    Rng(uint32_t pixel, uint32_t sample, uint32_t seed)
        : state(hash32(pixel * 0x9E3779B9u ^ hash32(sample * 0x85EBCA6Bu + seed * 0xC2B2AE35u + 0x27d4eb2fu))) {}
    /// In [0, 1).
    float next() {
        state = state * 747796405u + 2891336453u;
        uint32_t w = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
        w = (w >> 22u) ^ w;
        return static_cast<float>(w >> 8) * (1.0f / 16777216.0f);
    }
};

}  // namespace pg::render
