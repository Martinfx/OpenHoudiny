// OpenEXR in: one part of lines of pixels, each chunk as its compression
// left it -- none, RLE, ZIPS and ZIP (deflate, then the bytes' differences
// and halves put back), PIZ (Huffman coded, a wavelet, a table of the values
// used), PXR24 (deflate of 24-bit floats' byte planes) and B44 / B44A (4 x 4
// blocks of halves in 14 or 3 bytes). The layouts are the ones the OpenEXR
// file format document and library define.
#include "pg/core/Half.h"
#include "pg/io/Exr.h"
#include "pg/io/Inflate.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <iterator>

namespace pg::io {

namespace {

enum Compression : int { kNone = 0, kRle = 1, kZips = 2, kZip = 3, kPiz = 4, kPxr24 = 5, kB44 = 6, kB44a = 7, kDwaa = 8, kDwab = 9 };

int linesPerChunk(int compression) {
    switch (compression) {
        case kZip: case kPxr24: return 16;
        case kPiz: case kB44: case kB44a: case kDwaa: return 32;
        case kDwab: return 256;
        default: return 1;
    }
}

struct Channel {
    std::string name;
    int type = 1;  ///< 0 uint, 1 half, 2 float
    bool pLinear = false;
    int bytes() const { return type == 1 ? 2 : 4; }
};

uint32_t le32(const uint8_t* p) { return uint32_t{p[0]} | (uint32_t{p[1]} << 8) | (uint32_t{p[2]} << 16) | (uint32_t{p[3]} << 24); }
uint64_t le64(const uint8_t* p) { return uint64_t{le32(p)} | (uint64_t{le32(p + 4)} << 32); }
uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

// --- RLE, ZIP: runs, then the differences and the two halves undone ----------------------------

bool unRle(const uint8_t* in, size_t n, std::vector<uint8_t>& out, size_t expected) {
    out.clear();
    out.reserve(expected);
    size_t i = 0;
    while (i < n) {
        const int count = static_cast<int8_t>(in[i++]);
        if (count < 0) {
            const size_t run = static_cast<size_t>(-count);
            if (i + run > n || out.size() + run > expected) return false;
            out.insert(out.end(), in + i, in + i + run);
            i += run;
        } else {
            if (i >= n || out.size() + static_cast<size_t>(count) + 1 > expected) return false;
            out.insert(out.end(), static_cast<size_t>(count) + 1, in[i++]);
        }
    }
    return out.size() == expected;
}

/// OpenEXR keeps a chunk's bytes as the differences of each from the one
/// before, the even bytes first and the odd after: put back.
void unpredict(std::vector<uint8_t>& t) {
    for (size_t i = 1; i < t.size(); ++i) t[i] = static_cast<uint8_t>(t[i - 1] + t[i] - 128);
    std::vector<uint8_t> out(t.size());
    const size_t half = (t.size() + 1) / 2;
    for (size_t i = 0; i < half; ++i) {
        out[2 * i] = t[i];
        if (half + i < t.size()) out[2 * i + 1] = t[half + i];
    }
    t.swap(out);
}

// --- PIZ -------------------------------------------------------------------------------------

constexpr int kHufEncBits = 16, kHufDecBits = 14;
constexpr int kHufEncSize = (1 << kHufEncBits) + 1, kHufDecSize = 1 << kHufDecBits;

struct HufDec {
    int len = 0, lit = 0;
    std::vector<int> longs;  ///< the symbols of the longer codes that start here
};

class HufBits {
public:
    HufBits(const uint8_t* p, const uint8_t* end) : p_(p), end_(end) {}
    bool get(int n, uint64_t& v) {
        while (lc_ < n) {
            if (p_ >= end_) return false;
            c_ = (c_ << 8) | *p_++;
            lc_ += 8;
        }
        lc_ -= n;
        v = (c_ >> lc_) & ((uint64_t{1} << n) - 1);
        return true;
    }
    const uint8_t* at() const { return p_; }

private:
    const uint8_t* p_;
    const uint8_t* end_;
    uint64_t c_ = 0;
    int lc_ = 0;
};

/// OpenEXR's Huffman decoding (ImfHuf): code lengths packed in six bits with
/// runs of zeros, canonical codes, and a run-length code that repeats the
/// value before.
bool hufUncompress(const uint8_t* in, size_t n, std::vector<uint16_t>& out, size_t count) {
    out.clear();
    if (n == 0) return count == 0;
    if (n < 20) return false;
    const uint32_t im = le32(in), iM = le32(in + 4), nBits = le32(in + 12);
    if (im >= static_cast<uint32_t>(kHufEncSize) || iM >= static_cast<uint32_t>(kHufEncSize) || im > iM) return false;
    const uint8_t* p = in + 20;
    const uint8_t* end = in + n;
    // The code lengths.
    std::vector<uint64_t> hcode(kHufEncSize, 0);
    {
        HufBits bits(p, end);
        for (uint32_t i = im; i <= iM; ++i) {
            uint64_t l = 0;
            if (!bits.get(6, l)) return false;
            hcode[i] = l;
            if (l >= 59) {
                uint64_t run = l - 59 + 2;  // a short run of zeros
                if (l == 63) {
                    uint64_t more = 0;
                    if (!bits.get(8, more)) return false;
                    run = more + 6;
                }
                if (i + run > iM + 1) return false;
                for (uint64_t k = 0; k < run; ++k) hcode[i + k] = 0;
                i += static_cast<uint32_t>(run) - 1;
            }
        }
        p = bits.at();
    }
    // Canonical codes: the longest first.
    {
        std::array<uint64_t, 59> counts{};
        for (int i = 0; i < kHufEncSize; ++i) ++counts[hcode[static_cast<size_t>(i)]];
        uint64_t c = 0;
        for (int i = 58; i > 0; --i) {
            const uint64_t nc = (c + counts[static_cast<size_t>(i)]) >> 1;
            counts[static_cast<size_t>(i)] = c;
            c = nc;
        }
        for (int i = 0; i < kHufEncSize; ++i) {
            const uint64_t l = hcode[static_cast<size_t>(i)];
            if (l > 0) hcode[static_cast<size_t>(i)] = l | (counts[l]++ << 6);
        }
    }
    // The decoding table.
    std::vector<HufDec> table(kHufDecSize);
    for (uint32_t i = im; i <= iM; ++i) {
        const uint64_t c = hcode[i] >> 6;
        const int l = static_cast<int>(hcode[i] & 63);
        if (l == 0) continue;
        if (c >> l) return false;
        if (l > kHufDecBits) {
            HufDec& d = table[static_cast<size_t>(c >> (l - kHufDecBits))];
            if (d.len) return false;
            d.longs.push_back(static_cast<int>(i));
        } else {
            const size_t first = static_cast<size_t>(c << (kHufDecBits - l));
            for (size_t k = 0; k < (size_t{1} << (kHufDecBits - l)); ++k) {
                HufDec& d = table[first + k];
                if (d.len || !d.longs.empty()) return false;
                d.len = l;
                d.lit = static_cast<int>(i);
            }
        }
    }
    // The codes.
    if (nBits > 8 * static_cast<uint64_t>(end - p)) return false;
    out.reserve(count);
    const int rlc = static_cast<int>(iM);
    uint64_t c = 0;
    int lc = 0;
    const uint8_t* ie = p + (nBits + 7) / 8;
    const auto emit = [&](int symbol) -> bool {
        if (symbol == rlc) {
            if (lc < 8) {
                if (p >= ie) return false;
                c = (c << 8) | *p++;
                lc += 8;
            }
            lc -= 8;
            const uint8_t repeat = static_cast<uint8_t>(c >> lc);
            if (out.empty() || out.size() + repeat > count) return false;
            out.insert(out.end(), repeat, out.back());
            return true;
        }
        if (out.size() >= count) return false;
        out.push_back(static_cast<uint16_t>(symbol));
        return true;
    };
    while (p < ie) {
        c = (c << 8) | *p++;
        lc += 8;
        while (lc >= kHufDecBits) {
            const HufDec& d = table[static_cast<size_t>((c >> (lc - kHufDecBits)) & (kHufDecSize - 1))];
            if (d.len) {
                lc -= d.len;
                if (!emit(d.lit)) return false;
                continue;
            }
            if (d.longs.empty()) return false;
            bool found = false;
            for (const int symbol : d.longs) {
                const int l = static_cast<int>(hcode[static_cast<size_t>(symbol)] & 63);
                while (lc < l && p < ie) {
                    c = (c << 8) | *p++;
                    lc += 8;
                }
                if (lc >= l && (hcode[static_cast<size_t>(symbol)] >> 6) == ((c >> (lc - l)) & ((uint64_t{1} << l) - 1))) {
                    lc -= l;
                    if (!emit(symbol)) return false;
                    found = true;
                    break;
                }
            }
            if (!found) return false;
        }
    }
    // The last, short codes.
    const int drop = static_cast<int>((8 - nBits) & 7);
    c >>= drop;
    lc -= drop;
    while (lc > 0) {
        const HufDec& d = table[static_cast<size_t>((c << (kHufDecBits - lc)) & (kHufDecSize - 1))];
        if (!d.len || d.len > lc) return false;
        lc -= d.len;
        if (!emit(d.lit)) return false;
    }
    return out.size() == count;
}

void wdec14(uint16_t l, uint16_t h, uint16_t& a, uint16_t& b) {
    const int16_t ls = static_cast<int16_t>(l), hs = static_cast<int16_t>(h);
    const int hi = hs;
    const int ai = ls + (hi & 1) + (hi >> 1);
    a = static_cast<uint16_t>(static_cast<int16_t>(ai));
    b = static_cast<uint16_t>(static_cast<int16_t>(ai - hi));
}

void wdec16(uint16_t l, uint16_t h, uint16_t& a, uint16_t& b) {
    constexpr int kMask = (1 << 16) - 1, kOffset = 1 << 15;
    const int m = l, d = h;
    const int bb = (m - (d >> 1)) & kMask;
    const int aa = (d + bb - kOffset) & kMask;
    b = static_cast<uint16_t>(bb);
    a = static_cast<uint16_t>(aa);
}

/// OpenEXR's 2D Haar wavelet undone, level by level.
void wav2Decode(uint16_t* in, int nx, int ox, int ny, int oy, uint16_t mx) {
    const bool w14 = mx < (1 << 14);
    const int n = std::min(nx, ny);
    int p = 1;
    while (p <= n) p <<= 1;
    p >>= 1;
    int p2 = p;
    p >>= 1;
    const auto dec = [&](uint16_t l, uint16_t h, uint16_t& a, uint16_t& b) { w14 ? wdec14(l, h, a, b) : wdec16(l, h, a, b); };
    while (p >= 1) {
        uint16_t* py = in;
        uint16_t* ey = in + static_cast<ptrdiff_t>(oy) * (ny - p2);
        const int oy1 = oy * p, oy2 = oy * p2, ox1 = ox * p, ox2 = ox * p2;
        uint16_t i00, i01, i10, i11;
        for (; py <= ey; py += oy2) {
            uint16_t* px = py;
            uint16_t* ex = py + static_cast<ptrdiff_t>(ox) * (nx - p2);
            for (; px <= ex; px += ox2) {
                uint16_t* p01 = px + ox1;
                uint16_t* p10 = px + oy1;
                uint16_t* p11 = p10 + ox1;
                dec(*px, *p10, i00, i10);
                dec(*p01, *p11, i01, i11);
                dec(i00, i01, *px, *p01);
                dec(i10, i11, *p10, *p11);
            }
            if (nx & p) {
                uint16_t* p10 = px + oy1;
                dec(*px, *p10, i00, *p10);
                *px = i00;
            }
        }
        if (ny & p) {
            uint16_t* px = py;
            uint16_t* ex = py + static_cast<ptrdiff_t>(ox) * (nx - p2);
            for (; px <= ex; px += ox2) {
                uint16_t* p01 = px + ox1;
                dec(*px, *p01, i00, *p01);
                *px = i00;
            }
        }
        p2 = p;
        p >>= 1;
    }
}

/// The ushorts of a chunk kept channel by channel (PIZ, B44) put in lines:
/// each line, each channel's samples of it, little-endian.
void toLines(const std::vector<uint16_t>& planar, const std::vector<Channel>& channels, int width, int lines,
             std::vector<uint8_t>& out) {
    out.resize(planar.size() * 2);
    std::vector<size_t> start(channels.size());
    size_t at = 0;
    for (size_t c = 0; c < channels.size(); ++c) {
        start[c] = at;
        at += static_cast<size_t>(width) * static_cast<size_t>(lines) * static_cast<size_t>(channels[c].bytes() / 2);
    }
    uint8_t* o = out.data();
    for (int y = 0; y < lines; ++y) {
        for (size_t c = 0; c < channels.size(); ++c) {
            const size_t n = static_cast<size_t>(width) * static_cast<size_t>(channels[c].bytes() / 2);
            for (size_t i = 0; i < n; ++i) {
                const uint16_t v = planar[start[c] + i];
                *o++ = static_cast<uint8_t>(v & 0xFF);
                *o++ = static_cast<uint8_t>(v >> 8);
            }
            start[c] += n;
        }
    }
}

bool unPiz(const uint8_t* in, size_t n, const std::vector<Channel>& channels, int width, int lines, std::vector<uint8_t>& out) {
    if (n < 4) return false;
    const uint16_t minNonZero = le16(in), maxNonZero = le16(in + 2);
    size_t at = 4;
    constexpr int kBitmapSize = 8192;
    std::vector<uint8_t> bitmap(kBitmapSize, 0);
    if (maxNonZero >= kBitmapSize) return false;
    if (minNonZero <= maxNonZero) {
        const size_t size = static_cast<size_t>(maxNonZero - minNonZero) + 1;
        if (at + size > n) return false;
        std::memcpy(bitmap.data() + minNonZero, in + at, size);
        at += size;
    }
    // The table from the values' numbers back to the values.
    std::vector<uint16_t> lut(65536, 0);
    int k = 0;
    for (int i = 0; i < 65536; ++i) {
        if (i == 0 || (bitmap[static_cast<size_t>(i >> 3)] & (1 << (i & 7)))) lut[static_cast<size_t>(k++)] = static_cast<uint16_t>(i);
    }
    const uint16_t maxValue = static_cast<uint16_t>(k - 1);
    if (at + 4 > n) return false;
    const uint32_t length = le32(in + at);
    at += 4;
    if (length > n - at) return false;
    size_t count = 0;
    for (const Channel& c : channels) count += static_cast<size_t>(width) * static_cast<size_t>(lines) * static_cast<size_t>(c.bytes() / 2);
    std::vector<uint16_t> planar;
    if (!hufUncompress(in + at, length, planar, count)) return false;
    size_t start = 0;
    for (const Channel& c : channels) {
        const int size = c.bytes() / 2;
        for (int j = 0; j < size; ++j) wav2Decode(planar.data() + start + static_cast<size_t>(j), width, size, lines, width * size, maxValue);
        start += static_cast<size_t>(width) * static_cast<size_t>(lines) * static_cast<size_t>(size);
    }
    for (uint16_t& v : planar) v = lut[v];
    toLines(planar, channels, width, lines, out);
    return true;
}

// --- PXR24 -----------------------------------------------------------------------------------

bool unPxr24(const uint8_t* in, size_t n, const std::vector<Channel>& channels, int width, int lines, size_t expected,
             std::vector<uint8_t>& out) {
    std::vector<uint8_t> tmp;
    std::string error;
    if (!zlibInflate({in, n}, tmp, expected, error)) return false;
    out.resize(expected);
    const uint8_t* t = tmp.data();
    const uint8_t* tEnd = tmp.data() + tmp.size();
    uint8_t* o = out.data();
    const size_t w = static_cast<size_t>(width);
    for (int y = 0; y < lines; ++y) {
        for (const Channel& c : channels) {
            const int planes = c.type == 1 ? 2 : c.type == 2 ? 3 : 4;
            if (static_cast<size_t>(tEnd - t) < w * static_cast<size_t>(planes)) return false;
            const uint8_t* ptr[4] = {t, t + w, t + 2 * w, t + 3 * w};
            t += w * static_cast<size_t>(planes);
            uint32_t pixel = 0;
            for (size_t x = 0; x < w; ++x) {
                uint32_t diff = 0;
                if (c.type == 1) diff = (uint32_t{ptr[0][x]} << 8) | ptr[1][x];
                else if (c.type == 2) diff = (uint32_t{ptr[0][x]} << 24) | (uint32_t{ptr[1][x]} << 16) | (uint32_t{ptr[2][x]} << 8);
                else diff = (uint32_t{ptr[0][x]} << 24) | (uint32_t{ptr[1][x]} << 16) | (uint32_t{ptr[2][x]} << 8) | ptr[3][x];
                pixel += diff;
                for (int b = 0; b < c.bytes(); ++b) *o++ = static_cast<uint8_t>(pixel >> (8 * b));
            }
        }
    }
    return true;
}

// --- B44 -------------------------------------------------------------------------------------

void unpack14(const uint8_t* b, uint16_t s[16]) {
    uint32_t v[16];
    v[0] = (uint32_t{b[0]} << 8) | b[1];
    const uint32_t shift = b[2] >> 2;
    const uint32_t bias = 0x20u << shift;
    const auto step = [&](uint32_t from, uint32_t bits) { return (from + (bits << shift) - bias) & 0xFFFFu; };
    v[4] = step(v[0], ((uint32_t{b[2]} << 4) | (b[3] >> 4)) & 0x3Fu);
    v[8] = step(v[4], ((uint32_t{b[3]} << 2) | (b[4] >> 6)) & 0x3Fu);
    v[12] = step(v[8], b[4] & 0x3Fu);
    v[1] = step(v[0], b[5] >> 2);
    v[5] = step(v[4], ((uint32_t{b[5]} << 4) | (b[6] >> 4)) & 0x3Fu);
    v[9] = step(v[8], ((uint32_t{b[6]} << 2) | (b[7] >> 6)) & 0x3Fu);
    v[13] = step(v[12], b[7] & 0x3Fu);
    v[2] = step(v[1], b[8] >> 2);
    v[6] = step(v[5], ((uint32_t{b[8]} << 4) | (b[9] >> 4)) & 0x3Fu);
    v[10] = step(v[9], ((uint32_t{b[9]} << 2) | (b[10] >> 6)) & 0x3Fu);
    v[14] = step(v[13], b[10] & 0x3Fu);
    v[3] = step(v[2], b[11] >> 2);
    v[7] = step(v[6], ((uint32_t{b[11]} << 4) | (b[12] >> 4)) & 0x3Fu);
    v[11] = step(v[10], ((uint32_t{b[12]} << 2) | (b[13] >> 6)) & 0x3Fu);
    v[15] = step(v[14], b[13] & 0x3Fu);
    for (int i = 0; i < 16; ++i) s[i] = static_cast<uint16_t>(v[i] & 0x8000u ? v[i] & 0x7FFFu : ~v[i] & 0xFFFFu);
}

void unpack3(const uint8_t* b, uint16_t s[16]) {
    uint32_t v = (uint32_t{b[0]} << 8) | b[1];
    v = v & 0x8000u ? v & 0x7FFFu : ~v & 0xFFFFu;
    for (int i = 0; i < 16; ++i) s[i] = static_cast<uint16_t>(v);
}

bool unB44(const uint8_t* in, size_t n, const std::vector<Channel>& channels, int width, int lines, std::vector<uint8_t>& out) {
    size_t count = 0;
    for (const Channel& c : channels) count += static_cast<size_t>(width) * static_cast<size_t>(lines) * static_cast<size_t>(c.bytes() / 2);
    std::vector<uint16_t> planar(count, 0);
    size_t start = 0;
    const size_t w = static_cast<size_t>(width);
    for (const Channel& c : channels) {
        if (c.type != 1) {
            // Not halves: as they are.
            const size_t size = w * static_cast<size_t>(lines) * static_cast<size_t>(c.bytes() / 2);
            if (n < size * 2) return false;
            for (size_t i = 0; i < size; ++i) planar[start + i] = le16(in + 2 * i);
            in += size * 2;
            n -= size * 2;
            start += size;
            continue;
        }
        if (c.pLinear) return false;  // kept in a logarithmic form: not read
        for (int y = 0; y < lines; y += 4) {
            for (int x = 0; x < width; x += 4) {
                uint16_t s[16];
                if (n < 3) return false;
                if (in[2] >= (13 << 2)) {
                    unpack3(in, s);
                    in += 3;
                    n -= 3;
                } else {
                    if (n < 14) return false;
                    unpack14(in, s);
                    in += 14;
                    n -= 14;
                }
                for (int dy = 0; dy < 4 && y + dy < lines; ++dy) {
                    for (int dx = 0; dx < 4 && x + dx < width; ++dx) {
                        planar[start + static_cast<size_t>(y + dy) * w + static_cast<size_t>(x + dx)] = s[dy * 4 + dx];
                    }
                }
            }
        }
        start += w * static_cast<size_t>(lines);
    }
    toLines(planar, channels, width, lines, out);
    return true;
}

// --- The file --------------------------------------------------------------------------------

class Reader {
public:
    Reader(std::span<const uint8_t> bytes, ExrImage& out, std::string& error)
        : b_(bytes.data()), n_(bytes.size()), out_(out), error_(error) {}

    bool run() {
        if (n_ < 8 || le32(b_) != 20000630u) return fail("not an OpenEXR file");
        const uint32_t version = le32(b_ + 4);
        if ((version & 0xFF) != 2) return fail("an OpenEXR file of version " + std::to_string(version & 0xFF));
        if (version & 0x200) return fail("a tiled OpenEXR file (only lines of pixels are read)");
        if (version & 0x800) return fail("a deep OpenEXR file");
        if (version & 0x1000) return fail("an OpenEXR file of several parts");
        size_t at = 8;
        if (!header(at)) return false;
        return pixels(at);
    }

private:
    const uint8_t* b_;
    size_t n_;
    ExrImage& out_;
    std::string& error_;
    std::vector<Channel> channels_;
    int compression_ = -1;
    std::array<int32_t, 4> data_{}, display_{};
    bool hasData_ = false, hasDisplay_ = false;

    bool fail(const std::string& why) {
        error_ = why;
        return false;
    }

    bool text(size_t& at, std::string& s) {
        s.clear();
        while (at < n_ && b_[at] != 0) {
            s += static_cast<char>(b_[at++]);
            if (s.size() > 255) return false;
        }
        if (at >= n_) return false;
        ++at;
        return true;
    }

    bool header(size_t& at) {
        for (;;) {
            std::string name, type;
            if (!text(at, name)) return fail("an OpenEXR header that ends early");
            if (name.empty()) break;
            if (!text(at, type) || at + 4 > n_) return fail("an OpenEXR header that ends early");
            const uint32_t size = le32(b_ + at);
            at += 4;
            if (size > n_ - at) return fail("an OpenEXR attribute that runs past the end");
            const uint8_t* v = b_ + at;
            if (type == "chlist") {
                size_t i = 0;
                while (i < size && v[i] != 0) {
                    Channel c;
                    while (i < size && v[i] != 0) c.name += static_cast<char>(v[i++]);
                    ++i;
                    if (i + 16 > size) return fail("an OpenEXR channel list that ends early");
                    c.type = static_cast<int>(le32(v + i));
                    c.pLinear = v[i + 4] != 0;
                    const int32_t xs = static_cast<int32_t>(le32(v + i + 8)), ys = static_cast<int32_t>(le32(v + i + 12));
                    i += 16;
                    if (c.type < 0 || c.type > 2) return fail("an OpenEXR channel of a type there is not: " + c.name);
                    if (xs != 1 || ys != 1) return fail("an OpenEXR file with subsampled channels (" + c.name + ")");
                    channels_.push_back(std::move(c));
                }
            } else if (type == "compression" && size >= 1) {
                compression_ = v[0];
            } else if (type == "box2i" && size >= 16 && (name == "dataWindow" || name == "displayWindow")) {
                std::array<int32_t, 4>& box = name == "dataWindow" ? data_ : display_;
                for (int k = 0; k < 4; ++k) box[static_cast<size_t>(k)] = static_cast<int32_t>(le32(v + 4 * k));
                (name == "dataWindow" ? hasData_ : hasDisplay_) = true;
            } else if (type == "string") {
                out_.strings.emplace_back(name, std::string(reinterpret_cast<const char*>(v), size));
            } else if (type == "m44f" && size >= 64) {
                std::array<float, 16> m{};
                for (int k = 0; k < 16; ++k) {
                    const uint32_t bits = le32(v + 4 * k);
                    std::memcpy(&m[static_cast<size_t>(k)], &bits, 4);
                }
                out_.matrices.emplace_back(name, m);
            }
            at += size;
        }
        if (channels_.empty() || compression_ < 0 || !hasData_) return fail("an OpenEXR header without its channels, compression or data window");
        if (compression_ == kDwaa || compression_ == kDwab) return fail("an OpenEXR file compressed with DWA (not read: save it with ZIP or PIZ)");
        if (compression_ > kB44a) return fail("an OpenEXR compression there is not: " + std::to_string(compression_));
        if (!hasDisplay_) display_ = data_;
        const int64_t dw = int64_t{data_[2]} - data_[0] + 1, dh = int64_t{data_[3]} - data_[1] + 1;
        const int64_t w = int64_t{display_[2]} - display_[0] + 1, h = int64_t{display_[3]} - display_[1] + 1;
        if (dw <= 0 || dh <= 0 || w <= 0 || h <= 0 || w * h > (int64_t{1} << 28) || dw * dh > (int64_t{1} << 28)) {
            return fail("an OpenEXR picture of a size that cannot be");
        }
        return true;
    }

    bool pixels(size_t at) {
        const int x0 = data_[0], y0 = data_[1];
        const int dw = data_[2] - data_[0] + 1, dh = data_[3] - data_[1] + 1;
        out_.width = display_[2] - display_[0] + 1;
        out_.height = display_[3] - display_[1] + 1;
        out_.channels.clear();
        for (const Channel& c : channels_) {
            ExrChannel e;
            e.name = c.name;
            e.half = c.type == 1;
            e.values.assign(static_cast<size_t>(out_.width) * static_cast<size_t>(out_.height), 0.0f);
            out_.channels.push_back(std::move(e));
        }
        const int per = linesPerChunk(compression_);
        const size_t chunks = (static_cast<size_t>(dh) + static_cast<size_t>(per) - 1) / static_cast<size_t>(per);
        if (at + chunks * 8 > n_) return fail("an OpenEXR file that ends in its table of chunks");
        size_t lineBytes = 0;
        for (const Channel& c : channels_) lineBytes += static_cast<size_t>(dw) * static_cast<size_t>(c.bytes());
        std::vector<uint8_t> raw;
        for (size_t k = 0; k < chunks; ++k) {
            const uint64_t offset = le64(b_ + at + k * 8);
            if (offset > n_ || n_ - offset < 8) return fail("an OpenEXR chunk past the end of the file");
            const int32_t y = static_cast<int32_t>(le32(b_ + offset));
            const uint32_t size = le32(b_ + offset + 4);
            if (size > n_ - offset - 8) return fail("an OpenEXR chunk that runs past the end of the file");
            if (y < y0 || y > data_[3] || (y - y0) % per != 0) return fail("an OpenEXR chunk of lines that are not there");
            const int lines = std::min(per, data_[3] - y + 1);
            const size_t expected = lineBytes * static_cast<size_t>(lines);
            const uint8_t* in = b_ + offset + 8;
            if (!decompress(in, size, dw, lines, expected, raw)) {
                return fail("an OpenEXR chunk that does not decompress (lines " + std::to_string(y) + " on)");
            }
            // The lines: each channel's samples in turn.
            const uint8_t* p = raw.data();
            for (int line = 0; line < lines; ++line) {
                const int oy = y + line - display_[1];
                for (size_t c = 0; c < channels_.size(); ++c) {
                    const Channel& ch = channels_[c];
                    std::vector<float>& values = out_.channels[c].values;
                    for (int x = 0; x < dw; ++x, p += ch.bytes()) {
                        const int ox = x0 + x - display_[0];
                        if (oy < 0 || oy >= out_.height || ox < 0 || ox >= out_.width) continue;
                        float v = 0.0f;
                        if (ch.type == 1) {
                            v = floatFromHalf(le16(p));
                        } else if (ch.type == 2) {
                            const uint32_t bits = le32(p);
                            std::memcpy(&v, &bits, 4);
                        } else {
                            v = static_cast<float>(le32(p));
                        }
                        values[static_cast<size_t>(oy) * static_cast<size_t>(out_.width) + static_cast<size_t>(ox)] = v;
                    }
                }
            }
        }
        return true;
    }

    bool decompress(const uint8_t* in, size_t size, int width, int lines, size_t expected, std::vector<uint8_t>& out) {
        if (size == expected || compression_ == kNone) {
            if (size != expected) return false;
            out.assign(in, in + size);
            return true;
        }
        std::string error;
        switch (compression_) {
            case kRle:
                if (!unRle(in, size, out, expected)) return false;
                unpredict(out);
                return true;
            case kZips:
            case kZip:
                out.clear();
                if (!zlibInflate({in, size}, out, expected, error) || out.size() != expected) return false;
                unpredict(out);
                return true;
            case kPiz: return unPiz(in, size, channels_, width, lines, out) && out.size() == expected;
            case kPxr24: return unPxr24(in, size, channels_, width, lines, expected, out);
            case kB44:
            case kB44a: return unB44(in, size, channels_, width, lines, out) && out.size() == expected;
            default: return false;
        }
    }
};

}  // namespace

bool parseExr(std::span<const uint8_t> bytes, ExrImage& out, std::string& error) {
    ExrImage image;
    Reader r(bytes, image, error);
    if (!r.run()) return false;
    out = std::move(image);
    return true;
}

bool readExr(const std::string& path, ExrImage& out, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "cannot open " + path;
        return false;
    }
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (!parseExr(bytes, out, error)) {
        error = path + ": " + error;
        return false;
    }
    return true;
}

}  // namespace pg::io
