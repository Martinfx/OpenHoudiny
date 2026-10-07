#include "pg/gpu/SelfTest.h"

#include "pg/core/Parallel.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <random>
#include <utility>
#include <vector>

namespace pg::gpu {
namespace {

constexpr int kRounds = 5;  ///< each kernel timed over so many runs

double msSince(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}

/// The work groups of `n` items 256 a group, as x by y: no more than the
/// least any device takes along x.
void groupsOf(size_t n, uint32_t& x, uint32_t& y) {
    const size_t groups = std::max<size_t>((n + 255) / 256, 1);
    x = static_cast<uint32_t>(std::min<size_t>(groups, 65535));
    y = static_cast<uint32_t>((groups + x - 1) / x);
}

/// One sweep on the CPU, as jacobi.comp makes it.
void jacobiCpu(const std::vector<float>& in, const std::vector<float>& rhs, std::vector<float>& out, int n) {
    const auto at = [&](int x, int y, int z) {
        if (x < 0 || y < 0 || z < 0 || x >= n || y >= n || z >= n) return 0.0f;
        return in[static_cast<size_t>(x) + static_cast<size_t>(n) * (static_cast<size_t>(y) + static_cast<size_t>(n) * static_cast<size_t>(z))];
    };
    const float sixth = static_cast<float>(1.0 / 6.0);
    pg::parallelFor(static_cast<size_t>(n), 1, [&](size_t begin, size_t end) {
        for (int z = static_cast<int>(begin); z < static_cast<int>(end); ++z) {
            for (int y = 0; y < n; ++y) {
                for (int x = 0; x < n; ++x) {
                    const size_t i = static_cast<size_t>(x) + static_cast<size_t>(n) * (static_cast<size_t>(y) + static_cast<size_t>(n) * static_cast<size_t>(z));
                    float sum = at(x - 1, y, z) + at(x + 1, y, z);
                    sum = sum + at(x, y - 1, z);
                    sum = sum + at(x, y + 1, z);
                    sum = sum + at(x, y, z - 1);
                    sum = sum + at(x, y, z + 1);
                    out[i] = (sum - rhs[i]) * sixth;
                }
            }
        }
    });
}

uint32_t bitsOf(float v) {
    uint32_t b = 0;
    std::memcpy(&b, &v, sizeof b);
    return b;
}

float floatOf(uint32_t b) {
    float v = 0.0f;
    std::memcpy(&v, &b, sizeof v);
    return v;
}

bool tiny(float v) { return v != 0.0f && std::fpclassify(v) == FP_SUBNORMAL; }

/// The same bits, or a NaN for a NaN: which NaN is no matter.
bool same(float gpu, float cpu) { return bitsOf(gpu) == bitsOf(cpu) || (std::isnan(gpu) && std::isnan(cpu)); }

}  // namespace

ArithmeticCheck checkArithmetic(Device& device, size_t numbers, uint32_t seed) {
    // Of every kind, a quarter each: any bits at all; subnormal numbers by
    // ordinary ones; numbers whose products and quotients come out near
    // 2^-126; and numbers between 1 and 2, where most of the rounding is.
    std::mt19937 random(seed);
    std::vector<float> x(numbers), y(numbers);
    auto between = [&](int lo, int hi) {  // a positive or negative number of 2^lo .. 2^hi
        const uint32_t e = static_cast<uint32_t>(lo + 127 + static_cast<int>(random() % static_cast<uint32_t>(hi - lo + 1)));
        return floatOf((random() & 0x807fffffu) | (e << 23));
    };
    for (size_t i = 0; i < numbers; ++i) {
        switch (i % 4) {
            case 0: x[i] = floatOf(random()); y[i] = floatOf(random()); break;
            case 1: x[i] = floatOf(random() & 0x807fffffu); y[i] = between(-4, 4); break;
            case 2: x[i] = between(-80, -40); y[i] = i % 8 == 2 ? between(-80, -40) : between(40, 80); break;
            default: x[i] = between(0, 0); y[i] = between(0, 0); break;
        }
    }
    return checkArithmetic(device, x, y);
}

ArithmeticCheck checkArithmetic(Device& device, const std::vector<float>& x, const std::vector<float>& y) {
    ArithmeticCheck r;
    const size_t numbers = std::min(x.size(), y.size());
    r.checked = numbers;
    const size_t bytes = numbers * sizeof(float);
    auto bx = device.buffer(bytes), by = device.buffer(bytes), bo = device.buffer(3 * bytes);
    if (!bx || !by || !bo || !device.upload(*bx, x.data(), bytes) || !device.upload(*by, y.data(), bytes)) {
        r.error = device.error();
        return r;
    }
    const uint32_t n = static_cast<uint32_t>(numbers);
    const uint32_t groups = static_cast<uint32_t>(std::clamp<size_t>((numbers + 255) / 256, 1, 65535));
    Batch batch(device);
    batch.dispatch("arith_check", {bx.get(), by.get(), bo.get()}, n, groups);
    std::vector<uint32_t> out(3 * numbers);
    if (batch.run() < 0.0 || !device.download(*bo, out.data(), 3 * bytes)) {
        r.error = device.error();
        return r;
    }
    for (size_t i = 0; i < numbers; ++i) {
        bool wrong = false;
        if (!same(floatOf(out[3 * i]), x[i] / y[i])) {
            ++r.divisionWrong;
            wrong = true;
        }
        if (!same(floatOf(out[3 * i + 1]), std::sqrt(x[i]))) {
            ++r.sqrtWrong;
            wrong = true;
        }
        const float product = x[i] * y[i];
        if (!same(floatOf(out[3 * i + 2]), product)) {
            if (tiny(x[i]) || tiny(y[i]) || tiny(product)) {
                ++r.subnormalsLost;
            } else {
                ++r.productWrong;
                wrong = true;
            }
        }
        if (wrong && r.divisionWrong + r.sqrtWrong + r.productWrong == 1) {
            r.x = x[i];
            r.y = y[i];
        }
    }
    return r;
}

float sumLikeTheGpu(const float* v, size_t n) {
    std::vector<float> now(v, v + n), next;
    do {
        const size_t blocks = std::max<size_t>((now.size() + 255) / 256, 1);
        next.assign(blocks, 0.0f);
        for (size_t b = 0; b < blocks; ++b) {
            float part[256];
            for (size_t t = 0; t < 256; ++t) part[t] = b * 256 + t < now.size() ? now[b * 256 + t] : 0.0f;
            for (size_t half = 128; half > 0; half >>= 1) {
                for (size_t t = 0; t < half; ++t) part[t] = part[t] + part[t + half];
            }
            next[b] = part[0];
        }
        now.swap(next);
    } while (now.size() > 1);
    return now.empty() ? 0.0f : now[0];
}

SelfTest selfTest(Device& device, size_t floats, int side) {
    SelfTest r;
    r.floats = floats;
    r.side = side;
    auto failed = [&] {
        r.error = device.error();
        return r;
    };

    // Memory: y = 2 x + y, the numbers small whole ones -- exact however
    // the device rounds.
    std::vector<float> x(floats), y(floats, 1.0f);
    for (size_t i = 0; i < floats; ++i) x[i] = static_cast<float>(i % 1024);
    auto bx = device.buffer(floats * sizeof(float)), by = device.buffer(floats * sizeof(float));
    if (!bx || !by || !device.upload(*bx, x.data(), floats * sizeof(float)) ||
        !device.upload(*by, y.data(), floats * sizeof(float))) {
        return failed();
    }
    struct {
        uint32_t n;
        float a;
    } saxpy{static_cast<uint32_t>(floats), 2.0f};
    const uint32_t groups = static_cast<uint32_t>(std::min<size_t>((floats + 255) / 256, 65535));
    Batch warm(device);
    warm.dispatch("saxpy", {bx.get(), by.get()}, saxpy, groups);
    if (warm.run() < 0.0) return failed();
    Batch timed(device);
    for (int k = 0; k < kRounds; ++k) timed.dispatch("saxpy", {bx.get(), by.get()}, saxpy, groups);
    const double gpuMs = timed.run();
    if (gpuMs < 0.0) return failed();
    std::vector<float> back(floats);
    if (!device.download(*by, back.data(), floats * sizeof(float))) return failed();
    r.saxpyRight = true;
    for (size_t i = 0; i < floats && r.saxpyRight; ++i) r.saxpyRight = back[i] == 1.0f + 2.0f * (kRounds + 1) * x[i];
    const double moved = 3.0 * sizeof(float) * static_cast<double>(floats) * kRounds;
    r.gpuGBs = moved / (gpuMs * 1e-3) / 1e9;
    auto t = std::chrono::steady_clock::now();
    for (int k = 0; k < kRounds; ++k) {
        pg::parallelFor(floats, 65536, [&](size_t b, size_t e) {
            for (size_t i = b; i < e; ++i) y[i] = 2.0f * x[i] + y[i];
        });
    }
    r.cpuGBs = moved / (msSince(t) * 1e-3) / 1e9;

    // A sum, the same to the bit: numbers of 4096ths, no subnormals.
    for (size_t i = 0; i < floats; ++i) x[i] = static_cast<float>(i % 4096) / 4096.0f;
    if (!device.upload(*bx, x.data(), floats * sizeof(float))) return failed();
    {
        Batch sum(device);
        // In passes, each of a 256th as many, from one buffer into the other
        // and back: what the first held is not needed after it.
        Buffer* from = bx.get();
        Buffer* to = by.get();
        size_t n = floats;
        do {
            uint32_t gx = 0, gy = 0;
            groupsOf(n, gx, gy);
            sum.dispatch("reduce", {from, to}, static_cast<uint32_t>(n), gx, gy);
            n = (n + 255) / 256;
            std::swap(from, to);
        } while (n > 1);
        Buffer* last = from;
        r.reduceMs = sum.run();
        if (r.reduceMs < 0.0 || !device.download(*last, &r.gpuSum, sizeof(float))) return failed();
    }
    r.cpuSum = sumLikeTheGpu(x.data(), floats);

    // Jacobi sweeps over side^3 cells: in -> out, then back.
    const size_t cells = static_cast<size_t>(side) * side * side;
    std::vector<float> p(cells), rhs(cells), q(cells);
    for (size_t i = 0; i < cells; ++i) {
        p[i] = static_cast<float>(i % 97) / 97.0f;
        rhs[i] = static_cast<float>(i % 89) / 89.0f - 0.5f;
    }
    auto bp = device.buffer(cells * sizeof(float)), bq = device.buffer(cells * sizeof(float)),
         brhs = device.buffer(cells * sizeof(float));
    if (!bp || !bq || !brhs || !device.upload(*bp, p.data(), cells * sizeof(float)) ||
        !device.upload(*brhs, rhs.data(), cells * sizeof(float))) {
        return failed();
    }
    struct {
        uint32_t nx, ny, nz;
    } grid{static_cast<uint32_t>(side), static_cast<uint32_t>(side), static_cast<uint32_t>(side)};
    const uint32_t gx = static_cast<uint32_t>((side + 7) / 8), gz = static_cast<uint32_t>((side + 3) / 4);
    Batch sweeps(device);
    for (int k = 0; k < 2 * kRounds; ++k) {
        sweeps.dispatch("jacobi", {k % 2 == 0 ? bp.get() : bq.get(), brhs.get(), k % 2 == 0 ? bq.get() : bp.get()}, grid,
                        gx, gx, gz);
    }
    const double jacobiMs = sweeps.run();
    if (jacobiMs < 0.0) return failed();
    std::vector<float> gpuP(cells);
    if (!device.download(*bp, gpuP.data(), cells * sizeof(float))) return failed();
    r.gpuCellsPerS = static_cast<double>(cells) * 2 * kRounds / (jacobiMs * 1e-3);
    t = std::chrono::steady_clock::now();
    for (int k = 0; k < kRounds; ++k) {
        jacobiCpu(p, rhs, q, side);
        jacobiCpu(q, rhs, p, side);
    }
    r.cpuCellsPerS = static_cast<double>(cells) * 2 * kRounds / (msSince(t) * 1e-3);
    r.jacobiSame = gpuP == p;
    r.arithmetic = checkArithmetic(device, std::min<size_t>(floats / 8, size_t{1} << 20));
    if (!r.arithmetic.error.empty()) r.error = r.arithmetic.error;
    return r;
}

}  // namespace pg::gpu
