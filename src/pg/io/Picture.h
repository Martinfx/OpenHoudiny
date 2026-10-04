#pragma once
//
// Pictures in: a plate -- the footage an effect goes over -- or any still,
// read without libraries. PNG, JPEG and OpenEXR, the three a plate comes
// as:
//
//   .png          8 and 16 bits, grey, grey and alpha, RGB, RGBA and
//                 palettes, interlaced or not; sRGB
//   .jpg .jpeg    baseline and progressive, grey and YCbCr, any
//                 subsampling; sRGB
//   .exr          lines of pixels, uncompressed or RLE, ZIP, PIZ, PXR24,
//                 B44 or B44A; half, float and uint channels; linear --
//                 light of another space than Rec. 709 (ACEScg, ACES2065-1:
//                 the file's chromaticities say so) brought to it
//
// A picture keeps its values as the file has them: PNG's and JPEG's as
// they are shown (sRGB, 0 to 1), EXR's in linear light. What they are in
// the light of a render is for the renderer to say: its own view transform
// is what a shown picture has to go back through.
//
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace pg::io {

struct Picture {
    int width = 0, height = 0;
    std::vector<float> rgba;  ///< width x height x 4, the top row first; alpha 1 where the file has none
    bool linear = false;      ///< linear light (EXR); else as shown, sRGB (PNG, JPEG)

    bool empty() const { return width <= 0 || height <= 0; }
    const float* pixel(int x, int y) const { return &rgba[(static_cast<size_t>(y) * width + x) * 4]; }
};

/// A picture file, told by its bytes (not its name). False, with why, for a
/// file that cannot be read or holds what is not read here.
bool readPicture(const std::string& path, Picture& out, std::string& error);
bool decodePicture(std::span<const uint8_t> bytes, Picture& out, std::string& error);

/// Each kind on its own.
bool decodePng(std::span<const uint8_t> bytes, Picture& out, std::string& error);
bool decodeJpeg(std::span<const uint8_t> bytes, Picture& out, std::string& error);

/// A PNG's bytes: `pixels` the rows top to bottom, `channels` bytes a pixel
/// -- 3 RGB, 4 RGBA -- 8 bits each. The data goes into stored deflate
/// blocks: larger than a compressor would make it, opened by everything.
/// Empty for a size of nothing or other channels.
std::string encodePng(const uint8_t* pixels, int width, int height, int channels);

/// A picture to a file, the kind its name says: .png and .jpg (.jpeg) as
/// shown, 8 bits a channel -- values of 0 to 1, rounded, a JPEG without
/// alpha, at `quality` (1 to 100) -- and .exr as it is, linear light in
/// half floats. False, with why, for another kind or a file that cannot be
/// written.
bool writePicture(const std::string& path, const Picture& picture, int quality, std::string& error);

/// sRGB's curve: a value as a file stores it to linear light, and back.
float srgbToLinear(float v);
float linearToSrgb(float v);

/// The file of `frame` in a numbered sequence. `pattern` names it as Nuke,
/// Houdini and ffmpeg do: "plate.####.exr" (as many digits as #), "plate.
/// $F4.exr" ($F: no padding), "plate.%04d.exr" ("%d": none). A name with
/// none of them is the file itself, every frame.
std::string sequenceFile(const std::string& pattern, int frame);
/// Whether a name numbers frames.
bool isSequence(const std::string& pattern);

}  // namespace pg::io
