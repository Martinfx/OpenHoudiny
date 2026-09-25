#pragma once
//
// Writes PNG files without zlib: the image data goes into uncompressed
// ("stored") deflate blocks. Files are larger than a compressor would make
// them, but every viewer opens them and nothing needs to be linked.
//
#include <cstdint>
#include <string>
#include <vector>

namespace pg::gl {

/// `pixels` holds rows top to bottom, `channels` (3 or 4) bytes per pixel.
bool writePng(const std::string& path, int width, int height, int channels,
              const std::vector<uint8_t>& pixels);

}  // namespace pg::gl
