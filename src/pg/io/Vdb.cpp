#include "pg/io/Vdb.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <string_view>

namespace pg::io {
namespace {

/// Little-endian bytes into a string, whatever the machine is.
class Out {
public:
    std::string bytes;

    void u8(uint8_t v) { bytes.push_back(static_cast<char>(v)); }
    void u32(uint32_t v) {
        for (int i = 0; i < 4; ++i) u8(static_cast<uint8_t>(v >> (8 * i)));
    }
    void i32(int32_t v) { u32(static_cast<uint32_t>(v)); }
    void u64(uint64_t v) {
        for (int i = 0; i < 8; ++i) u8(static_cast<uint8_t>(v >> (8 * i)));
    }
    void i64(int64_t v) { u64(static_cast<uint64_t>(v)); }
    void f32(float v) {
        uint32_t b = 0;
        std::memcpy(&b, &v, sizeof b);
        u32(b);
    }
    void f64(double v) {
        uint64_t b = 0;
        std::memcpy(&b, &v, sizeof b);
        u64(b);
    }
    void raw(std::string_view s) { bytes.append(s); }
    /// OpenVDB's writeString: the length, 32 bits, then the characters.
    void string(std::string_view s) {
        u32(static_cast<uint32_t>(s.size()));
        raw(s);
    }
    size_t pos() const { return bytes.size(); }
    void i64At(size_t at, int64_t v) {
        for (int i = 0; i < 8; ++i) bytes[at + static_cast<size_t>(i)] = static_cast<char>(static_cast<uint64_t>(v) >> (8 * i));
    }
};

constexpr int64_t kMagic = 0x56444220;         // "VDB "
constexpr uint32_t kFileVersion = 224;         // what OpenVDB 6 to 12 write
constexpr uint32_t kCompressActiveMask = 0x2;  // COMPRESS_ACTIVE_MASK, no zip, no blosc
constexpr uint8_t kNoMaskOrInactiveVals = 0;   // every inactive value is the background: only active ones follow

/// A bit per slot of a node, as OpenVDB's NodeMask keeps them: 64-bit
/// words, slot n in bit n % 64 of word n / 64. A slot's number runs x
/// slowest, z fastest.
struct Mask {
    std::vector<uint64_t> words;
    explicit Mask(size_t bits = 512) : words(bits / 64, 0) {}
    void set(size_t n) { words[n >> 6] |= uint64_t(1) << (n & 63); }
    bool on(size_t n) const { return ((words[n >> 6] >> (n & 63)) & 1u) != 0; }
    size_t count() const {
        size_t c = 0;
        for (const uint64_t w : words) c += static_cast<size_t>(std::popcount(w));
        return c;
    }
    void save(Out& out) const {
        for (const uint64_t w : words) out.u64(w);
    }
};

using Coord = std::array<int, 3>;  // compared x, then y, then z: as OpenVDB's Coord

/// A leaf: 8^3 voxels, those not 0 active -- a float each, or three (a
/// vector's x, y and z).
struct Leaf {
    Mask active{512};
    std::array<float, 512 * 3> values{};
};

/// The slot of `c` in a node of 2^log2 slots a side whose slots are
/// 2^shift voxels wide, at `origin`.
size_t slot(const Coord& c, const Coord& origin, int log2, int shift) {
    const size_t x = static_cast<size_t>((c[0] - origin[0]) >> shift), y = static_cast<size_t>((c[1] - origin[1]) >> shift),
                 z = static_cast<size_t>((c[2] - origin[2]) >> shift);
    return (x << (2 * log2)) | (y << log2) | z;
}

Coord below(const Coord& c, int bits) {
    const int m = ~((1 << bits) - 1);
    return {c[0] & m, c[1] & m, c[2] & m};
}

uint64_t fnv(uint64_t h, const void* data, size_t n) {
    const auto* p = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

/// A UUID from what the file holds: the same volumes, the same file.
std::string uuidOf(const std::vector<Volume>& volumes) {
    uint64_t a = 0xcbf29ce484222325ull, b = 0x84222325cbf29ce4ull;
    for (const Volume& v : volumes) {
        a = fnv(a, v.name.data(), v.name.size());
        b = fnv(b, v.res, sizeof v.res);
        if (v.values && !v.values->empty()) {
            a = fnv(a, v.values->data(), v.values->size() * sizeof(float));
            b = fnv(b, &v.voxel, sizeof v.voxel);
        }
    }
    char s[40];
    std::snprintf(s, sizeof s, "%08x-%04x-4%03x-%04x-%012llx", static_cast<unsigned>(a >> 32), static_cast<unsigned>((a >> 16) & 0xffff),
                  static_cast<unsigned>(a & 0xfff), static_cast<unsigned>(0x8000 | ((b >> 48) & 0x3fff)),
                  static_cast<unsigned long long>(b & 0xffffffffffffull));
    return s;
}

void metadata(Out& out, std::string_view name, std::string_view type, const std::string& value) {
    out.string(name);
    out.string(type);
    out.string(value);  // the size, then the value's bytes
}

/// One grid, from where its descriptor starts: a float grid of `parts[0]`,
/// or -- three parts, laid out alike -- a vector grid of their x, y and z.
void writeGrid(Out& out, std::span<const Volume* const> parts, const std::string& name) {
    const Volume& v = *parts[0];
    const size_t width = parts.size();
    // The voxels that are not 0, in the leaves they fall in.
    std::map<Coord, Leaf> leaves;
    int lo[3] = {1 << 30, 1 << 30, 1 << 30}, hi[3] = {-(1 << 30), -(1 << 30), -(1 << 30)};
    size_t active = 0;
    bool fog = width == 1;
    for (int k = 0; k < v.res[2]; ++k) {
        for (int j = 0; j < v.res[1]; ++j) {
            for (int i = 0; i < v.res[0]; ++i) {
                const size_t at = static_cast<size_t>(i) +
                                  static_cast<size_t>(v.res[0]) * (static_cast<size_t>(j) + static_cast<size_t>(v.res[1]) * static_cast<size_t>(k));
                float x[3] = {0.0f, 0.0f, 0.0f};
                bool any = false;
                for (size_t c = 0; c < width; ++c) {
                    const Volume& part = *parts[c];
                    x[c] = part.values && at < part.values->size() ? (*part.values)[at] : 0.0f;
                    if (!std::isfinite(x[c])) x[c] = 0.0f;
                    any = any || x[c] != 0.0f;
                }
                if (!any) continue;
                fog = fog && x[0] > 0.0f;
                Leaf& leaf = leaves[below({i, j, k}, 3)];
                const size_t n = (static_cast<size_t>(i & 7) << 6) | (static_cast<size_t>(j & 7) << 3) | static_cast<size_t>(k & 7);
                leaf.active.set(n);
                for (size_t c = 0; c < width; ++c) leaf.values[width * n + c] = x[c];
                ++active;
                lo[0] = std::min(lo[0], i), lo[1] = std::min(lo[1], j), lo[2] = std::min(lo[2], k);
                hi[0] = std::max(hi[0], i), hi[1] = std::max(hi[1], j), hi[2] = std::max(hi[2], k);
            }
        }
    }
    if (active == 0) lo[0] = lo[1] = lo[2] = hi[0] = hi[1] = hi[2] = 0;
    // Leaves under the 16^3 nodes (128 voxels a side), those under the 32^3
    // nodes (4096): in the order of their slots, as a map keeps them.
    std::map<Coord, std::vector<Coord>> mids, tops;
    for (const auto& [origin, leaf] : leaves) mids[below(origin, 7)].push_back(origin);
    for (const auto& [origin, children] : mids) tops[below(origin, 12)].push_back(origin);

    // The descriptor: name, type, no parent; where the grid is, filled in below.
    out.string(name);
    out.string(width == 3 ? "Tree_vec3s_5_4_3" : "Tree_float_5_4_3");
    out.string("");
    const size_t positions = out.pos();
    out.i64(0);
    out.i64(0);
    out.i64(0);
    const size_t gridPos = out.pos();
    out.u32(kCompressActiveMask);

    // Metadata, by name.
    Out vec;
    auto bbox = [&](const int c[3]) {
        vec.bytes.clear();
        for (int a = 0; a < 3; ++a) vec.i32(c[a]);
        return vec.bytes;
    };
    Out count;
    count.i64(static_cast<int64_t>(active));
    Out bytes;
    bytes.i64(static_cast<int64_t>(leaves.size() * (512 * 4 * width + 128) + mids.size() * 20480 + tops.size() * 135168));
    out.u32(width == 3 ? 7 : 6);
    metadata(out, "class", "string", fog ? "fog volume" : "unknown");
    metadata(out, "file_bbox_max", "vec3i", bbox(hi));
    metadata(out, "file_bbox_min", "vec3i", bbox(lo));
    metadata(out, "file_mem_bytes", "int64", bytes.bytes);
    metadata(out, "file_voxel_count", "int64", count.bytes);
    metadata(out, "name", "string", name);
    // A velocity's: the same in any space (OpenVDB's VEC_INVARIANT).
    if (width == 3) metadata(out, "vector_type", "string", "invariant");

    // The transform: voxel (i, j, k) at origin + (i + 1/2, j + 1/2, k + 1/2) x voxel.
    const double s = v.voxel > 0.0f ? static_cast<double>(v.voxel) : 1.0;
    out.string("UniformScaleTranslateMap");
    auto vec3 = [&](double x, double y, double z) {
        out.f64(x);
        out.f64(y);
        out.f64(z);
    };
    vec3(static_cast<double>(v.origin.x) + 0.5 * s, static_cast<double>(v.origin.y) + 0.5 * s, static_cast<double>(v.origin.z) + 0.5 * s);
    vec3(s, s, s);                    // scale
    vec3(s, s, s);                    // voxel size
    vec3(1.0 / s, 1.0 / s, 1.0 / s);  // the inverse scale
    vec3(1.0 / (s * s), 1.0 / (s * s), 1.0 / (s * s));
    vec3(0.5 / s, 0.5 / s, 0.5 / s);

    // The topology: one buffer; the root -- background 0, no tiles -- and
    // its nodes, each with its masks and (none but the background) values.
    out.i32(1);
    for (size_t c = 0; c < width; ++c) out.f32(0.0f);
    out.u32(0);
    out.u32(static_cast<uint32_t>(tops.size()));
    for (const auto& [top, children] : tops) {
        for (int a = 0; a < 3; ++a) out.i32(top[static_cast<size_t>(a)]);
        Mask child(32768), value(32768);
        for (const Coord& mid : children) child.set(slot(mid, top, 5, 7));
        child.save(out);
        value.save(out);
        out.u8(kNoMaskOrInactiveVals);
        for (const Coord& mid : children) {
            Mask c(4096), val(4096);
            for (const Coord& leaf : mids.at(mid)) c.set(slot(leaf, mid, 4, 3));
            c.save(out);
            val.save(out);
            out.u8(kNoMaskOrInactiveVals);
            for (const Coord& leaf : mids.at(mid)) leaves.at(leaf).active.save(out);  // a leaf's topology: its mask
        }
    }
    // The leaves' values, in the same order: the mask again, then the active values.
    const size_t blockPos = out.pos();
    for (const auto& [top, children] : tops) {
        for (const Coord& mid : children) {
            for (const Coord& origin : mids.at(mid)) {
                const Leaf& leaf = leaves.at(origin);
                leaf.active.save(out);
                out.u8(kNoMaskOrInactiveVals);
                for (size_t n = 0; n < 512; ++n) {
                    if (!leaf.active.on(n)) continue;
                    for (size_t c = 0; c < width; ++c) out.f32(leaf.values[width * n + c]);
                }
            }
        }
    }
    const size_t endPos = out.pos();
    out.i64At(positions, static_cast<int64_t>(gridPos));
    out.i64At(positions + 8, static_cast<int64_t>(blockPos));
    out.i64At(positions + 16, static_cast<int64_t>(endPos));
}

}  // namespace

std::string formatVdb(const std::vector<Volume>& volumes) {
    Out out;
    out.i64(kMagic);
    out.u32(kFileVersion);
    out.u32(10);  // the library version the format is OpenVDB 10's
    out.u32(0);
    out.u8(1);    // the grids' offsets are there
    out.raw(uuidOf(volumes));
    // File metadata.
    out.u32(1);
    metadata(out, "creator", "string", "prototype");
    // A vector's parts -- "vel.x", "vel.y", "vel.z" in turn, laid out alike
    // -- are one grid of it, "vel".
    std::vector<std::vector<const Volume*>> grids;
    for (size_t i = 0; i < volumes.size(); ++i) {
        const Volume& v = volumes[i];
        const std::string& n = v.name;
        auto alike = [&](const Volume& o, char axis) {
            return o.name.size() == n.size() && o.name.compare(0, n.size() - 1, n, 0, n.size() - 1) == 0 &&
                   o.name.back() == axis && o.res[0] == v.res[0] && o.res[1] == v.res[1] && o.res[2] == v.res[2] &&
                   o.origin == v.origin && o.voxel == v.voxel;
        };
        if (n.size() > 2 && n.compare(n.size() - 2, 2, ".x") == 0 && i + 2 < volumes.size() && alike(volumes[i + 1], 'y') &&
            alike(volumes[i + 2], 'z')) {
            grids.push_back({&v, &volumes[i + 1], &volumes[i + 2]});
            i += 2;
        } else {
            grids.push_back({&v});
        }
    }
    out.i32(static_cast<int32_t>(grids.size()));
    std::set<std::string> names;
    for (const std::vector<const Volume*>& parts : grids) {
        std::string base = parts[0]->name.empty() ? "volume" : parts[0]->name;
        if (parts.size() == 3) base = base.substr(0, base.size() - 2);
        std::string name = base;
        for (int n = 2; names.count(name); ++n) name = base + "_" + std::to_string(n);
        names.insert(name);
        writeGrid(out, parts, name);
    }
    return std::move(out.bytes);
}

bool writeVdb(const std::vector<Volume>& volumes, const std::string& path, std::string& error) {
    if (volumes.empty()) {
        error = "no volume to write";
        return false;
    }
    const std::string bytes = formatVdb(volumes);
    std::ofstream file(path, std::ios::binary);
    if (!file || !file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()))) {
        error = path + ": cannot write it";
        return false;
    }
    return true;
}

}  // namespace pg::io
