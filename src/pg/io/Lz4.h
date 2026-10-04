#pragma once
//
// LZ4 blocks decompressed (the block format, without a frame round it):
// what USD's crate files and OpenVDB's Blosc-compressed grids hold. A block
// is sequences of a token -- how many literal bytes, how long a match --
// the literals, and where the match starts back in what came out.
//
#include <cstddef>
#include <cstdint>
#include <vector>

namespace pg::io {

/// Decompresses an LZ4 block of `size` bytes onto the end of `out`, which
/// may grow to `limit`. False for bytes that are not one.
bool lz4Block(const uint8_t* in, size_t size, std::vector<uint8_t>& out, size_t limit);

}  // namespace pg::io
