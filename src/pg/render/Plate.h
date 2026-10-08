#pragma once
//
// The plate in a render: what the camera filmed (sim::Camera::plate), which
// the CG goes over when the shot is rendered through that camera.
//
//   the render   over a plate a renderer gives the CG alone -- its light,
//                premultiplied -- with how much of each pixel it covers
//                (alpha), and what the plate is multiplied by there
//                (catcher): 1 where the CG changes nothing, less in its
//                shadows on a shadow catcher, more where its fire lights
//                one. Holdouts and catchers (sim::Matte) are the real things
//                the plate shows: the CG behind them is hidden.
//   the picture  plate x catcher x (1 - alpha) + CG, in linear light. A
//                plate as shown (PNG, JPEG) goes back through the view
//                transform first (unshown), so that where nothing covers it,
//                it comes out of the render as it went in; one in linear
//                light (EXR) is taken as light.
//
// Without a plate everything is drawn as itself, holdouts and catchers too.
//
#include "pg/io/Picture.h"
#include "pg/render/PathTracer.h"
#include "pg/sim/Camera.h"

#include <memory>
#include <string>

namespace pg::render {

struct Plate {
    io::Picture picture;  ///< as the file holds it: shown values (PNG, JPEG), or light (EXR)
    sim::Camera camera;   ///< the camera that filmed it: its frame is the picture's
    std::string file;
};

/// The plate of `file`, filmed by `camera`; null, with why, when the file
/// cannot be read.
std::shared_ptr<const Plate> loadPlate(const std::string& file, const sim::Camera& camera, std::string& error);
/// An empty plate -- black, in linear light -- seen by `camera`: what a
/// transparent render (sim::Look::transparent) is drawn over, the CG alone.
std::shared_ptr<const Plate> blankPlate(const sim::Camera& camera);

/// The plate in the renderer's light, a picture the size of the plate's: a
/// shown one back through `view` at `exposure` (shown(light x exposure)
/// gives the picture back), one in linear light as it is.
Image plateLight(const Plate& plate, Settings::View view, float exposure, const OcioView* ocio = nullptr);

/// What `light` (plateLight) shows in each pixel of a picture `width` x
/// `height` seen by `camera`: along each pixel's middle ray, what the camera
/// that filmed the plate saw that way -- pixel for pixel when they are one
/// camera and one size; black outside the plate's frame.
Image plateSeen(const Plate& plate, const Image& light, const sim::Camera& camera, int width, int height);

/// The CG over the plate: `plate` (plateSeen) x `catcher` x (1 - `alpha`) +
/// `cg`. An alpha or a catcher of another size counts as none: 0, and 1.
Image overPlate(const Image& cg, const Image& alpha, const Image& catcher, const Image& plate);

}  // namespace pg::render
