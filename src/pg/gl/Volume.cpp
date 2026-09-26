#include "pg/gl/Volume.h"

#include "pg/sim/Pyro.h"

#include <algorithm>
#include <cmath>

namespace pg::gl {
namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kFovY = 35.0f;  // degrees, as the shader preview

const char* kVertex = R"(#version 330 core
out vec2 v_ndc;
void main() {
    // One triangle that covers the screen.
    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2) * 2.0 - 1.0;
    v_ndc = p;
    gl_Position = vec4(p, 0.0, 1.0);
}
)";

const char* kFragment = R"(#version 330 core
in vec2 v_ndc;
out vec4 o_color;

uniform vec3 u_eye, u_right, u_up, u_forward;
uniform vec2 u_tanHalfFov;
uniform vec3 u_boxMin, u_boxSize;
uniform sampler3D u_fields;    // r: smoke, g: temperature, b: flame
uniform sampler3D u_sunlight;  // r: share of the light that gets through to here
uniform vec3 u_lightDir;       // towards the light
uniform vec3 u_light;          // its colour x intensity
uniform vec3 u_sky;
uniform vec3 u_albedo;
uniform float u_extinction;    // per unit of smoke per world unit
uniform float u_occlusion;
uniform float u_flame, u_flameStart, u_flameRange, u_fireLight;
uniform float u_exposure;
uniform float u_step;          // world units
uniform vec3 u_texel;          // one cell, in texture coordinates
uniform vec3 u_backgroundTop, u_backgroundBottom;

// The colour of a black body at `kelvin`, normalised: Tanner Helland's fit to
// the CIE data, in sRGB, made linear.
vec3 blackbody(float kelvin) {
    float t = kelvin / 100.0;
    vec3 c;
    c.r = t <= 66.0 ? 1.0 : 1.29293618606 * pow(t - 60.0, -0.1332047592);
    c.g = t <= 66.0 ? 0.39008157876 * log(t) - 0.63184144378 : 1.12989086089 * pow(t - 60.0, -0.0755148492);
    c.b = t >= 66.0 ? 1.0 : (t <= 19.0 ? 0.0 : 0.54320678911 * log(t - 10.0) - 1.19625408914);
    return pow(clamp(c, 0.0, 1.0), vec3(2.2));
}

// Light a flame gives off at temperature `heat`, relative to the hottest: a
// black body from 1000 K to 3000 K, its power going with T^4.
vec3 glowAt(float heat) {
    float x = clamp((heat - u_flameStart) / u_flameRange, 0.0, 1.0);
    float kelvin = mix(1000.0, 3000.0, x);
    float k = kelvin / 3000.0;
    return blackbody(kelvin) * (k * k * k * k) * smoothstep(0.0, 0.1, x);
}

// Henyey-Greenstein, times 4 pi: 1 for light scattered evenly in all directions.
float henyeyGreenstein(float cosTheta, float g) {
    float g2 = g * g;
    return (1.0 - g2) / pow(1.0 + g2 - 2.0 * g * cosTheta, 1.5);
}

// Noise for jittering the samples: interleaved gradient noise (Jimenez).
// Neighbouring pixels get values far apart, so the jitter shows as a fine,
// even grain rather than white noise's clumps.
float ign(vec2 p) {
    return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

// Sample i of a ray: a different pattern for each step.
vec3 jitter(float i) {
    vec2 p = gl_FragCoord.xy + i * vec2(5.588238);
    return vec3(ign(p), ign(p + vec2(47.0, 17.0)), ign(p + vec2(23.0, 71.0))) - 0.5;
}

vec3 toneMap(vec3 x) {  // ACES, Narkowicz's fit
    return clamp(x * (2.51 * x + 0.03) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}

void main() {
    vec3 dir = normalize(u_forward + v_ndc.x * u_tanHalfFov.x * u_right + v_ndc.y * u_tanHalfFov.y * u_up);
    vec3 background = mix(u_backgroundBottom, u_backgroundTop, clamp(v_ndc.y * 0.5 + 0.5, 0.0, 1.0));

    // Where the ray is inside the box.
    vec3 safe = mix(vec3(1e-6), dir, greaterThan(abs(dir), vec3(1e-6)));
    vec3 ta = (u_boxMin - u_eye) / safe, tb = (u_boxMin + u_boxSize - u_eye) / safe;
    vec3 near = min(ta, tb), far = max(ta, tb);
    float tNear = max(max(near.x, near.y), max(near.z, 0.0));
    float tFar = min(min(far.x, far.y), far.z);

    vec3 radiance = vec3(0.0);
    float transmittance = 1.0;
    float cosTheta = dot(dir, u_lightDir);
    // Mostly forwards, a little back.
    float phase = mix(henyeyGreenstein(cosTheta, 0.55), henyeyGreenstein(cosTheta, -0.25), 0.3);
    // Where along its first step each ray starts: at the same depth for
    // every pixel, the steps would show as bands.
    float start = tNear + u_step * ign(gl_FragCoord.xy);
    int steps = int(clamp(ceil((tFar - start) / u_step), 0.0, 2048.0));
    for (int i = 0; i < steps; ++i) {
        vec3 uvw = (u_eye + dir * (start + float(i) * u_step) - u_boxMin) / u_boxSize;
        // Each sample moved by up to half a cell: a ray running along a layer
        // of cells would see the interpolation between them as stripes; this
        // way it is fine noise that anti-aliasing averages away.
        vec3 here = textureLod(u_fields, uvw + jitter(float(i)) * u_texel, 0.0).rgb;
        // Faded out towards the open faces, the sides and the top: the gas
        // goes on past them, and a hard cut would look like a lid.
        float side = min(min(uvw.x, 1.0 - uvw.x), min(uvw.z, 1.0 - uvw.z)) / 0.12;
        float fade = smoothstep(0.0, 1.0, min(side, (1.0 - uvw.y) / 0.2));
        float smoke = max(here.r, 0.0) * fade, heat = here.g, flame = max(here.b, 0.0) * fade;
        if (smoke < 1e-4 && flame < 1e-4) continue;

        // In the flame the soot glows -- it is what makes the flame yellow --
        // and hides less of what is behind it than when it has cooled.
        float sigma = u_extinction * smoke / (1.0 + 4.0 * flame);
        vec3 emitted = u_flame * (1.0 - exp(-4.0 * flame)) * glowAt(heat);
        // What the smoke here scatters towards the eye. `around` averages the
        // fields over a few cells: thick smoke nearby hides the sky, fire
        // nearby lights it.
        vec3 around = textureLod(u_fields, uvw, 2.0).rgb;
        float sun = textureLod(u_sunlight, uvw, 0.0).r;
        vec3 lit = u_light * sun * phase + u_sky * exp(-u_occlusion * around.r) +
                   u_fireLight * (1.0 - exp(-4.0 * around.b)) * glowAt(around.g);
        vec3 source = u_albedo * lit * sigma + emitted;
        // The step integrated exactly: what it adds is dimmed by itself too.
        float a = exp(-sigma * u_step);
        radiance += transmittance * (sigma > 1e-4 ? source * (1.0 - a) / sigma : source * u_step);
        transmittance *= a;
        if (transmittance < 0.004) break;
    }
    vec3 colour = toneMap(radiance * u_exposure + transmittance * background);
    o_color = vec4(pow(colour, vec3(1.0 / 2.2)), 1.0);
}
)";

void normalize3(const float in[3], float out[3]) {
    const float l = std::sqrt(in[0] * in[0] + in[1] * in[1] + in[2] * in[2]);
    for (int i = 0; i < 3; ++i) out[i] = l > 0.0f ? in[i] / l : (i == 1 ? 1.0f : 0.0f);
}

/// Creates a 3D texture if needed and (re)fills it from `data`.
void fill3D(const Api& gl, GLuint& texture, int size[3], int nx, int ny, int nz, GLint internal, GLenum format,
            const float* data, bool mipmaps) {
    if (!texture) gl.GenTextures(1, &texture);
    gl.ActiveTexture(TEXTURE0);
    gl.BindTexture(TEXTURE_3D, texture);
    gl.PixelStorei(UNPACK_ALIGNMENT, 4);
    if (size[0] != nx || size[1] != ny || size[2] != nz) {
        gl.TexImage3D(TEXTURE_3D, 0, internal, nx, ny, nz, 0, format, FLOAT, data);
        gl.TexParameteri(TEXTURE_3D, TEXTURE_MIN_FILTER, mipmaps ? LINEAR_MIPMAP_LINEAR : LINEAR);
        gl.TexParameteri(TEXTURE_3D, TEXTURE_MAG_FILTER, LINEAR);
        gl.TexParameteri(TEXTURE_3D, TEXTURE_WRAP_S, CLAMP_TO_EDGE);
        gl.TexParameteri(TEXTURE_3D, TEXTURE_WRAP_T, CLAMP_TO_EDGE);
        gl.TexParameteri(TEXTURE_3D, TEXTURE_WRAP_R, CLAMP_TO_EDGE);
        size[0] = nx;
        size[1] = ny;
        size[2] = nz;
    } else {
        gl.TexSubImage3D(TEXTURE_3D, 0, 0, 0, 0, nx, ny, nz, format, FLOAT, data);
    }
    if (mipmaps) gl.GenerateMipmap(TEXTURE_3D);
    gl.BindTexture(TEXTURE_3D, 0);
}

}  // namespace

VolumeStyle VolumeStyle::fire() {
    VolumeStyle s;
    s.smokeDensity = 24.0f;
    s.smokeColor[0] = 0.20f;
    s.smokeColor[1] = 0.19f;
    s.smokeColor[2] = 0.18f;
    s.flameIntensity = 30.0f;
    s.fireLight = 3.0f;
    return s;
}

VolumeStyle VolumeStyle::smoke() { return VolumeStyle{}; }

const std::vector<VolumeParam>& volumeParams() {
    static const std::vector<VolumeParam> params = {
        {"smokeDensity", "Smoke density", &VolumeStyle::smokeDensity, 0.0f, 100.0f,
         "How much light the smoke stops: thin haze to thick soot."},
        {"lightIntensity", "Light", &VolumeStyle::lightIntensity, 0.0f, 10.0f, "Brightness of the sun."},
        {"skyIntensity", "Sky", &VolumeStyle::skyIntensity, 0.0f, 2.0f, "Brightness of the light from the sky."},
        {"occlusion", "Occlusion", &VolumeStyle::occlusion, 0.0f, 20.0f,
         "How much thick smoke around darkens the light of the sky."},
        {"flameIntensity", "Flame", &VolumeStyle::flameIntensity, 0.0f, 100.0f, "Brightness of the fire."},
        {"flameStart", "Flame start", &VolumeStyle::flameStart, 0.0f, 5.0f,
         "Temperature where the gas starts to glow."},
        {"flameRange", "Flame range", &VolumeStyle::flameRange, 0.1f, 20.0f,
         "How much hotter than that it glows yellow-white."},
        {"fireLight", "Fire light", &VolumeStyle::fireLight, 0.0f, 10.0f, "How much the fire lights the smoke."},
        {"exposure", "Exposure", &VolumeStyle::exposure, 0.05f, 8.0f, "Brightness of the whole image."},
    };
    return params;
}

VolumeRenderer::VolumeRenderer(const Api& gl) : gl_(gl) { gl_.GenVertexArrays(1, &vao_); }

VolumeRenderer::~VolumeRenderer() {
    if (program_) gl_.DeleteProgram(program_);
    gl_.DeleteVertexArrays(1, &vao_);
    if (fields_) gl_.DeleteTextures(1, &fields_);
    if (light_) gl_.DeleteTextures(1, &light_);
    if (fbo_) {
        gl_.DeleteFramebuffers(1, &fbo_);
        gl_.DeleteTextures(1, &colorTex_);
    }
}

bool VolumeRenderer::init(std::string& log) {
    const GLuint p = buildProgram(gl_, kVertex, kFragment, log);
    if (!p) return false;
    if (program_) gl_.DeleteProgram(program_);
    program_ = p;
    return true;
}

GLint VolumeRenderer::location(const char* name) const { return gl_.GetUniformLocation(program_, name); }

void VolumeRenderer::upload(const sim::Grid& density, const sim::Grid& temperature, const sim::Grid& flame) {
    upload(density, temperature, flame, shadows(density, style, lightDirection));
}

void VolumeRenderer::upload(const sim::Grid& density, const sim::Grid& temperature, const sim::Grid& flame,
                            const sim::Grid& shadows) {
    const int nx = density.nx(), ny = density.ny(), nz = density.nz();
    if (nx == 0 || temperature.size() != density.size() || flame.size() != density.size() || shadows.size() == 0) {
        return;
    }
    staging_.resize(3 * density.size());
    for (size_t i = 0; i < density.size(); ++i) {
        staging_[3 * i] = density.data()[i];
        staging_[3 * i + 1] = temperature.data()[i];
        staging_[3 * i + 2] = flame.data()[i];
    }
    fill3D(gl_, fields_, size_, nx, ny, nz, static_cast<GLint>(RGB16F), RGB, staging_.data(), true);
    fill3D(gl_, light_, lightSize_, shadows.nx(), shadows.ny(), shadows.nz(), static_cast<GLint>(R16F), RED,
           shadows.data(), false);
}

sim::Grid VolumeRenderer::shadows(const sim::Grid& density, const VolumeStyle& style, const float lightDirection[3]) {
    float towards[3];
    normalize3(lightDirection, towards);
    const float perCell = style.smokeDensity / static_cast<float>(std::max(density.nx(), 1));
    return sim::lightTransmittance(density, towards, perCell, 2);
}

void VolumeRenderer::ensureTarget(int width, int height) {
    if (fbo_ && width == width_ && height == height_) return;
    if (!fbo_) {
        gl_.GenFramebuffers(1, &fbo_);
        gl_.GenTextures(1, &colorTex_);
    }
    width_ = width;
    height_ = height;
    gl_.BindTexture(TEXTURE_2D, colorTex_);
    gl_.TexImage2D(TEXTURE_2D, 0, static_cast<GLint>(RGBA8), width, height, 0, RGBA, UNSIGNED_BYTE, nullptr);
    gl_.TexParameteri(TEXTURE_2D, TEXTURE_MIN_FILTER, LINEAR);
    gl_.TexParameteri(TEXTURE_2D, TEXTURE_MAG_FILTER, LINEAR);
    gl_.BindTexture(TEXTURE_2D, 0);
    gl_.BindFramebuffer(FRAMEBUFFER, fbo_);
    gl_.FramebufferTexture2D(FRAMEBUFFER, COLOR_ATTACHMENT0, TEXTURE_2D, colorTex_, 0);
    gl_.BindFramebuffer(FRAMEBUFFER, 0);
}

void VolumeRenderer::render(int width, int height) {
    ensureTarget(width, height);
    gl_.BindFramebuffer(FRAMEBUFFER, fbo_);
    gl_.Viewport(0, 0, width, height);
    gl_.Disable(DEPTH_TEST);
    gl_.Disable(BLEND);
    gl_.ColorMask(1, 1, 1, 1);
    gl_.ClearColor(0.02f, 0.02f, 0.025f, 1.0f);
    gl_.Clear(COLOR_BUFFER_BIT);
    if (!program_ || !fields_) {
        gl_.BindFramebuffer(FRAMEBUFFER, 0);
        return;
    }

    // The camera: where it is, and the directions of the screen's axes.
    float eye[3], forward[3], right[3], up[3];
    orbit.eye(eye);
    const float back[3] = {-eye[0], -eye[1], -eye[2]};
    normalize3(back, forward);
    const float side[3] = {-forward[2], 0.0f, forward[0]};  // forward x (0, 1, 0)
    normalize3(side, right);
    up[0] = right[1] * forward[2] - right[2] * forward[1];
    up[1] = right[2] * forward[0] - right[0] * forward[2];
    up[2] = right[0] * forward[1] - right[1] * forward[0];
    const float tanHalf = std::tan(kFovY * kPi / 360.0f);
    const float aspect = static_cast<float>(width) / static_cast<float>(height);

    // The domain: one unit wide, centred on the origin.
    const float sx = 1.0f, sy = static_cast<float>(size_[1]) / static_cast<float>(size_[0]),
                sz = static_cast<float>(size_[2]) / static_cast<float>(size_[0]);
    float towards[3];
    normalize3(lightDirection, towards);
    const VolumeStyle& s = style;

    gl_.UseProgram(program_);
    gl_.Uniform3f(location("u_eye"), eye[0], eye[1], eye[2]);
    gl_.Uniform3f(location("u_right"), right[0], right[1], right[2]);
    gl_.Uniform3f(location("u_up"), up[0], up[1], up[2]);
    gl_.Uniform3f(location("u_forward"), forward[0], forward[1], forward[2]);
    gl_.Uniform2f(location("u_tanHalfFov"), tanHalf * aspect, tanHalf);
    gl_.Uniform3f(location("u_boxMin"), -0.5f * sx, -0.5f * sy, -0.5f * sz);
    gl_.Uniform3f(location("u_boxSize"), sx, sy, sz);
    gl_.Uniform3f(location("u_lightDir"), towards[0], towards[1], towards[2]);
    gl_.Uniform3f(location("u_light"), s.lightColor[0] * s.lightIntensity, s.lightColor[1] * s.lightIntensity,
                  s.lightColor[2] * s.lightIntensity);
    gl_.Uniform3f(location("u_sky"), s.skyColor[0] * s.skyIntensity, s.skyColor[1] * s.skyIntensity,
                  s.skyColor[2] * s.skyIntensity);
    gl_.Uniform3f(location("u_albedo"), s.smokeColor[0], s.smokeColor[1], s.smokeColor[2]);
    gl_.Uniform1f(location("u_extinction"), s.smokeDensity);
    gl_.Uniform1f(location("u_occlusion"), s.occlusion);
    gl_.Uniform1f(location("u_flame"), s.flameIntensity);
    gl_.Uniform1f(location("u_flameStart"), s.flameStart);
    gl_.Uniform1f(location("u_flameRange"), std::max(s.flameRange, 1e-3f));
    gl_.Uniform1f(location("u_fireLight"), s.fireLight);
    gl_.Uniform1f(location("u_exposure"), s.exposure);
    gl_.Uniform1f(location("u_step"), 0.6f / static_cast<float>(size_[0]));
    gl_.Uniform3f(location("u_texel"), 1.0f / static_cast<float>(size_[0]), 1.0f / static_cast<float>(size_[1]),
                  1.0f / static_cast<float>(size_[2]));
    gl_.Uniform3f(location("u_backgroundTop"), 0.055f, 0.06f, 0.07f);
    gl_.Uniform3f(location("u_backgroundBottom"), 0.012f, 0.012f, 0.015f);

    gl_.ActiveTexture(TEXTURE0);
    gl_.BindTexture(TEXTURE_3D, fields_);
    gl_.Uniform1i(location("u_fields"), 0);
    gl_.ActiveTexture(TEXTURE1);
    gl_.BindTexture(TEXTURE_3D, light_);
    gl_.Uniform1i(location("u_sunlight"), 1);

    gl_.BindVertexArray(vao_);
    gl_.DrawArrays(TRIANGLES, 0, 3);
    gl_.BindVertexArray(0);

    gl_.BindTexture(TEXTURE_3D, 0);
    gl_.ActiveTexture(TEXTURE0);
    gl_.BindTexture(TEXTURE_3D, 0);
    gl_.UseProgram(0);
    gl_.BindFramebuffer(FRAMEBUFFER, 0);
}

std::vector<uint8_t> VolumeRenderer::readPixels(int factor) const {
    return readRgb(gl_, fbo_, width_, height_, factor);
}

}  // namespace pg::gl
