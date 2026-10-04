#pragma once
//
// Deflate streams (RFC 1951) and zlib streams round them (RFC 1950) made:
// what an OpenVDB grid zipped (COMPRESS_ZIP) holds, read by every zlib.
// Repeats found back through the last 32 KiB along hash chains, each block
// in the Huffman codes its own symbols want (dynamic), or stored where that
// is no smaller. Inflate.h reads them back.
//
#include <cstdint>
#include <span>
#include <vector>

namespace pg::io {

/// A raw deflate stream of `in`. `effort` 1 (fast) to 9 (smallest): how far
/// along the chains to look.
std::vector<uint8_t> deflate(std::span<const uint8_t> in, int effort = 6);

/// The same in a zlib stream: its two header bytes, the deflate data, and
/// the Adler-32 of `in`.
std::vector<uint8_t> zlibDeflate(std::span<const uint8_t> in, int effort = 6);

}  // namespace pg::io
