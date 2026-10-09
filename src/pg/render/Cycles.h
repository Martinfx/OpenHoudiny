#pragma once
//
// Rendering through Cycles, Blender's renderer (Apache 2.0), as a library:
// what a frame shows (Scene.h) made into a Cycles scene and rendered on all
// the processor's cores --
//
//   meshes       each of ours once, its triangles with their normals and
//                colours (an attribute "Col"); an instance, an object that
//                places a mesh -- a meadow of 122 577 clumps of grass, 84
//                trees and 65 shrubs is 22 meshes and the objects that
//                stand them in it;
//   objects      the scene's spheres, boxes, cylinders, cones and tori, cut
//                into triangles finely enough not to show it;
//   materials    a Principled BSDF of each surface's colour times its
//                instance's tint, roughness and metallic, reflecting as
//                Blender's does; a translucent one mixed in by its
//                translucency (leaves, grass); glass as glass, and water
//                bending light, losing it with depth as Clarity says and
//                taking on the Water Look's colour -- both letting the sun
//                through to what is behind them, as ours do, not only along
//                the caustics Cycles finds;
//   gas          the smoke and the fire as NanoVDB grids of what they stop
//                and give off in each cell (Gas::dense) in a box round them:
//                a Principled Volume scattering in the smoke's colour, where
//                Cycles' rays scatter found without stepping through it;
//   floor        the look's floor, fading out far away as the viewport's;
//   light        the look's sun as a distant light as wide as Sun Angle; its
//                sky, as the viewport draws it, an environment Cycles samples
//                as it lights the scene -- the camera seeing the studio's
//                backdrop instead, without Sky Behind;
//   camera       the shot's: its lens, the f-number and the distance in
//                focus of the Output.
//   plate        over one (Plate.h): the film transparent where the CG is
//                not -- glass too -- the scene's holdouts and catchers, and
//                the floor's, Cycles' own; its shadow catcher pass, what the
//                plate is multiplied by. Without one, they are drawn as
//                themselves.
//
// Cycles is Z up; ours, Y: the whole scene is turned onto its side for it.
// The light it renders is ours -- linear, the same exposure, the same tone
// curve (toDisplay) -- and Open Image Denoise takes the noise out of it, as
// Cycles does in Blender. Rendering for the Render tab, its pictures come as
// Cycles shows them in Blender's viewport: the first of fewer, larger
// pixels. Without Cycles in the build (PG_CYCLES=OFF, or no OpenImageIO to
// build it with) the path tracer (PathTracer.h) renders.
//
#include "pg/render/PathTracer.h"
#include "pg/render/Save.h"
#include "pg/render/Scene.h"

#include <cstdint>
#include <memory>
#include <string>

namespace pg::render {

/// Whether the build has Cycles; "Cycles 4.5.0", or "".
bool cyclesAvailable();
std::string cyclesVersion();
/// What takes the noise out of Cycles' pictures: "Open Image Denoise", or
/// "" where Cycles was built without it.
std::string cyclesDenoiser();

/// A render through Cycles, of one scene after another.
class CyclesRender {
public:
    /// `interactive`: as the Render tab wants it -- a picture now and then
    /// while the samples add up, the first of fewer, larger pixels, and the
    /// noise taken out of each; else as the command line: the end alone.
    explicit CyclesRender(bool interactive = false);
    ~CyclesRender();
    CyclesRender(const CyclesRender&) = delete;
    CyclesRender& operator=(const CyclesRender&) = delete;

    /// Renders `scene` with `settings`, from the start -- what it rendered
    /// before, it stops. Returns at once; the render goes on on threads of
    /// Cycles' own. The meshes of the last scene it was given that this one
    /// has too are not made again.
    void start(std::shared_ptr<const Scene> scene, const Settings& settings);
    /// Stopped where it is, or going on.
    void setPaused(bool paused);
    /// Stops, as soon as it can.
    void cancel();
    /// Until the end, or it stopped.
    void wait();

    bool done() const;
    int samples() const;     ///< in each pixel so far
    double seconds() const;  ///< spent rendering
    /// Why it could not render, or "".
    std::string error() const;

    /// A picture newer than the last taken -- linear light, the noise taken
    /// out when the settings ask for it -- false when there is none. Over a
    /// plate, the CG alone, with its alpha -- and at the end the catcher
    /// pass (alpha(), catcher()).
    bool takePicture(Image& beauty, Image* alpha = nullptr, Image* catcher = nullptr);

    /// After the end: the picture, and what the first surface each pixel
    /// sees is -- its colour, its normal, how far it is (as PathTracer's).
    Image beauty() const;
    Image albedo() const;
    Image normal() const;
    Image depth() const;
    /// Over a plate (Scene::plate, Plate.h) the picture is the CG alone:
    /// this how much of each pixel it covers, one channel -- and what the
    /// plate is multiplied by there, Cycles' shadow catcher pass (none
    /// without catchers).
    Image alpha() const;
    Image catcher() const;
    /// All of it, as a file takes it (Save.h): over a plate, with the
    /// plate's light in each pixel.
    Rendered rendered() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace pg::render
