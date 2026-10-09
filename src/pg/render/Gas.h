#pragma once
//
// The gas of a frame as the path tracer goes through it (PathTracer.h):
// smoke that stops light and scatters it, fire that gives it off -- as the
// Volume Look has them (sim/Look.h), the look the viewport draws with
// (pg/gl/Volume.h), the light found as it goes instead of guessed.
//
//   the grid    the smoke, the temperature, the flame and the steam of each
//               cell of the frame (sim::Frame) in a NanoVDB grid (OpenVDB's,
//               Apache 2.0): the tiles of 8 x 8 x 8 cells that hold any are
//               its leaves.
//               Read between the cells' middles, trilinearly, as the
//               viewport's texture is; faded out over the last cells before
//               the open sides and the top, as the viewport fades it.
//   the most    of each tile: the most smoke, temperature, flame and steam
//               a point in it reads -- its own cells and the layer round them
//               that the reading takes in. How far apart the steps may be.
//
// Steam stops and scatters light as smoke does, as dense as the Volume
// Look's Steam Density says and white: where both are, a scattering keeps
// of the light what each would, by how much of it each stops.
//
// While the shutter is open the gas moves (sim::Frame::velocity, a velocity
// for each block of 2 x 2 x 2 cells): a ray at `time` seconds from the
// frame's moment reads the gas where it was then -- back along the
// velocity, twice over, as Cycles does (Kim and Ko's Eulerian motion blur).
// The velocity is kept on the gas's tiles and spread out over a ring of
// tiles round them, so that the gas is read there too as it moves out; the
// most of each tile then takes in the tiles as far round as the gas goes
// in half a frame.
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
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

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
    float steamDensity = 8.0f;          ///< light the steam stops, per unit of steam per world unit
    Vec3 steamColor{0.92f, 0.93f, 0.95f};  ///< the colour thick steam looks
    Vec3 steamAlbedo = albedoOf(steamColor);

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
    /// `frameTime`: seconds from a frame to the next -- how far it may move
    /// while a shutter is open, half of it either way at the most.
    static std::shared_ptr<const Gas> build(const sim::Frame& frame, float frameTime = 1.0f / 30.0f);
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
    /// ... and the steam (0 or more), as w.
    Vec4 fieldsAt(const Vec3& p) const;
    /// Whether any cell holds steam.
    bool steamy() const;
    /// How fast the gas goes at a world point, world units a second: between
    /// the blocks' middles, trilinearly; 0 where none is kept.
    Vec3 velocityAt(const Vec3& p) const;
    /// Whether it moves: a frame with its velocity.
    bool moves() const;
    /// The most it goes, world units a second.
    float fastest() const;
    /// Where the gas that is at `p` at `time` seconds from the frame's
    /// moment was at the moment: back along the velocity there, twice over.
    Vec3 advected(const Vec3& p, float time) const;
    /// Light the gas stops per world unit, where the fields are `f` (at()):
    /// the smoke's -- less in the flames, where it glows.
    static float extinction(const Vec3& f, const GasLook& look);
    /// ... with the steam's, where the fields are `f` (fieldsAt()).
    static float extinction(const Vec4& f, const GasLook& look);
    /// The share of the light a scattering keeps where the fields are `f`:
    /// the smoke's and the steam's, by how much of the light each stops.
    static Vec3 albedo(const Vec4& f, const GasLook& look);
    /// Light the flames give off per world unit, where the fields are `f`:
    /// a black body from 1000 K to 3000 K as the temperature rises -- the
    /// viewport's.
    static Vec3 emission(const Vec3& f, const GasLook& look);

    /// Delta tracking along the unit `dir` from `origin`, between `tMin`
    /// and `tMax`: whether the light is scattered on the way, `t` where, and
    /// the share of it the scattering keeps (albedo()); `emitted`, the light
    /// the flames give off along the way to there, as it gets to the origin.
    /// The gas as it is `time` seconds from the frame's moment (advected()).
    bool track(const Vec3& origin, const Vec3& dir, float tMin, float tMax, const GasLook& look, Rng& rng, float& t,
               Vec3& emitted, Vec3& kept, float time = 0.0f) const;
    /// Ratio tracking: the share of the light that gets through between
    /// `tMin` and `tMax`.
    float transmittance(const Vec3& origin, const Vec3& dir, float tMin, float tMax, const GasLook& look,
                        Rng& rng, float time = 0.0f) const;
    /// What the eye sees of the gas along a ray before `tMax`, without
    /// noise -- a step a cell -- for the denoiser to go by: `through`, the
    /// share of what is behind that shows through it; `depth`, how far on
    /// average the light the smoke sends back comes from (infinity: none).
    void seen(const Vec3& origin, const Vec3& dir, float tMax, const GasLook& look, float& through,
              float& depth) const;

    /// The gas as a renderer that reads a grid with no holes in it takes it
    /// (Cycles): the light it stops and gives off per world unit (as
    /// extinction() and emission() have them) at the middles of the cells
    /// of the box round the tiles it fills -- of blocks of cells, as few as
    /// make no more than `most`. x fastest, then y, then z.
    struct Dense {
        Box box;                      ///< what the cells fill
        int size[3] = {0, 0, 0};      ///< cells along each axis
        std::vector<float> extinction;
        std::vector<Vec3> emission;   ///< empty where nothing glows
        /// The share of the light a scattering keeps (albedo()) -- empty
        /// without steam: the smoke's everywhere.
        std::vector<Vec3> albedo;
        /// How fast it goes (velocityAt()), at the middles of cells twice as
        /// large over `box` grown by `reach` all round -- where the gas may
        /// be read as it moves; empty when it does not move.
        Box velocityBox;
        int velocitySize[3] = {0, 0, 0};
        std::vector<Vec3> velocity;
    };
    Dense dense(const GasLook& look, size_t most, float reach = 0.0f) const;
    /// How fast the gas goes (velocityAt()) over `box` grown by `reach` all
    /// round, within the domain, at the middles of cells `edge` wide: into
    /// `out`'s velocityBox, velocitySize and velocity (Dense).
    void denseVelocity(const Box& box, float edge, float reach, Dense& out) const;

    /// The gas as a renderer that reads sparse grids takes it (Cycles): a
    /// NanoVDB grid a field, of every cell the gas fills -- as fine as the
    /// simulation's, however many -- the rest reading the grid's background:
    /// the light it stops per world unit (extinction(): floats, 0), the
    /// light it gives off (emission(): x y z of four floats, 0) and the
    /// share a scattering keeps (albedo(): x y z of four, the smoke's).
    /// Cell (i, j, k) has its middle at origin + (i, j, k) + 0.5 cells.
    struct Sparse {
        Box box;                       ///< the cells it fills and one round them
        Vec3 origin{0.0f, 0.0f, 0.0f};
        float cell = 1.0f;             ///< world units
        std::vector<uint8_t> extinction;  ///< the grid's buffer; empty: no gas
        std::vector<uint8_t> emission;    ///< empty where nothing glows
        std::vector<uint8_t> albedo;      ///< empty without steam
    };
    Sparse sparse(const GasLook& look) const;

    struct Grid;  // NanoVDB's, and the most of each tile

private:
    Gas();
    std::unique_ptr<Grid> grid_;
};

}  // namespace pg::render
