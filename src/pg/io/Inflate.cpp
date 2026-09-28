#include "pg/io/Inflate.h"

#include <array>
#include <cstring>

namespace pg::io {

namespace {

/// The input as deflate reads it: bits from the lowest of each byte up.
/// Past the end it gives zeros, and counts them.
class BitReader {
public:
    BitReader(const uint8_t* data, size_t size) : p_(data), end_(data + size) {}

    /// At least `n` bits (57 at most) in the buffer.
    void fill(int n) {
        while (count_ < n) {
            uint64_t byte = 0;
            if (p_ < end_) byte = *p_++;
            else ++past_;
            bits_ |= byte << count_;
            count_ += 8;
        }
    }
    uint32_t peek(int n) {
        fill(n);
        return static_cast<uint32_t>(bits_ & ((uint64_t{1} << n) - 1));
    }
    void drop(int n) {
        bits_ >>= n;
        count_ -= n;
    }
    uint32_t take(int n) {
        if (n == 0) return 0;
        const uint32_t v = peek(n);
        drop(n);
        return v;
    }
    /// To the next whole byte, and the whole bytes in the buffer given back:
    /// what follows is read as bytes.
    void toByte() {
        drop(count_ & 7);
        const int buffered = count_ / 8;
        p_ -= buffered > past_ ? buffered - past_ : 0;
        past_ = buffered > past_ ? 0 : past_ - buffered;
        bits_ = 0;
        count_ = 0;
    }
    /// Whether more bits were used than there are.
    bool overrun() const { return past_ * 8 > count_; }
    const uint8_t* at() const { return p_; }
    size_t left() const { return static_cast<size_t>(end_ - p_); }
    void skip(size_t n) { p_ += n; }

private:
    const uint8_t* p_;
    const uint8_t* end_;
    uint64_t bits_ = 0;
    int count_ = 0;
    int past_ = 0;
};

uint32_t reversed(uint32_t v, int n) {
    uint32_t r = 0;
    for (int i = 0; i < n; ++i) {
        r = (r << 1) | (v & 1);
        v >>= 1;
    }
    return r;
}

constexpr int kFastBits = 10;

/// A canonical Huffman code: codes of up to ten bits looked up at once, the
/// longer ones counted out by length.
struct Huffman {
    std::array<uint16_t, 1 << kFastBits> fast{};  ///< length << 9 | symbol; 0: a longer code
    std::array<uint16_t, 16> firstCode{}, firstSymbol{};
    std::array<int32_t, 17> maxCode{};  ///< the first code past each length, as 16 bits
    std::array<uint8_t, 288> size{};
    std::array<uint16_t, 288> value{};

    /// From the code lengths of symbols 0 .. n - 1 (0: not used). False for
    /// lengths no code can have.
    bool build(const uint8_t* lengths, int n) {
        std::array<int, 16> count{};
        for (int i = 0; i < n; ++i) ++count[lengths[i]];
        count[0] = 0;
        std::array<int, 16> next{};
        int code = 0, k = 0;
        for (int len = 1; len < 16; ++len) {
            if (count[len] > (1 << len)) return false;
            next[len] = code;
            firstCode[len] = static_cast<uint16_t>(code);
            firstSymbol[len] = static_cast<uint16_t>(k);
            code += count[len];
            if (count[len] && code - 1 >= (1 << len)) return false;
            maxCode[len] = code << (16 - len);
            code <<= 1;
            k += count[len];
        }
        maxCode[16] = 0x10000;
        fast.fill(0);
        for (int i = 0; i < n; ++i) {
            const int len = lengths[i];
            if (!len) continue;
            const int c = next[len] - firstCode[len] + firstSymbol[len];
            size[static_cast<size_t>(c)] = static_cast<uint8_t>(len);
            value[static_cast<size_t>(c)] = static_cast<uint16_t>(i);
            if (len <= kFastBits) {
                for (uint32_t j = reversed(static_cast<uint32_t>(next[len]), len); j < (1u << kFastBits); j += 1u << len) {
                    fast[j] = static_cast<uint16_t>((len << 9) | i);
                }
            }
            ++next[len];
        }
        return true;
    }

    /// The next symbol; -1 for bits that are no code.
    int decode(BitReader& in) const {
        const uint32_t bits = in.peek(16);
        if (const uint16_t f = fast[bits & ((1u << kFastBits) - 1)]) {
            in.drop(f >> 9);
            return f & 511;
        }
        const int32_t k = static_cast<int32_t>(reversed(bits, 16));
        int len = kFastBits + 1;
        while (len < 16 && k >= maxCode[static_cast<size_t>(len)]) ++len;
        if (len >= 16) return -1;
        const int c = (k >> (16 - len)) - firstCode[static_cast<size_t>(len)] + firstSymbol[static_cast<size_t>(len)];
        if (c < 0 || c >= 288 || size[static_cast<size_t>(c)] != len) return -1;
        in.drop(len);
        return value[static_cast<size_t>(c)];
    }
};

constexpr std::array<uint16_t, 29> kLengthBase = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                                  31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr std::array<uint8_t, 29> kLengthExtra = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                                  2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr std::array<uint16_t, 30> kDistanceBase = {1,    2,    3,    4,    5,    7,     9,     13,    17,  25,
                                                    33,   49,   65,   97,   129,  193,   257,   385,   513, 769,
                                                    1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr std::array<uint8_t, 30> kDistanceExtra = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                                    6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
constexpr std::array<uint8_t, 19> kLengthOrder = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

bool inflateFrom(BitReader& in, std::vector<uint8_t>& out, size_t limit, std::string& error) {
    const size_t start = out.size();
    const auto fail = [&](const char* why) {
        error = why;
        return false;
    };
    Huffman literals, distances;
    for (bool last = false; !last;) {
        last = in.take(1) != 0;
        const uint32_t type = in.take(2);
        if (type == 0) {
            // Stored: its length, the length's complement, the bytes.
            in.toByte();
            if (in.left() < 4) return fail("deflate data that ends early");
            const uint8_t* p = in.at();
            const unsigned length = p[0] | (p[1] << 8), check = p[2] | (p[3] << 8);
            if (length != (~check & 0xFFFFu)) return fail("a stored deflate block whose length does not check");
            in.skip(4);
            if (in.left() < length) return fail("deflate data that ends early");
            if (out.size() - start + length > limit) return fail("deflate data larger than it should be");
            out.insert(out.end(), in.at(), in.at() + length);
            in.skip(length);
            continue;
        }
        if (type == 3) return fail("a deflate block of a kind there is not");
        if (type == 1) {
            // The fixed codes.
            std::array<uint8_t, 288> lengths{};
            for (int i = 0; i < 288; ++i) lengths[static_cast<size_t>(i)] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
            std::array<uint8_t, 32> distance{};
            distance.fill(5);
            literals.build(lengths.data(), 288);
            distances.build(distance.data(), 32);
        } else {
            // Codes of its own: their lengths, coded in turn.
            const int nLiterals = static_cast<int>(in.take(5)) + 257;
            const int nDistances = static_cast<int>(in.take(5)) + 1;
            const int nLengths = static_cast<int>(in.take(4)) + 4;
            if (nLiterals > 286 || nDistances > 30) return fail("deflate code lengths out of range");
            std::array<uint8_t, 19> codeLengths{};
            for (int i = 0; i < nLengths; ++i) codeLengths[kLengthOrder[static_cast<size_t>(i)]] = static_cast<uint8_t>(in.take(3));
            Huffman lengthCode;
            if (!lengthCode.build(codeLengths.data(), 19)) return fail("deflate code lengths that are no code");
            std::array<uint8_t, 286 + 30> lengths{};
            const int total = nLiterals + nDistances;
            for (int n = 0; n < total;) {
                const int symbol = lengthCode.decode(in);
                if (symbol < 0) return fail("a deflate code length that is no code");
                if (symbol < 16) {
                    lengths[static_cast<size_t>(n++)] = static_cast<uint8_t>(symbol);
                    continue;
                }
                int repeat = 0;
                uint8_t what = 0;
                if (symbol == 16) {
                    if (n == 0) return fail("a deflate code length repeated before there is one");
                    repeat = 3 + static_cast<int>(in.take(2));
                    what = lengths[static_cast<size_t>(n - 1)];
                } else if (symbol == 17) {
                    repeat = 3 + static_cast<int>(in.take(3));
                } else {
                    repeat = 11 + static_cast<int>(in.take(7));
                }
                if (n + repeat > total) return fail("deflate code lengths past their end");
                while (repeat-- > 0) lengths[static_cast<size_t>(n++)] = what;
            }
            if (lengths[256] == 0) return fail("a deflate block without an end");
            if (!literals.build(lengths.data(), nLiterals) || !distances.build(lengths.data() + nLiterals, nDistances)) {
                return fail("deflate code lengths that are no code");
            }
        }
        // The block: literals, and lengths back at a distance, to its end.
        for (;;) {
            int symbol = literals.decode(in);
            if (symbol < 0) return fail("deflate data that is no code");
            if (symbol < 256) {
                if (out.size() - start >= limit) return fail("deflate data larger than it should be");
                out.push_back(static_cast<uint8_t>(symbol));
                continue;
            }
            if (symbol == 256) break;
            symbol -= 257;
            if (symbol >= 29) return fail("a deflate length that there is not");
            const size_t length = kLengthBase[static_cast<size_t>(symbol)] + in.take(kLengthExtra[static_cast<size_t>(symbol)]);
            const int d = distances.decode(in);
            if (d < 0 || d >= 30) return fail("a deflate distance that there is not");
            const size_t distance = kDistanceBase[static_cast<size_t>(d)] + in.take(kDistanceExtra[static_cast<size_t>(d)]);
            if (distance > out.size() - start) return fail("a deflate distance back past the start");
            if (out.size() - start + length > limit) return fail("deflate data larger than it should be");
            const size_t at = out.size();
            out.resize(at + length);
            uint8_t* o = out.data();
            if (distance >= length) {
                std::memcpy(o + at, o + at - distance, length);
            } else {
                for (size_t i = 0; i < length; ++i) o[at + i] = o[at - distance + i];
            }
            if (in.overrun()) return fail("deflate data that ends early");
        }
        if (in.overrun()) return fail("deflate data that ends early");
    }
    return true;
}

uint32_t adler32(const uint8_t* data, size_t n) {
    uint32_t a = 1, b = 0;
    while (n > 0) {
        const size_t run = n < 5552 ? n : 5552;
        for (size_t i = 0; i < run; ++i) {
            a += data[i];
            b += a;
        }
        a %= 65521;
        b %= 65521;
        data += run;
        n -= run;
    }
    return (b << 16) | a;
}

}  // namespace

bool inflate(std::span<const uint8_t> in, std::vector<uint8_t>& out, size_t limit, std::string& error) {
    BitReader bits(in.data(), in.size());
    return inflateFrom(bits, out, limit, error);
}

bool zlibInflate(std::span<const uint8_t> in, std::vector<uint8_t>& out, size_t limit, std::string& error) {
    if (in.size() < 2) {
        error = "a zlib stream that ends early";
        return false;
    }
    const unsigned cmf = in[0], flg = in[1];
    if ((cmf & 15) != 8 || (cmf >> 4) > 7 || ((cmf << 8) | flg) % 31 != 0) {
        error = "not a zlib stream";
        return false;
    }
    if (flg & 0x20) {
        error = "a zlib stream with a preset dictionary";
        return false;
    }
    const size_t start = out.size();
    BitReader bits(in.data() + 2, in.size() - 2);
    if (!inflateFrom(bits, out, limit, error)) return false;
    // Its Adler-32, where it is there.
    bits.toByte();
    if (bits.left() >= 4) {
        const uint8_t* p = bits.at();
        const uint32_t sum = (uint32_t{p[0]} << 24) | (uint32_t{p[1]} << 16) | (uint32_t{p[2]} << 8) | p[3];
        if (sum != adler32(out.data() + start, out.size() - start)) {
            error = "a zlib stream whose checksum does not match";
            return false;
        }
    }
    return true;
}

}  // namespace pg::io
