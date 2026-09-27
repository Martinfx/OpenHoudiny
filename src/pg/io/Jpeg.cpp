#include "pg/io/Jpeg.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <vector>

namespace pg::io {
namespace {

/// The coefficient each place of the zigzag takes, in the block's own order.
constexpr uint8_t kZigzag[64] = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
                                 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
                                 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
                                 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

// The quantisation tables of the standard (Annex K.1), row by row.
constexpr uint8_t kLuma[64] = {16, 11, 10, 16, 24,  40,  51,  61,  12, 12, 14, 19, 26,  58,  60,  55,
                               14, 13, 16, 24, 40,  57,  69,  56,  14, 17, 22, 29, 51,  87,  80,  62,
                               18, 22, 37, 56, 68,  109, 103, 77,  24, 35, 55, 64, 81,  104, 113, 92,
                               49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99};
constexpr uint8_t kChroma[64] = {17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99,
                                 24, 26, 56, 99, 99, 99, 99, 99, 47, 66, 99, 99, 99, 99, 99, 99,
                                 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
                                 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99};

// The Huffman tables of the standard (Annex K.3): how many codes of each
// length, 1 to 16 bits, then the symbols in the order of their codes.
constexpr uint8_t kDcLumaBits[16] = {0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0};
constexpr uint8_t kDcChromaBits[16] = {0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0};
constexpr uint8_t kDcValues[12] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
constexpr uint8_t kAcLumaBits[16] = {0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d};
constexpr uint8_t kAcLumaValues[162] = {
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07, 0x22, 0x71, 0x14,
    0x32, 0x81, 0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0, 0x24, 0x33, 0x62, 0x72, 0x82, 0x09,
    0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a,
    0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65,
    0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88,
    0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9,
    0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca,
    0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea,
    0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa};
constexpr uint8_t kAcChromaBits[16] = {0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77};
constexpr uint8_t kAcChromaValues[162] = {
    0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71, 0x13, 0x22, 0x32,
    0x81, 0x08, 0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0, 0x15, 0x62, 0x72, 0xd1, 0x0a, 0x16,
    0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38, 0x39,
    0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64,
    0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86,
    0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
    0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8,
    0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9,
    0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa};

/// A Huffman table as codes: the code and its length for each symbol.
struct Huffman {
    std::array<uint16_t, 256> code{};
    std::array<uint8_t, 256> length{};

    Huffman(const uint8_t* bits, const uint8_t* values) {
        uint16_t c = 0;
        size_t k = 0;
        for (int len = 1; len <= 16; ++len) {
            for (int i = 0; i < bits[len - 1]; ++i) {
                code[values[k]] = c++;
                length[values[k]] = static_cast<uint8_t>(len);
                ++k;
            }
            c = static_cast<uint16_t>(c << 1);
        }
    }
};

/// The entropy-coded bits, a byte at a time; a 0xFF byte is followed by 0.
class Bits {
public:
    explicit Bits(std::string& out) : out_(out) {}
    void put(uint32_t value, int count) {
        for (int i = count - 1; i >= 0; --i) {
            byte_ = static_cast<uint8_t>((byte_ << 1) | ((value >> i) & 1u));
            if (++filled_ == 8) flush();
        }
    }
    void code(const Huffman& h, int symbol) { put(h.code[static_cast<size_t>(symbol)], h.length[static_cast<size_t>(symbol)]); }
    /// The last byte filled with ones.
    void finish() {
        while (filled_ != 0) put(1, 1);
    }

private:
    void flush() {
        out_.push_back(static_cast<char>(byte_));
        if (byte_ == 0xFF) out_.push_back('\0');
        byte_ = 0;
        filled_ = 0;
    }
    std::string& out_;
    uint8_t byte_ = 0;
    int filled_ = 0;
};

/// The forward DCT of Arai, Agui and Nakajima, in place, rows then columns --
/// its outputs scaled by what the quantisation divides back out (as in
/// libjpeg's jfdctflt.c).
void fdct(float* d) {
    for (int pass = 0; pass < 2; ++pass) {
        const int step = pass == 0 ? 1 : 8, next = pass == 0 ? 8 : 1;
        for (int i = 0; i < 8; ++i) {
            float* p = d + i * next;
            const float t0 = p[0] + p[7 * step], t7 = p[0] - p[7 * step];
            const float t1 = p[step] + p[6 * step], t6 = p[step] - p[6 * step];
            const float t2 = p[2 * step] + p[5 * step], t5 = p[2 * step] - p[5 * step];
            const float t3 = p[3 * step] + p[4 * step], t4 = p[3 * step] - p[4 * step];
            // The even part.
            float t10 = t0 + t3, t13 = t0 - t3, t11 = t1 + t2, t12 = t1 - t2;
            p[0] = t10 + t11;
            p[4 * step] = t10 - t11;
            const float z1 = (t12 + t13) * 0.707106781f;
            p[2 * step] = t13 + z1;
            p[6 * step] = t13 - z1;
            // The odd part.
            t10 = t4 + t5;
            t11 = t5 + t6;
            t12 = t6 + t7;
            const float z5 = (t10 - t12) * 0.382683433f;
            const float z2 = 0.541196100f * t10 + z5;
            const float z4 = 1.306562965f * t12 + z5;
            const float z3 = t11 * 0.707106781f;
            const float z11 = t7 + z3, z13 = t7 - z3;
            p[5 * step] = z13 + z2;
            p[3 * step] = z13 - z2;
            p[step] = z11 + z4;
            p[7 * step] = z11 - z4;
        }
    }
}

/// A quantisation table for `quality`, row by row, and what the DCT's
/// outputs are multiplied by to be quantised with it.
struct Quant {
    std::array<uint8_t, 64> table{};
    std::array<float, 64> factor{};

    Quant(const uint8_t* base, int quality) {
        static const float aan[8] = {1.0f, 1.387039845f, 1.306562965f, 1.175875602f,
                                     1.0f, 0.785694958f, 0.541196100f, 0.275899379f};
        const int q = std::clamp(quality, 1, 100);
        const int scale = q < 50 ? 5000 / q : 200 - 2 * q;
        for (int i = 0; i < 64; ++i) {
            table[static_cast<size_t>(i)] = static_cast<uint8_t>(std::clamp((base[i] * scale + 50) / 100, 1, 255));
            factor[static_cast<size_t>(i)] = 1.0f / (static_cast<float>(table[static_cast<size_t>(i)]) * aan[i / 8] * aan[i % 8] * 8.0f);
        }
    }
};

/// The magnitude category of a value: how many bits it takes.
int category(int v) {
    int n = 0;
    for (unsigned a = static_cast<unsigned>(v < 0 ? -v : v); a; a >>= 1) ++n;
    return n;
}

/// One 8 x 8 block: transformed, quantised, coded. `dc` is the component's
/// last DC value, which the next block's is coded against.
void codeBlock(Bits& bits, float* block, const Quant& q, int& dc, const Huffman& dcTable, const Huffman& acTable) {
    fdct(block);
    int zz[64];
    for (int k = 0; k < 64; ++k) {
        const int i = kZigzag[k];
        zz[k] = static_cast<int>(std::lround(block[i] * q.factor[static_cast<size_t>(i)]));
    }
    const int diff = zz[0] - dc;
    dc = zz[0];
    const int s = category(diff);
    bits.code(dcTable, s);
    if (s) bits.put(static_cast<uint32_t>(diff < 0 ? diff - 1 : diff) & ((1u << s) - 1u), s);
    int run = 0;
    for (int k = 1; k < 64; ++k) {
        const int v = zz[k];
        if (v == 0) {
            ++run;
            continue;
        }
        for (; run > 15; run -= 16) bits.code(acTable, 0xF0);  // sixteen zeros
        const int n = category(v);
        bits.code(acTable, (run << 4) | n);
        bits.put(static_cast<uint32_t>(v < 0 ? v - 1 : v) & ((1u << n) - 1u), n);
        run = 0;
    }
    if (run > 0) bits.code(acTable, 0x00);  // the end of the block
}

void put16(std::string& out, int v) {
    out.push_back(static_cast<char>((v >> 8) & 0xFF));
    out.push_back(static_cast<char>(v & 0xFF));
}

void huffmanTable(std::string& out, int id, const uint8_t* bits, const uint8_t* values) {
    out.push_back(static_cast<char>(id));
    int n = 0;
    for (int i = 0; i < 16; ++i) {
        out.push_back(static_cast<char>(bits[i]));
        n += bits[i];
    }
    out.append(reinterpret_cast<const char*>(values), static_cast<size_t>(n));
}

}  // namespace

std::string encodeJpeg(const uint8_t* rgb, int width, int height, int quality) {
    if (!rgb || width <= 0 || height <= 0 || width > 65535 || height > 65535) return {};
    static const Huffman dcLuma(kDcLumaBits, kDcValues), acLuma(kAcLumaBits, kAcLumaValues);
    static const Huffman dcChroma(kDcChromaBits, kDcValues), acChroma(kAcChromaBits, kAcChromaValues);
    const Quant ql(kLuma, quality), qc(kChroma, quality);

    // The planes, shifted to be centred on 0: Y, Cb and Cr (JFIF's).
    const size_t n = static_cast<size_t>(width) * static_cast<size_t>(height);
    std::vector<float> Y(n), Cb(n), Cr(n);
    for (size_t i = 0; i < n; ++i) {
        const float r = rgb[3 * i], g = rgb[3 * i + 1], b = rgb[3 * i + 2];
        Y[i] = 0.299f * r + 0.587f * g + 0.114f * b - 128.0f;
        Cb[i] = -0.168736f * r - 0.331264f * g + 0.5f * b;
        Cr[i] = 0.5f * r - 0.418688f * g - 0.081312f * b;
    }
    auto at = [&](const std::vector<float>& plane, int x, int y) {
        x = std::min(x, width - 1);
        y = std::min(y, height - 1);
        return plane[static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)];
    };

    std::string out;
    out.reserve(n / 4 + 1024);
    out += "\xFF\xD8";  // SOI
    // JFIF: version 1.1, no density.
    out += "\xFF\xE0";
    put16(out, 16);
    out.append("JFIF\0\x01\x01\x00\x00\x01\x00\x01\x00\x00", 14);
    // The quantisation tables, in zigzag order.
    out += "\xFF\xDB";
    put16(out, 2 + 2 * 65);
    for (int t = 0; t < 2; ++t) {
        out.push_back(static_cast<char>(t));
        const Quant& q = t == 0 ? ql : qc;
        for (int k = 0; k < 64; ++k) out.push_back(static_cast<char>(q.table[kZigzag[k]]));
    }
    // The frame: 8 bits; Y sampled 2 x 2, Cb and Cr 1 x 1.
    out += "\xFF\xC0";
    put16(out, 17);
    out.push_back(8);
    put16(out, height);
    put16(out, width);
    out.push_back(3);
    out.append("\x01\x22\x00\x02\x11\x01\x03\x11\x01", 9);
    // The Huffman tables.
    out += "\xFF\xC4";
    put16(out, 2 + (17 + 12) * 2 + (17 + 162) * 2);
    huffmanTable(out, 0x00, kDcLumaBits, kDcValues);
    huffmanTable(out, 0x10, kAcLumaBits, kAcLumaValues);
    huffmanTable(out, 0x01, kDcChromaBits, kDcValues);
    huffmanTable(out, 0x11, kAcChromaBits, kAcChromaValues);
    // The scan: every component, every coefficient.
    out += "\xFF\xDA";
    put16(out, 12);
    out.append("\x03\x01\x00\x02\x11\x03\x11\x00\x3F\x00", 10);

    // 16 x 16 pixels at a time: four blocks of Y, one of Cb, one of Cr. The
    // last row and column repeat past the picture's edge.
    Bits bits(out);
    int dcY = 0, dcCb = 0, dcCr = 0;
    float block[64];
    for (int my = 0; my < height; my += 16) {
        for (int mx = 0; mx < width; mx += 16) {
            for (int b = 0; b < 4; ++b) {
                const int ox = mx + (b & 1) * 8, oy = my + (b >> 1) * 8;
                for (int y = 0; y < 8; ++y) {
                    for (int x = 0; x < 8; ++x) block[y * 8 + x] = at(Y, ox + x, oy + y);
                }
                codeBlock(bits, block, ql, dcY, dcLuma, acLuma);
            }
            for (int c = 0; c < 2; ++c) {
                const std::vector<float>& plane = c == 0 ? Cb : Cr;
                for (int y = 0; y < 8; ++y) {
                    for (int x = 0; x < 8; ++x) {
                        const int px = mx + 2 * x, py = my + 2 * y;
                        block[y * 8 + x] =
                            0.25f * (at(plane, px, py) + at(plane, px + 1, py) + at(plane, px, py + 1) + at(plane, px + 1, py + 1));
                    }
                }
                codeBlock(bits, block, qc, c == 0 ? dcCb : dcCr, dcChroma, acChroma);
            }
        }
    }
    bits.finish();
    out += "\xFF\xD9";  // EOI
    return out;
}

bool writeJpeg(const std::string& path, const uint8_t* rgb, int width, int height, int quality, std::string& error) {
    const std::string bytes = encodeJpeg(rgb, width, height, quality);
    if (bytes.empty()) {
        error = path + ": a picture of " + std::to_string(width) + " x " + std::to_string(height) + " is not one JPEG holds";
        return false;
    }
    std::ofstream file(path, std::ios::binary);
    if (!file || !file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()))) {
        error = path + ": cannot write it";
        return false;
    }
    return true;
}

}  // namespace pg::io
