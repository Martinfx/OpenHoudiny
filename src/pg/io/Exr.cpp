#include "pg/io/Exr.h"

#include "pg/core/Half.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <set>

namespace pg::io {

namespace {

constexpr uint32_t kMagic = 20000630;
constexpr uint32_t kVersion = 2, kLongNames = 0x400;
constexpr uint8_t kRle = 1;

/// Little-endian bytes, whatever the machine is.
struct Bytes {
    std::string b;
    void u8(uint8_t v) { b.push_back(static_cast<char>(v)); }
    void u16(uint16_t v) {
        u8(static_cast<uint8_t>(v));
        u8(static_cast<uint8_t>(v >> 8));
    }
    void u32(uint32_t v) {
        for (int i = 0; i < 4; ++i) u8(static_cast<uint8_t>(v >> (8 * i)));
    }
    void i32(int32_t v) { u32(static_cast<uint32_t>(v)); }
    void u64(uint64_t v) {
        for (int i = 0; i < 8; ++i) u8(static_cast<uint8_t>(v >> (8 * i)));
    }
    void f32(float v) {
        uint32_t x;
        std::memcpy(&x, &v, sizeof x);
        u32(x);
    }
    /// A name as the header has it: its letters, then a 0.
    void name(const std::string& s) {
        b.append(s);
        b.push_back('\0');
    }
    /// An attribute: its name, its type, its size, then the value `put` writes.
    template <class Put>
    void attribute(const std::string& attr, const std::string& type, Put&& put) {
        name(attr);
        name(type);
        const size_t at = b.size();
        u32(0);  // the size, known once the value is written
        const size_t start = b.size();
        put(*this);
        const uint32_t size = static_cast<uint32_t>(b.size() - start);
        for (int i = 0; i < 4; ++i) b[at + static_cast<size_t>(i)] = static_cast<char>(size >> (8 * i));
    }
    void put64(size_t at, uint64_t v) {
        for (int i = 0; i < 8; ++i) b[at + static_cast<size_t>(i)] = static_cast<char>(v >> (8 * i));
    }
};

/// A line compressed as OpenEXR's RLE does it (ImfRleCompressor): the bytes
/// reordered -- the even ones, then the odd ones --, each then the
/// difference from the one before plus 128, and the result in runs: a count
/// n >= 0 and a byte meaning n + 1 of it, or a count -n and n bytes as they
/// are. The line as it is when that is no smaller.
std::string rle(const std::string& raw) {
    const size_t n = raw.size();
    if (n == 0) return raw;
    std::string t(n, '\0');
    size_t even = 0, odd = (n + 1) / 2;
    for (size_t i = 0; i < n; ++i) t[i % 2 == 0 ? even++ : odd++] = raw[i];
    int before = static_cast<unsigned char>(t[0]);
    for (size_t i = 1; i < n; ++i) {
        const int now = static_cast<unsigned char>(t[i]);
        t[i] = static_cast<char>(now - before + 128 + 256);
        before = now;
    }
    constexpr std::ptrdiff_t kMinRun = 3, kMaxRun = 127;
    std::string out;
    out.reserve(n);
    const char* end = t.data() + n;
    const char* runStart = t.data();
    const char* runEnd = runStart + 1;
    while (runStart < end) {
        while (runEnd < end && *runStart == *runEnd && runEnd - runStart - 1 < kMaxRun) ++runEnd;
        if (runEnd - runStart >= kMinRun) {
            out.push_back(static_cast<char>((runEnd - runStart) - 1));
            out.push_back(*runStart);
            runStart = runEnd;
        } else {
            while (runEnd < end &&
                   ((runEnd + 1 >= end || *runEnd != *(runEnd + 1)) || (runEnd + 2 >= end || *(runEnd + 1) != *(runEnd + 2))) &&
                   runEnd - runStart < kMaxRun) {
                ++runEnd;
            }
            out.push_back(static_cast<char>(runStart - runEnd));
            while (runStart < runEnd) out.push_back(*runStart++);
        }
        ++runEnd;
    }
    return out.size() < n ? out : raw;
}

}  // namespace

std::string formatExr(const ExrImage& image) {
    const int w = std::max(image.width, 0), h = std::max(image.height, 0);
    std::vector<const ExrChannel*> channels;
    for (const ExrChannel& c : image.channels) channels.push_back(&c);
    std::sort(channels.begin(), channels.end(), [](const ExrChannel* a, const ExrChannel* b) { return a->name < b->name; });
    // Names over 31 letters need the file to say so.
    bool longNames = false;
    for (const ExrChannel* c : channels) longNames = longNames || c->name.size() > 31;
    for (const auto& [n, v] : image.strings) longNames = longNames || n.size() > 31;
    for (const auto& [n, m] : image.matrices) longNames = longNames || n.size() > 31;

    Bytes out;
    out.u32(kMagic);
    out.u32(kVersion | (longNames ? kLongNames : 0u));
    out.attribute("channels", "chlist", [&](Bytes& o) {
        for (const ExrChannel* c : channels) {
            o.name(c->name);
            o.i32(c->half ? 1 : 2);  // HALF, FLOAT
            o.u32(0);                // pLinear and three bytes reserved
            o.i32(1);                // x sampling
            o.i32(1);                // y sampling
        }
        o.u8(0);
    });
    out.attribute("compression", "compression", [](Bytes& o) { o.u8(kRle); });
    auto window = [&](Bytes& o) {
        o.i32(0);
        o.i32(0);
        o.i32(w - 1);
        o.i32(h - 1);
    };
    out.attribute("dataWindow", "box2i", window);
    out.attribute("displayWindow", "box2i", window);
    out.attribute("lineOrder", "lineOrder", [](Bytes& o) { o.u8(0); });  // top to bottom
    out.attribute("pixelAspectRatio", "float", [](Bytes& o) { o.f32(1.0f); });
    out.attribute("screenWindowCenter", "v2f", [](Bytes& o) {
        o.f32(0.0f);
        o.f32(0.0f);
    });
    out.attribute("screenWindowWidth", "float", [](Bytes& o) { o.f32(1.0f); });
    for (const auto& [attr, text] : image.strings) {
        out.attribute(attr, "string", [&](Bytes& o) { o.b.append(text); });
    }
    for (const auto& [attr, m] : image.matrices) {
        out.attribute(attr, "m44f", [&](Bytes& o) {
            for (const float x : m) o.f32(x);
        });
    }
    out.u8(0);  // the end of the header

    // Where each line starts, then the lines.
    const size_t table = out.b.size();
    for (int y = 0; y < h; ++y) out.u64(0);
    Bytes line;
    for (int y = 0; y < h; ++y) {
        out.put64(table + 8 * static_cast<size_t>(y), out.b.size());
        line.b.clear();
        const size_t row = static_cast<size_t>(y) * static_cast<size_t>(w);
        for (const ExrChannel* c : channels) {
            for (int x = 0; x < w; ++x) {
                const size_t i = row + static_cast<size_t>(x);
                const float v = i < c->values.size() ? c->values[i] : 0.0f;
                if (c->half) {
                    line.u16(halfFromFloat(v));
                } else {
                    line.f32(v);
                }
            }
        }
        const std::string packed = rle(line.b);
        out.i32(y);
        out.i32(static_cast<int32_t>(packed.size()));
        out.b.append(packed);
    }
    return std::move(out.b);
}

bool writeExr(const ExrImage& image, const std::string& path, std::string& error) {
    const size_t pixels = static_cast<size_t>(std::max(image.width, 0)) * static_cast<size_t>(std::max(image.height, 0));
    if (pixels == 0 || image.channels.empty()) {
        error = path + ": no picture to write";
        return false;
    }
    std::set<std::string> names;
    for (const ExrChannel& c : image.channels) {
        if (c.values.size() != pixels) {
            error = path + ": channel " + c.name + " has " + std::to_string(c.values.size()) + " values, not " +
                    std::to_string(pixels);
            return false;
        }
        if (c.name.empty() || !names.insert(c.name).second) {
            error = path + ": channel names must be there and differ (" + c.name + ")";
            return false;
        }
    }
    std::ofstream file(path, std::ios::binary);
    if (!file) {
        error = "cannot write " + path;
        return false;
    }
    const std::string bytes = formatExr(image);
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    file.flush();
    if (!file) {
        error = "cannot write " + path + " -- is the disk full?";
        return false;
    }
    return true;
}

}  // namespace pg::io
