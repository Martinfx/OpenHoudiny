#pragma once
//
// A render's picture into a file: a PNG as a screen shows it, or an OpenEXR
// of the light itself with what the first surface each pixel sees -- its
// depth (Z), colour (albedo.*) and normal (N.*) -- for compositing. From the
// path tracer, or from what any renderer made (Cycles.h). Over a plate
// (Plate.h) the PNG is the CG over it; the EXR the CG alone, its alpha (A)
// and what the plate is multiplied by (catcher.*), for compositing:
// plate x catcher x (1 - A) + RGB. The EXR's light and albedo are in the
// colour space the settings ask (Settings::exrSpace), its chromaticities
// saying which.
//
#include "pg/render/PathTracer.h"

#include <string>

namespace pg::render {

/// What a render made: the light -- the noise taken out or not -- and what
/// the first surface each pixel sees is (as PathTracer's albedo(), normal()
/// and depth() have them); the exposure and the view it is shown with.
struct Rendered {
    Image beauty, albedo, normal, depth;
    /// Over a plate: beauty is the CG alone; `alpha` how much of each pixel
    /// it covers, `catcher` what the plate is multiplied by there, `plate`
    /// its light in each pixel (plateSeen). Else all empty.
    Image alpha, catcher, plate;
    float exposure = 1.0f;
    Settings::View view = Settings::View::AgXPunchy;  ///< how a PNG shows its light
    std::shared_ptr<const OcioView> ocio;              ///< with View Ocio: the config's view
    LinearSpace space = LinearSpace::Rec709;           ///< what an EXR's light and colours are in
    /// Rendered transparent (sim::Look::transparent): over an empty plate,
    /// the picture the CG alone with its alpha (transparentAlpha).
    bool transparent = false;
};

/// Of a transparent render: how much of each pixel is covered -- by the CG
/// (alpha), and by the shadows the catchers take: where the catcher
/// darkens nothing behind by its mean, that much more. One channel.
Image transparentAlpha(const Rendered& rendered);
/// As a screen shows it with its alpha: RGBA, 8 bits, the top row first --
/// the colour not premultiplied. Of a render that is not transparent, alpha 1.
std::vector<uint8_t> displayRgba(const Rendered& rendered);

/// The path tracer's: `denoise`, the picture with the noise taken out.
Rendered renderedOf(const PathTracer& tracer, bool denoise);

/// To `path`: an EXR when its extension says so, else a PNG. False, with
/// why, when it cannot be written or there is nothing rendered.
bool savePicture(const Rendered& rendered, const std::string& path, const std::string& comment, std::string& error);
bool savePicture(const PathTracer& tracer, const std::string& path, bool denoise, const std::string& comment,
                 std::string& error);

/// The light of the picture: the beauty, over the plate when there is one.
Image composited(const Rendered& rendered);
/// As a screen shows it: RGB, 8 bits, the top row first.
std::vector<uint8_t> displayRgb(const Rendered& rendered);
std::vector<uint8_t> displayRgb(const PathTracer& tracer, bool denoise);

}  // namespace pg::render
