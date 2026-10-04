#include "pg/io/Lz4.h"

#include <algorithm>
#include <cstring>

namespace pg::io {

/// Decompresses an LZ4 block of `size` bytes onto the end of `out`, which
/// may grow to `limit`. False for bytes that are not one.
bool lz4Block(const uint8_t* in, size_t size, std::vector<uint8_t>& out, size_t limit) {
    const uint8_t* end = in + size;
    while (in < end) {
        const uint8_t token = *in++;
        size_t literals = token >> 4;
        if (literals == 15) {
            uint8_t more;
            do {
                if (in >= end) return false;
                more = *in++;
                literals += more;
            } while (more == 255);
        }
        if (static_cast<size_t>(end - in) < literals || out.size() + literals > limit) return false;
        out.insert(out.end(), in, in + literals);
        in += literals;
        if (in >= end) break;  // the last sequence has only literals
        if (end - in < 2) return false;
        const size_t offset = static_cast<size_t>(in[0]) | static_cast<size_t>(in[1]) << 8;
        in += 2;
        if (offset == 0 || offset > out.size()) return false;
        size_t match = (token & 15u) + 4u;
        if ((token & 15u) == 15u) {
            uint8_t more;
            do {
                if (in >= end) return false;
                more = *in++;
                match += more;
            } while (more == 255);
        }
        if (out.size() + match > limit) return false;
        const size_t from = out.size() - offset;
        for (size_t k = 0; k < match; ++k) out.push_back(out[from + k]);
    }
    return true;
}

void lz4Compress(const uint8_t* in, size_t size, std::vector<uint8_t>& out) {
    constexpr size_t kMinMatch = 4, kLastLiterals = 5, kMatchLimit = 12;  // LZ4's MINMATCH, LASTLITERALS, MFLIMIT
    constexpr int kHashBits = 14;
    std::vector<int64_t> table(size_t(1) << kHashBits, -1);
    auto read32 = [&](size_t i) {
        uint32_t v = 0;
        std::memcpy(&v, in + i, 4);
        return v;
    };
    auto hash = [&](size_t i) { return (read32(i) * 2654435761u) >> (32 - kHashBits); };
    auto length = [&](size_t n) {  // the rest of a length past its 15, in bytes of 255
        while (n >= 255) {
            out.push_back(255);
            n -= 255;
        }
        out.push_back(static_cast<uint8_t>(n));
    };
    auto sequence = [&](size_t litStart, size_t litEnd, size_t matchLen, size_t offset) {
        const size_t lits = litEnd - litStart;
        const size_t ml = matchLen ? matchLen - kMinMatch : 0;
        out.push_back(static_cast<uint8_t>((std::min<size_t>(lits, 15) << 4) | (matchLen ? std::min<size_t>(ml, 15) : 0)));
        if (lits >= 15) length(lits - 15);
        out.insert(out.end(), in + litStart, in + litEnd);
        if (matchLen) {
            out.push_back(static_cast<uint8_t>(offset));
            out.push_back(static_cast<uint8_t>(offset >> 8));
            if (ml >= 15) length(ml - 15);
        }
    };
    size_t anchor = 0, i = 0;
    if (size > kMatchLimit) {
        const size_t limit = size - kMatchLimit;  // a match starts no later
        while (i < limit) {
            const uint32_t h = hash(i);
            const int64_t cand = table[h];
            table[h] = static_cast<int64_t>(i);
            if (cand >= 0 && i - static_cast<size_t>(cand) <= 65535 && read32(static_cast<size_t>(cand)) == read32(i)) {
                size_t len = kMinMatch;
                const size_t most = size - kLastLiterals - i;
                while (len < most && in[static_cast<size_t>(cand) + len] == in[i + len]) ++len;
                sequence(anchor, i, len, i - static_cast<size_t>(cand));
                i += len;
                anchor = i;
                continue;
            }
            ++i;
        }
    }
    sequence(anchor, size, 0, 0);  // the rest, literal
}

}  // namespace pg::io
