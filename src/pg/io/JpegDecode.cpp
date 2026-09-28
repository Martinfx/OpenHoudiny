// JPEG in (ITU T.81): baseline, extended and progressive frames, Huffman
// coded, 8-bit samples, grey or YCbCr (or RGB), any subsampling of whole
// steps. The inverse DCT, the upsampling and the colour conversion are
// libjpeg's defaults -- its "islow" integer transform, "fancy" triangle
// upsampling, its fixed-point YCbCr -- so a picture comes out as the
// programs that use libjpeg show it.
#include "pg/io/Picture.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

namespace pg::io {

namespace {

/// Where the k-th coefficient of the zigzag goes in the block.
constexpr uint8_t kNatural[64] = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
                                  12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
                                  35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
                                  58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

/// The entropy-coded data: bits from the highest of each byte down, a
/// stuffed 0 after each 0xFF dropped. At a marker it stops and gives zeros.
class Entropy {
public:
    Entropy(const uint8_t* p, const uint8_t* end) : p_(p), end_(end) {}

    void fill(int n) {
        while (count_ < n) {
            uint64_t byte = 0;
            if (!marker_ && p_ < end_) {
                if (*p_ == 0xFF) {
                    const uint8_t* q = p_ + 1;
                    while (q < end_ && *q == 0xFF) ++q;
                    if (q < end_ && *q == 0x00) {
                        byte = 0xFF;
                        p_ = q + 1;
                    } else {
                        marker_ = true;
                    }
                } else {
                    byte = *p_++;
                }
            }
            bits_ |= byte << (56 - count_);
            count_ += 8;
        }
    }
    uint32_t peek16() {
        fill(16);
        return static_cast<uint32_t>(bits_ >> 48);
    }
    void consume(int n) {
        bits_ <<= n;
        count_ -= n;
    }
    int bits(int n) {
        if (n == 0) return 0;
        fill(n);
        const int v = static_cast<int>(bits_ >> (64 - n));
        consume(n);
        return v;
    }
    /// A value of `n` bits as JPEG codes it: the lower half negative.
    int extend(int n) {
        const int v = bits(n);
        return n == 0 ? 0 : v < (1 << (n - 1)) ? v - (1 << n) + 1 : v;
    }
    /// Past a restart marker: the bits left in the byte dropped, the marker skipped.
    void restart() {
        bits_ = 0;
        count_ = 0;
        marker_ = false;
        while (p_ + 1 < end_) {
            if (p_[0] == 0xFF && p_[1] >= 0xD0 && p_[1] <= 0xD7) {
                p_ += 2;
                return;
            }
            if (p_[0] == 0xFF && p_[1] != 0x00 && p_[1] != 0xFF) return;  // another marker: leave it
            ++p_;
        }
    }
    /// Where the markers go on: the next marker after the data.
    const uint8_t* next() const {
        const uint8_t* q = p_;
        while (q + 1 < end_ && !(q[0] == 0xFF && q[1] != 0x00 && q[1] != 0xFF)) ++q;
        return q;
    }

private:
    const uint8_t* p_;
    const uint8_t* end_;
    uint64_t bits_ = 0;
    int count_ = 0;
    bool marker_ = false;
};

/// A Huffman table: codes of up to nine bits looked up at once.
struct Table {
    bool defined = false;
    std::array<uint8_t, 512> fastLength{};  ///< 0: a longer code
    std::array<uint8_t, 512> fastValue{};
    std::array<int32_t, 17> maxCode{};  ///< the last code of each length; -1 none
    std::array<int32_t, 17> offset{};   ///< a code of that length plus this: its value's place
    std::array<uint8_t, 256> values{};

    bool build(const uint8_t* counts, const uint8_t* vals, int n) {
        fastLength.fill(0);
        int code = 0, k = 0;
        for (int len = 1; len <= 16; ++len) {
            offset[static_cast<size_t>(len)] = k - code;
            for (int i = 0; i < counts[len - 1]; ++i, ++code, ++k) {
                if (len <= 9) {
                    const int shift = 9 - len;
                    for (int j = code << shift; j < ((code + 1) << shift) && j < 512; ++j) {
                        fastLength[static_cast<size_t>(j)] = static_cast<uint8_t>(len);
                        fastValue[static_cast<size_t>(j)] = vals[k];
                    }
                }
            }
            maxCode[static_cast<size_t>(len)] = counts[len - 1] ? code - 1 : -1;
            if (code > (1 << len)) return false;
            code <<= 1;
        }
        std::copy(vals, vals + n, values.begin());
        defined = true;
        return true;
    }

    /// The next value; -1 for bits that are no code.
    int decode(Entropy& in) const {
        const uint32_t look = in.peek16();
        if (const int len = fastLength[look >> 7]) {
            in.consume(len);
            return fastValue[look >> 7];
        }
        for (int len = 10; len <= 16; ++len) {
            const int32_t code = static_cast<int32_t>(look >> (16 - len));
            if (code <= maxCode[static_cast<size_t>(len)]) {
                in.consume(len);
                const int at = code + offset[static_cast<size_t>(len)];
                return at >= 0 && at < 256 ? values[static_cast<size_t>(at)] : -1;
            }
        }
        return -1;
    }
};

struct Component {
    int id = 0, h = 1, v = 1, quant = 0;
    int dc = 0, ac = 0;           ///< its tables in the scan
    int width = 0, height = 0;    ///< its samples
    int blocksW = 0, blocksH = 0; ///< blocks stored: whole MCUs
    int usedW = 0, usedH = 0;     ///< blocks with samples of the picture
    std::vector<int16_t> coef;    ///< 64 a block, in the block's order
    int pred = 0;
};

class Decoder {
public:
    Decoder(std::span<const uint8_t> bytes, Picture& out, std::string& error)
        : b_(bytes.data()), end_(bytes.data() + bytes.size()), out_(out), error_(error) {}

    bool run() {
        if (end_ - b_ < 4 || b_[0] != 0xFF || b_[1] != 0xD8) return fail("not a JPEG");
        const uint8_t* p = b_ + 2;
        while (p + 4 <= end_) {
            if (p[0] != 0xFF) {
                ++p;  // between segments: what should not be there, stepped over
                continue;
            }
            const uint8_t marker = p[1];
            if (marker == 0xFF) {
                ++p;
                continue;
            }
            p += 2;
            if (marker == 0xD9) break;                        // end of image
            if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) continue;  // no length
            if (end_ - p < 2) return fail("a JPEG that ends early");
            const size_t length = (size_t{p[0]} << 8) | p[1];
            if (length < 2 || length > static_cast<size_t>(end_ - p)) return fail("a JPEG segment that runs past the end");
            const uint8_t* s = p + 2;
            const size_t n = length - 2;
            switch (marker) {
                case 0xC0:
                case 0xC1:
                case 0xC2:
                    if (!frame(s, n, marker == 0xC2)) return false;
                    break;
                case 0xC3: return fail("a lossless JPEG");
                case 0xC5: case 0xC6: case 0xC7: return fail("a hierarchical JPEG");
                case 0xC9: case 0xCA: case 0xCB: case 0xCD: case 0xCE: case 0xCF:
                    return fail("an arithmetic-coded JPEG");
                case 0xC4:
                    if (!huffman(s, n)) return false;
                    break;
                case 0xDB:
                    if (!quantization(s, n)) return false;
                    break;
                case 0xDD:
                    if (n < 2) return fail("a JPEG restart interval that is too short");
                    restartInterval_ = (s[0] << 8) | s[1];
                    break;
                case 0xE0:
                    if (n >= 5 && std::memcmp(s, "JFIF\0", 5) == 0) jfif_ = true;
                    break;
                case 0xEE:
                    if (n >= 12 && std::memcmp(s, "Adobe", 5) == 0) adobeTransform_ = s[11];
                    break;
                case 0xDA: {
                    if (!scanHeader(s, n)) return false;
                    Entropy in(s + n, end_);
                    if (!scan(in)) return false;
                    p = in.next();
                    continue;
                }
                default: break;  // APPn, COM and the rest: not needed
            }
            p += length;
        }
        if (components_.empty()) return fail("a JPEG without a frame");
        return finish();
    }

private:
    const uint8_t* b_;
    const uint8_t* end_;
    Picture& out_;
    std::string& error_;
    std::array<std::array<uint16_t, 64>, 4> quant_{};  ///< in the block's order
    std::array<Table, 4> dcTables_, acTables_;
    std::vector<Component> components_;
    int width_ = 0, height_ = 0, hmax_ = 1, vmax_ = 1, mcusX_ = 0, mcusY_ = 0;
    bool progressive_ = false;
    int restartInterval_ = 0;
    bool jfif_ = false;
    int adobeTransform_ = -1;
    // The scan being read.
    std::vector<Component*> scan_;
    int ss_ = 0, se_ = 63, ah_ = 0, al_ = 0;
    int eobrun_ = 0;

    bool fail(const std::string& why) {
        error_ = why;
        return false;
    }

    bool frame(const uint8_t* s, size_t n, bool progressive) {
        if (!components_.empty()) return fail("a JPEG with two frames");
        if (n < 6) return fail("a JPEG frame header that is too short");
        if (s[0] != 8) return fail("a JPEG of " + std::to_string(s[0]) + "-bit samples");
        height_ = (s[1] << 8) | s[2];
        width_ = (s[3] << 8) | s[4];
        const int count = s[5];
        if (width_ == 0 || height_ == 0) return fail("a JPEG without a size");
        if (count != 1 && count != 3) {
            return fail(count == 4 ? "a CMYK JPEG" : "a JPEG of " + std::to_string(count) + " components");
        }
        if (n < 6 + static_cast<size_t>(count) * 3) return fail("a JPEG frame header that is too short");
        if (static_cast<uint64_t>(width_) * static_cast<uint64_t>(height_) > (uint64_t{1} << 28)) {
            return fail("a JPEG of " + std::to_string(width_) + " x " + std::to_string(height_) + " pixels");
        }
        progressive_ = progressive;
        for (int i = 0; i < count; ++i) {
            Component c;
            c.id = s[6 + i * 3];
            c.h = s[7 + i * 3] >> 4;
            c.v = s[7 + i * 3] & 15;
            c.quant = s[8 + i * 3];
            if (c.h < 1 || c.h > 4 || c.v < 1 || c.v > 4 || c.quant > 3) return fail("a JPEG component that cannot be");
            hmax_ = std::max(hmax_, c.h);
            vmax_ = std::max(vmax_, c.v);
            components_.push_back(c);
        }
        mcusX_ = (width_ + 8 * hmax_ - 1) / (8 * hmax_);
        mcusY_ = (height_ + 8 * vmax_ - 1) / (8 * vmax_);
        for (Component& c : components_) {
            if (hmax_ % c.h != 0 || vmax_ % c.v != 0) return fail("a JPEG subsampled by a fraction");
            c.width = (width_ * c.h + hmax_ - 1) / hmax_;
            c.height = (height_ * c.v + vmax_ - 1) / vmax_;
            c.blocksW = mcusX_ * c.h;
            c.blocksH = mcusY_ * c.v;
            c.usedW = (c.width + 7) / 8;
            c.usedH = (c.height + 7) / 8;
            c.coef.assign(static_cast<size_t>(c.blocksW) * static_cast<size_t>(c.blocksH) * 64, 0);
        }
        return true;
    }

    bool huffman(const uint8_t* s, size_t n) {
        while (n > 0) {
            if (n < 17) return fail("a JPEG Huffman table that is too short");
            const int kind = s[0] >> 4, slot = s[0] & 15;
            if (kind > 1 || slot > 3) return fail("a JPEG Huffman table that cannot be");
            int total = 0;
            for (int i = 0; i < 16; ++i) total += s[1 + i];
            if (total > 256 || n < 17 + static_cast<size_t>(total)) return fail("a JPEG Huffman table that is too short");
            Table& t = (kind == 0 ? dcTables_ : acTables_)[static_cast<size_t>(slot)];
            if (!t.build(s + 1, s + 17, total)) return fail("a JPEG Huffman table that is no code");
            s += 17 + total;
            n -= 17 + static_cast<size_t>(total);
        }
        return true;
    }

    bool quantization(const uint8_t* s, size_t n) {
        while (n > 0) {
            const int precision = s[0] >> 4, slot = s[0] & 15;
            const size_t size = precision ? 128 : 64;
            if (precision > 1 || slot > 3 || n < 1 + size) return fail("a JPEG quantization table that cannot be");
            for (int k = 0; k < 64; ++k) {
                quant_[static_cast<size_t>(slot)][kNatural[k]] =
                    precision ? static_cast<uint16_t>((s[1 + 2 * k] << 8) | s[2 + 2 * k]) : s[1 + k];
            }
            s += 1 + size;
            n -= 1 + size;
        }
        return true;
    }

    bool scanHeader(const uint8_t* s, size_t n) {
        if (components_.empty()) return fail("a JPEG scan before its frame");
        if (n < 1) return fail("a JPEG scan header that is too short");
        const int count = s[0];
        if (count < 1 || count > 4 || n < 4 + static_cast<size_t>(count) * 2) return fail("a JPEG scan header that cannot be");
        scan_.clear();
        for (int i = 0; i < count; ++i) {
            const int id = s[1 + i * 2];
            auto it = std::find_if(components_.begin(), components_.end(), [&](const Component& c) { return c.id == id; });
            if (it == components_.end()) return fail("a JPEG scan of a component there is not");
            it->dc = s[2 + i * 2] >> 4;
            it->ac = s[2 + i * 2] & 15;
            if (it->dc > 3 || it->ac > 3) return fail("a JPEG scan with a table there is not");
            scan_.push_back(&*it);
        }
        ss_ = s[1 + count * 2];
        se_ = s[2 + count * 2];
        ah_ = s[3 + count * 2] >> 4;
        al_ = s[3 + count * 2] & 15;
        if (progressive_) {
            if (ss_ > se_ || se_ > 63 || (ss_ == 0 && se_ != 0) || (ss_ > 0 && count != 1) || al_ > 13) {
                return fail("a progressive JPEG scan that cannot be");
            }
        } else {
            ss_ = 0;
            se_ = 63;
            ah_ = al_ = 0;
        }
        for (const Component* c : scan_) {
            const bool needDc = ss_ == 0 && ah_ == 0;
            const bool needAc = se_ > 0;
            if ((needDc && !dcTables_[static_cast<size_t>(c->dc)].defined) ||
                (needAc && !acTables_[static_cast<size_t>(c->ac)].defined)) {
                return fail("a JPEG scan with a Huffman table it does not have");
            }
        }
        return true;
    }

    bool scan(Entropy& in) {
        for (Component& c : components_) c.pred = 0;
        eobrun_ = 0;
        int done = 0;  // MCUs, for the restart markers
        const auto restartAt = [&]() {
            if (restartInterval_ && done > 0 && done % restartInterval_ == 0) {
                in.restart();
                for (Component& c : components_) c.pred = 0;
                eobrun_ = 0;
            }
        };
        if (scan_.size() == 1) {
            Component& c = *scan_[0];
            for (int by = 0; by < c.usedH; ++by) {
                for (int bx = 0; bx < c.usedW; ++bx, ++done) {
                    restartAt();
                    if (!block(in, c, &c.coef[(static_cast<size_t>(by) * c.blocksW + bx) * 64])) return false;
                }
            }
            return true;
        }
        for (int my = 0; my < mcusY_; ++my) {
            for (int mx = 0; mx < mcusX_; ++mx, ++done) {
                restartAt();
                for (Component* c : scan_) {
                    for (int v = 0; v < c->v; ++v) {
                        for (int h = 0; h < c->h; ++h) {
                            const size_t bx = static_cast<size_t>(mx * c->h + h), by = static_cast<size_t>(my * c->v + v);
                            if (!block(in, *c, &c->coef[(by * static_cast<size_t>(c->blocksW) + bx) * 64])) return false;
                        }
                    }
                }
            }
        }
        return true;
    }

    bool block(Entropy& in, Component& c, int16_t* coef) {
        const Table& dc = dcTables_[static_cast<size_t>(c.dc)];
        const Table& ac = acTables_[static_cast<size_t>(c.ac)];
        if (!progressive_) {
            const int t = dc.decode(in);
            if (t < 0 || t > 11) return fail("JPEG data that is no code");
            c.pred += in.extend(t);
            coef[0] = static_cast<int16_t>(c.pred);
            for (int k = 1; k < 64;) {
                const int rs = ac.decode(in);
                if (rs < 0) return fail("JPEG data that is no code");
                const int r = rs >> 4, s = rs & 15;
                if (s == 0) {
                    if (r != 15) break;
                    k += 16;
                    continue;
                }
                k += r;
                if (k > 63) return fail("JPEG data past the end of a block");
                coef[kNatural[k++]] = static_cast<int16_t>(in.extend(s));
            }
            return true;
        }
        if (ss_ == 0) {
            // DC: its first bits, or one bit more.
            if (ah_ == 0) {
                const int t = dc.decode(in);
                if (t < 0 || t > 11) return fail("JPEG data that is no code");
                c.pred += in.extend(t);
                coef[0] = static_cast<int16_t>(c.pred * (1 << al_));
            } else if (in.bits(1)) {
                coef[0] = static_cast<int16_t>(coef[0] | (1 << al_));
            }
            return true;
        }
        if (ah_ == 0) {
            // AC, first bits: runs of zeros, values, runs of blocks with nothing more.
            if (eobrun_ > 0) {
                --eobrun_;
                return true;
            }
            for (int k = ss_; k <= se_; ++k) {
                const int rs = ac.decode(in);
                if (rs < 0) return fail("JPEG data that is no code");
                const int r = rs >> 4, s = rs & 15;
                if (s == 0) {
                    if (r < 15) {
                        eobrun_ = (1 << r) - 1;
                        if (r) eobrun_ += in.bits(r);
                        break;
                    }
                    k += 15;
                    continue;
                }
                k += r;
                if (k > 63) return fail("JPEG data past the end of a block");
                coef[kNatural[k]] = static_cast<int16_t>(in.extend(s) * (1 << al_));
            }
            return true;
        }
        // AC, one bit more: for what is there already, a bit that makes it
        // larger; new values of one, placed after runs of zeros.
        const int p1 = 1 << al_, m1 = -1 * (1 << al_);
        int k = ss_;
        const auto refine = [&](int16_t& value) {
            if (in.bits(1) && (value & p1) == 0) value = static_cast<int16_t>(value + (value >= 0 ? p1 : m1));
        };
        if (eobrun_ == 0) {
            for (; k <= se_; ++k) {
                const int rs = ac.decode(in);
                if (rs < 0) return fail("JPEG data that is no code");
                int r = rs >> 4;
                int s = rs & 15;
                if (s) {
                    s = in.bits(1) ? p1 : m1;
                } else if (r != 15) {
                    eobrun_ = 1 << r;
                    if (r) eobrun_ += in.bits(r);
                    break;
                }
                for (; k <= se_; ++k) {
                    int16_t& value = coef[kNatural[k]];
                    if (value != 0) {
                        refine(value);
                    } else if (--r < 0) {
                        break;
                    }
                }
                if (s && k <= se_) coef[kNatural[k]] = static_cast<int16_t>(s);
            }
        }
        if (eobrun_ > 0) {
            for (; k <= se_; ++k) {
                int16_t& value = coef[kNatural[k]];
                if (value != 0) refine(value);
            }
            --eobrun_;
        }
        return true;
    }

    // --- Samples --------------------------------------------------------------------------

    /// libjpeg's jpeg_idct_islow: 8 x 8 coefficients to samples.
    static void idct(const int16_t* in, const uint16_t* q, uint8_t* out, size_t stride) {
        constexpr int kConst = 13, kPass1 = 2;
        constexpr int64_t k0298 = 2446, k0390 = 3196, k0541 = 4433, k0765 = 6270, k0899 = 7373, k1175 = 9633,
                          k1501 = 12299, k1847 = 15137, k1961 = 16069, k2053 = 16819, k2562 = 20995, k3072 = 25172;
        const auto descale = [](int64_t x, int n) { return (x + (int64_t{1} << (n - 1))) >> n; };
        const auto limit = [](int64_t x) -> uint8_t {
            // libjpeg's range limit: -128..127 to 0..255, beyond it clamped (and wrapped as libjpeg does)
            const int m = static_cast<int>(x & 1023);
            if (m < 128) return static_cast<uint8_t>(m + 128);
            if (m < 512) return 255;
            if (m < 896) return 0;
            return static_cast<uint8_t>(m - 896);
        };
        int ws[64];
        for (int col = 0; col < 8; ++col) {
            const auto dq = [&](int row) { return int64_t{in[row * 8 + col]} * q[row * 8 + col]; };
            if (in[8 + col] == 0 && in[16 + col] == 0 && in[24 + col] == 0 && in[32 + col] == 0 && in[40 + col] == 0 &&
                in[48 + col] == 0 && in[56 + col] == 0) {
                const int dcval = static_cast<int>(dq(0) * (1 << kPass1));
                for (int row = 0; row < 8; ++row) ws[row * 8 + col] = dcval;
                continue;
            }
            int64_t z2 = dq(2), z3 = dq(6);
            int64_t z1 = (z2 + z3) * k0541;
            const int64_t tmp2e = z1 + z3 * -k1847, tmp3e = z1 + z2 * k0765;
            z2 = dq(0);
            z3 = dq(4);
            const int64_t tmp0e = (z2 + z3) * (int64_t{1} << kConst), tmp1e = (z2 - z3) * (int64_t{1} << kConst);
            const int64_t tmp10 = tmp0e + tmp3e, tmp13 = tmp0e - tmp3e, tmp11 = tmp1e + tmp2e, tmp12 = tmp1e - tmp2e;
            int64_t tmp0 = dq(7), tmp1 = dq(5), tmp2 = dq(3), tmp3 = dq(1);
            z1 = tmp0 + tmp3;
            z2 = tmp1 + tmp2;
            z3 = tmp0 + tmp2;
            int64_t z4 = tmp1 + tmp3;
            const int64_t z5 = (z3 + z4) * k1175;
            tmp0 *= k0298;
            tmp1 *= k2053;
            tmp2 *= k3072;
            tmp3 *= k1501;
            z1 *= -k0899;
            z2 *= -k2562;
            z3 *= -k1961;
            z4 *= -k0390;
            z3 += z5;
            z4 += z5;
            tmp0 += z1 + z3;
            tmp1 += z2 + z4;
            tmp2 += z2 + z3;
            tmp3 += z1 + z4;
            constexpr int shift = kConst - kPass1;
            ws[0 * 8 + col] = static_cast<int>(descale(tmp10 + tmp3, shift));
            ws[7 * 8 + col] = static_cast<int>(descale(tmp10 - tmp3, shift));
            ws[1 * 8 + col] = static_cast<int>(descale(tmp11 + tmp2, shift));
            ws[6 * 8 + col] = static_cast<int>(descale(tmp11 - tmp2, shift));
            ws[2 * 8 + col] = static_cast<int>(descale(tmp12 + tmp1, shift));
            ws[5 * 8 + col] = static_cast<int>(descale(tmp12 - tmp1, shift));
            ws[3 * 8 + col] = static_cast<int>(descale(tmp13 + tmp0, shift));
            ws[4 * 8 + col] = static_cast<int>(descale(tmp13 - tmp0, shift));
        }
        for (int row = 0; row < 8; ++row) {
            const int* w = ws + row * 8;
            uint8_t* o = out + static_cast<size_t>(row) * stride;
            constexpr int shift = kConst + kPass1 + 3;
            if (w[1] == 0 && w[2] == 0 && w[3] == 0 && w[4] == 0 && w[5] == 0 && w[6] == 0 && w[7] == 0) {
                const uint8_t dc = limit(descale(w[0], kPass1 + 3));
                for (int i = 0; i < 8; ++i) o[i] = dc;
                continue;
            }
            int64_t z2 = w[2], z3 = w[6];
            int64_t z1 = (z2 + z3) * k0541;
            const int64_t tmp2e = z1 + z3 * -k1847, tmp3e = z1 + z2 * k0765;
            const int64_t tmp0e = (int64_t{w[0]} + w[4]) * (int64_t{1} << kConst);
            const int64_t tmp1e = (int64_t{w[0]} - w[4]) * (int64_t{1} << kConst);
            const int64_t tmp10 = tmp0e + tmp3e, tmp13 = tmp0e - tmp3e, tmp11 = tmp1e + tmp2e, tmp12 = tmp1e - tmp2e;
            int64_t tmp0 = w[7], tmp1 = w[5], tmp2 = w[3], tmp3 = w[1];
            z1 = tmp0 + tmp3;
            z2 = tmp1 + tmp2;
            z3 = tmp0 + tmp2;
            int64_t z4 = tmp1 + tmp3;
            const int64_t z5 = (z3 + z4) * k1175;
            tmp0 *= k0298;
            tmp1 *= k2053;
            tmp2 *= k3072;
            tmp3 *= k1501;
            z1 *= -k0899;
            z2 *= -k2562;
            z3 *= -k1961;
            z4 *= -k0390;
            z3 += z5;
            z4 += z5;
            tmp0 += z1 + z3;
            tmp1 += z2 + z4;
            tmp2 += z2 + z3;
            tmp3 += z1 + z4;
            o[0] = limit(descale(tmp10 + tmp3, shift));
            o[7] = limit(descale(tmp10 - tmp3, shift));
            o[1] = limit(descale(tmp11 + tmp2, shift));
            o[6] = limit(descale(tmp11 - tmp2, shift));
            o[2] = limit(descale(tmp12 + tmp1, shift));
            o[5] = limit(descale(tmp12 - tmp1, shift));
            o[3] = limit(descale(tmp13 + tmp0, shift));
            o[4] = limit(descale(tmp13 - tmp0, shift));
        }
    }

    /// A component's samples at the picture's size: as they are, or
    /// upsampled as libjpeg does -- fancy (triangle) for halves, repeated
    /// for the rest.
    std::vector<uint8_t> upsample(const Component& c, const std::vector<uint8_t>& plane) const {
        const size_t stride = static_cast<size_t>(c.blocksW) * 8;
        const int rh = hmax_ / c.h, rv = vmax_ / c.v;
        const int W = width_, H = height_, w = c.width, h = c.height;
        std::vector<uint8_t> full(static_cast<size_t>(W) * static_cast<size_t>(H));
        const auto row = [&](int y) { return plane.data() + static_cast<size_t>(std::clamp(y, 0, h - 1)) * stride; };
        std::vector<uint8_t> line(static_cast<size_t>(w) * 2 + 2);
        for (int y = 0; y < H; ++y) {
            uint8_t* dst = full.data() + static_cast<size_t>(y) * static_cast<size_t>(W);
            if (rh == 1 && rv == 1) {
                std::memcpy(dst, row(y), static_cast<size_t>(W));
            } else if (rh == 2 && rv == 1 && w > 2) {
                const uint8_t* in = row(y);
                uint8_t* o = line.data();
                o[0] = in[0];
                o[1] = static_cast<uint8_t>((in[0] * 3 + in[1] + 2) >> 2);
                for (int i = 1; i < w - 1; ++i) {
                    const int v = in[i] * 3;
                    o[2 * i] = static_cast<uint8_t>((v + in[i - 1] + 1) >> 2);
                    o[2 * i + 1] = static_cast<uint8_t>((v + in[i + 1] + 2) >> 2);
                }
                o[2 * w - 2] = static_cast<uint8_t>((in[w - 1] * 3 + in[w - 2] + 1) >> 2);
                o[2 * w - 1] = in[w - 1];
                std::memcpy(dst, o, static_cast<size_t>(W));
            } else if (rh == 1 && rv == 2) {
                const int inRow = y / 2;
                const bool below = y % 2 == 1;
                const uint8_t* near = row(inRow);
                const uint8_t* far = row(below ? inRow + 1 : inRow - 1);
                const int bias = below ? 2 : 1;
                for (int x = 0; x < W; ++x) dst[x] = static_cast<uint8_t>((near[x] * 3 + far[x] + bias) >> 2);
            } else if (rh == 2 && rv == 2 && w > 2) {
                const int inRow = y / 2;
                const bool below = y % 2 == 1;
                const uint8_t* near = row(inRow);
                const uint8_t* far = row(below ? inRow + 1 : inRow - 1);
                uint8_t* o = line.data();
                int thisSum = near[0] * 3 + far[0];
                int nextSum = near[1] * 3 + far[1];
                o[0] = static_cast<uint8_t>((thisSum * 4 + 8) >> 4);
                o[1] = static_cast<uint8_t>((thisSum * 3 + nextSum + 7) >> 4);
                int lastSum = thisSum;
                thisSum = nextSum;
                for (int i = 1; i < w - 1; ++i) {
                    nextSum = near[i + 1] * 3 + far[i + 1];
                    o[2 * i] = static_cast<uint8_t>((thisSum * 3 + lastSum + 8) >> 4);
                    o[2 * i + 1] = static_cast<uint8_t>((thisSum * 3 + nextSum + 7) >> 4);
                    lastSum = thisSum;
                    thisSum = nextSum;
                }
                o[2 * w - 2] = static_cast<uint8_t>((thisSum * 3 + lastSum + 8) >> 4);
                o[2 * w - 1] = static_cast<uint8_t>((thisSum * 4 + 7) >> 4);
                std::memcpy(dst, o, static_cast<size_t>(W));
            } else {
                const uint8_t* in = row(y / rv);
                for (int x = 0; x < W; ++x) dst[x] = in[std::min(x / rh, w - 1)];
            }
        }
        return full;
    }

    bool finish() {
        // Samples of each component from its coefficients.
        std::vector<std::vector<uint8_t>> planes;
        for (const Component& c : components_) {
            const size_t stride = static_cast<size_t>(c.blocksW) * 8;
            std::vector<uint8_t> plane(stride * static_cast<size_t>(c.blocksH) * 8);
            const uint16_t* q = quant_[static_cast<size_t>(c.quant)].data();
            for (int by = 0; by < c.usedH; ++by) {
                for (int bx = 0; bx < c.usedW; ++bx) {
                    idct(&c.coef[(static_cast<size_t>(by) * c.blocksW + bx) * 64], q,
                         plane.data() + static_cast<size_t>(by) * 8 * stride + static_cast<size_t>(bx) * 8, stride);
                }
            }
            planes.push_back(upsample(c, plane));
        }
        out_.width = width_;
        out_.height = height_;
        const size_t n = static_cast<size_t>(width_) * static_cast<size_t>(height_);
        out_.rgba.assign(n * 4, 1.0f);
        std::array<float, 256> level{};
        for (int i = 0; i < 256; ++i) level[static_cast<size_t>(i)] = static_cast<float>(i) / 255.0f;
        if (components_.size() == 1) {
            for (size_t i = 0; i < n; ++i) out_.rgba[i * 4] = out_.rgba[i * 4 + 1] = out_.rgba[i * 4 + 2] = level[planes[0][i]];
            return true;
        }
        // RGB as it is: Adobe's transform 0, or components named R, G and B.
        const bool rgb = !jfif_ && (adobeTransform_ == 0 || (adobeTransform_ < 0 && components_[0].id == 'R' &&
                                                               components_[1].id == 'G' && components_[2].id == 'B'));
        if (rgb) {
            for (size_t i = 0; i < n; ++i) {
                for (int c = 0; c < 3; ++c) out_.rgba[i * 4 + static_cast<size_t>(c)] = level[planes[static_cast<size_t>(c)][i]];
            }
            return true;
        }
        // YCbCr as libjpeg converts it: 16-bit fixed point, rounded.
        std::array<int, 256> crR{}, cbB{};
        std::array<int64_t, 256> crG{}, cbG{};
        constexpr int kBits = 16;
        const auto fix = [](double x) { return static_cast<int64_t>(x * (1 << kBits) + 0.5); };
        for (int i = 0; i < 256; ++i) {
            const int64_t x = i - 128;
            crR[static_cast<size_t>(i)] = static_cast<int>((fix(1.40200) * x + (int64_t{1} << (kBits - 1))) >> kBits);
            cbB[static_cast<size_t>(i)] = static_cast<int>((fix(1.77200) * x + (int64_t{1} << (kBits - 1))) >> kBits);
            crG[static_cast<size_t>(i)] = -fix(0.71414) * x;
            cbG[static_cast<size_t>(i)] = -fix(0.34414) * x + (int64_t{1} << (kBits - 1));
        }
        const auto clamp255 = [](int v) { return v < 0 ? 0 : v > 255 ? 255 : v; };
        for (size_t i = 0; i < n; ++i) {
            const int y = planes[0][i], cb = planes[1][i], cr = planes[2][i];
            out_.rgba[i * 4 + 0] = level[static_cast<size_t>(clamp255(y + crR[static_cast<size_t>(cr)]))];
            out_.rgba[i * 4 + 1] =
                level[static_cast<size_t>(clamp255(y + static_cast<int>((cbG[static_cast<size_t>(cb)] + crG[static_cast<size_t>(cr)]) >> kBits)))];
            out_.rgba[i * 4 + 2] = level[static_cast<size_t>(clamp255(y + cbB[static_cast<size_t>(cb)]))];
        }
        return true;
    }
};

}  // namespace

bool decodeJpeg(std::span<const uint8_t> bytes, Picture& out, std::string& error) {
    Picture picture;
    Decoder d(bytes, picture, error);
    if (!d.run()) return false;
    out = std::move(picture);
    return true;
}

}  // namespace pg::io
