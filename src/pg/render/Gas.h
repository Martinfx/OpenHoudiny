#pragma once
//
// The gas of a frame as the path tracer goes through it (PathTracer.h):
// smoke that stops light and scatters it, fire that gives it off -- as the
// Volume Look has them (sim/Look.h), the look the viewport draws with
// (pg/gl/Volume.h), the light found as it goes instead of guessed.
//
//   the grid    the smoke, the temperature and the flame of each cell of the
//               frame (sim::Frame) in a NanoVDB grid (OpenVDB's, Apache 2.0):
//               the tiles of 8 x 8 x 8 cells that hold any are its leaves.
//               Read between the cells' middles, trilinearly, as the
//               viewport's texture is; faded out over the last cells before
//               the open sides and the top, as the viewport fades it.
//   the most    of each tile: the most smoke, temperature and flame a point
//               in it reads -- its own cells and the layer round them that
//               the reading takes in. How far apart the steps may be.
//
// A ray goes through it by delta tracking. It takes steps as long as the
// densest smoke the tile could hold would make them, and at each the smoke
// there decides -- as likely as it is dense to that -- whether the light is
// scattered there or whether the ray goes on, the step having met nothing.
// Through fire it takes a step a cell at least, each adding the light the
// flames give off there. A shadow ray takes the same steps and keeps the
// share of the light each lets through (ratio tracking). Both leave noise,
// not error: the more samples, the closer the picture.
//
// Without NanoVDB (PG_NANOVDB=OFF), build() gives null: the path tracer
// renders without the gas.
//
#include "pg/render/Bvh.h"

#include <cstddef>
#include <memory>
#include <string>

namespace pg::sim {
struct Frame;
struct Look;
}  // namespace pg::sim

namespace pg::render {

struct Rng;

/// How the gas looks to the path tracer: the Volume Look's numbers.
struct GasLook {
    float density = 20.0f;              ///< light the smoke stops, per unit of smoke per world unit
    Vec3 color{0.75f, 0.75f, 0.77f};    ///< the colour thick smoke looks: the look's Smoke Color
    /// The share of the light it stops that each scattering keeps: what,
    /// scattered again and again in thick smoke, comes out as `color`.
    Vec3 albedo = albedoOf(color);
    float flame = 30.0f;                ///< light the flames give off per world unit, at their hottest
    float flameStart = 0.3f;            ///< temperature where flames start to glow
    float flameRange = 4.0f;            ///< ... and how much hotter they glow white

    static GasLook of(const sim::Look& look);
    /// The share each scattering keeps of a medium that looks `color` where
    /// it is thick: Chiang, Kutz and Burley's inversion (Practical and
    /// Controllable Subsurface Scattering for Production Path Tracing,
    /// 2016), as Cycles takes it for skin -- 0.75 keeps 0.984.
    static Vec3 albedoOf(const Vec3& color);
};

/// Whether the build has NanoVDB: whether the path tracer renders the gas.
bool gasAvailable();
/// "NanoVDB 32.9.2", or "" without it.
std::string gasLibrary();

class Gas {
public:
    /// The gas of `frame`; null when it has none -- or without NanoVDB.
    static std::shared_ptr<const Gas> build(const sim::Frame& frame);
    ~Gas();
    Gas(const Gas&) = delete;
    Gas& operator=(const Gas&) = delete;

    /// The simulation's box.
    const Box& bounds() const;
    /// The cells that hold any smoke, temperature or flame.
    size_t cells() const;
    /// Of the grid and the most of the tiles.
    size_t bytes() const;

    /// The smoke (0 or more), the temperature and the flame (0 or more) at
    /// a world point: between the cells' middles, trilinearly; 0 outside.
    Vec3 at(const Vec3& p) const;
    /// Light the gas stops per world unit, where the fields are `f` (at()):
    /// the smoke's -- less in the flames, where it glows.
    static float extinction(const Vec3& f, const GasLook& look);
    /// Light the flames give off per world unit, where the fields are `f`:
    /// a black body from 1000 K to 3000 K as the temperature rises -- the
    /// viewport's.
    static Vec3 emission(const Vec3& f, const GasLook& look);

    /// Delta tracking along the unit `dir` from `origin`, between `tMin`
    /// and `tMax`: whether the light is scattered on the way, `t` where;
    /// `emitted`, the light the flames give off along the way to there, as
    /// it gets to the origin.
    bool track(const Vec3& origin, const Vec3& dir, float tMin, float tMax, const GasLook& look, Rng& rng, float& t,
               Vec3& emitted) const;
    /// Ratio tracking: the share of the light that gets through between
    /// `tMin` and `tMax`.
    float transmittance(const Vec3& origin, const Vec3& dir, float tMin, float tMax, const GasLook& look,
                        Rng& rng) const;
    /// What the eye sees of the gas along a ray before `tMax`, without
    /// noise -- a step a cell -- for the denoiser to go by: `through`, the
    /// share of what is behind that shows through it; `depth`, how far on
    /// average the light the smoke sends back comes from (infinity: none).
    void seen(const Vec3& origin, const Vec3& dir, float tMax, const GasLook& look, float& through,
              float& depth) const;

    struct Grid;  // NanoVDB's, and the most of each tile

private:
    Gas();
    std::unique_ptr<Grid> grid_;
};

}  // namespace pg::render
