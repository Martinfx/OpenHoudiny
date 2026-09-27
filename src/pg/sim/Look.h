#pragma once
//
// How simulated gas is drawn: the colour and density of the smoke, the glow of
// the fire, the light. What a Volume Look node compiles to (Network.h) and the
// volume renderer draws with (pg/gl/Volume.h). Plain data: a change of look
// draws the frames again, it never simulates them again.
//
#include "pg/core/Types.h"

namespace pg::sim {

struct Look {
    // Smoke
    Vec3 smokeColor{0.75f, 0.75f, 0.77f};  ///< share of the light it scatters
    float smokeDensity = 20.0f;   ///< light it stops per unit of smoke per world unit
    float occlusion = 3.0f;       ///< how much thick smoke around darkens the sky light

    // Fire
    float flameIntensity = 30.0f; ///< light the flames give off per world unit, at their hottest
    float flameStart = 0.3f;      ///< temperature where flames start to glow
    float flameRange = 4.0f;      ///< ... and how much hotter they glow white
    float fireLight = 2.0f;       ///< how much the fire lights the smoke around it

    // Light
    float lightAzimuth = 169.0f;  ///< degrees round the vertical, from +x towards +z
    float lightElevation = 38.0f; ///< degrees above the horizon
    Vec3 lightColor{1.0f, 0.95f, 0.88f};
    float lightIntensity = 2.2f;
    Vec3 skyColor{0.55f, 0.65f, 0.8f};
    float skyIntensity = 0.25f;

    // Image
    float exposure = 1.0f;
    bool floor = true;            ///< a floor the gas stands on, with its shadow

    /// Unit vector towards the light.
    Vec3 lightDirection() const {
        const float az = lightAzimuth * 0.01745329252f, el = lightElevation * 0.01745329252f;
        return {std::cos(el) * std::cos(az), std::sin(el), std::cos(el) * std::sin(az)};
    }

    bool operator==(const Look&) const = default;
};

}  // namespace pg::sim
