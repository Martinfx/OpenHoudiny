#pragma once
//
// Linear colour spaces: what light is in when it is not linear Rec. 709
// (sRGB's primaries, D65) -- the space the renderers work in, as Blender
// and OpenColorIO's default "scene_linear" are:
//
//   ACEScg       AP1 primaries, the ACES white (about D60): what ACES
//                pipelines render and composite in
//   ACES2065-1   AP0 primaries, which hold every colour there is: what ACES
//                pipelines hand pictures over in
//
// Between them by Bradford's adaptation of the whites, as OpenColorIO's ACES
// configs convert -- the matrices built in double as there. OpenEXR says
// which a picture is in by its chromaticities attribute: io/Exr.h writes it
// and reads it, and a picture read in another space comes to Rec. 709.
//
#include "pg/core/Types.h"

#include <array>
#include <cstdint>

namespace pg {

enum class LinearSpace : uint8_t { Rec709, ACEScg, ACES2065_1 };

/// Chromaticities, x and y each, of red, green, blue and white -- in
/// OpenEXR's order.
using Chromaticities = std::array<float, 8>;

Chromaticities chromaticitiesOf(LinearSpace space);
/// Whether `c` are Rec. 709's (to the digits files write them with).
bool isRec709(const Chromaticities& c);

/// Linear Rec. 709 light in `space`, and back.
Vec3 fromRec709(const Vec3& c, LinearSpace space);
Vec3 toRec709(const Vec3& c, LinearSpace space);
/// The matrices for light of the primaries and white `c`: to linear Rec.
/// 709, and from it.
Mat3 toRec709From(const Chromaticities& c);
Mat3 fromRec709To(const Chromaticities& c);

namespace color {

/// A 3 x 3 matrix row by row, applied to a column.
using D33 = std::array<double, 9>;

/// Chromaticities to build matrices from: red, green, blue, white.
struct Primaries {
    double xy[4][2];
};

inline constexpr Primaries kAP0{{{0.7347, 0.2653}, {0.0, 1.0}, {0.0001, -0.0770}, {0.32168, 0.33767}}};
inline constexpr Primaries kAP1{{{0.713, 0.293}, {0.165, 0.830}, {0.128, 0.044}, {0.32168, 0.33767}}};
inline constexpr Primaries kRec709{{{0.64, 0.33}, {0.30, 0.60}, {0.15, 0.06}, {0.3127, 0.3290}}};

D33 inverse(const D33& m);
D33 times(const D33& a, const D33& b);
std::array<double, 3> times(const D33& m, const std::array<double, 3>& v);
/// RGB of `p` to CIE XYZ, white at Y 1 (OpenColorIO's rgb2xyz_from_xy).
D33 rgbToXyz(const Primaries& p);
/// RGB of `from` to RGB of `to`, the whites adapted by Bradford where they
/// differ and `adapt` says so (OpenColorIO's build_conversion_matrix).
D33 conversion(const Primaries& from, const Primaries& to, bool adapt);
/// RGB of `from` to CIE XYZ of a display's white, D65 (Bradford).
D33 toXyzD65(const Primaries& from);
/// Bradford's chromatic adaptation of CIE XYZ from the white `from` (its
/// XYZ) to `to`: the cone responses scaled from one white to the other.
D33 bradford(const std::array<double, 3>& from, const std::array<double, 3>& to);

}  // namespace color

}  // namespace pg
