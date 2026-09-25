#include "pg/gl/Png.h"

#include <algorithm>
#include <array>
#include <fstream>

namespace pg::gl {
namespace {

uint32_t crc32(const uint8_t* data, size_t n, uint32_t crc = 0) {
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    crc = ~crc;
    for (size_t i = 0; i < n; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

void put32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v >> 24));
    out.push_back(static_cast<uint8_t>(v >> 16));
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v));
}

void chunk(std::vector<uint8_t>& png, const char* type, const std::vector<uint8_t>& body) {
    put32(png, static_cast<uint32_t>(body.size()));
    std::vector<uint8_t> typed(type, type + 4);
    typed.insert(typed.end(), body.begin(), body.end());
    png.insert(png.end(), typed.begin(), typed.end());
    put32(png, crc32(typed.data(), typed.size()));
}

}  // namespace

bool writePng(const std::string& path, int width, int height, int channels,
              const std::vector<uint8_t>& pixels) {
    if (width <= 0 || height <= 0 || (channels != 3 && channels != 4)) return false;
    const size_t rowBytes = static_cast<size_t>(width) * static_cast<size_t>(channels);
    if (pixels.size() < rowBytes * static_cast<size_t>(height)) return false;

    // Scanlines, each prefixed with filter type 0 (none).
    std::vector<uint8_t> raw;
    raw.reserve((rowBytes + 1) * static_cast<size_t>(height));
    for (int y = 0; y < height; ++y) {
        raw.push_back(0);
        const uint8_t* row = pixels.data() + rowBytes * static_cast<size_t>(y);
        raw.insert(raw.end(), row, row + rowBytes);
    }

    // zlib stream of stored blocks, at most 65535 bytes each, then Adler-32.
    std::vector<uint8_t> z{0x78, 0x01};
    for (size_t pos = 0; pos < raw.size() || raw.empty();) {
        const size_t len = std::min<size_t>(65535, raw.size() - pos);
        const bool last = pos + len >= raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back(static_cast<uint8_t>(len));
        z.push_back(static_cast<uint8_t>(len >> 8));
        z.push_back(static_cast<uint8_t>(~len));
        z.push_back(static_cast<uint8_t>(~len >> 8));
        z.insert(z.end(), raw.begin() + static_cast<std::ptrdiff_t>(pos),
                 raw.begin() + static_cast<std::ptrdiff_t>(pos + len));
        pos += len;
        if (last) break;
    }
    uint32_t a = 1, b = 0;
    for (uint8_t v : raw) {
        a = (a + v) % 65521;
        b = (b + a) % 65521;
    }
    put32(z, (b << 16) | a);

    std::vector<uint8_t> png{0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    std::vector<uint8_t> ihdr;
    put32(ihdr, static_cast<uint32_t>(width));
    put32(ihdr, static_cast<uint32_t>(height));
    ihdr.push_back(8);                                   // bits per channel
    ihdr.push_back(static_cast<uint8_t>(channels == 4 ? 6 : 2));  // RGBA or RGB
    ihdr.push_back(0);                                   // deflate
    ihdr.push_back(0);                                   // adaptive filtering
    ihdr.push_back(0);                                   // no interlace
    chunk(png, "IHDR", ihdr);
    chunk(png, "IDAT", z);
    chunk(png, "IEND", {});

    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
    return static_cast<bool>(out);
}

}  // namespace pg::gl
