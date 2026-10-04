#include "pg/io/Blosc.h"

#include "pg/io/Inflate.h"
#include "pg/io/Lz4.h"

#include <cstring>

namespace pg::io {
namespace {

constexpr size_t kHeader = 16;  // BLOSC_MAX_OVERHEAD
constexpr uint8_t kShuffle = 0x01, kMemcpyed = 0x02, kBitShuffle = 0x04, kDontSplit = 0x10;
constexpr size_t kMaxSplits = 16, kMinBufferSize = 128;  // c-blosc 1's MAX_SPLITS and MIN_BUFFERSIZE
constexpr size_t kMaxDistance = 8191;                    // BloscLZ's, as FastLZ's
enum Codec { kBloscLz = 0, kLz4 = 1, kSnappy = 2, kZlib = 3, kZstd = 4 };

uint32_t u32At(std::span<const uint8_t> in, size_t at) {
    return static_cast<uint32_t>(in[at]) | static_cast<uint32_t>(in[at + 1]) << 8 | static_cast<uint32_t>(in[at + 2]) << 16 |
           static_cast<uint32_t>(in[at + 3]) << 24;
}

/// A BloscLZ stream -- FastLZ's level 2 -- decompressed into all of `out`.
/// Runs of literals (a control byte under 32: how many, less one) and
/// matches (the top three bits how long, more bytes when they are all set;
/// the rest and the next byte how far back, or two more bytes when that is
/// farther than 8191). False for a stream that is not one, or does not
/// fill `out` exactly.
bool bloscLz(std::span<const uint8_t> in, std::span<uint8_t> out) {
    const size_t n = in.size(), m = out.size();
    if (n == 0) return false;
    size_t ip = 0, op = 0;
    uint32_t ctrl = in[ip++] & 31u;  // the first byte's top bits are FastLZ's level
    for (;;) {
        if (ctrl >= 32) {
            size_t length = (ctrl >> 5) - 1;
            const size_t high = static_cast<size_t>(ctrl & 31u) << 8;
            if (length == 6) {
                uint8_t more = 0;
                do {
                    if (ip >= n) return false;
                    more = in[ip++];
                    length += more;
                } while (more == 255);
            }
            if (ip >= n) return false;
            const uint8_t low = in[ip++];
            length += 3;
            size_t distance = high + low + 1;
            if (low == 255 && high == (31u << 8)) {
                if (n - ip < 2) return false;
                distance = ((static_cast<size_t>(in[ip]) << 8) | in[ip + 1]) + kMaxDistance + 1;
                ip += 2;
            }
            if (distance > op || length > m - op) return false;
            // A byte at a time: a match may run on into what it makes.
            for (size_t i = 0; i < length; ++i, ++op) out[op] = out[op - distance];
        } else {
            const size_t run = ctrl + 1;
            if (run > m - op || run > n - ip) return false;
            std::memcpy(out.data() + op, in.data() + ip, run);
            op += run;
            ip += run;
        }
        if (ip >= n) break;
        ctrl = in[ip++];
    }
    return op == m;
}

/// The bytes of `size` / `typesize` numbers put back in order: shuffled,
/// the first bytes of them all came first, then the second... What does not
/// make a whole number at the end was not shuffled.
void unshuffle(size_t typesize, size_t size, const uint8_t* in, uint8_t* out) {
    const size_t n = size / typesize;
    for (size_t i = 0; i < n; ++i) {
        for (size_t j = 0; j < typesize; ++j) out[i * typesize + j] = in[j * n + i];
    }
    std::memcpy(out + n * typesize, in + n * typesize, size - n * typesize);
}

bool fail(std::string& error, std::string why) {
    error = "Blosc: " + std::move(why);
    return false;
}

}  // namespace

bool bloscDecompress(std::span<const uint8_t> in, std::vector<uint8_t>& out, size_t limit, std::string& error) {
    if (in.size() < kHeader) return fail(error, "a frame shorter than its header");
    const uint8_t version = in[0], flags = in[2];
    const size_t typesize = in[3];
    const size_t nbytes = u32At(in, 4), blocksize = u32At(in, 8), cbytes = u32At(in, 12);
    if (version == 0 || version > 2) return fail(error, "format version " + std::to_string(version) + " is not read");
    if (cbytes < kHeader || cbytes > in.size()) return fail(error, "the frame is cut short");
    if (nbytes > limit) return fail(error, "the frame holds more than it should");
    out.assign(nbytes, 0);
    if (nbytes == 0) return true;
    if (flags & kMemcpyed) {
        // Copied as it was: what could not be made smaller.
        if (cbytes != nbytes + kHeader) return fail(error, "a copied frame of the wrong size");
        std::memcpy(out.data(), in.data() + kHeader, nbytes);
        return true;
    }
    const int codec = flags >> 5;
    if (codec == kSnappy || codec == kZstd || codec > kZstd) {
        return fail(error, std::string("frames compressed with ") + (codec == kSnappy ? "Snappy" : codec == kZstd ? "Zstd" : "an unknown compressor") +
                               " are not read");
    }
    if (typesize == 0 || blocksize == 0) return fail(error, "a frame with no blocks");
    const size_t blocks = nbytes / blocksize + (nbytes % blocksize ? 1 : 0), leftover = nbytes % blocksize;
    if (kHeader + 4 * blocks > cbytes) return fail(error, "the frame is cut short");
    std::vector<uint8_t> shuffled(blocksize), unpacked;
    std::string why;
    for (size_t b = 0; b < blocks; ++b) {
        const bool last = leftover != 0 && b + 1 == blocks;
        const size_t size = last ? leftover : blocksize;
        const bool shuffle = (flags & kShuffle) && typesize > 1;
        if (!shuffle && (flags & kBitShuffle) && size >= typesize) return fail(error, "bit-shuffled frames are not read");
        // A block in a stream per byte of the type -- unless the frame says
        // not, or (as before it could) the type is too wide or the block too small.
        const size_t splits =
            (!(flags & kDontSplit) && typesize <= kMaxSplits && blocksize / typesize >= kMinBufferSize && !last) ? typesize : 1;
        const size_t part = size / splits;
        if (part * splits != size) return fail(error, "a block that does not split evenly");
        uint8_t* block = out.data() + b * blocksize;
        uint8_t* into = shuffle ? shuffled.data() : block;
        size_t at = u32At(in, kHeader + 4 * b);
        for (size_t s = 0; s < splits; ++s, into += part) {
            if (at < kHeader + 4 * blocks || at > cbytes || cbytes - at < 4) return fail(error, "the frame is cut short");
            const size_t csize = u32At(in, at);
            at += 4;
            if (csize > cbytes - at) return fail(error, "the frame is cut short");
            const std::span<const uint8_t> data = in.subspan(at, csize);
            at += csize;
            if (csize == part) {
                std::memcpy(into, data.data(), part);  // stored as it was
                continue;
            }
            bool ok = false;
            if (codec == kBloscLz) {
                ok = bloscLz(data, {into, part});
            } else {
                unpacked.clear();
                ok = codec == kLz4 ? lz4Block(data.data(), data.size(), unpacked, part) : zlibInflate(data, unpacked, part, why);
                ok = ok && unpacked.size() == part;
                if (ok) std::memcpy(into, unpacked.data(), part);
            }
            if (!ok) return fail(error, std::string("a block that is not ") + (codec == kBloscLz ? "BloscLZ" : codec == kLz4 ? "LZ4" : "zlib"));
        }
        if (shuffle) unshuffle(typesize, size, shuffled.data(), block);
    }
    return true;
}

}  // namespace pg::io
