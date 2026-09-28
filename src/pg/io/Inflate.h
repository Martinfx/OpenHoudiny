#pragma once
//
// Deflate streams (RFC 1951) and zlib streams around them (RFC 1950)
// decompressed -- what a PNG's pixels and an EXR's ZIP and PXR24 chunks
// are made of. Written from the RFCs: stored, fixed and dynamic Huffman
// blocks, codes looked up ten bits at a time.
//
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace pg::io {

/// A raw deflate stream decompressed onto the end of `out`. False, with why,
/// for data that is not deflate, that ends early, or that would make `out`
/// grow past `limit` bytes.
bool inflate(std::span<const uint8_t> in, std::vector<uint8_t>& out, size_t limit, std::string& error);

/// The same for a zlib stream: its two header bytes, the deflate data, and
/// the Adler-32 of what it holds, checked.
bool zlibInflate(std::span<const uint8_t> in, std::vector<uint8_t>& out, size_t limit, std::string& error);

}  // namespace pg::io
