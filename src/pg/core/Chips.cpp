#include "pg/core/Chips.h"

#include <algorithm>
#include <cmath>

namespace pg {
namespace {

constexpr float kPi = 3.14159265358979f;

/// A way, any way as likely as any other.
Vec3 anyWay(float u, float v) {
    const float z = 2.0f * u - 1.0f, r = std::sqrt(std::max(0.0f, 1.0f - z * z)), phi = 2.0f * kPi * v;
    return {r * std::cos(phi), r * std::sin(phi), z};
}

/// The faces of a box `h` from its middle each way.
std::vector<ChipFace> boxFaces(const Vec3& h) {
    auto corner = [&](int i) { return Vec3(i & 1 ? h.x : -h.x, i & 2 ? h.y : -h.y, i & 4 ? h.z : -h.z); };
    const int faces[6][4] = {{0, 4, 6, 2}, {1, 3, 7, 5}, {0, 1, 5, 4}, {2, 6, 7, 3}, {0, 2, 3, 1}, {4, 5, 7, 6}};
    std::vector<ChipFace> out;
    for (const auto& f : faces) out.push_back({corner(f[0]), corner(f[1]), corner(f[2]), corner(f[3])});
    return out;
}

/// `p` without the corners that are where the one before them is.
void dropRepeats(ChipFace& p) {
    ChipFace kept;
    for (const Vec3& c : p) {
        if (kept.empty() || length(c - kept.back()) > 1e-6f) kept.push_back(c);
    }
    while (kept.size() > 1 && length(kept.front() - kept.back()) <= 1e-6f) kept.pop_back();
    p = std::move(kept);
}

/// What of a convex solid's `faces` is on the near side of the plane where
/// dot(n, p) = d: the faces it cuts cut, the cut a face of its own.
void clip(std::vector<ChipFace>& faces, const Vec3& n, float d) {
    std::vector<ChipFace> out;
    std::vector<Vec3> cut;
    for (const ChipFace& f : faces) {
        ChipFace kept;
        for (size_t i = 0; i < f.size(); ++i) {
            const Vec3& a = f[i];
            const Vec3& b = f[(i + 1) % f.size()];
            const float da = dot(n, a) - d, db = dot(n, b) - d;
            if (da <= 0.0f) kept.push_back(a);
            if ((da <= 0.0f) != (db <= 0.0f)) {
                const Vec3 x = a + (b - a) * (da / (da - db));
                kept.push_back(x);
                cut.push_back(x);
            }
        }
        dropRepeats(kept);
        if (kept.size() >= 3) out.push_back(std::move(kept));
    }
    // The cut's corners, each once, round the plane's normal.
    ChipFace corners;
    for (const Vec3& p : cut) {
        if (std::none_of(corners.begin(), corners.end(), [&](const Vec3& q) { return length(p - q) <= 1e-5f; })) {
            corners.push_back(p);
        }
    }
    if (corners.size() >= 3) {
        Vec3 mid(0.0f);
        for (const Vec3& p : corners) mid += p;
        mid /= static_cast<float>(corners.size());
        const Vec3 u = normalize(cross(n, std::fabs(n.x) < 0.9f ? Vec3(1.0f, 0.0f, 0.0f) : Vec3(0.0f, 1.0f, 0.0f)));
        const Vec3 v = cross(n, u);
        std::sort(corners.begin(), corners.end(), [&](const Vec3& a, const Vec3& b) {
            return std::atan2(dot(a - mid, v), dot(a - mid, u)) < std::atan2(dot(b - mid, v), dot(b - mid, u));
        });
        out.push_back(std::move(corners));
    }
    faces = std::move(out);
}

/// A chip of stone: a box flattened and stretched, six to nine of its
/// corners and edges broken off -- each plane cutting the chip as it is
/// by then, somewhere between a third and a tenth of the way in.
std::vector<ChipFace> stone(uint32_t seed) {
    std::vector<ChipFace> faces =
        boxFaces(Vec3(1.0f, 0.55f + 0.3f * chipRandom(seed, 1), 0.3f + 0.25f * chipRandom(seed, 2)));
    const int breaks = 6 + static_cast<int>(chipRandom(seed, 3) * 4.0f);
    for (int k = 0; k < breaks; ++k) {
        const uint32_t i = static_cast<uint32_t>(k);
        const Vec3 n = anyWay(chipRandom(seed, 10 + 2 * i), chipRandom(seed, 11 + 2 * i));
        float reach = 0.0f;
        for (const ChipFace& f : faces) {
            for (const Vec3& p : f) reach = std::max(reach, dot(n, p));
        }
        clip(faces, n, reach * (0.6f + 0.3f * chipRandom(seed, 40 + i)));
    }
    return faces;
}

/// A sliver of glass: flat, of three to five sides, as thick as a pane
/// is to a hand-sized shard.
std::vector<ChipFace> sliver(uint32_t seed) {
    const int sides = 3 + static_cast<int>(chipRandom(seed, 1) * 3.0f);
    const float half = 0.025f + 0.02f * chipRandom(seed, 2);
    ChipFace ring;
    for (int k = 0; k < sides; ++k) {
        const uint32_t i = static_cast<uint32_t>(k);
        const float a =
            2.0f * kPi * (static_cast<float>(k) + 0.6f * (chipRandom(seed, 10 + i) - 0.5f)) / static_cast<float>(sides);
        const float r = 0.55f + 0.45f * chipRandom(seed, 20 + i);
        ring.push_back(Vec3(r * std::cos(a), 0.0f, r * std::sin(a)));
    }
    const Vec3 up(0.0f, half, 0.0f);
    // Round the ring as the angle grows is clockwise seen from above.
    ChipFace top, bottom;
    for (size_t k = ring.size(); k-- > 0;) top.push_back(ring[k] + up);
    for (const Vec3& q : ring) bottom.push_back(q - up);
    std::vector<ChipFace> faces = {top, bottom};
    for (size_t k = 0; k < ring.size(); ++k) {
        const Vec3& a = ring[k];
        const Vec3& b = ring[(k + 1) % ring.size()];
        faces.push_back({a - up, a + up, b + up, b - up});
    }
    return faces;
}

/// `faces` moved to have their corners' middle at the origin, sized to
/// have the farthest of them a unit from it.
void centred(std::vector<ChipFace>& faces) {
    Vec3 mid(0.0f);
    size_t n = 0;
    for (const ChipFace& f : faces) {
        for (const Vec3& p : f) {
            mid += p;
            ++n;
        }
    }
    if (n == 0) return;
    mid /= static_cast<float>(n);
    float far = 0.0f;
    for (const ChipFace& f : faces) {
        for (const Vec3& p : f) far = std::max(far, length(p - mid));
    }
    const float k = far > 0.0f ? 1.0f / far : 1.0f;
    for (ChipFace& f : faces) {
        for (Vec3& p : f) p = (p - mid) * k;
    }
}

}  // namespace

float chipRandom(uint32_t seed, uint32_t k) {
    uint32_t h = seed + k * 0x9e3779b9u;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return static_cast<float>(h >> 8) / 16777216.0f;
}

std::vector<ChipFace> chipFaces(size_t shape, bool glass) {
    const size_t which = shape % (glass ? kSliverShapes : kChipShapes);
    const uint32_t seed = static_cast<uint32_t>(which) * 7919u + (glass ? 104729u : 15485863u);
    std::vector<ChipFace> faces = glass ? sliver(seed) : stone(seed);
    centred(faces);
    return faces;
}

size_t chipShapeOf(uint32_t seed, bool glass) {
    const size_t shapes = glass ? kSliverShapes : kChipShapes;
    return std::min(static_cast<size_t>(chipRandom(seed, 0) * static_cast<float>(shapes)), shapes - 1);
}

Vec3 chipTint(const Vec3& color, uint32_t seed, bool glass) {
    if (glass) return color;
    const float grey = dot(color, Vec3(0.3f, 0.5f, 0.2f));
    return (color + (Vec3(grey) - color) * (0.4f * chipRandom(seed, 40))) * (0.7f + 0.55f * chipRandom(seed, 41));
}

Vec4 chipTurn(uint32_t seed) {
    const float u1 = chipRandom(seed, 60), u2 = 2.0f * kPi * chipRandom(seed, 61), u3 = 2.0f * kPi * chipRandom(seed, 62);
    const float a = std::sqrt(1.0f - u1), b = std::sqrt(u1);
    return Vec4(a * std::sin(u2), a * std::cos(u2), b * std::sin(u3), b * std::cos(u3));
}

}  // namespace pg
