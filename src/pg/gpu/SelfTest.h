#pragma once
//
// What a device can do, measured against the CPU (prototype gpu): how fast
// it moves memory, a sum that comes out the same to the bit, and a sweep of
// the Poisson equation as the pressure of a gas solver makes them -- each
// checked against the same done on the CPU.
//
#include "pg/gpu/Gpu.h"

#include <cstddef>
#include <string>
#include <vector>

namespace pg::gpu {

/// The device's arithmetic against the CPU's over numbers of every kind --
/// normal, subnormal, infinite, NaN: exact.glsl's division and square root,
/// and the device's own product.
struct ArithmeticCheck {
    size_t checked = 0;
    size_t divisionWrong = 0, sqrtWrong = 0;  ///< not the CPU's to the bit (a NaN for a NaN will do)
    size_t productWrong = 0;                  ///< products not the CPU's, of numbers far from 0
    size_t subnormalsLost = 0;                ///< ... where a number, or the product, is below 2^-126
    float x = 0.0f, y = 0.0f;                 ///< the first numbers it got wrong
    std::string error;
    bool exact() const { return error.empty() && divisionWrong == 0 && sqrtWrong == 0 && productWrong == 0; }
};

ArithmeticCheck checkArithmetic(Device& device, size_t numbers, uint32_t seed = 1);
/// ... over x[i] / y[i], sqrt(x[i]), x[i] y[i].
ArithmeticCheck checkArithmetic(Device& device, const std::vector<float>& x, const std::vector<float>& y);

struct SelfTest {
    // y = a x + y over `floats` numbers: memory read twice and written once.
    size_t floats = 0;
    double gpuGBs = 0.0, cpuGBs = 0.0;
    bool saxpyRight = false;
    // The sum of `floats` numbers, in the same order on both.
    double reduceMs = 0.0;
    float gpuSum = 0.0f, cpuSum = 0.0f;
    // Jacobi sweeps over side^3 cells.
    int side = 0;
    double gpuCellsPerS = 0.0, cpuCellsPerS = 0.0;
    bool jacobiSame = false;
    ArithmeticCheck arithmetic;
    std::string error;  ///< what went wrong on the device; empty if nothing
};

/// Runs the self-test on `device`: `floats` numbers, a grid `side` cells a
/// side -- and the same on the CPU's threads.
SelfTest selfTest(Device& device, size_t floats, int side);

/// The sum of v[0..n) as reduce.comp makes it: blocks of 256, each added in
/// pairs, the halves one onto the other; then the same over the sums.
float sumLikeTheGpu(const float* v, size_t n);

}  // namespace pg::gpu
