#pragma once
//
// Pictures out to OpenEXR (.exr): what compositing -- Nuke, Fusion,
// Resolve, Blender -- reads a render from: linear light beyond white, as
// many channels as there are passes. Written without the library, as the
// OpenEXR 2 file layout has it:
//
//   magic, version 2 -- one part, lines of pixels
//   header      the channels, sorted by name, a half or a float each;
//               compression RLE; the data and display windows; lines top
//               to bottom; the pixel aspect ratio and the screen window;
//               the chromaticities of the light's colour space, when the
//               caller says them (core/ColorSpace.h); strings and 4 x 4
//               matrices the caller adds (Nuke builds a camera from
//               worldToCamera and worldToNDC)
//   offsets     where each line's chunk starts in the file
//   chunks      a line each: its number, its size, its bytes -- the
//               channels' values of the line one channel after the other,
//               run-length encoded as OpenEXR's RLE does it (the bytes split
//               in two halves, each the difference from the one before,
//               then runs); a line that would not get smaller is stored as
//               it is
//
#include "pg/core/ColorSpace.h"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace pg::io {

struct ExrChannel {
    std::string name;           ///< "R", "Z", "forward.u", "mask.pieces"...
    bool half = true;           ///< 16-bit float, else 32-bit
    std::vector<float> values;  ///< width x height, the top line first
};

struct ExrImage {
    int width = 0, height = 0;
    std::vector<ExrChannel> channels;
    std::vector<std::pair<std::string, std::string>> strings;             ///< attributes of type string
    std::vector<std::pair<std::string, std::array<float, 16>>> matrices;  ///< m44f, row by row
    /// The primaries and white of the light (the chromaticities attribute):
    /// none said, Rec. 709's, as OpenEXR takes a file without it.
    bool hasChromaticities = false;
    Chromaticities chromaticities{};
};

/// The file's bytes. Channels in any order: the file has them sorted.
std::string formatExr(const ExrImage& image);

/// An OpenEXR file read (ExrRead.cpp): one part of lines of pixels,
/// uncompressed or RLE, ZIPS, ZIP, PIZ, PXR24, B44 or B44A; half, float and
/// uint channels, each as floats, the data window placed in the display
/// window (the rest 0); its string, 4 x 4 matrix and chromaticities
/// attributes. False, with
/// why, for what is not read: tiles, deep data, several parts, DWAA/DWAB,
/// subsampled channels.
bool parseExr(std::span<const uint8_t> bytes, ExrImage& out, std::string& error);
bool readExr(const std::string& path, ExrImage& out, std::string& error);
/// ... written to `path`. False, with why: no picture, a channel of the
/// wrong size, or a file that cannot be written.
bool writeExr(const ExrImage& image, const std::string& path, std::string& error);
/// The chromaticities an OpenEXR file's header says its light is in; false
/// when it says none, or is not one. The pixels are not read.
bool exrChromaticities(const std::string& path, Chromaticities& out);

}  // namespace pg::io
