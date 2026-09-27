#pragma once
//
// Baseline JPEG (ITU T.81) from RGB pixels: YCbCr with the colour halved
// both ways (4:2:0), the standard's quantisation tables scaled by quality as
// libjpeg scales them, and the standard's Huffman tables. A frame of a Motion
// JPEG video is one of these (Video.h); so is a still.
//
#include <cstdint>
#include <string>

namespace pg::io {

/// `rgb`: width x height pixels, three bytes each, the top row first.
/// `quality`: 1 to 100 -- at 90 a frame is hard to tell from its original.
/// Empty for a size of nothing or more than 65535 a side.
std::string encodeJpeg(const uint8_t* rgb, int width, int height, int quality = 90);

bool writeJpeg(const std::string& path, const uint8_t* rgb, int width, int height, int quality, std::string& error);

}  // namespace pg::io
