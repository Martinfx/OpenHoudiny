#pragma once
//
// A path tracer's picture into a file: a PNG as a screen shows it, or an
// OpenEXR of the light itself with what the first surface each pixel sees
// -- its depth (Z), colour (albedo.*) and normal (N.*) -- for compositing.
//
#include "pg/render/PathTracer.h"

#include <string>

namespace pg::render {

/// To `path`: an EXR when its extension says so, else a PNG. `denoise`:
/// the picture with the noise taken out. False, with why, when it cannot be
/// written or there is nothing rendered.
bool savePicture(const PathTracer& tracer, const std::string& path, bool denoise, const std::string& comment,
                 std::string& error);

/// As a screen shows it: RGB, 8 bits, the top row first.
std::vector<uint8_t> displayRgb(const PathTracer& tracer, bool denoise);

}  // namespace pg::render
