#include "pg/sim/Cache.h"

#include "pg/io/Obj.h"

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
constexpr uint32_t kVersion = 1;

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
    /// A grid of a sensible size -- up to 1024 cells a side, 2^27 in all:
    /// the solvers' grids go to 256, the water's drawn one to twice that --
    /// with a voxel of a size.
    Domain domain() {
        Domain d;
        for (int& c : d.cells) c = i32();
        d.voxel = f32();
        for (const int c : d.cells) {
            if (c < 0 || c > 1024) ok_ = false;
        }
        if (!ok_ || d.cellCount() > (size_t(1) << 27) || !(d.voxel > 0.0f && d.voxel < 1e6f)) {
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
    void bytesOf(std::vector<uint8_t>& v) {
        const size_t n = count(1);
        v.assign(data_.begin() + static_cast<std::ptrdiff_t>(pos_), data_.begin() + static_cast<std::ptrdiff_t>(pos_ + n));
        pos_ += n;
    }
    /// Half floats, `expected` of them -- or none: runs of zeros take
    /// little room, so the data cannot say how many are too many.
    void halves(std::vector<uint16_t>& v, size_t expected) {
        const uint64_t n = u64();
        v.clear();
        if (!ok_ || n == 0) return;
        if (n != expected) {
            ok_ = false;
            return;
        }
        v.assign(static_cast<size_t>(n), 0);
        size_t i = 0;
        while (ok_ && i < v.size()) {
            const size_t zeros = u32(), literals = u32();
            if (!ok_ || zeros + literals > v.size() - i || (zeros == 0 && literals == 0)) {
                ok_ = false;
                return;
            }
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
    in.halves(f.fields, 3 * f.domain.cellCount());
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
    if (!in.ok() || !ripples) {
        error = "the frame is cut short, or not what it says it is";
        return false;
    }
    // What is drawn from it indexes these by the sizes it gives.
    if ((!w.cells.empty() && w.cells.size() != 2 * w.domain.cellCount()) ||
        (!w.whiteness.empty() && w.whiteness.size() != w.positions.size()) || r.drops.size() % 6 != 0 ||
        r.droplets.size() % 6 != 0) {
        error = "the frame's parts do not fit their grids";
        return false;
    }
    return true;
}

std::string frameFile(const std::string& folder, int number) {
    char name[32];
    std::snprintf(name, sizeof name, "frame.%04d.pgframe", number);
    return (std::filesystem::path(folder) / name).string();
}

bool writeFrame(const Frame& frame, const std::string& folder, std::string& error) {
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    const std::string path = frameFile(folder, frame.number);
    const std::string bytes = formatFrame(frame);
    std::ofstream file(path, std::ios::binary);
    if (!file || !file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()))) {
        error = path + ": cannot write it";
        return false;
    }
    return true;
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
    std::ofstream file(path, std::ios::binary);
    char hash[32];
    std::snprintf(hash, sizeof hash, "%016llx", static_cast<unsigned long long>(info.network));
    // To the thousandth: 1 / (1 / 30) is 29.999998 in floats, and the note says 30.
    char fps[32];
    const auto written = std::to_chars(fps, fps + sizeof fps, std::round(info.fps * 1000.0f) / 1000.0f);  // a point, whatever the locale
    if (!file || !(file << "pgcache 1\nframes " << info.frames << "\nfps " << std::string(fps, written.ptr) << "\nnetwork "
                        << hash << "\n")) {
        error = path + ": cannot write it";
        return false;
    }
    return true;
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
    }
    if (info.frames < 1) {
        error = path + ": no frames";
        return false;
    }
    return true;
}

}  // namespace pg::sim
