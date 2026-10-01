#pragma once
//
// A render's picture into a file: a PNG as a screen shows it, or an OpenEXR
// of the light itself with what the first surface each pixel sees -- its
// depth (Z), colour (albedo.*) and normal (N.*) -- for compositing. From the
// path tracer, or from what any renderer made (Cycles.h).
//
#include "pg/render/PathTracer.h"

#include <string>

namespace pg::render {

/// What a render made: the light -- the noise taken out or not -- and what
/// the first surface each pixel sees is (as PathTracer's albedo(), normal()
/// and depth() have them); the exposure it is shown with.
struct Rendered {
    Image beauty, albedo, normal, depth;
    float exposure = 1.0f;
};

/// The path tracer's: `denoise`, the picture with the noise taken out.
Rendered renderedOf(const PathTracer& tracer, bool denoise);

/// To `path`: an EXR when its extension says so, else a PNG. False, with
/// why, when it cannot be written or there is nothing rendered.
bool savePicture(const Rendered& rendered, const std::string& path, const std::string& comment, std::string& error);
bool savePicture(const PathTracer& tracer, const std::string& path, bool denoise, const std::string& comment,
                 std::string& error);

/// As a screen shows it: RGB, 8 bits, the top row first.
std::vector<uint8_t> displayRgb(const Rendered& rendered);
std::vector<uint8_t> displayRgb(const PathTracer& tracer, bool denoise);

}  // namespace pg::render
