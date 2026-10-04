#include "pg/sim/Cache.h"

#include "pg/io/Obj.h"
#include "pg/sim/SparseGrid.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace pg::sim {
namespace {

constexpr char kMagic[8] = {'P', 'G', 'F', 'R', 'A', 'M', 'E', '\0'};
// 2: the rigid bodies after the rain; 3: and their grit; 4: the particles'
// numbers -- the water's, the drops', the droplets', the grit's -- and how
// fast the grit goes; 5: how fast the water goes, on the solver's grid;
// 6: what became of the bars; 7: which grit is glass, which bodies came
// unglued; 8: what became of each joint of the glue; 9: how each bit of
// grit is turned; 10: a sparse gas's tiles; 11: the cloth; 12: the cloth
// torn; 13: a sparse liquid's tiles; 14: its tiles deep in the water;
// 15: the grains; 16: the pieces that broke as it ran; 17: the steam.
constexpr uint32_t kVersion = 17;

/// Little-endian bytes, whatever the machine is.
class Out {
public:
    std::string bytes;
    void u8(uint8_t v) { bytes.push_back(static_cast<char>(v)); }
    void u32(uint32_t v) {
        for (int i = 0; i < 4; ++i) u8(static_cast<uint8_t>(v >> (8 * i)));
    }
    void u64(uint64_t v) {
        for (int i = 0; i < 8; ++i) u8(static_cast<uint8_t>(v >> (8 * i)));
    }
    void i32(int32_t v) { u32(static_cast<uint32_t>(v)); }
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
    void u16(uint16_t v) {
        u8(static_cast<uint8_t>(v));
        u8(static_cast<uint8_t>(v >> 8));
    }
    void vec3(const Vec3& v) {
        f32(v.x);
        f32(v.y);
        f32(v.z);
    }
    void domain(const Domain& d) {
        for (const int c : d.cells) i32(c);
        f32(d.voxel);
    }
    void floats(const std::vector<float>& v) {
        u64(v.size());
        for (const float x : v) f32(x);
    }
    void bytesOf(const std::vector<uint8_t>& v) {
        u64(v.size());
        bytes.append(reinterpret_cast<const char*>(v.data()), v.size());
    }
    void words(const std::vector<uint32_t>& v) {
        u64(v.size());
        for (const uint32_t x : v) u32(x);
    }
    void text(const std::string& s) {
        u64(s.size());
        bytes.append(s);
    }
    /// Half floats, their runs of zeros packed: the count, then runs of
    /// (zeros, values that follow, the values).
    void halves(const std::vector<uint16_t>& v) {
        const size_t n = v.size();
        u64(n);
        size_t i = 0;
        while (i < n) {
            size_t zeros = 0;
            while (i + zeros < n && v[i + zeros] == 0) ++zeros;
            // The values after them, up to the next run of eight zeros.
            const size_t start = i + zeros;
            size_t j = start, trailing = 0;
            while (j < n && trailing < 8) {
                trailing = v[j] == 0 ? trailing + 1 : 0;
                ++j;
            }
            const size_t end = j - trailing;
            u32(static_cast<uint32_t>(zeros));
            u32(static_cast<uint32_t>(end - start));
            for (size_t k = start; k < end; ++k) u16(v[k]);
            i = end;
        }
    }
};

class In {
public:
    explicit In(std::string_view data) : data_(data) {}
    bool ok() const { return ok_; }

    uint64_t bits(int n) {
        if (pos_ + static_cast<size_t>(n) > data_.size()) {
            ok_ = false;
            pos_ = data_.size();
            return 0;
        }
        uint64_t v = 0;
        for (int i = 0; i < n; ++i) v |= static_cast<uint64_t>(static_cast<unsigned char>(data_[pos_ + static_cast<size_t>(i)])) << (8 * i);
        pos_ += static_cast<size_t>(n);
        return v;
    }
    uint8_t u8() { return static_cast<uint8_t>(bits(1)); }
    uint16_t u16() { return static_cast<uint16_t>(bits(2)); }
    uint32_t u32() { return static_cast<uint32_t>(bits(4)); }
    uint64_t u64() { return bits(8); }
    int32_t i32() { return static_cast<int32_t>(u32()); }
    float f32() {
        const uint32_t b = u32();
        float f = 0.0f;
        std::memcpy(&f, &b, sizeof f);
        return f;
    }
    double f64() {
        const uint64_t b = u64();
        double d = 0.0;
        std::memcpy(&d, &b, sizeof d);
        return d;
    }
    Vec3 vec3() {
        const float x = f32(), y = f32();
        return {x, y, f32()};
    }
    /// A grid of a sensible size -- up to 2048 cells a side, 2^33 in all:
    /// the solvers' grids go to 1024, the water's drawn one to twice that --
    /// with a voxel of a size.
    Domain domain() {
        Domain d;
        for (int& c : d.cells) c = i32();
        d.voxel = f32();
        for (const int c : d.cells) {
            if (c < 0 || c > 2048) ok_ = false;
        }
        if (!ok_ || d.cellCount() > (size_t(1) << 33) || !(d.voxel > 0.0f && d.voxel < 1e6f)) {
            ok_ = false;
            d = Domain();
        }
        return d;
    }
    /// A count of items `size` bytes each that the data can hold.
    size_t count(size_t size) {
        const uint64_t n = u64();
        if (!ok_ || n > (data_.size() - pos_) / std::max<size_t>(size, 1)) {
            ok_ = false;
            return 0;
        }
        return static_cast<size_t>(n);
    }
    void floats(std::vector<float>& v) {
        v.resize(count(4));
        for (float& x : v) x = f32();
    }
    void words(std::vector<uint32_t>& v) {
        v.resize(count(4));
        for (uint32_t& x : v) x = u32();
    }
    void bytesOf(std::vector<uint8_t>& v) {
        const size_t n = count(1);
        v.assign(data_.begin() + static_cast<std::ptrdiff_t>(pos_), data_.begin() + static_cast<std::ptrdiff_t>(pos_ + n));
        pos_ += n;
    }
    std::string text() {
        const size_t n = count(1);
        std::string s(data_.substr(pos_, n));
        pos_ += n;
        return s;
    }
    /// Half floats, `expected` of them -- or none: runs of zeros take
    /// little room, so the data cannot say how many are too many.
    void halves(std::vector<uint16_t>& v, size_t expected) { halves(v, expected, true); }
    /// As many as there are, up to `most`.
    void halvesAtMost(std::vector<uint16_t>& v, size_t most) { halves(v, most, false); }
    void halves(std::vector<uint16_t>& v, size_t expected, bool exactly) {
        const uint64_t n = u64();
        v.clear();
        if (!ok_ || n == 0) return;
        if (exactly ? n != expected : n > expected) {
            ok_ = false;
            return;
        }
        // The runs first: that they come to the count, their values there --
        // what is not a frame's says so before it takes the memory.
        const size_t start = pos_;
        for (size_t i = 0; i < n;) {
            const size_t zeros = u32(), literals = u32();
            if (!ok_ || zeros + literals > n - i || (zeros == 0 && literals == 0) || literals > (data_.size() - pos_) / 2) {
                ok_ = false;
                return;
            }
            i += zeros + literals;
            pos_ += 2 * literals;
        }
        pos_ = start;
        v.assign(static_cast<size_t>(n), 0);
        for (size_t i = 0; i < v.size();) {
            const size_t zeros = u32(), literals = u32();
            i += zeros;
            for (size_t k = 0; k < literals; ++k) v[i++] = u16();
        }
    }

private:
    std::string_view data_;
    size_t pos_ = 0;
    bool ok_ = true;
};

}  // namespace

uint64_t networkHash(std::string_view text) {
    uint64_t h = 0xcbf29ce484222325ull;  // FNV-1a
    auto add = [&](std::string_view s) {
        for (const char c : s) h = (h ^ static_cast<unsigned char>(c)) * 0x100000001b3ull;
    };
    while (!text.empty()) {
        const size_t eol = text.find('\n');
        std::string_view line = text.substr(0, eol);
        text = eol == std::string_view::npos ? std::string_view() : text.substr(eol + 1);
        // "node ID TYPE VERSION NAME X Y": where a node sits on the canvas is
        // not what it simulates.
        if (line.substr(0, 5) == "node ") {
            for (int k = 0; k < 2; ++k) {
                const size_t space = line.rfind(' ');
                if (space != std::string_view::npos && space > 4) line = line.substr(0, space);
            }
        }
        add(line);
        add("\n");
    }
    return h;
}

std::string formatFrame(const Frame& f) {
    Out out;
    out.bytes.append(kMagic, sizeof kMagic);
    out.u32(kVersion);
    out.i32(f.number);
    out.f32(f.time);
    out.f64(f.stepMs);
    out.domain(f.domain);
    out.halves(f.fields);
    // The water.
    const WaterFrame& w = f.water;
    out.domain(w.domain);
    out.f32(w.band);
    out.bytesOf(w.cells);
    out.u64(w.particles);
    out.f64(w.litres);
    out.u64(w.positions.size());
    for (const Vec3& p : w.positions) out.vec3(p);
    out.halves(w.velocities);
    out.bytesOf(w.whiteness);
    // The rain.
    const RainFrame& r = f.rain;
    out.floats(r.drops);
    out.floats(r.droplets);
    out.f32(r.timeStep);
    out.vec3(r.rippleOrigin);
    out.f32(r.rippleCell);
    out.i32(r.rippleCells[0]);
    out.i32(r.rippleCells[1]);
    out.halves(r.ripples);
    // The rigid bodies (version 2): where each piece is, how it is turned,
    // how it moves. Their rest geometry is the network's: the reader puts
    // it back (adoptPieces).
    const RigidFrame& b = f.rigid;
    out.text(b.attribute);
    out.u64(b.joints);
    out.u64(b.broken);
    out.u64(b.poses.size());
    for (const RigidPose& p : b.poses) {
        out.vec3(p.position);
        out.f32(p.rotation.x);
        out.f32(p.rotation.y);
        out.f32(p.rotation.z);
        out.f32(p.rotation.w);
        out.vec3(p.velocity);
        out.vec3(p.spin);
    }
    out.floats(b.debris);  // version 3
    out.u64(b.vanished.size());
    for (const uint32_t k : b.vanished) out.u32(k);
    // Version 4: the numbers of the particles, and the grit's velocity.
    out.words(w.ids);
    out.words(r.dropIds);
    out.words(r.dropletIds);
    out.words(b.debrisIds);
    out.floats(b.debrisVelocity);
    out.halves(w.flow);  // version 5
    out.bytesOf(b.rebarState);  // version 6: what became of the bars
    out.bytesOf(b.debrisGlass);  // version 7: which grit is glass, which bodies came loose
    out.words(b.unglued);
    out.bytesOf(b.jointState);  // version 8: what became of each joint of the glue, and when
    out.floats(b.jointTime);
    // Version 9: how each bit of grit is turned, in half floats.
    std::vector<uint16_t> turned(b.debrisOrient.size());
    for (size_t i = 0; i < turned.size(); ++i) turned[i] = toHalf(b.debrisOrient[i]);
    out.halves(turned);
    // Version 10: a sparse gas's tiles -- the gas above holds theirs alone.
    out.u64(f.gasTiles.size());
    for (const uint32_t t : f.gasTiles) out.u32(t);
    // Version 11: the cloth -- where each point is, and how fast it goes.
    out.u64(f.cloth.positions.size());
    for (const Vec3& p : f.cloth.positions) out.vec3(p);
    out.halves(f.cloth.velocities);
    // Version 12: the cloth torn -- the points split off, the corners on
    // them, the lines parted.
    out.words(f.cloth.copies);
    out.words(f.cloth.corners);
    out.words(f.cloth.cuts);
    // Version 13: a sparse liquid's tiles -- the water's cells and flow
    // above hold theirs alone.
    out.words(w.tiles);
    out.words(w.flowTiles);
    out.words(w.deepTiles);  // version 14: deep in the water, without their cells
    // Version 15: the grains -- where each is, how fast it goes, how big it
    // is, its number and its colour.
    const GrainFrame& g = f.grains;
    out.u64(g.positions.size());
    for (const Vec3& p : g.positions) out.vec3(p);
    out.halves(g.velocities);
    out.halves(g.radii);
    out.words(g.ids);
    out.bytesOf(g.colors);
    // Version 16: the pieces that broke as it ran -- what their fragments
    // are made again from (rigidBroken).
    out.u64(b.shatters.size());
    for (const RigidShatter& s : b.shatters) {
        out.u32(s.body);
        out.vec3(s.at);
        out.u32(s.seed);
        out.u32(s.count);
        out.f32(s.time);
    }
    // Version 17: the gas's steam, a half a cell as the gas lays it out --
    // none, when there is none.
    out.halves(f.steam);
    return std::move(out.bytes);
}

bool parseFrame(std::string_view data, Frame& f, std::string& error) {
    if (data.size() < sizeof kMagic + 4 || std::memcmp(data.data(), kMagic, sizeof kMagic) != 0) {
        error = "not a frame of a simulation";
        return false;
    }
    In in(data.substr(sizeof kMagic));
    const uint32_t version = in.u32();
    if (version > kVersion) {
        error = "a frame of a newer version (" + std::to_string(version) + ")";
        return false;
    }
    f = Frame();
    f.number = in.i32();
    f.time = in.f32();
    f.stepMs = in.f64();
    f.domain = in.domain();
    // Every cell's -- or, sparse, the tiles' (version 10), which come last:
    // at most every tile, whole.
    const size_t tileCount = static_cast<size_t>((f.domain.cells[0] + Tiles::kSide - 1) / Tiles::kSide) *
                             static_cast<size_t>((f.domain.cells[1] + Tiles::kSide - 1) / Tiles::kSide) *
                             static_cast<size_t>((f.domain.cells[2] + Tiles::kSide - 1) / Tiles::kSide);
    in.halvesAtMost(f.fields, 3 * std::max(f.domain.cellCount(), Tiles::kCells * tileCount));
    WaterFrame& w = f.water;
    w.domain = in.domain();
    w.band = in.f32();
    in.bytesOf(w.cells);
    w.particles = static_cast<size_t>(in.u64());
    w.litres = in.f64();
    w.positions.resize(in.count(12));
    for (Vec3& p : w.positions) p = in.vec3();
    in.halves(w.velocities, 3 * w.positions.size());
    in.bytesOf(w.whiteness);
    RainFrame& r = f.rain;
    in.floats(r.drops);
    in.floats(r.droplets);
    r.timeStep = in.f32();
    r.rippleOrigin = in.vec3();
    r.rippleCell = in.f32();
    r.rippleCells[0] = in.i32();
    r.rippleCells[1] = in.i32();
    const bool ripples = r.rippleCells[0] >= 0 && r.rippleCells[0] <= 65536 && r.rippleCells[1] >= 0 && r.rippleCells[1] <= 65536;
    in.halves(r.ripples, ripples ? static_cast<size_t>(r.rippleCells[0]) * static_cast<size_t>(r.rippleCells[1]) : 0);
    if (version >= 2) {
        RigidFrame& b = f.rigid;
        b.attribute = in.text();
        b.joints = static_cast<size_t>(in.u64());
        b.broken = static_cast<size_t>(in.u64());
        b.poses.resize(in.count(4 * 13));
        for (RigidPose& p : b.poses) {
            p.position = in.vec3();
            p.rotation.x = in.f32();
            p.rotation.y = in.f32();
            p.rotation.z = in.f32();
            p.rotation.w = in.f32();
            p.velocity = in.vec3();
            p.spin = in.vec3();
        }
        if (version >= 3) {
            in.floats(b.debris);
            b.vanished.resize(in.count(4));
            for (uint32_t& k : b.vanished) k = in.u32();
        }
    }
    if (version >= 4) {
        in.words(w.ids);
        in.words(r.dropIds);
        in.words(r.dropletIds);
        in.words(f.rigid.debrisIds);
        in.floats(f.rigid.debrisVelocity);
    }
    // Every cell's, or -- sparse, from version 13 -- its tiles', which come
    // last: at most every tile, whole.
    const Domain flowGrid = w.flowDomain();
    const size_t flowTileCount = static_cast<size_t>((flowGrid.cells[0] + Tiles::kSide - 1) / Tiles::kSide) *
                                 static_cast<size_t>((flowGrid.cells[1] + Tiles::kSide - 1) / Tiles::kSide) *
                                 static_cast<size_t>((flowGrid.cells[2] + Tiles::kSide - 1) / Tiles::kSide);
    if (version >= 13) {
        in.halvesAtMost(w.flow, 3 * std::max(flowGrid.cellCount(), Tiles::kCells * flowTileCount));
    } else if (version >= 5) {
        in.halves(w.flow, 3 * flowGrid.cellCount());
    }
    if (version >= 6) in.bytesOf(f.rigid.rebarState);
    if (version >= 7) {
        in.bytesOf(f.rigid.debrisGlass);
        in.words(f.rigid.unglued);
    }
    if (version >= 8) {
        in.bytesOf(f.rigid.jointState);
        in.floats(f.rigid.jointTime);
    }
    if (version >= 9) {
        std::vector<uint16_t> turned;
        in.halves(turned, 4 * (f.rigid.debris.size() / 4));
        f.rigid.debrisOrient.resize(turned.size());
        for (size_t i = 0; i < turned.size(); ++i) f.rigid.debrisOrient[i] = fromHalf(turned[i]);
        // In half floats a turn is a little longer or shorter: of unit length again.
        for (size_t i = 0; i + 3 < f.rigid.debrisOrient.size(); i += 4) {
            float* q = f.rigid.debrisOrient.data() + i;
            const float n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
            if (n > 0.0f) {
                for (int k = 0; k < 4; ++k) q[k] /= n;
            }
        }
    }
    if (version >= 10) {
        f.gasTiles.resize(in.count(4));
        for (uint32_t& t : f.gasTiles) t = in.u32();
    }
    if (version >= 11) {
        f.cloth.positions.resize(in.count(12));
        for (Vec3& p : f.cloth.positions) p = in.vec3();
        in.halvesAtMost(f.cloth.velocities, 3 * f.cloth.positions.size());
        if (!f.cloth.velocities.empty() && f.cloth.velocities.size() != 3 * f.cloth.positions.size()) {
            error = "the frame's cloth does not fit its points";
            return false;
        }
    }
    if (version >= 12) {
        in.words(f.cloth.copies);
        in.words(f.cloth.corners);
        in.words(f.cloth.cuts);
        if (f.cloth.copies.size() > f.cloth.positions.size()) {
            error = "the frame's cloth does not fit its points";
            return false;
        }
    }
    if (version >= 13) {
        in.words(w.tiles);
        in.words(w.flowTiles);
    }
    if (version >= 14) in.words(w.deepTiles);
    if (version >= 15) {
        GrainFrame& g = f.grains;
        g.positions.resize(in.count(12));
        for (Vec3& p : g.positions) p = in.vec3();
        in.halves(g.velocities, 3 * g.positions.size());
        in.halves(g.radii, g.positions.size());
        in.words(g.ids);
        in.bytesOf(g.colors);
        // Halves of zeros come back as none: a grain standing still.
        if (g.velocities.empty()) g.velocities.assign(3 * g.positions.size(), 0);
        if (in.ok() && !g.fits()) {
            error = "the frame's grains do not fit together";
            return false;
        }
    }
    if (version >= 16) {
        f.rigid.shatters.resize(in.count(28));
        for (RigidShatter& s : f.rigid.shatters) {
            s.body = in.u32();
            s.at = in.vec3();
            s.seed = in.u32();
            s.count = in.u32();
            s.time = in.f32();
        }
    }
    if (version >= 17) in.halvesAtMost(f.steam, f.fields.size() / 3);
    if (!in.ok() || !ripples) {
        error = "the frame is cut short, or not what it says it is";
        return false;
    }
    // The gas: every cell of its grid, or 512 for each of its tiles -- tiles
    // of the grid, in order.
    bool gasFits = f.gasTiles.empty() ? f.fields.empty() || f.fields.size() == 3 * f.domain.cellCount()
                                      : f.fields.size() == 3 * Tiles::kCells * f.gasTiles.size();
    for (size_t t = 0; t < f.gasTiles.size() && gasFits; ++t) {
        gasFits = f.gasTiles[t] < tileCount && (t == 0 || f.gasTiles[t - 1] < f.gasTiles[t]);
    }
    if (!gasFits || (!f.steam.empty() && 3 * f.steam.size() != f.fields.size())) {
        error = "the frame's gas does not fit its grid";
        return false;
    }
    // The water: every cell of its grids, or 512 for each of their tiles.
    bool flowFits = w.flow.empty() || w.hasFlow();
    for (size_t t = 0; t < w.flowTiles.size() && flowFits; ++t) {
        flowFits = w.flowTiles[t] < flowTileCount && (t == 0 || w.flowTiles[t - 1] < w.flowTiles[t]);
    }
    if ((w.cells.empty() ? !w.tiles.empty() || !w.deepTiles.empty() : !w.fits()) || !flowFits) {
        error = "the frame's water does not fit its grid";
        return false;
    }
    // What is drawn from it indexes these by the sizes it gives.
    // Numbers and velocities: none, or one for each.
    auto fits = [](size_t have, size_t each) { return have == 0 || have == each; };
    const RigidFrame& b = f.rigid;
    if ((!w.whiteness.empty() && w.whiteness.size() != w.positions.size()) || r.drops.size() % 6 != 0 ||
        r.droplets.size() % 6 != 0 || b.debris.size() % 4 != 0 || !fits(w.ids.size(), w.positions.size()) ||
        !fits(r.dropIds.size(), r.dropCount()) || !fits(r.dropletIds.size(), r.dropletCount()) ||
        !fits(b.debrisIds.size(), b.debris.size() / 4) || !fits(b.debrisVelocity.size(), 3 * (b.debris.size() / 4)) ||
        !fits(b.debrisGlass.size(), b.debris.size() / 4) || b.jointTime.size() != b.jointState.size() ||
        !fits(b.debrisOrient.size(), 4 * (b.debris.size() / 4))) {
        error = "the frame's parts do not fit their grids";
        return false;
    }
    return true;
}

void adoptCloth(Frame& frame, const ClothScene& scene) {
    ClothFrame& c = frame.cloth;
    if (c.positions.empty() || !scene.geometry || !clothFits(c, *scene.geometry)) return;
    c.geometry = scene.geometry;
}

void adoptPieces(Frame& frame, const RigidScene& scene, std::shared_ptr<const RigidLayout>* memo,
                 std::shared_ptr<const RigidRebar>* rebarMemo, std::shared_ptr<const RigidGlue>* glueMemo,
                 std::shared_ptr<const RigidBroken>* brokenMemo) {
    RigidFrame& b = frame.rigid;
    if (b.poses.empty() || !scene.pieces) return;
    const std::string& attribute = b.attribute.empty() ? scene.attribute : b.attribute;
    std::shared_ptr<const RigidLayout> layout = memo ? *memo : nullptr;
    if (!layout || layout->bodyOf.size() != scene.pieces->primitiveCount()) layout = rigidLayout(*scene.pieces, attribute);
    if (memo) *memo = layout;
    // The pieces as they came in -- and, after the breaks the frame keeps,
    // the fragments made again from them, on from those of the frame before.
    std::shared_ptr<const Geometry> pieces = scene.pieces;
    std::shared_ptr<const RigidLayout> laid = layout;
    if (!b.shatters.empty()) {
        std::shared_ptr<const RigidBroken> broken = brokenMemo ? *brokenMemo : nullptr;
        if (!broken || broken->shatters != b.shatters) {
            RigidScene adopted = scene;
            adopted.attribute = attribute;
            broken = rigidBroken(adopted, b.shatters, layout, broken.get());
        }
        if (brokenMemo) *brokenMemo = broken;
        if (!broken) return;  // breaks of other pieces
        pieces = broken->pieces;
        laid = broken->layout;
    }
    if (static_cast<size_t>(laid->bodies) != b.poses.size()) return;  // another geometry: not these pieces
    const size_t restBodies = static_cast<size_t>(layout->bodies);
    b.pieces = pieces;
    b.layout = laid;
    if (b.attribute.empty()) b.attribute = scene.attribute;
    // The joints of the glue, where the frame says what became of as many
    // as the world's pieces -- and its network -- make.
    b.glue = nullptr;
    if (!b.jointState.empty()) {
        std::shared_ptr<const RigidGlue> glue = glueMemo ? *glueMemo : nullptr;
        if (!glue || glue->centres.size() != restBodies) {
            glue = rigidGlue(*scene.pieces, *layout, b.attribute, scene.constraints.get());
        }
        if (glueMemo) *glueMemo = glue;
        if (glue->joints.size() == b.jointState.size()) b.glue = glue;
    }
    // The bars, where the frame says what became of them -- as many
    // stations as the world's bars make in these pieces.
    b.rebar = nullptr;
    if (!scene.rebar || b.rebarState.empty()) return;
    std::shared_ptr<const RigidRebar> bars = rebarMemo ? *rebarMemo : nullptr;
    if (!bars) bars = rigidRebar(*scene.pieces, *layout, *scene.rebar);
    if (rebarMemo) *rebarMemo = bars;
    if (bars->stations.size() == b.rebarState.size()) b.rebar = bars;
}

std::string frameFile(const std::string& folder, int number) {
    char name[32];
    std::snprintf(name, sizeof name, "frame.%04d.pgframe", number);
    return (std::filesystem::path(folder) / name).string();
}

bool writeWhole(const std::string& path, std::string_view bytes, std::string& error) {
    const std::string part = path + ".part";
    {
        std::ofstream file(part, std::ios::binary | std::ios::trunc);
        if (!file || !file.write(bytes.data(), static_cast<std::streamsize>(bytes.size())) || !file.flush()) {
            error = path + ": cannot write it";
            std::error_code ec;
            std::filesystem::remove(part, ec);
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::rename(part, path, ec);
    if (ec) {
        error = path + ": cannot write it (" + ec.message() + ")";
        std::filesystem::remove(part, ec);
        return false;
    }
    return true;
}

bool writeFrame(const Frame& frame, const std::string& folder, std::string& error) {
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    return writeWhole(frameFile(folder, frame.number), formatFrame(frame), error);
}

bool readFrame(const std::string& folder, int number, Frame& frame, std::string& error) {
    const std::string path = frameFile(folder, number);
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = path + ": no such file";
        return false;
    }
    std::ostringstream ss;
    ss << file.rdbuf();
    if (!parseFrame(ss.str(), frame, error)) {
        error = path + ": " + error;
        return false;
    }
    return true;
}

bool writeCacheInfo(const std::string& folder, const CacheInfo& info, std::string& error) {
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    const std::string path = (std::filesystem::path(folder) / "cache.txt").string();
    char hash[32];
    std::snprintf(hash, sizeof hash, "%016llx", static_cast<unsigned long long>(info.network));
    // To the thousandth: 1 / (1 / 30) is 29.999998 in floats, and the note says 30.
    char fps[32];
    const auto written = std::to_chars(fps, fps + sizeof fps, std::round(info.fps * 1000.0f) / 1000.0f);  // a point, whatever the locale
    std::string text = "pgcache 1\nframes " + std::to_string(info.frames) + "\nfps " + std::string(fps, written.ptr) +
                       "\nnetwork " + hash + "\n";
    // What a bake that runs says of itself.
    if (info.of > info.frames) text += "of " + std::to_string(info.of) + "\n";
    if (info.stepMs > 0.0) {
        char ms[32];
        const auto end = std::to_chars(ms, ms + sizeof ms, std::round(info.stepMs * 10.0) / 10.0);
        text += "ms " + std::string(ms, end.ptr) + "\n";
    }
    if (info.checkpoint > 0) text += "checkpoint " + std::to_string(info.checkpoint) + "\n";
    return writeWhole(path, text, error);
}

bool readCacheInfo(const std::string& folder, CacheInfo& info, std::string& error) {
    const std::string path = (std::filesystem::path(folder) / "cache.txt").string();
    std::ifstream file(path);
    if (!file) {
        error = folder + ": no simulation cache here (cache.txt)";
        return false;
    }
    std::string word;
    int version = 0;
    if (!(file >> word >> version) || word != "pgcache" || version != 1) {
        error = path + ": not a simulation cache";
        return false;
    }
    info = CacheInfo();
    while (file >> word) {
        std::string value;
        if (!(file >> value)) break;
        if (word == "frames") info.frames = std::atoi(value.c_str());
        else if (word == "fps") {
            const char* p = value.c_str();
            float fps = 0.0f;
            if (io::readNumber(p, p + value.size(), fps)) info.fps = fps;
        }
        else if (word == "network") info.network = std::strtoull(value.c_str(), nullptr, 16);
        else if (word == "of") info.of = std::atoi(value.c_str());
        else if (word == "ms") {
            const char* p = value.c_str();
            float ms = 0.0f;
            if (io::readNumber(p, p + value.size(), ms)) info.stepMs = std::max(0.0, static_cast<double>(ms));
        }
        else if (word == "checkpoint") info.checkpoint = std::max(0, std::atoi(value.c_str()));
    }
    if (info.frames < 1) {
        error = path + ": no frames";
        return false;
    }
    return true;
}

std::string checkpointFile(const std::string& folder) {
    return (std::filesystem::path(folder) / "checkpoint.pgstate").string();
}

bool writeCheckpoint(const std::string& folder, std::string_view state, std::string& error) {
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    return writeWhole(checkpointFile(folder), state, error);
}

bool readCheckpoint(const std::string& folder, std::string& state, std::string& error) {
    const std::string path = checkpointFile(folder);
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = folder + ": no checkpoint here (checkpoint.pgstate)";
        return false;
    }
    std::ostringstream ss;
    ss << file.rdbuf();
    state = ss.str();
    return true;
}

}  // namespace pg::sim
