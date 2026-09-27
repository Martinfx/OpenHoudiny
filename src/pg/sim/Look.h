#pragma once
//
// How a simulation is drawn: the colour and density of the smoke, the glow of
// the fire, the colour and clarity of the water, the pieces of what broke,
// the light. What the Volume Look, the Water Look, the RBD Solver and the
// Output compile to (Network.h) and the renderer draws with
// (pg/gl/Volume.h). Plain data: a change of look draws the frames again, it
// never simulates them again.
//
#include "pg/core/Types.h"

#include <string>

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

    // Water
    Vec3 waterColor{0.1f, 0.42f, 0.5f};  ///< the colour water takes on where it is deep
    float waterClarity = 1.5f;    ///< metres: how far one sees into it
    float foam = 1.0f;            ///< how white foam and spray are drawn; 0 not at all
    bool waterSurface = true;     ///< the surface is drawn; off, the water is simulated unseen

    // Rain
    Vec3 rainColor{0.75f, 0.8f, 0.9f};
    float rainOpacity = 0.35f;    ///< how much of what is behind a drop it hides
    float rainStreak = 0.5f;      ///< a drop drawn as long as it falls in this share of a frame
    float wetness = 0.6f;         ///< how wet the rain makes the floor look: darker, shining

    // The pieces of an RBD Solver (Rigid.h: drawnPieces)
    bool pieces = false;          ///< drawn with the displayed geometry: the solver is linked into the Output
    Vec3 piecesColor{0.62f, 0.6f, 0.57f};   ///< where they have no Cd of their own
    Vec3 piecesInside{0.5f, 0.47f, 0.43f};  ///< the faces a fracture cut
    std::string insideGroup = "inside";     ///< ... the group they are in

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
    Vec3 groundColor{0.075f, 0.075f, 0.075f};  ///< the floor's colour
    bool grid = true;             ///< lines on the floor every 10 cm and every metre

    /// Unit vector towards the light.
    Vec3 lightDirection() const {
        const float az = lightAzimuth * 0.01745329252f, el = lightElevation * 0.01745329252f;
        return {std::cos(el) * std::cos(az), std::sin(el), std::cos(el) * std::sin(az)};
    }

    bool operator==(const Look&) const = default;
};

}  // namespace pg::sim
