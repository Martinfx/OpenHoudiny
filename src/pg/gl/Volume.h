#pragma once
//
// Draws a simulated gas: smoke that absorbs and scatters light, and fire that
// glows. The viewport of the Pyro workspace and of `pgshader pyro`.
//
// The fields go to 3D textures: smoke, temperature and flame in one, the
// light that reaches each cell in another (sim::lightTransmittance, on the
// CPU: the shadows the smoke casts on itself). A fragment shader marches the ray
// behind each pixel through the box, front to back:
//
//   smoke  absorbs what is behind it and scatters light towards the eye:
//          sunlight where it is not in shadow -- mostly forwards, so smoke
//          glows at the rim against the light -- sky light, dimmer where the
//          smoke around is thick, and the glow of the fire nearby;
//   fire   emits where the flame is, like a black body at the temperature
//          there: deep red, orange, then yellow-white as it heats up, and far
//          brighter (Stefan-Boltzmann: power goes with T^4).
//
// Then a filmic tone curve. Needs a current OpenGL 3.3 core context.
//
#include "pg/gl/Camera.h"
#include "pg/gl/Gl.h"
#include "pg/sim/Grid.h"

#include <string>
#include <vector>

namespace pg::gl {

struct VolumeStyle {
    float smokeDensity = 20.0f;   ///< extinction per unit of smoke over one domain width
    float smokeColor[3] = {0.75f, 0.75f, 0.77f};  ///< share of the light smoke scatters
    float lightColor[3] = {1.0f, 0.95f, 0.88f};
    float lightIntensity = 2.2f;
    float skyColor[3] = {0.55f, 0.65f, 0.8f};
    float skyIntensity = 0.25f;
    float occlusion = 3.0f;       ///< how much thick smoke around darkens the sky light
    float flameIntensity = 30.0f; ///< light the flames give off per domain width, at their hottest
    float flameStart = 0.3f;      ///< temperature where flames start to glow
    float flameRange = 4.0f;      ///< ... and how much hotter they glow white
    float fireLight = 2.0f;       ///< how much the fire lights the smoke around it
    float exposure = 1.0f;

    /// Dark soot over a bright fire.
    static VolumeStyle fire();
    /// Pale smoke in sunlight.
    static VolumeStyle smoke();
};

/// The numbers of a VolumeStyle by name: what the editor shows as sliders and
/// `pgshader pyro --set name=value` changes.
struct VolumeParam {
    const char* name;
    const char* label;
    float VolumeStyle::*member;
    float min, max;
    const char* help;
};
const std::vector<VolumeParam>& volumeParams();

class VolumeRenderer {
public:
    explicit VolumeRenderer(const Api& gl);
    ~VolumeRenderer();
    VolumeRenderer(const VolumeRenderer&) = delete;
    VolumeRenderer& operator=(const VolumeRenderer&) = delete;

    /// Compiles the shader. False, with the driver's message in `log`, if not.
    bool init(std::string& log);

    /// The fields to draw: cell-centred, all the same size, the domain one
    /// unit wide. Works out the light reaching each cell -- upload again after
    /// the light or the smoke density of the style change.
    void upload(const sim::Grid& density, const sim::Grid& temperature, const sim::Grid& flame);
    /// The same, with the shadows worked out already.
    void upload(const sim::Grid& density, const sim::Grid& temperature, const sim::Grid& flame,
                const sim::Grid& shadows);
    /// The light that reaches each cell through the smoke, at half the
    /// resolution: the shadows it casts on itself. Plain CPU work, no GL --
    /// a caller with a thread of its own can do it there.
    static sim::Grid shadows(const sim::Grid& density, const VolumeStyle& style, const float lightDirection[3]);

    /// Draws into the offscreen framebuffer at `width` x `height` pixels.
    void render(int width, int height);

    GLuint colorTexture() const { return colorTex_; }
    int width() const { return width_; }
    int height() const { return height_; }
    /// The last frame as RGB rows, top to bottom, averaged down by `factor`.
    std::vector<uint8_t> readPixels(int factor = 1) const;

    /// Where the camera starts: a little above the source, the whole domain in view.
    static Orbit defaultOrbit() { return Orbit{35.0f, 8.0f, 3.2f}; }

    Orbit orbit = defaultOrbit();
    float lightDirection[3] = {-0.75f, 0.6f, 0.15f};  ///< towards the light
    VolumeStyle style;

private:
    void ensureTarget(int width, int height);
    GLint location(const char* name) const;

    const Api& gl_;
    GLuint program_ = 0, vao_ = 0;
    GLuint fields_ = 0, light_ = 0;  // 3D textures
    int size_[3] = {0, 0, 0}, lightSize_[3] = {0, 0, 0};
    GLuint fbo_ = 0, colorTex_ = 0;
    int width_ = 0, height_ = 0;
    std::vector<float> staging_;
};

}  // namespace pg::gl
