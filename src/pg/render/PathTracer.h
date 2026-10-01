#pragma once
//
// A path tracer: for each pixel, light followed back from the camera --
// bouncing off surfaces as they scatter it, through glass and water as they
// bend it -- to the sun and the sky (Scene.h). What the viewport draws
// quickly and roughly, this renders as light goes:
//
//   - light that bounces: a shadow lit by the grass beside it, green;
//   - the sun as a disc: shadows sharp where the caster is near, soft far
//     from it; the sun found directly at each bounce and by the bounces
//     themselves, the two weighed together (multiple importance sampling);
//   - surfaces that scatter light diffusely, reflect it off a rough or
//     smooth sheen (GGX), and -- thin ones, leaves and blades of grass --
//     let some of it through (translucency);
//   - glass and water that reflect and bend light (Fresnel, Snell), water
//     taking on its colour with depth;
//   - smoke that scatters light, again and again where it is thick, and
//     shades itself and what is under it; flames that give light off, to
//     the smoke round them and further (Gas.h);
//   - a lens that blurs what is out of focus (an f-number), and each pixel
//     sampled all over, so edges come out smooth.
//
// A render is progressive: pass after pass adds a sample to every pixel,
// the picture getting less noisy as the samples add up. The numbers each
// sample takes come from its pixel and its number alone, so a render is the
// same on any number of threads. What noise is left, Intel Open Image
// Denoise takes out (Denoise.h) -- or, in a build without it, an
// edge-avoiding filter guided by the colour, the normal and the depth of what
// each pixel sees (denoise()), keeping edges, a blade from the blade behind it.
//
#include "pg/render/Scene.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace pg::render {

struct Settings {
    int width = 1280, height = 720;  ///< pixels
    int samples = 128;               ///< a pixel, when the render is done
    int bounces = 4;                 ///< the most times light bounces between the camera and the sky
    bool denoise = true;
    float fstop = 0.0f;              ///< the lens's f-number: 2.8 a shallow focus, 0 everything sharp
    float focus = 0.0f;              ///< m from the camera to what is sharp; 0: what is in the middle of the picture
    float clamp = 20.0f;             ///< the most a bounce adds to a pixel: no fireflies
    float sunAngle = 0.53f;          ///< degrees across the sun: larger, softer shadows
    uint32_t seed = 0;

    bool operator==(const Settings&) const = default;
};

/// A picture in linear light: `channels` floats a pixel, top row first.
struct Image {
    int width = 0, height = 0, channels = 3;
    std::vector<float> pixels;
};

class PathTracer {
public:
    /// Starts again, on `scene`.
    void setScene(std::shared_ptr<const Scene> scene);
    /// Starts again, when they change the picture.
    void setSettings(const Settings& settings);
    const Settings& settings() const { return settings_; }
    const std::shared_ptr<const Scene>& scene() const { return scene_; }
    void restart();

    /// A sample more in every pixel, on all the threads. False -- and no
    /// sample added -- without a scene, or when `stop` is set on the way.
    bool pass(const std::atomic<bool>* stop = nullptr);
    int samples() const;
    bool done() const { return samples() >= settings_.samples; }
    /// How far the lens focuses: Settings::focus, else what the middle of
    /// the picture sees.
    float focusDistance() const { return focus_; }

    /// The average of the samples, linear light.
    Image beauty() const;
    /// ... with the noise taken out: by Open Image Denoise, else our own
    /// filter (defaultDenoiser()).
    Image denoised() const;
    /// What the first surface each pixel sees is: its colour (1 for the sky),
    /// its normal, how far it is along the view (infinity for the sky).
    Image albedo() const;
    Image normal() const;
    Image depth() const;
    /// As a screen shows it: exposure, the tone curve, gamma -- RGBA.
    std::vector<uint8_t> display(bool denoise) const;

    double seconds() const;   ///< spent rendering
    uint64_t paths() const;   ///< followed so far

private:
    Image average(const std::vector<float>& sum, int channels) const;

    std::shared_ptr<const Scene> scene_;
    Settings settings_;
    float focus_ = 10.0f;
    mutable std::mutex mutex_;  // the sums, as a pass adds to them and a picture is taken of them
    int samples_ = 0;
    std::vector<float> sum_, square_, albedo_, normal_, depth_;
    // One pass's samples, before they are added.
    std::vector<float> passColor_, passAlbedo_, passNormal_, passDepth_;
    /// What each pixel sees of the gas, without noise -- the share of what
    /// is behind that shows, how far the gas is, the way the eye looks --
    /// made with the first pass; empty without gas.
    std::vector<float> gasSeen_;
    double seconds_ = 0.0;
    uint64_t paths_ = 0;
};

/// Linear light as a screen shows it, as the viewport's: times `exposure`,
/// ACES (Narkowicz's fit), gamma 2.2; RGBA, alpha 255.
std::vector<uint8_t> toDisplay(const Image& image, float exposure);

/// `beauty` with its noise taken out: an edge-avoiding a-trous wavelet
/// filter (Dammertz et al.) over the light each surface gets -- its colour
/// divided out, put back after -- stopping at edges of colour, normal and
/// depth, and where the pixels differ by more than their noise
/// (`variance`: of each pixel's mean luminance).
Image denoise(const Image& beauty, const Image& albedo, const Image& normal, const Image& depth,
              const std::vector<float>& variance);

}  // namespace pg::render
