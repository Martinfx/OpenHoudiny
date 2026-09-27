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
//               strings and 4 x 4 matrices the caller adds (Nuke builds a
//               camera from worldToCamera and worldToNDC)
//   offsets     where each line's chunk starts in the file
//   chunks      a line each: its number, its size, its bytes -- the
//               channels' values of the line one channel after the other,
//               run-length encoded as OpenEXR's RLE does it (the bytes split
//               in two halves, each the difference from the one before,
//               then runs); a line that would not get smaller is stored as
//               it is
//
#include <array>
#include <cstdint>
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
};

/// The file's bytes. Channels in any order: the file has them sorted.
std::string formatExr(const ExrImage& image);
/// ... written to `path`. False, with why: no picture, a channel of the
/// wrong size, or a file that cannot be written.
bool writeExr(const ExrImage& image, const std::string& path, std::string& error);

}  // namespace pg::io
