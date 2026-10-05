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
#include "pg/core/ColorSpace.h"
#include "pg/core/Types.h"

#include <cstdint>
#include <string_view>

namespace pg::render {

enum class AcesOutput : uint8_t { V1, V2 };

/// Linear Rec. 709 light as an sRGB screen shows it through `output`: 0 to 1.
Vec3 acesShown(const Vec3& linear, AcesOutput output);
/// The light `acesShown` shows as `display` (0 to 1): its inverse. The
/// brightest the transform shows -- white, a saturated primary at its
/// full -- the least light that shows as it.
Vec3 acesUnshown(const Vec3& display, AcesOutput output);

/// OpenColorIO's built-in ACES output transforms for SDR screens, as its
/// ACES configs name them -- "ACES-OUTPUT - ACES2065-1_to_CIE-XYZ-D65 - "
/// and then the name below: ACES2065-1 light to the CIE XYZ (D65) of the
/// screen, 1 its white -- what a display's colour space then encodes --,
/// in the steps OpenColorIO takes; and back as its inverse takes them.
///
/// ACES 1.x: the RRT and the 48-nit cinema curve; for video, a dim room's
/// gamma and less saturation; held to a gamut ("lim"), or a white of D60
/// or D65 simulated ("sim") -- the brightest a little less bright that it
/// stays white. ACES 2.0, at 100 nits: the limiting gamut and its white,
/// in the encoding's gamut where they say so ("-in-").
enum class AcesOutputXyz : uint8_t {
    SdrCinema10,                     ///< SDR-CINEMA_1.0
    SdrVideo10,                      ///< SDR-VIDEO_1.0
    SdrCinemaRec709lim11,            ///< SDR-CINEMA-REC709lim_1.1
    SdrVideoRec709lim11,             ///< SDR-VIDEO-REC709lim_1.1
    SdrVideoP3lim11,                 ///< SDR-VIDEO-P3lim_1.1
    SdrCinemaD60simD6511,            ///< SDR-CINEMA-D60sim-D65_1.1
    SdrVideoD60simD6510,             ///< SDR-VIDEO-D60sim-D65_1.0
    SdrCinemaD60simDci10,            ///< SDR-CINEMA-D60sim-DCI_1.0
    SdrCinemaD65simDci11,            ///< SDR-CINEMA-D65sim-DCI_1.1
    Sdr100Rec709_20,                 ///< SDR-100nit-REC709_2.0
    Sdr100P3D65_20,                  ///< SDR-100nit-P3-D65_2.0
    Sdr100Rec709D60InRec709D65_20,   ///< SDR-100nit-REC709-D60-in-REC709-D65_2.0
    Sdr100Rec709D60InP3D65_20,       ///< SDR-100nit-REC709-D60-in-P3-D65_2.0
    Sdr100Rec709D60InRec2020D65_20,  ///< SDR-100nit-REC709-D60-in-REC2020-D65_2.0
    Sdr100P3D60InP3D65_20,           ///< SDR-100nit-P3-D60-in-P3-D65_2.0
    Sdr100P3D60InXyzE_20,            ///< SDR-100nit-P3-D60-in-XYZ-E_2.0
};
/// The transform of OpenColorIO's name `name` (any case); false for one
/// not here -- HDR's among them.
bool acesOutputXyzNamed(std::string_view name, AcesOutputXyz& out);
Vec3 acesOutputXyz(const Vec3& ap0, AcesOutputXyz which);
/// Back: the ACES2065-1 light that shows as `xyz`, as OpenColorIO's inverse
/// has it (for ACES 1.x, the red modifier's inverse only near).
Vec3 acesOutputXyzBack(const Vec3& xyz, AcesOutputXyz which);
/// The same in OpenColorIO's two steps -- so that the matrix that ends it
/// and those after it make one, as there: to the light of the gamut it ends
/// in (and back), then this matrix to CIE XYZ.
Vec3 acesOutputRgb(const Vec3& ap0, AcesOutputXyz which);
Vec3 acesOutputRgbBack(const Vec3& rgb, AcesOutputXyz which);
color::D33 acesOutputRgbToXyz(AcesOutputXyz which);

/// sRGB's encoding of linear light, as OpenColorIO's "sRGB - Display" has
/// it (a power of 1/2.4 with a straight line near black); and back.
float srgbEncoded(float linear);
float srgbDecoded(float encoded);

}  // namespace pg::render
