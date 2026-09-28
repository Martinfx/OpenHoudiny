// PNG in (ISO/IEC 15948): the chunks, the zlib stream of the image data,
// the filters undone line by line, Adam7's seven passes put back in place.
// And out, as simply as a PNG can be: no filters, stored blocks.
#include "pg/io/Inflate.h"
#include "pg/io/Picture.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>

namespace pg::io {

namespace {

uint32_t be32(const uint8_t* p) { return (uint32_t{p[0]} << 24) | (uint32_t{p[1]} << 16) | (uint32_t{p[2]} << 8) | p[3]; }
unsigned be16(const uint8_t* p) { return (unsigned{p[0]} << 8) | p[1]; }

int paeth(int a, int b, int c) {
    const int p = a + b - c;
    const int pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
    if (pa <= pb && pa <= pc) return a;
    return pb <= pc ? b : c;
}

/// Lines as the filters left them (each led by its filter's byte) made
/// the bytes they were: `rows` lines of `rowBytes`, `bpp` bytes a pixel.
bool unfilter(const uint8_t* in, size_t rows, size_t rowBytes, size_t bpp, uint8_t* out, std::string& error) {
    const uint8_t* above = nullptr;
    for (size_t y = 0; y < rows; ++y, in += rowBytes + 1) {
        const uint8_t* src = in + 1;
        uint8_t* dst = out + y * rowBytes;
        switch (in[0]) {
            case 0: std::memcpy(dst, src, rowBytes); break;
            case 1:
                for (size_t i = 0; i < rowBytes; ++i) dst[i] = static_cast<uint8_t>(src[i] + (i >= bpp ? dst[i - bpp] : 0));
                break;
            case 2:
                for (size_t i = 0; i < rowBytes; ++i) dst[i] = static_cast<uint8_t>(src[i] + (above ? above[i] : 0));
                break;
            case 3:
                for (size_t i = 0; i < rowBytes; ++i) {
                    const int a = i >= bpp ? dst[i - bpp] : 0, b = above ? above[i] : 0;
                    dst[i] = static_cast<uint8_t>(src[i] + ((a + b) >> 1));
                }
                break;
            case 4:
                for (size_t i = 0; i < rowBytes; ++i) {
                    const int a = i >= bpp ? dst[i - bpp] : 0, b = above ? above[i] : 0;
                    const int c = above && i >= bpp ? above[i - bpp] : 0;
                    dst[i] = static_cast<uint8_t>(src[i] + paeth(a, b, c));
                }
                break;
            default: error = "a PNG line with a filter there is not"; return false;
        }
        above = dst;
    }
    return true;
}

}  // namespace

bool decodePng(std::span<const uint8_t> bytes, Picture& out, std::string& error) {
    static constexpr uint8_t kSignature[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (bytes.size() < 8 || std::memcmp(bytes.data(), kSignature, 8) != 0) {
        error = "not a PNG";
        return false;
    }
    uint32_t width = 0, height = 0;
    int depth = 0, color = -1, interlace = 0;
    std::vector<uint8_t> data;
    std::array<uint8_t, 256 * 3> palette{};
    size_t paletteSize = 0;
    std::array<uint8_t, 256> paletteAlpha;
    paletteAlpha.fill(255);
    bool keyed = false;
    std::array<unsigned, 3> key{};
    bool header = false;
    for (size_t at = 8; at + 12 <= bytes.size();) {
        const uint32_t length = be32(&bytes[at]);
        if (length > bytes.size() - at - 12) {
            error = "a PNG chunk that runs past the end of the file";
            return false;
        }
        const uint8_t* type = &bytes[at + 4];
        const uint8_t* body = &bytes[at + 8];
        const std::string name(reinterpret_cast<const char*>(type), 4);
        if (name == "IHDR") {
            if (length < 13) {
                error = "a PNG header that is too short";
                return false;
            }
            width = be32(body);
            height = be32(body + 4);
            depth = body[8];
            color = body[9];
            if (body[10] != 0 || body[11] != 0) {
                error = "a PNG compressed or filtered in a way there is not";
                return false;
            }
            interlace = body[12];
            header = true;
        } else if (name == "PLTE") {
            paletteSize = std::min<size_t>(length / 3, 256);
            std::memcpy(palette.data(), body, paletteSize * 3);
        } else if (name == "tRNS") {
            if (color == 3) {
                std::memcpy(paletteAlpha.data(), body, std::min<size_t>(length, 256));
            } else if (color == 0 && length >= 2) {
                key[0] = be16(body);
                keyed = true;
            } else if (color == 2 && length >= 6) {
                key = {be16(body), be16(body + 2), be16(body + 4)};
                keyed = true;
            }
        } else if (name == "IDAT") {
            data.insert(data.end(), body, body + length);
        } else if (name == "IEND") {
            break;
        } else if (!(type[0] & 0x20)) {
            error = "a PNG with a chunk it cannot be read without: " + name;
            return false;
        }
        at += 12 + length;
    }
    if (!header) {
        error = "a PNG without its header";
        return false;
    }
    const bool depthOk = color == 0   ? (depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16)
                         : color == 3 ? (depth == 1 || depth == 2 || depth == 4 || depth == 8)
                         : (color == 2 || color == 4 || color == 6) && (depth == 8 || depth == 16);
    if (!depthOk || interlace > 1) {
        error = "a PNG of a kind there is not (colour type " + std::to_string(color) + ", " + std::to_string(depth) + " bits)";
        return false;
    }
    if (width == 0 || height == 0 || width > (1u << 24) || height > (1u << 24) ||
        uint64_t{width} * height > (uint64_t{1} << 28)) {
        error = "a PNG of " + std::to_string(width) + " x " + std::to_string(height) + " pixels";
        return false;
    }
    if (color == 3 && paletteSize == 0) {
        error = "a PNG with a palette it does not have";
        return false;
    }
    const int channels = color == 0 ? 1 : color == 2 ? 3 : color == 3 ? 1 : color == 4 ? 2 : 4;
    const size_t bitsPerPixel = static_cast<size_t>(channels) * static_cast<size_t>(depth);
    const size_t bpp = std::max<size_t>(1, bitsPerPixel / 8);
    const auto rowBytes = [&](size_t w) { return (w * bitsPerPixel + 7) / 8; };

    // The passes: one, or Adam7's seven.
    struct Pass {
        size_t x0, y0, dx, dy, w, h;
    };
    std::vector<Pass> passes;
    if (interlace == 0) {
        passes.push_back({0, 0, 1, 1, width, height});
    } else {
        static constexpr int kX[7] = {0, 4, 0, 2, 0, 1, 0}, kY[7] = {0, 0, 4, 0, 2, 0, 1};
        static constexpr int kDx[7] = {8, 8, 4, 4, 2, 2, 1}, kDy[7] = {8, 8, 8, 4, 4, 2, 2};
        for (int p = 0; p < 7; ++p) {
            const size_t w = width > static_cast<uint32_t>(kX[p]) ? (width - kX[p] + kDx[p] - 1) / kDx[p] : 0;
            const size_t h = height > static_cast<uint32_t>(kY[p]) ? (height - kY[p] + kDy[p] - 1) / kDy[p] : 0;
            passes.push_back({static_cast<size_t>(kX[p]), static_cast<size_t>(kY[p]), static_cast<size_t>(kDx[p]),
                              static_cast<size_t>(kDy[p]), w, h});
        }
    }
    size_t expected = 0;
    for (const Pass& p : passes) {
        if (p.w && p.h) expected += p.h * (rowBytes(p.w) + 1);
    }
    std::vector<uint8_t> raw;
    raw.reserve(expected);
    if (!zlibInflate(data, raw, expected, error)) {
        error = "a PNG whose pixels do not decompress: " + error;
        return false;
    }
    if (raw.size() < expected) {
        error = "a PNG with fewer pixels than it says";
        return false;
    }

    out.width = static_cast<int>(width);
    out.height = static_cast<int>(height);
    out.rgba.assign(static_cast<size_t>(width) * height * 4, 1.0f);
    // Each sample as the nearest float to v / max: a table up to 8 bits.
    const float maxValue = static_cast<float>((1u << depth) - 1);
    std::vector<float> table(depth <= 8 ? (size_t{1} << depth) : 0);
    for (size_t i = 0; i < table.size(); ++i) table[i] = static_cast<float>(i) / maxValue;
    const auto norm = [&](unsigned v) { return depth <= 8 ? table[v] : static_cast<float>(v) / maxValue; };
    std::vector<uint8_t> lines;
    size_t offset = 0;
    for (const Pass& p : passes) {
        if (!p.w || !p.h) continue;
        const size_t bytesPerRow = rowBytes(p.w);
        lines.resize(bytesPerRow * p.h);
        if (!unfilter(raw.data() + offset, p.h, bytesPerRow, bpp, lines.data(), error)) return false;
        offset += p.h * (bytesPerRow + 1);
        for (size_t y = 0; y < p.h; ++y) {
            const uint8_t* row = lines.data() + y * bytesPerRow;
            float* dst = &out.rgba[((p.y0 + y * p.dy) * width + p.x0) * 4];
            for (size_t x = 0; x < p.w; ++x, dst += p.dx * 4) {
                unsigned v[4] = {0, 0, 0, 0};
                if (depth < 8) {
                    const size_t bit = x * static_cast<size_t>(depth);
                    v[0] = (row[bit / 8] >> (8 - depth - static_cast<int>(bit % 8))) & ((1u << depth) - 1);
                } else if (depth == 8) {
                    for (int c = 0; c < channels; ++c) v[c] = row[x * static_cast<size_t>(channels) + static_cast<size_t>(c)];
                } else {
                    for (int c = 0; c < channels; ++c) v[c] = be16(row + (x * static_cast<size_t>(channels) + static_cast<size_t>(c)) * 2);
                }
                switch (color) {
                    case 0:
                        dst[0] = dst[1] = dst[2] = norm(v[0]);
                        dst[3] = keyed && v[0] == key[0] ? 0.0f : 1.0f;
                        break;
                    case 2:
                        for (int c = 0; c < 3; ++c) dst[c] = norm(v[c]);
                        dst[3] = keyed && v[0] == key[0] && v[1] == key[1] && v[2] == key[2] ? 0.0f : 1.0f;
                        break;
                    case 3: {
                        const size_t i = v[0] < paletteSize ? v[0] : 0;
                        for (int c = 0; c < 3; ++c) dst[c] = palette[i * 3 + static_cast<size_t>(c)] / 255.0f;
                        dst[3] = paletteAlpha[i] / 255.0f;
                        break;
                    }
                    case 4:
                        dst[0] = dst[1] = dst[2] = norm(v[0]);
                        dst[3] = norm(v[1]);
                        break;
                    default:
                        for (int c = 0; c < 4; ++c) dst[c] = norm(v[c]);
                        break;
                }
            }
        }
    }
    return true;
}

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

void put32(std::string& out, uint32_t v) {
    for (int shift = 24; shift >= 0; shift -= 8) out += static_cast<char>(static_cast<uint8_t>(v >> shift));
}

void chunk(std::string& png, const char* type, const std::string& body) {
    put32(png, static_cast<uint32_t>(body.size()));
    const size_t typed = png.size();
    png.append(type, 4);
    png += body;
    put32(png, crc32(reinterpret_cast<const uint8_t*>(png.data()) + typed, png.size() - typed));
}

}  // namespace

std::string encodePng(const uint8_t* pixels, int width, int height, int channels) {
    if (!pixels || width <= 0 || height <= 0 || (channels != 3 && channels != 4)) return {};
    const size_t rowBytes = static_cast<size_t>(width) * static_cast<size_t>(channels);
    // Lines, each led by filter 0 (none); then the zlib stream of stored
    // blocks, at most 65535 bytes each, and the Adler-32 of the lines.
    std::string z = {'\x78', '\x01'};
    const size_t total = (rowBytes + 1) * static_cast<size_t>(height);
    uint32_t a = 1, b = 0;
    std::string block;
    size_t done = 0;
    for (int y = 0; y < height; ++y) {
        const uint8_t* row = pixels + rowBytes * static_cast<size_t>(y);
        for (size_t i = 0; i <= rowBytes; ++i) {
            const uint8_t v = i == 0 ? 0 : row[i - 1];
            block += static_cast<char>(v);
            a = (a + v) % 65521;
            b = (b + a) % 65521;
            ++done;
            if (block.size() == 65535 || done == total) {
                const size_t len = block.size();
                z += static_cast<char>(done == total ? 1 : 0);
                z += static_cast<char>(len & 0xFF);
                z += static_cast<char>(len >> 8);
                z += static_cast<char>(~len & 0xFF);
                z += static_cast<char>((~len >> 8) & 0xFF);
                z += block;
                block.clear();
            }
        }
    }
    put32(z, (b << 16) | a);

    std::string png = "\x89PNG\r\n\x1A\n";
    std::string ihdr;
    put32(ihdr, static_cast<uint32_t>(width));
    put32(ihdr, static_cast<uint32_t>(height));
    ihdr += '\x08';                                         // bits a channel
    ihdr += static_cast<char>(channels == 4 ? 6 : 2);      // RGBA or RGB
    ihdr.append(3, '\0');                                   // deflate, adaptive filters, no interlace
    chunk(png, "IHDR", ihdr);
    chunk(png, "IDAT", z);
    chunk(png, "IEND", {});
    return png;
}

}  // namespace pg::io
