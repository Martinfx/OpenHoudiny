#pragma once
//
// Taking out the noise a render has left. Intel Open Image Denoise (OIDN,
// Apache 2.0): a neural network trained on pictures path tracers make, given
// each pixel's light, the colour of what it sees (the albedo) and which way
// that faces (the normal) -- the denoiser of Blender, Houdini's Karma,
// Arnold, V-Ray, Unreal. It keeps detail the noise hides from our own filter
// and needs far fewer samples for a clean picture. In a build without it, and
// with PG_DENOISER=own, our own edge-avoiding filter (PathTracer.h:
// denoise()).
//
// The albedo and the normal are cleaned of their own noise first -- depth of
// field, the gas, and leaves and blades a pixel sees only part of leave some
// in them -- and the light is then cleaned guided by them, as OIDN's
// documentation advises. Its device is made once, at first need, its weights
// loaded; it runs on threads of its own.
//
#include "pg/render/PathTracer.h"

#include <cstdint>
#include <string>

namespace pg::render {

enum class Denoiser : uint8_t {
    Oidn,  ///< Intel Open Image Denoise
    Own,   ///< our edge-avoiding a-trous filter
};

/// Whether the build has Open Image Denoise and it runs on this machine.
bool oidnAvailable();
/// "Open Image Denoise 2.3.3", or "" without it.
std::string oidnVersion();
/// OIDN when it is available -- unless the environment says PG_DENOISER=own.
Denoiser defaultDenoiser();
/// "Open Image Denoise 2.3.3" or "own filter": what takes the noise out.
std::string denoiserName(Denoiser denoiser);

/// `beauty`, linear light, with its noise taken out by OIDN, guided by
/// `albedo` and `normal` of the same size; false, with why, when it cannot
/// be -- pictures of different sizes, a build without OIDN, OIDN failed.
bool oidnDenoise(const Image& beauty, const Image& albedo, const Image& normal, Image& out, std::string& error);

}  // namespace pg::render
