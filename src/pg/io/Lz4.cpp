#include "pg/io/Lz4.h"

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

}  // namespace pg::io
