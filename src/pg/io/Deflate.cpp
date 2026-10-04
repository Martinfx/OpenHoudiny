#include "pg/io/Deflate.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <queue>

namespace pg::io {
namespace {

/// Bits onto the end of bytes, the first bit the lowest, as deflate packs them.
struct Bits {
    std::vector<uint8_t> out;
    uint64_t acc = 0;
    int count = 0;

    void put(uint32_t value, int n) {
        acc |= static_cast<uint64_t>(value) << count;
        count += n;
        while (count >= 8) {
            out.push_back(static_cast<uint8_t>(acc));
            acc >>= 8;
            count -= 8;
        }
    }
    /// A Huffman code: its bits from the top, as deflate sends them.
    void code(uint32_t code, int n) {
        uint32_t r = 0;
        for (int i = 0; i < n; ++i) r |= ((code >> i) & 1u) << (n - 1 - i);
        put(r, n);
    }
    void align() {
        if (count > 0) put(0, 8 - count);
    }
};

// The lengths 3 to 258 and the distances 1 to 32768: the codes they go in
// and the extra bits after them (RFC 1951, 3.2.5).
constexpr uint16_t kLengthBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                      31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr uint8_t kLengthExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr uint16_t kDistBase[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                    193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
// The order code-length codes' lengths are sent in.
constexpr uint8_t kClOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

int lengthCode(int length) {
    int c = 0;
    while (c < 28 && kLengthBase[c + 1] <= length) ++c;
    return c;
}
int distCode(int dist) {
    int c = 0;
    while (c < 29 && kDistBase[c + 1] <= dist) ++c;
    return c;
}

/// Code lengths for `freq`, none longer than `limit`: a Huffman tree's,
/// shortened as miniz does (the counts at each length moved till the codes
/// fit), each symbol in order of how often it comes. Symbols never used get
/// no code -- but always two at least, as some decoders want.
std::vector<uint8_t> codeLengths(const std::vector<uint32_t>& freq, int limit) {
    const size_t n = freq.size();
    std::vector<uint8_t> lengths(n, 0);
    std::vector<size_t> used;
    for (size_t i = 0; i < n; ++i) {
        if (freq[i] > 0) used.push_back(i);
    }
    if (used.empty()) {
        lengths[0] = lengths[1] = 1;
        return lengths;
    }
    if (used.size() == 1) {
        lengths[used[0]] = 1;
        lengths[used[0] == 0 ? 1 : 0] = 1;
        return lengths;
    }
    // A Huffman tree: the two rarest joined, again and again.
    struct Node {
        uint64_t weight;
        int left, right;
    };
    std::vector<Node> nodes;
    using Item = std::pair<uint64_t, int>;
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> heap;
    for (const size_t s : used) {
        nodes.push_back({freq[s], -1, static_cast<int>(s)});
        heap.push({freq[s], static_cast<int>(nodes.size()) - 1});
    }
    while (heap.size() > 1) {
        const Item a = heap.top();
        heap.pop();
        const Item b = heap.top();
        heap.pop();
        nodes.push_back({a.first + b.first, a.second, b.second});
        heap.push({a.first + b.first, static_cast<int>(nodes.size()) - 1});
    }
    // How deep each leaf is.
    std::vector<int> depthOf(nodes.size(), 0);
    std::array<int, 64> count{};
    for (int i = static_cast<int>(nodes.size()) - 1; i >= 0; --i) {
        const Node& node = nodes[static_cast<size_t>(i)];
        if (node.left >= 0) {
            depthOf[static_cast<size_t>(node.left)] = depthOf[static_cast<size_t>(i)] + 1;
            depthOf[static_cast<size_t>(node.right)] = depthOf[static_cast<size_t>(i)] + 1;
        } else {
            ++count[static_cast<size_t>(std::min(depthOf[static_cast<size_t>(i)], 63))];
        }
    }
    // None longer than the limit: the deeper ones to it, then made to fit.
    for (int d = limit + 1; d < 64; ++d) count[static_cast<size_t>(limit)] += count[static_cast<size_t>(d)], count[static_cast<size_t>(d)] = 0;
    uint64_t total = 0;
    for (int d = 1; d <= limit; ++d) total += static_cast<uint64_t>(count[static_cast<size_t>(d)]) << (limit - d);
    while (total > (uint64_t(1) << limit)) {
        --count[static_cast<size_t>(limit)];
        for (int d = limit - 1; d > 0; --d) {
            if (count[static_cast<size_t>(d)] > 0) {
                --count[static_cast<size_t>(d)];
                count[static_cast<size_t>(d + 1)] += 2;
                break;
            }
        }
        --total;
    }
    // The commonest the shortest.
    std::vector<size_t> byFreq = used;
    std::stable_sort(byFreq.begin(), byFreq.end(), [&](size_t a, size_t b) { return freq[a] > freq[b]; });
    size_t at = 0;
    for (int d = 1; d <= limit; ++d) {
        for (int k = 0; k < count[static_cast<size_t>(d)]; ++k) lengths[byFreq[at++]] = static_cast<uint8_t>(d);
    }
    return lengths;
}

/// Canonical codes for the lengths (RFC 1951, 3.2.2).
std::vector<uint32_t> codesOf(const std::vector<uint8_t>& lengths) {
    std::array<uint32_t, 17> count{}, next{};
    for (const uint8_t l : lengths) {
        if (l) ++count[l];
    }
    uint32_t code = 0;
    for (int bits = 1; bits <= 16; ++bits) {
        code = (code + count[static_cast<size_t>(bits - 1)]) << 1;
        next[static_cast<size_t>(bits)] = code;
    }
    std::vector<uint32_t> codes(lengths.size(), 0);
    for (size_t i = 0; i < lengths.size(); ++i) {
        if (lengths[i]) codes[i] = next[lengths[i]]++;
    }
    return codes;
}

/// What the data is made of: literals, and repeats of so many bytes so far back.
struct Symbol {
    uint16_t length;  // 0: a literal
    uint16_t value;   // the literal, or the distance
};

void storedBlock(Bits& bits, std::span<const uint8_t> in, bool last) {
    for (size_t at = 0; at < in.size() || at == 0; at += 65535) {
        const size_t n = std::min<size_t>(65535, in.size() - at);
        const bool final = last && at + n >= in.size();
        bits.put(final ? 1 : 0, 1);
        bits.put(0, 2);
        bits.align();
        bits.put(static_cast<uint32_t>(n), 16);
        bits.put(static_cast<uint32_t>(~n & 0xffff), 16);
        for (size_t i = 0; i < n; ++i) bits.put(in[at + i], 8);
        if (in.empty()) break;
    }
}

/// A block of `symbols` in codes of their own, its header the code lengths
/// of its codes, run-length coded.
void dynamicBlock(Bits& bits, const std::vector<Symbol>& symbols, bool last) {
    std::vector<uint32_t> litFreq(286, 0), distFreq(30, 0);
    for (const Symbol& s : symbols) {
        if (s.length == 0) {
            ++litFreq[s.value];
        } else {
            ++litFreq[257 + static_cast<size_t>(lengthCode(s.length))];
            ++distFreq[static_cast<size_t>(distCode(s.value))];
        }
    }
    litFreq[256] = 1;  // the end of the block
    const std::vector<uint8_t> litLen = codeLengths(litFreq, 15), distLen = codeLengths(distFreq, 15);
    const std::vector<uint32_t> litCode = codesOf(litLen), distCode_ = codesOf(distLen);
    size_t hlit = 286, hdist = 30;
    while (hlit > 257 && litLen[hlit - 1] == 0) --hlit;
    while (hdist > 1 && distLen[hdist - 1] == 0) --hdist;
    // The lengths, run-length coded: 16 repeats the last, 17 and 18 zeros.
    std::vector<uint8_t> all(litLen.begin(), litLen.begin() + static_cast<std::ptrdiff_t>(hlit));
    all.insert(all.end(), distLen.begin(), distLen.begin() + static_cast<std::ptrdiff_t>(hdist));
    std::vector<std::pair<uint8_t, uint8_t>> runs;  // symbol, extra
    for (size_t i = 0; i < all.size();) {
        size_t j = i;
        while (j < all.size() && all[j] == all[i]) ++j;
        size_t run = j - i;
        if (all[i] == 0) {
            while (run >= 11) {
                const size_t k = std::min<size_t>(run, 138);
                runs.push_back({18, static_cast<uint8_t>(k - 11)});
                run -= k;
            }
            if (run >= 3) {
                runs.push_back({17, static_cast<uint8_t>(run - 3)});
                run = 0;
            }
        } else {
            runs.push_back({all[i], 0});
            --run;
            while (run >= 3) {
                const size_t k = std::min<size_t>(run, 6);
                runs.push_back({16, static_cast<uint8_t>(k - 3)});
                run -= k;
            }
        }
        for (size_t k = 0; k < run; ++k) runs.push_back({all[i], 0});
        i = j;
    }
    std::vector<uint32_t> clFreq(19, 0);
    for (const auto& r : runs) ++clFreq[r.first];
    const std::vector<uint8_t> clLen = codeLengths(clFreq, 7);
    const std::vector<uint32_t> clCode = codesOf(clLen);
    size_t hclen = 19;
    while (hclen > 4 && clLen[kClOrder[hclen - 1]] == 0) --hclen;

    bits.put(last ? 1 : 0, 1);
    bits.put(2, 2);  // dynamic Huffman
    bits.put(static_cast<uint32_t>(hlit - 257), 5);
    bits.put(static_cast<uint32_t>(hdist - 1), 5);
    bits.put(static_cast<uint32_t>(hclen - 4), 4);
    for (size_t i = 0; i < hclen; ++i) bits.put(clLen[kClOrder[i]], 3);
    for (const auto& [sym, extra] : runs) {
        bits.code(clCode[sym], clLen[sym]);
        if (sym == 16) bits.put(extra, 2);
        if (sym == 17) bits.put(extra, 3);
        if (sym == 18) bits.put(extra, 7);
    }
    for (const Symbol& s : symbols) {
        if (s.length == 0) {
            bits.code(litCode[s.value], litLen[s.value]);
            continue;
        }
        const int lc = lengthCode(s.length);
        bits.code(litCode[257 + static_cast<size_t>(lc)], litLen[257 + static_cast<size_t>(lc)]);
        if (kLengthExtra[lc]) bits.put(static_cast<uint32_t>(s.length - kLengthBase[lc]), kLengthExtra[lc]);
        const int dc = distCode(s.value);
        bits.code(distCode_[static_cast<size_t>(dc)], distLen[static_cast<size_t>(dc)]);
        if (kDistExtra[dc]) bits.put(static_cast<uint32_t>(s.value - kDistBase[dc]), kDistExtra[dc]);
    }
    bits.code(litCode[256], litLen[256]);
}

}  // namespace

std::vector<uint8_t> deflate(std::span<const uint8_t> in, int effort) {
    Bits bits;
    const size_t n = in.size();
    if (n == 0) {
        storedBlock(bits, in, true);
        bits.align();
        return std::move(bits.out);
    }
    constexpr size_t kWindow = 32768, kHashBits = 15, kBlock = 1 << 16;
    const int chain = std::clamp(effort, 1, 9) * 16;
    std::vector<int32_t> head(size_t(1) << kHashBits, -1), prev(n, -1);
    auto hashAt = [&](size_t i) {
        const uint32_t v = static_cast<uint32_t>(in[i]) | static_cast<uint32_t>(in[i + 1]) << 8 | static_cast<uint32_t>(in[i + 2]) << 16;
        return (v * 2654435761u) >> (32 - kHashBits);
    };
    auto insert = [&](size_t i) {
        if (i + 2 >= n) return;
        const uint32_t h = hashAt(i);
        prev[i] = head[h];
        head[h] = static_cast<int32_t>(i);
    };
    std::vector<Symbol> symbols;
    size_t blockStart = 0;
    size_t i = 0;
    auto flush = [&](bool last) {
        // Coded, unless storing it is no larger.
        Bits trial;
        dynamicBlock(trial, symbols, last);
        const size_t raw = i - blockStart;
        if ((trial.out.size() + 1) > raw + 5 * (raw / 65535 + 1)) {
            storedBlock(bits, in.subspan(blockStart, raw), last);
        } else {
            dynamicBlock(bits, symbols, last);
        }
        symbols.clear();
        blockStart = i;
    };
    while (i < n) {
        size_t bestLen = 0, bestDist = 0;
        if (i + 2 < n) {
            int32_t cand = head[hashAt(i)];
            int steps = chain;
            const size_t most = std::min<size_t>(258, n - i);
            while (cand >= 0 && steps-- > 0 && i - static_cast<size_t>(cand) <= kWindow) {
                const size_t c = static_cast<size_t>(cand);
                if (in[c + bestLen] == in[i + bestLen]) {
                    size_t len = 0;
                    while (len < most && in[c + len] == in[i + len]) ++len;
                    if (len > bestLen) {
                        bestLen = len;
                        bestDist = i - c;
                        if (len == most) break;
                    }
                }
                cand = prev[c];
            }
        }
        if (bestLen >= 3) {
            symbols.push_back({static_cast<uint16_t>(bestLen), static_cast<uint16_t>(bestDist)});
            for (size_t k = 0; k < bestLen; ++k) insert(i + k);
            i += bestLen;
        } else {
            symbols.push_back({0, in[i]});
            insert(i);
            ++i;
        }
        if (symbols.size() >= kBlock && i < n) flush(false);
    }
    flush(true);
    bits.align();
    return std::move(bits.out);
}

std::vector<uint8_t> zlibDeflate(std::span<const uint8_t> in, int effort) {
    std::vector<uint8_t> out = {0x78, 0x9c};  // deflate, a 32 KiB window; the check bits right
    const std::vector<uint8_t> body = deflate(in, effort);
    out.insert(out.end(), body.begin(), body.end());
    uint32_t a = 1, b = 0;
    for (const uint8_t c : in) {
        a = (a + c) % 65521;
        b = (b + a) % 65521;
    }
    const uint32_t adler = (b << 16) | a;
    for (int s = 24; s >= 0; s -= 8) out.push_back(static_cast<uint8_t>(adler >> s));
    return out;
}

}  // namespace pg::io
