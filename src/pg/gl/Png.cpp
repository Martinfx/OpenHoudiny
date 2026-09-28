#include "pg/gl/Png.h"

#include "pg/io/Picture.h"

#include <fstream>

namespace pg::gl {

bool writePng(const std::string& path, int width, int height, int channels,
              const std::vector<uint8_t>& pixels) {
    if (width <= 0 || height <= 0 || (channels != 3 && channels != 4)) return false;
    const size_t rowBytes = static_cast<size_t>(width) * static_cast<size_t>(channels);
    if (pixels.size() < rowBytes * static_cast<size_t>(height)) return false;
    const std::string png = io::encodePng(pixels.data(), width, height, channels);
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out.write(png.data(), static_cast<std::streamsize>(png.size()));
    return static_cast<bool>(out);
}

}  // namespace pg::gl
