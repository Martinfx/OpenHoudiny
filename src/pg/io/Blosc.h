#pragma once
//
// Blosc (version 1) frames decompressed: what OpenVDB compresses a grid's
// values with when it is built with Blosc -- as Houdini's and Blender's
// are. A frame is a 16-byte header -- versions, flags, the size of what it
// holds and of each block -- where each block starts, and the blocks: each
// one compressed whole or in a stream per byte of the type (split), by
// BloscLZ, LZ4 or zlib, after its bytes were shuffled (all the first bytes
// of the numbers, then all the second...). Snappy and Zstd are not read,
// nor bit-shuffled frames.
//
// Written from the frame format c-blosc documents (README_HEADER), and
// tested against what python-blosc makes.
//
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace pg::io {

/// The frame `in` decompressed into `out` (resized to what it holds), at
/// most `limit` bytes. False, with why, for a frame that is not one, is cut
/// short, or uses what is not read.
bool bloscDecompress(std::span<const uint8_t> in, std::vector<uint8_t>& out, size_t limit, std::string& error);

}  // namespace pg::io
