#pragma once
//
// ACES -- the Academy Color Encoding System -- as OpenColorIO's ACES configs
// show a picture, without the library: the light of a render (linear Rec.
// 709, D65) through an output transform onto an sRGB screen, and its colour
// spaces for compositing.
//
//   ACES 1.0   the view "ACES 1.0 - SDR Video" of OpenColorIO's ACES 1.3
//              configs: the Reference Rendering Transform (a glow in the
//              shadows of saturated colours, reds that do not go magenta, a
//              filmic curve in AP1) and the SDR video output (the 48-nit
//              cinema curve to 100 nits, a dim room's gamma, a little less
//              saturation), as the Academy's CTL has them.
//   ACES 2.0   the view "ACES 2.0 - SDR 100 nits (Rec.709)" of the ACES 2.0
//              configs: the tonescale on the lightness of a colour
//              appearance model (Hellwig 2022, as ACES tunes it), the
//              colourfulness compressed with it, and what lies outside Rec.
//              709 brought in along lines towards a focus -- bright colours
//              go towards white, hues stay.
//
// Both end in sRGB as OpenColorIO's "sRGB - Display" encodes it. The two
// match OpenColorIO 2.6's built-in configs to the last few bits of a float
// (tests/test_aces.cpp; the values from tests/data/aces/make_aces.py). Both
// go back too -- from a picture to the light that shows as it -- as
// OpenColorIO's inverse transforms do: what a plate (Plate.h) is to the
// renderer. ACES's colour spaces, ACEScg and ACES2065-1, are in
// core/ColorSpace.h.
//
#include "pg/core/Types.h"

#include <cstdint>

namespace pg::render {

enum class AcesOutput : uint8_t { V1, V2 };

/// Linear Rec. 709 light as an sRGB screen shows it through `output`: 0 to 1.
Vec3 acesShown(const Vec3& linear, AcesOutput output);
/// The light `acesShown` shows as `display` (0 to 1): its inverse. The
/// brightest the transform shows -- white, a saturated primary at its
/// full -- the least light that shows as it.
Vec3 acesUnshown(const Vec3& display, AcesOutput output);

/// sRGB's encoding of linear light, as OpenColorIO's "sRGB - Display" has
/// it (a power of 1/2.4 with a straight line near black); and back.
float srgbEncoded(float linear);
float srgbDecoded(float encoded);

}  // namespace pg::render
