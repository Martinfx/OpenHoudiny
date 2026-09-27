#include "pg/gl/Volume.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace pg::gl {
namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kFovY = VolumeRenderer::kFovY;
constexpr float kNear = 0.02f, kFar = 500.0f;

const char* kFullScreen = R"(#version 330 core
out vec2 v_ndc;
void main() {
    // One triangle that covers the screen.
    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2) * 2.0 - 1.0;
    v_ndc = p;
    gl_Position = vec4(p, 0.0, 1.0);
}
)";

// What both passes need: the box, the solids, the fade at the open faces.
const char* kCommon = R"(
uniform vec3 u_boxMin, u_boxSize;
uniform vec3 u_texel;          // one cell, in texture coordinates
// The solids: each a shape placed in the world (pg/sim/Shape.h), packed in
// fours -- centre and shape; its own axes, each with half its size along it;
// colour and highlight; a torus's ring and tube.
uniform int u_solidCount;
uniform vec4 u_solidA[16];     // centre, shape (0 sphere, 1 box, 2 cylinder, 3 cone, 4 torus)
uniform vec4 u_solidB[16];     // its x axis in the world, half its size along it
uniform vec4 u_solidC[16];     // y
uniform vec4 u_solidD[16];     // z
uniform vec4 u_solidE[16];     // colour, highlight: 0 none, 1 hovered, 2 selected
uniform vec4 u_solidF[16];     // a torus's ring and tube radius

vec3 safeDir(vec3 d) { return mix(vec3(1e-6), d, greaterThan(abs(d), vec3(1e-6))); }

// Where a ray is inside a box: from x to y; x > y when it misses.
vec2 boxSpan(vec3 o, vec3 d, vec3 lo, vec3 hi) {
    vec3 inv = 1.0 / safeDir(d);
    vec3 ta = (lo - o) * inv, tb = (hi - o) * inv;
    vec3 n = min(ta, tb), f = max(ta, tb);
    return vec2(max(max(n.x, n.y), n.z), min(min(f.x, f.y), f.z));
}

// The nearer root of a t^2 + 2 b t + c = 0 at tMin or later; 1e30 if none.
float firstRoot(float a, float b, float c, float tMin) {
    float disc = b * b - a * c;
    if (disc < 0.0 || abs(a) < 1e-12) return 1e30;
    float s = sqrt(disc);
    float t0 = (-b - s) / a, t1 = (-b + s) / a;
    float lo = min(t0, t1), hi = max(t0, t1);
    return lo >= tMin ? lo : (hi >= tMin ? hi : 1e30);
}

// Where the ray first meets solid i at tMin or later, with the normal there;
// 1e30 if it does not. In the solid's unit space -- where it fills [-1, 1]
// along its own axes -- the shapes are simple, and t stays the same.
float hitShape(int i, vec3 o, vec3 d, float tMin, out vec3 normal) {
    normal = vec3(0.0, 1.0, 0.0);
    vec3 ax = u_solidB[i].xyz, ay = u_solidC[i].xyz, az = u_solidD[i].xyz;
    vec3 h = vec3(u_solidB[i].w, u_solidC[i].w, u_solidD[i].w);
    vec3 oc = o - u_solidA[i].xyz;
    vec3 lo = vec3(dot(ax, oc), dot(ay, oc), dot(az, oc));
    vec3 ld = vec3(dot(ax, d), dot(ay, d), dot(az, d));
    int shape = int(u_solidA[i].w + 0.5);
    if (shape == 4) {
        // A torus: sphere tracing its exact distance, z squeezed to x's scale.
        float k = h.x / h.z;
        vec3 to = vec3(lo.x, lo.y, lo.z * k), td = vec3(ld.x, ld.y, ld.z * k);
        float speed = length(td);
        float ring = u_solidF[i].x, tube = u_solidF[i].y;
        vec2 span = boxSpan(to, td, -vec3(h.x, tube, h.x), vec3(h.x, tube, h.x));
        if (span.x > span.y || span.y < tMin || speed < 1e-12) return 1e30;
        float t = max(span.x, tMin);
        for (int s = 0; s < 96; ++s) {
            vec3 p = to + td * t;
            float r = length(p.xz);
            float dist = length(vec2(r - ring, p.y)) - tube;
            if (dist < 1e-4 * h.x) {
                float a = 1.0 - ring / max(r, 1e-6);
                vec3 g = vec3(p.x * a, p.y, p.z * a * k);
                normal = normalize(ax * g.x + ay * g.y + az * g.z);
                return t;
            }
            t += dist / speed;
            if (t > span.y) break;
        }
        return 1e30;
    }
    vec3 uo = lo / h, ud = ld / h;
    float best = 1e30;
    vec3 un = vec3(0.0, 1.0, 0.0);
    if (shape == 0) {
        best = firstRoot(dot(ud, ud), dot(uo, ud), dot(uo, uo) - 1.0, tMin);
        un = uo + ud * best;
    } else if (shape == 1) {
        vec2 span = boxSpan(uo, ud, vec3(-1.0), vec3(1.0));
        if (span.x <= span.y) {
            best = span.x >= tMin ? span.x : (span.y >= tMin ? span.y : 1e30);
            vec3 p = uo + ud * best, a = abs(p);
            un = a.x > a.y && a.x > a.z ? vec3(sign(p.x), 0.0, 0.0)
               : a.y > a.z             ? vec3(0.0, sign(p.y), 0.0)
                                       : vec3(0.0, 0.0, sign(p.z));
        }
    } else if (shape == 2 || shape == 3) {
        // The side: x^2 + z^2 = 1 for a cylinder; = (1 - y)^2 / 4 for a cone,
        // its base at y = -1 and its apex at y = 1.
        float k2 = shape == 2 ? 0.0 : 0.25;
        float w = 1.0 - uo.y, dw = -ud.y;
        float a = ud.x * ud.x + ud.z * ud.z - k2 * dw * dw;
        float b = uo.x * ud.x + uo.z * ud.z - k2 * w * dw;
        float c = uo.x * uo.x + uo.z * uo.z - (shape == 2 ? 1.0 : k2 * w * w);
        float disc = b * b - a * c;
        if (abs(a) > 1e-12 && disc >= 0.0) {
            for (int r = 0; r < 2; ++r) {
                float t = (-b + (r == 0 ? -1.0 : 1.0) * sqrt(disc)) / a;
                vec3 p = uo + ud * t;
                if (t >= tMin && t < best && abs(p.y) <= 1.0) {
                    best = t;
                    un = vec3(p.x, shape == 2 ? 0.0 : k2 * (1.0 - p.y), p.z);
                }
            }
        }
        // The caps: both for a cylinder, the base for a cone.
        if (abs(ud.y) > 1e-12) {
            for (int e = 0; e < 2; ++e) {
                float y = e == 0 ? -1.0 : 1.0;
                if (shape == 3 && e == 1) break;
                float t = (y - uo.y) / ud.y;
                vec3 p = uo + ud * t;
                if (t >= tMin && t < best && dot(p.xz, p.xz) <= 1.0) {
                    best = t;
                    un = vec3(0.0, y, 0.0);
                }
            }
        }
    }
    if (best >= 1e29) return 1e30;
    vec3 n = un / h;  // back through the inverse transpose
    normal = normalize(ax * n.x + ay * n.y + az * n.z);
    return best;
}

// The nearest solid the ray from o along d meets at tMin or later: how far,
// the normal there and which solid. 1e30 when it meets none.
float hitSolid(vec3 o, vec3 d, float tMin, out vec3 normal, out int which) {
    float best = 1e30;
    normal = vec3(0.0, 1.0, 0.0);
    which = -1;
    for (int i = 0; i < u_solidCount; ++i) {
        vec3 n;
        float t = hitShape(i, o, d, tMin, n);
        if (t < best) {
            best = t;
            normal = n;
            which = i;
        }
    }
    return best;
}

uniform float u_flameStart, u_flameRange;

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

// The gas fades out over the last cells before the open faces -- the sides
// and the top: it goes on past them, and a hard cut would look like a wall.
float fadeAt(vec3 uvw) {
    vec3 cells = min(uvw, 1.0 - uvw) / u_texel;
    float side = min(cells.x, cells.z) / 6.0, top = (1.0 - uvw.y) / u_texel.y / 10.0;
    return smoothstep(0.0, 1.0, min(side, top));
}
)";

const char* kShadowFragment = R"(
out vec4 o_light;
uniform sampler3D u_fields;
uniform vec3 u_lightDir;       // towards the light
uniform vec3 u_size;           // cells of the light texture
uniform float u_layer;         // the slice drawn
uniform float u_extinction;    // per unit of smoke per world unit
uniform float u_step;          // world units

void main() {
    vec3 uvw = vec3(gl_FragCoord.xy, u_layer + 0.5) / u_size;
    vec3 p = u_boxMin + uvw * u_boxSize;
    vec3 n;
    int which;
    if (hitSolid(p, u_lightDir, 0.0, n, which) < 1e29) {
        o_light = vec4(0.0);
        return;
    }
    // Through the smoke towards the light, half a step first: a cell does
    // not shade itself.
    float far = boxSpan(p, u_lightDir, u_boxMin, u_boxMin + u_boxSize).y;
    float depth = 0.0;
    for (float t = 0.5 * u_step; t < far; t += u_step) {
        vec3 q = (p + u_lightDir * t - u_boxMin) / u_boxSize;
        depth += textureLod(u_fields, q, 1.0).r * fadeAt(q);
    }
    o_light = vec4(exp(-u_extinction * depth * u_step));
}
)";

// The light the fire gives off, summed over blocks of cells: each block of
// the glow texture is a lamp that lights the floor and the colliders.
const char* kGlowFragment = R"(
out vec4 o_glow;
uniform sampler3D u_fields;
uniform ivec3 u_cells;      // of the gas
uniform ivec3 u_block;      // gas cells in a block, along each axis
uniform int u_layer;
uniform float u_flame;
uniform float u_cellVolume; // world units

void main() {
    ivec3 lo = ivec3(ivec2(gl_FragCoord.xy), u_layer) * u_block;
    ivec3 hi = min(lo + u_block, u_cells);
    vec3 sum = vec3(0.0);
    for (int k = lo.z; k < hi.z; ++k) {
        for (int j = lo.y; j < hi.y; ++j) {
            for (int i = lo.x; i < hi.x; ++i) {
                vec3 f = texelFetch(u_fields, ivec3(i, j, k), 0).rgb;
                if (f.b > 1e-4) sum += (1.0 - exp(-4.0 * f.b)) * glowAt(f.g);
            }
        }
    }
    o_glow = vec4(sum * (u_flame * u_cellVolume), 1.0);
}
)";

const char* kViewFragment = R"(
in vec2 v_ndc;
out vec4 o_color;

uniform vec3 u_eye, u_right, u_up, u_forward;
uniform vec2 u_tanHalfFov;
uniform mat4 u_viewProj;
uniform bool u_hasGas;
uniform sampler3D u_fields;    // r: smoke, g: temperature, b: flame
uniform sampler3D u_sunlight;  // r: share of the sunlight that gets through to here
uniform vec3 u_lightDir;       // towards the light
uniform vec3 u_light;          // its colour x intensity
uniform vec3 u_sky;
uniform vec3 u_albedo;
uniform float u_extinction;    // per unit of smoke per world unit
uniform float u_occlusion;
uniform float u_flame, u_fireLight;
uniform float u_exposure;
uniform float u_step;          // world units
uniform bool u_floor;
uniform vec3 u_floorCenter;    // where the floor fades out from
uniform float u_floorRadius;
uniform sampler3D u_glow;      // the light of the fire, a lamp for each block of cells
uniform ivec3 u_glowDims;      // blocks
uniform vec3 u_glowCell;       // the size of one, world units
uniform vec3 u_backgroundTop, u_backgroundBottom;

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

// Sunlight at a point of a solid: blocked by the solids, dimmed by the smoke.
float sunAt(vec3 p) {
    vec3 n;
    int which;
    if (hitSolid(p, u_lightDir, 1e-3, n, which) < 1e29) return 0.0;
    if (!u_hasGas) return 1.0;
    vec2 span = boxSpan(p, u_lightDir, u_boxMin, u_boxMin + u_boxSize);
    float t0 = max(span.x, 0.0), t1 = span.y;
    if (t1 <= t0) return 1.0;
    float dt = max((t1 - t0) / 48.0, 2.0 * u_step);
    float depth = 0.0;
    for (float t = t0 + 0.5 * dt; t < t1; t += dt) {
        vec3 q = (p + u_lightDir * t - u_boxMin) / u_boxSize;
        depth += textureLod(u_fields, q, 1.0).r * fadeAt(q);
    }
    return exp(-u_extinction * depth * dt);
}

// Light the fire casts on a point of a solid facing n, from the lamp of each
// block of the glow texture.
vec3 fireGlow(vec3 p, vec3 n) {
    if (!u_hasGas || u_fireLight <= 0.0) return vec3(0.0);
    vec3 sum = vec3(0.0);
    float soft = 0.25 * dot(u_glowCell, u_glowCell);
    for (int k = 0; k < u_glowDims.z; ++k) {
        for (int j = 0; j < u_glowDims.y; ++j) {
            for (int i = 0; i < u_glowDims.x; ++i) {
                vec3 power = texelFetch(u_glow, ivec3(i, j, k), 0).rgb;
                if (power.r + power.g + power.b <= 0.0) continue;
                vec3 d = u_boxMin + (vec3(i, j, k) + 0.5) * u_glowCell - p;
                float r2 = dot(d, d) + soft;
                sum += power * (max(dot(n, d), 0.0) * inversesqrt(r2) / r2);
            }
        }
    }
    return sum * (u_fireLight * 2.0 / 3.14159265);
}

vec3 shade(vec3 p, vec3 n, vec3 albedo) {
    float ndl = max(dot(n, u_lightDir), 0.0);
    float sun = ndl > 0.0 ? sunAt(p + n * 2e-3) : 0.0;
    vec3 sky = u_sky * (0.6 + 0.4 * n.y);
    return albedo * (u_light * sun * ndl + sky + fireGlow(p, n));
}

// An object: as shade(), with a soft highlight of the sun, and the rim of a
// selected (or hovered) one in the colour of the selection.
vec3 shadeSolid(vec3 p, vec3 n, vec3 view, int i) {
    vec3 albedo = u_solidE[i].rgb;
    float ndl = max(dot(n, u_lightDir), 0.0);
    float sun = ndl > 0.0 ? sunAt(p + n * 2e-3) : 0.0;
    vec3 sky = u_sky * (0.6 + 0.4 * n.y);
    vec3 half_ = normalize(u_lightDir - view);
    vec3 c = albedo * (u_light * sun * ndl + sky + fireGlow(p, n)) +
             u_light * sun * 0.12 * pow(max(dot(n, half_), 0.0), 40.0) * ndl;
    float mark = u_solidE[i].w;
    if (mark > 0.5) {
        float rim = pow(1.0 - abs(dot(n, view)), 2.0);
        c += vec3(1.0, 0.36, 0.08) * rim * (mark > 1.5 ? 1.4 : 0.6) + vec3(0.06, 0.025, 0.005) * (mark > 1.5 ? 1.0 : 0.0);
    }
    return c;
}

// The floor: grey, a line every 10 cm and a stronger one every metre, each
// as thin as the pixels allow and gone where they would crowd.
vec3 floorAlbedo(vec2 q, vec2 width) {
    vec2 w = max(width, vec2(1e-6));
    vec2 minor = abs(fract(q * 10.0 - 0.5) - 0.5) / (w * 10.0);
    vec2 major = abs(fract(q - 0.5) - 0.5) / w;
    float crowd = clamp(1.0 - max(w.x, w.y) * 10.0 / 0.35, 0.0, 1.0);
    float lines = 0.025 * (1.0 - min(min(minor.x, minor.y), 1.0)) * crowd +
                  0.06 * (1.0 - min(min(major.x, major.y), 1.0)) * clamp(1.0 - max(w.x, w.y) / 0.35, 0.0, 1.0);
    return vec3(0.075 + lines);
}

void main() {
    vec3 dir = normalize(u_forward + v_ndc.x * u_tanHalfFov.x * u_right + v_ndc.y * u_tanHalfFov.y * u_up);
    vec3 background = mix(u_backgroundBottom, u_backgroundTop, clamp(v_ndc.y * 0.5 + 0.5, 0.0, 1.0));

    // Where the ray meets the floor -- worked out for every pixel, so that
    // the grid knows how wide a pixel is there.
    bool down = dir.y < -1e-6 && u_eye.y > 0.0;
    float tFloor = down ? -u_eye.y / dir.y : 1e30;
    vec3 floorPoint = u_eye + dir * min(tFloor, 1e4);
    vec2 pixel = fwidth(floorPoint.xz);

    // The first solid on the way: an object, or the floor.
    vec3 normal;
    int which;
    float tSolid = hitSolid(u_eye, dir, 0.0, normal, which);
    vec3 surface = vec3(0.0);
    float cover = 0.0;  // how much of the pixel the solid covers
    float tEnd = 1e30;
    if (tSolid < tFloor && tSolid < 1e29) {
        tEnd = tSolid;
        surface = shadeSolid(u_eye + dir * tSolid, normal, dir, which);
        cover = 1.0;
    } else if (u_floor && down) {
        tEnd = tFloor;
        float away = length(floorPoint.xz - u_floorCenter.xz) / u_floorRadius;
        cover = 1.0 - smoothstep(0.35, 1.0, away);
        if (cover > 0.0) surface = shade(floorPoint, vec3(0.0, 1.0, 0.0), floorAlbedo(floorPoint.xz, pixel));
    }

    vec3 radiance = vec3(0.0);
    float transmittance = 1.0;
    if (u_hasGas) {
        vec2 span = boxSpan(u_eye, dir, u_boxMin, u_boxMin + u_boxSize);
        float tNear = max(span.x, 0.0), tFar = min(span.y, tEnd);
        float cosTheta = dot(dir, u_lightDir);
        // Mostly forwards, a little back.
        float phase = mix(henyeyGreenstein(cosTheta, 0.55), henyeyGreenstein(cosTheta, -0.25), 0.3);
        // Where along its first step each ray starts: at the same depth for
        // every pixel, the steps would show as bands.
        float start = tNear + u_step * ign(gl_FragCoord.xy);
        int steps = int(clamp(ceil((tFar - start) / u_step), 0.0, 4096.0));
        for (int i = 0; i < steps; ++i) {
            vec3 uvw = (u_eye + dir * (start + float(i) * u_step) - u_boxMin) / u_boxSize;
            // Each sample moved by up to half a cell: a ray running along a
            // layer of cells would see the interpolation between them as
            // stripes; this way it is fine noise that anti-aliasing averages.
            vec3 here = textureLod(u_fields, uvw + jitter(float(i)) * u_texel, 0.0).rgb;
            float fade = fadeAt(uvw);
            float smoke = max(here.r, 0.0) * fade, heat = here.g, flame = max(here.b, 0.0) * fade;
            if (smoke < 1e-4 && flame < 1e-4) continue;

            // In the flame the soot glows -- it is what makes the flame
            // yellow -- and hides less of what is behind it than when cooled.
            float sigma = u_extinction * smoke / (1.0 + 4.0 * flame);
            vec3 emitted = u_flame * (1.0 - exp(-4.0 * flame)) * glowAt(heat);
            // What the smoke here scatters towards the eye. `around` averages
            // the fields over a few cells: thick smoke nearby hides the sky,
            // fire nearby lights it.
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
    }
    vec3 colour = toneMap(u_exposure * (radiance + transmittance * cover * surface) +
                          transmittance * (1.0 - cover) * background);
    o_color = vec4(pow(colour, vec3(1.0 / 2.2)), 1.0);

    // The depth of the solid, for the guide lines drawn next.
    if (tEnd < 1e29) {
        vec4 clip = u_viewProj * vec4(u_eye + dir * tEnd, 1.0);
        gl_FragDepth = clamp(clip.z / clip.w * 0.5 + 0.5, 0.0, 1.0);
    } else {
        gl_FragDepth = 1.0;
    }
}
)";

const char* kLineVertex = R"(#version 330 core
layout(location = 0) in vec3 a_position;
layout(location = 1) in vec4 a_color;
uniform mat4 u_viewProj;
out vec4 v_color;
void main() {
    v_color = a_color;
    gl_Position = u_viewProj * vec4(a_position, 1.0);
    gl_Position.z -= 0.0004 * gl_Position.w;  // lines on the floor stay on top of it
}
)";

const char* kLineFragment = R"(#version 330 core
in vec4 v_color;
out vec4 o_color;
void main() { o_color = v_color; }
)";

/// Two unit vectors square to `axis` and to each other.
void basis(const Vec3& axis, Vec3& u, Vec3& v) {
    const Vec3 a = normalize(axis);
    const Vec3 helper = std::fabs(a.y) < 0.9f ? Vec3(0.0f, 1.0f, 0.0f) : Vec3(1.0f, 0.0f, 0.0f);
    u = normalize(cross(a, helper));
    v = cross(a, u);
}

}  // namespace

// --- guide lines -------------------------------------------------------------------

void Lines::segment(const Vec3& a, const Vec3& b, const float color[4]) {
    vertices.push_back({{a.x, a.y, a.z}, {color[0], color[1], color[2], color[3]}});
    vertices.push_back({{b.x, b.y, b.z}, {color[0], color[1], color[2], color[3]}});
    owners.push_back(owner);
}

void Lines::box(const Vec3& lo, const Vec3& hi, const float color[4]) {
    auto corner = [&](int i) { return Vec3(i & 1 ? hi.x : lo.x, i & 2 ? hi.y : lo.y, i & 4 ? hi.z : lo.z); };
    for (int i = 0; i < 8; ++i) {
        for (const int bit : {1, 2, 4}) {
            if (!(i & bit)) segment(corner(i), corner(i | bit), color);
        }
    }
}

void Lines::circle(const Vec3& center, const Vec3& axis, float radius, const float color[4], int segments) {
    Vec3 u, v;
    basis(axis, u, v);
    Vec3 previous = center + u * radius;
    for (int s = 1; s <= segments; ++s) {
        const float a = 2.0f * kPi * static_cast<float>(s) / static_cast<float>(segments);
        const Vec3 p = center + u * (radius * std::cos(a)) + v * (radius * std::sin(a));
        segment(previous, p, color);
        previous = p;
    }
}

void Lines::sphere(const Vec3& center, float radius, const float color[4]) {
    circle(center, Vec3(1.0f, 0.0f, 0.0f), radius, color);
    circle(center, Vec3(0.0f, 1.0f, 0.0f), radius, color);
    circle(center, Vec3(0.0f, 0.0f, 1.0f), radius, color);
}

void Lines::cylinder(const Vec3& center, const Vec3& axis, float radius, float height, const float color[4]) {
    const Vec3 a = normalize(axis);
    const Vec3 lo = center - a * (0.5f * height), hi = center + a * (0.5f * height);
    circle(lo, a, radius, color);
    circle(hi, a, radius, color);
    Vec3 u, v;
    basis(a, u, v);
    for (const Vec3& side : {u, v, u * -1.0f, v * -1.0f}) segment(lo + side * radius, hi + side * radius, color);
}

void Lines::arrow(const Vec3& from, const Vec3& to, const float color[4]) {
    segment(from, to, color);
    const Vec3 d = to - from;
    const float len = length(d);
    if (len <= 0.0f) return;
    Vec3 u, v;
    basis(d, u, v);
    const Vec3 back = to - d * (0.18f);
    const float w = 0.07f * len;
    for (const Vec3& side : {u, v, u * -1.0f, v * -1.0f}) segment(to, back + side * w, color);
}

void Lines::shape(const sim::ShapeInstance& s, const float color[4]) {
    const Vec3& h = s.half();
    // A ring in the shape's own plane through `centre`, spanned by u and v
    // (local, each as long as the ring's radius along it).
    auto ring = [&](const Vec3& centre, const Vec3& u, const Vec3& v, int segments) {
        Vec3 previous = s.toWorld(centre + u);
        for (int i = 1; i <= segments; ++i) {
            const float a = 2.0f * kPi * static_cast<float>(i) / static_cast<float>(segments);
            const Vec3 p = s.toWorld(centre + u * std::cos(a) + v * std::sin(a));
            segment(previous, p, color);
            previous = p;
        }
    };
    const Vec3 x(h.x, 0.0f, 0.0f), y(0.0f, h.y, 0.0f), z(0.0f, 0.0f, h.z);
    switch (s.shape()) {
        case sim::Shape::Sphere:
            ring(Vec3(), x, y, 48);
            ring(Vec3(), y, z, 48);
            ring(Vec3(), x, z, 48);
            break;
        case sim::Shape::Box:
            for (int i = 0; i < 8; ++i) {
                for (const int bit : {1, 2, 4}) {
                    if (i & bit) continue;
                    auto corner = [&](int c) {
                        return s.toWorld(Vec3(c & 1 ? h.x : -h.x, c & 2 ? h.y : -h.y, c & 4 ? h.z : -h.z));
                    };
                    segment(corner(i), corner(i | bit), color);
                }
            }
            break;
        case sim::Shape::Cylinder:
            ring(y * -1.0f, x, z, 48);
            ring(y, x, z, 48);
            for (const Vec3& side : {x, z, x * -1.0f, z * -1.0f}) segment(s.toWorld(side - y), s.toWorld(side + y), color);
            break;
        case sim::Shape::Cone:
            ring(y * -1.0f, x, z, 48);
            for (const Vec3& side : {x, z, x * -1.0f, z * -1.0f}) segment(s.toWorld(side - y), s.toWorld(y), color);
            break;
        case sim::Shape::Torus: {
            // The outer and the inner rim, and the tube at four places round.
            const float out = s.ring() + s.tube(), in = s.ring() - s.tube(), k = h.z / h.x;
            ring(Vec3(), Vec3(out, 0, 0), Vec3(0, 0, out * k), 64);
            ring(Vec3(), Vec3(in, 0, 0), Vec3(0, 0, in * k), 64);
            for (int q = 0; q < 4; ++q) {
                const float a = 0.5f * kPi * static_cast<float>(q);
                const Vec3 dir(std::cos(a), 0.0f, std::sin(a) * k);
                ring(dir * s.ring(), dir * s.tube(), Vec3(0.0f, s.tube(), 0.0f), 24);
            }
            break;
        }
    }
}

Lines sceneGuides(const sim::Scene* gas, const std::vector<sim::Solid>& solids, const std::vector<int>& selected,
                  int domainNode) {
    Lines lines;
    const sim::Scene scene = gas ? *gas : sim::Scene();
    const sim::Domain domain = scene.sanitized().solver.domain();
    const float dim = 0.35f, bright = 0.95f;
    auto isSelected = [&](int node) {
        return node != 0 && std::find(selected.begin(), selected.end(), node) != selected.end();
    };
    auto colour = [&](float r, float g, float b, int node) {
        return std::array<float, 4>{r, g, b, isSelected(node) ? bright : dim};
    };
    const std::array<float, 4> box = {0.62f, 0.65f, 0.72f, isSelected(domainNode) ? 0.8f : 0.45f};
    lines.owner = domainNode;
    if (gas) lines.box(domain.origin(), domain.origin() + domain.size(), box.data());
    for (const sim::Emitter& e : scene.emitters) {
        lines.owner = e.node;
        const auto c = colour(1.0f, 0.62f, 0.25f, e.node);
        lines.shape(e.shapeAt(0.0f), c.data());
        if (e.motion == sim::Motion::Circle) {
            auto path = c;
            path[3] *= 0.6f;
            lines.circle(e.center, Vec3(0.0f, 1.0f, 0.0f), e.motionSize, path.data(), 64);
        } else if (e.motion == sim::Motion::Sway) {
            lines.segment(e.center - Vec3(e.motionSize, 0.0f, 0.0f), e.center + Vec3(e.motionSize, 0.0f, 0.0f),
                          c.data());
        }
        const Vec3 jet = e.shapeAt(0.0f).turn().apply(e.velocity);
        const float speed = length(jet);
        if (speed > 1e-4f) {
            const float r = 0.5f * length(e.size);
            lines.arrow(e.center, e.center + jet * ((r + 0.15f) / std::max(speed, 0.5f)), c.data());
        }
    }
    const Vec3 middle = domain.origin() + domain.size() * 0.5f;
    for (const sim::Force& f : scene.forces) {
        lines.owner = f.node;
        switch (f.kind) {
            case sim::ForceKind::Vortex: {
                const auto c = colour(0.35f, 0.85f, 1.0f, f.node);
                const float height = f.height > 0.0f ? f.height : domain.size().y;
                lines.cylinder(f.center, f.direction, f.radius, height, c.data());
                lines.arrow(f.center, f.center + normalize(f.direction) * (0.5f * height), c.data());
                break;
            }
            case sim::ForceKind::Attractor: {
                const auto c = colour(0.95f, 0.45f, 0.95f, f.node);
                lines.sphere(f.center, f.radius, c.data());
                break;
            }
            case sim::ForceKind::Wind: {
                const auto c = colour(0.55f, 0.8f, 1.0f, f.node);
                const Vec3 d = normalize(f.direction);
                const float reach = 0.35f * length(domain.size());
                for (const float up : {0.3f, 0.6f}) {
                    const Vec3 at(middle.x, domain.size().y * up, middle.z);
                    lines.arrow(at - d * reach, at - d * (0.4f * reach), c.data());
                }
                break;
            }
            case sim::ForceKind::Turbulence:
            case sim::ForceKind::Drag: break;
        }
    }
    // The selected objects, outlined a hair outside their surface.
    const std::array<float, 4> outline = {1.0f, 0.6f, 0.25f, 0.9f};
    for (const sim::Solid& solid : solids) {
        const sim::Collider& b = solid.body;
        if (!isSelected(b.node)) continue;
        lines.owner = b.node;
        lines.shape(sim::ShapeInstance(b.shape, b.center, b.rotation, b.size * 1.01f), outline.data());
    }
    lines.owner = 0;
    return lines;
}

// --- the renderer ------------------------------------------------------------------

VolumeRenderer::VolumeRenderer(const Api& gl) : gl_(gl) {
    gl_.GenVertexArrays(1, &vao_);
    gl_.GenVertexArrays(1, &lineVao_);
    gl_.GenBuffers(1, &lineBuffer_);
    gl_.BindVertexArray(lineVao_);
    gl_.BindBuffer(ARRAY_BUFFER, lineBuffer_);
    gl_.EnableVertexAttribArray(0);
    gl_.VertexAttribPointer(0, 3, FLOAT, 0, sizeof(LineVertex), nullptr);
    gl_.EnableVertexAttribArray(1);
    gl_.VertexAttribPointer(1, 4, FLOAT, 0, sizeof(LineVertex), reinterpret_cast<const void*>(3 * sizeof(float)));
    gl_.BindVertexArray(0);
    gl_.BindBuffer(ARRAY_BUFFER, 0);
    orbit = viewOf(domain_);
}

VolumeRenderer::~VolumeRenderer() {
    for (GLuint p : {program_, shadowProgram_, glowProgram_, lineProgram_}) {
        if (p) gl_.DeleteProgram(p);
    }
    gl_.DeleteVertexArrays(1, &vao_);
    gl_.DeleteVertexArrays(1, &lineVao_);
    gl_.DeleteBuffers(1, &lineBuffer_);
    for (GLuint t : {fields_, light_, glow_, colorTex_}) {
        if (t) gl_.DeleteTextures(1, &t);
    }
    for (GLuint f : {fbo_, passFbo_}) {
        if (f) gl_.DeleteFramebuffers(1, &f);
    }
    if (depthBuffer_) gl_.DeleteRenderbuffers(1, &depthBuffer_);
}

bool VolumeRenderer::init(std::string& log) {
    const std::string header = "#version 330 core\n";
    const GLuint view = buildProgram(gl_, kFullScreen, header + kCommon + kViewFragment, log);
    if (!view) return false;
    const GLuint shadow = buildProgram(gl_, kFullScreen, header + kCommon + kShadowFragment, log);
    if (!shadow) {
        gl_.DeleteProgram(view);
        return false;
    }
    const GLuint glow = buildProgram(gl_, kFullScreen, header + kCommon + kGlowFragment, log);
    const GLuint lines = glow ? buildProgram(gl_, kLineVertex, kLineFragment, log) : 0;
    if (!lines) {
        for (GLuint p : {view, shadow, glow}) {
            if (p) gl_.DeleteProgram(p);
        }
        return false;
    }
    for (GLuint p : {program_, shadowProgram_, glowProgram_, lineProgram_}) {
        if (p) gl_.DeleteProgram(p);
    }
    program_ = view;
    shadowProgram_ = shadow;
    glowProgram_ = glow;
    lineProgram_ = lines;
    lightingDirty_ = true;
    return true;
}

GLint VolumeRenderer::location(GLuint program, const char* name) const {
    return gl_.GetUniformLocation(program, name);
}

void VolumeRenderer::setDomain(const sim::Domain& domain) {
    const bool changed = domain.cells[0] != domain_.cells[0] || domain.cells[1] != domain_.cells[1] ||
                         domain.cells[2] != domain_.cells[2] || domain.voxel != domain_.voxel;
    domain_ = domain;
    if (changed) {
        hasFrame_ = false;
        lightingDirty_ = true;
    }
}

void VolumeRenderer::setFrame(const sim::Frame& frame) {
    const int nx = frame.domain.cells[0], ny = frame.domain.cells[1], nz = frame.domain.cells[2];
    if (frame.fields.size() != 3 * frame.domain.cellCount() || nx <= 0) return;
    setDomain(frame.domain);
    if (!fields_) gl_.GenTextures(1, &fields_);
    gl_.ActiveTexture(TEXTURE0);
    gl_.BindTexture(TEXTURE_3D, fields_);
    gl_.PixelStorei(UNPACK_ALIGNMENT, 2);
    if (size_[0] != nx || size_[1] != ny || size_[2] != nz) {
        gl_.TexImage3D(TEXTURE_3D, 0, static_cast<GLint>(RGB16F), nx, ny, nz, 0, RGB, HALF_FLOAT,
                       frame.fields.data());
        gl_.TexParameteri(TEXTURE_3D, TEXTURE_MIN_FILTER, LINEAR_MIPMAP_LINEAR);
        gl_.TexParameteri(TEXTURE_3D, TEXTURE_MAG_FILTER, LINEAR);
        for (GLenum wrap : {TEXTURE_WRAP_S, TEXTURE_WRAP_T, TEXTURE_WRAP_R}) {
            gl_.TexParameteri(TEXTURE_3D, wrap, CLAMP_TO_EDGE);
        }
        size_[0] = nx;
        size_[1] = ny;
        size_[2] = nz;
    } else {
        gl_.TexSubImage3D(TEXTURE_3D, 0, 0, 0, 0, nx, ny, nz, RGB, HALF_FLOAT, frame.fields.data());
    }
    gl_.GenerateMipmap(TEXTURE_3D);
    gl_.BindTexture(TEXTURE_3D, 0);
    gl_.PixelStorei(UNPACK_ALIGNMENT, 4);
    hasFrame_ = true;
    lightingDirty_ = true;
}

void VolumeRenderer::clearFrame() { hasFrame_ = false; }

void VolumeRenderer::setSolids(const std::vector<sim::Solid>& solids) {
    std::vector<sim::Solid> kept(solids.begin(), solids.begin() + std::min<size_t>(solids.size(), kMaxSolids));
    // The shadows in the smoke hang on the bodies, not on their colours.
    bool moved = kept.size() != solids_.size();
    for (size_t i = 0; !moved && i < kept.size(); ++i) moved = !(kept[i].body == solids_[i].body);
    solids_ = std::move(kept);
    if (moved) lightingDirty_ = true;
}

void VolumeRenderer::setHighlight(const std::vector<int>& selected, int hovered) {
    selected_ = selected;
    hovered_ = hovered;
}

void VolumeRenderer::setLines(const Lines& lines) {
    gl_.BindBuffer(ARRAY_BUFFER, lineBuffer_);
    gl_.BufferData(ARRAY_BUFFER, static_cast<GLsizeiptr>(lines.vertices.size() * sizeof(LineVertex)),
                   lines.vertices.data(), STATIC_DRAW);
    gl_.BindBuffer(ARRAY_BUFFER, 0);
    lineCount_ = lines.vertices.size();
}

void VolumeRenderer::setSceneUniforms(GLuint program) {
    float a[4 * kMaxSolids] = {}, b[4 * kMaxSolids] = {}, c[4 * kMaxSolids] = {}, d[4 * kMaxSolids] = {},
          e[4 * kMaxSolids] = {}, f[4 * kMaxSolids] = {};
    const int n = static_cast<int>(solids_.size());
    for (int i = 0; i < n; ++i) {
        const sim::Solid& solid = solids_[static_cast<size_t>(i)];
        const sim::ShapeInstance s = solid.body.instance();
        const Vec3& h = s.half();
        auto put = [&](float* to, const Vec3& v, float w) {
            to[4 * i] = v.x;
            to[4 * i + 1] = v.y;
            to[4 * i + 2] = v.z;
            to[4 * i + 3] = w;
        };
        put(a, s.center(), static_cast<float>(s.shape()));
        put(b, s.turn().x, h.x);
        put(c, s.turn().y, h.y);
        put(d, s.turn().z, h.z);
        const int node = solid.body.node;
        const bool selected = node != 0 && std::find(selected_.begin(), selected_.end(), node) != selected_.end();
        put(e, solid.color, selected ? 2.0f : node != 0 && node == hovered_ ? 1.0f : 0.0f);
        put(f, Vec3(s.ring(), s.tube(), 0.0f), 0.0f);
    }
    gl_.Uniform1i(location(program, "u_solidCount"), n);
    gl_.Uniform4fv(location(program, "u_solidA"), kMaxSolids, a);
    gl_.Uniform4fv(location(program, "u_solidB"), kMaxSolids, b);
    gl_.Uniform4fv(location(program, "u_solidC"), kMaxSolids, c);
    gl_.Uniform4fv(location(program, "u_solidD"), kMaxSolids, d);
    gl_.Uniform4fv(location(program, "u_solidE"), kMaxSolids, e);
    gl_.Uniform4fv(location(program, "u_solidF"), kMaxSolids, f);
    const Vec3 lo = domain_.origin(), size = domain_.size();
    gl_.Uniform3f(location(program, "u_boxMin"), lo.x, lo.y, lo.z);
    gl_.Uniform3f(location(program, "u_boxSize"), size.x, size.y, size.z);
    gl_.Uniform3f(location(program, "u_texel"), 1.0f / static_cast<float>(domain_.cells[0]),
                  1.0f / static_cast<float>(domain_.cells[1]), 1.0f / static_cast<float>(domain_.cells[2]));
}

namespace {

/// Makes `texture` a 3D texture of `format` and size n, unless it is one.
void allocate3D(const Api& gl, GLuint& texture, int have[3], const int n[3], GLenum format, GLenum channels) {
    if (!texture) gl.GenTextures(1, &texture);
    if (have[0] == n[0] && have[1] == n[1] && have[2] == n[2]) return;
    gl.BindTexture(TEXTURE_3D, texture);
    gl.TexImage3D(TEXTURE_3D, 0, static_cast<GLint>(format), n[0], n[1], n[2], 0, channels, FLOAT, nullptr);
    gl.TexParameteri(TEXTURE_3D, TEXTURE_MIN_FILTER, LINEAR);
    gl.TexParameteri(TEXTURE_3D, TEXTURE_MAG_FILTER, LINEAR);
    for (GLenum wrap : {TEXTURE_WRAP_S, TEXTURE_WRAP_T, TEXTURE_WRAP_R}) gl.TexParameteri(TEXTURE_3D, wrap, CLAMP_TO_EDGE);
    gl.BindTexture(TEXTURE_3D, 0);
    for (int a = 0; a < 3; ++a) have[a] = n[a];
}

}  // namespace

void VolumeRenderer::updateLighting() {
    if (!hasFrame_ || !shadowProgram_ || !glowProgram_) return;
    const LightingKey key{look.lightDirection(), look.smokeDensity, look.flameIntensity, look.flameStart,
                          look.flameRange};
    if (!lightingDirty_ && key == lighting_) return;
    lightingDirty_ = false;
    lighting_ = key;

    // The shadows at half the resolution of the gas: they are soft. The
    // fire's lamps in blocks, six along the domain's longest side.
    const int light[3] = {std::max(1, size_[0] / 2), std::max(1, size_[1] / 2), std::max(1, size_[2] / 2)};
    allocate3D(gl_, light_, lightSize_, light, R16F, RED);
    glowBlock_ = std::max(1, (std::max({size_[0], size_[1], size_[2]}) + 5) / 6);
    const int glow[3] = {(size_[0] + glowBlock_ - 1) / glowBlock_, (size_[1] + glowBlock_ - 1) / glowBlock_,
                         (size_[2] + glowBlock_ - 1) / glowBlock_};
    allocate3D(gl_, glow_, glowSize_, glow, RGBA32F, RGBA);

    if (!passFbo_) gl_.GenFramebuffers(1, &passFbo_);
    gl_.BindFramebuffer(FRAMEBUFFER, passFbo_);
    gl_.Disable(DEPTH_TEST);
    gl_.Disable(BLEND);
    gl_.ActiveTexture(TEXTURE0);
    gl_.BindTexture(TEXTURE_3D, fields_);
    gl_.BindVertexArray(vao_);

    gl_.UseProgram(shadowProgram_);
    setSceneUniforms(shadowProgram_);
    const Vec3 l = normalize(key.light);
    gl_.Uniform3f(location(shadowProgram_, "u_lightDir"), l.x, l.y, l.z);
    gl_.Uniform3f(location(shadowProgram_, "u_size"), static_cast<float>(light[0]), static_cast<float>(light[1]),
                  static_cast<float>(light[2]));
    gl_.Uniform1f(location(shadowProgram_, "u_extinction"), look.smokeDensity);
    gl_.Uniform1f(location(shadowProgram_, "u_step"), 2.0f * domain_.voxel);
    gl_.Uniform1i(location(shadowProgram_, "u_fields"), 0);
    GLint layer = location(shadowProgram_, "u_layer");
    gl_.Viewport(0, 0, light[0], light[1]);
    for (int z = 0; z < light[2]; ++z) {
        gl_.FramebufferTextureLayer(FRAMEBUFFER, COLOR_ATTACHMENT0, light_, 0, z);
        gl_.Uniform1f(layer, static_cast<float>(z));
        gl_.DrawArrays(TRIANGLES, 0, 3);
    }

    gl_.UseProgram(glowProgram_);
    gl_.Uniform1i(location(glowProgram_, "u_fields"), 0);
    gl_.Uniform3i(location(glowProgram_, "u_cells"), size_[0], size_[1], size_[2]);
    gl_.Uniform3i(location(glowProgram_, "u_block"), glowBlock_, glowBlock_, glowBlock_);
    gl_.Uniform1f(location(glowProgram_, "u_flame"), look.flameIntensity);
    gl_.Uniform1f(location(glowProgram_, "u_flameStart"), look.flameStart);
    gl_.Uniform1f(location(glowProgram_, "u_flameRange"), std::max(look.flameRange, 1e-3f));
    gl_.Uniform1f(location(glowProgram_, "u_cellVolume"), domain_.voxel * domain_.voxel * domain_.voxel);
    layer = location(glowProgram_, "u_layer");
    gl_.Viewport(0, 0, glow[0], glow[1]);
    for (int z = 0; z < glow[2]; ++z) {
        gl_.FramebufferTextureLayer(FRAMEBUFFER, COLOR_ATTACHMENT0, glow_, 0, z);
        gl_.Uniform1i(layer, z);
        gl_.DrawArrays(TRIANGLES, 0, 3);
    }

    gl_.BindVertexArray(0);
    gl_.BindTexture(TEXTURE_3D, 0);
    gl_.UseProgram(0);
    gl_.BindFramebuffer(FRAMEBUFFER, 0);
}

void VolumeRenderer::ensureTarget(int width, int height) {
    if (fbo_ && width == width_ && height == height_) return;
    if (!fbo_) {
        gl_.GenFramebuffers(1, &fbo_);
        gl_.GenTextures(1, &colorTex_);
        gl_.GenRenderbuffers(1, &depthBuffer_);
    }
    width_ = width;
    height_ = height;
    gl_.BindTexture(TEXTURE_2D, colorTex_);
    gl_.TexImage2D(TEXTURE_2D, 0, static_cast<GLint>(RGBA8), width, height, 0, RGBA, UNSIGNED_BYTE, nullptr);
    gl_.TexParameteri(TEXTURE_2D, TEXTURE_MIN_FILTER, LINEAR);
    gl_.TexParameteri(TEXTURE_2D, TEXTURE_MAG_FILTER, LINEAR);
    gl_.BindTexture(TEXTURE_2D, 0);
    gl_.BindRenderbuffer(RENDERBUFFER, depthBuffer_);
    gl_.RenderbufferStorage(RENDERBUFFER, DEPTH_COMPONENT24, width, height);
    gl_.BindRenderbuffer(RENDERBUFFER, 0);
    gl_.BindFramebuffer(FRAMEBUFFER, fbo_);
    gl_.FramebufferTexture2D(FRAMEBUFFER, COLOR_ATTACHMENT0, TEXTURE_2D, colorTex_, 0);
    gl_.FramebufferRenderbuffer(FRAMEBUFFER, DEPTH_ATTACHMENT, RENDERBUFFER, depthBuffer_);
    gl_.BindFramebuffer(FRAMEBUFFER, 0);
}

Orbit VolumeRenderer::viewOf(const sim::Domain& domain) {
    const Vec3 size = domain.size();
    Orbit o;
    o.yaw = 35.0f;
    o.pitch = 12.0f;
    // Far enough for the sphere round the domain to fit the view, a little tighter.
    o.distance = 0.92f * 0.5f * length(size) / std::sin(kFovY * kPi / 360.0f);
    o.target[0] = 0.0f;
    o.target[1] = 0.46f * size.y;
    o.target[2] = 0.0f;
    return o;
}

void VolumeRenderer::render(int width, int height) {
    ensureTarget(width, height);
    updateLighting();
    gl_.BindFramebuffer(FRAMEBUFFER, fbo_);
    gl_.Viewport(0, 0, width, height);
    gl_.ColorMask(1, 1, 1, 1);
    gl_.DepthMask(1);
    gl_.ClearColor(0.02f, 0.02f, 0.025f, 1.0f);
    gl_.Clear(COLOR_BUFFER_BIT | DEPTH_BUFFER_BIT);
    if (!program_) {
        gl_.BindFramebuffer(FRAMEBUFFER, 0);
        return;
    }

    // The camera: where it is, and the directions of the screen's axes.
    float eye[3];
    orbit.eye(eye);
    const Vec3 e(eye[0], eye[1], eye[2]), target(orbit.target[0], orbit.target[1], orbit.target[2]);
    const Vec3 forward = normalize(target - e);
    Vec3 right = normalize(cross(forward, Vec3(0.0f, 1.0f, 0.0f)));
    if (length(right) < 0.5f) right = Vec3(1.0f, 0.0f, 0.0f);
    const Vec3 up = cross(right, forward);
    const float tanHalf = std::tan(kFovY * kPi / 360.0f);
    const float aspect = static_cast<float>(width) / static_cast<float>(height);
    viewProjection_ = multiply(perspective(kFovY, aspect, kNear, kFar), lookAt(eye, orbit.target));

    const sim::Look& s = look;
    const Vec3 light = normalize(s.lightDirection());
    const Vec3 size = domain_.size();

    gl_.Enable(DEPTH_TEST);
    gl_.DepthFunc(ALWAYS);
    gl_.Disable(BLEND);
    gl_.UseProgram(program_);
    setSceneUniforms(program_);
    gl_.Uniform3f(location(program_, "u_eye"), e.x, e.y, e.z);
    gl_.Uniform3f(location(program_, "u_right"), right.x, right.y, right.z);
    gl_.Uniform3f(location(program_, "u_up"), up.x, up.y, up.z);
    gl_.Uniform3f(location(program_, "u_forward"), forward.x, forward.y, forward.z);
    gl_.Uniform2f(location(program_, "u_tanHalfFov"), tanHalf * aspect, tanHalf);
    gl_.UniformMatrix4fv(location(program_, "u_viewProj"), 1, 0, viewProjection_.data());
    gl_.Uniform1i(location(program_, "u_hasGas"), hasFrame_ ? 1 : 0);
    gl_.Uniform3f(location(program_, "u_lightDir"), light.x, light.y, light.z);
    gl_.Uniform3f(location(program_, "u_light"), s.lightColor.x * s.lightIntensity, s.lightColor.y * s.lightIntensity,
                  s.lightColor.z * s.lightIntensity);
    gl_.Uniform3f(location(program_, "u_sky"), s.skyColor.x * s.skyIntensity, s.skyColor.y * s.skyIntensity,
                  s.skyColor.z * s.skyIntensity);
    gl_.Uniform3f(location(program_, "u_albedo"), s.smokeColor.x, s.smokeColor.y, s.smokeColor.z);
    gl_.Uniform1f(location(program_, "u_extinction"), s.smokeDensity);
    gl_.Uniform1f(location(program_, "u_occlusion"), s.occlusion);
    gl_.Uniform1f(location(program_, "u_flame"), s.flameIntensity);
    gl_.Uniform1f(location(program_, "u_flameStart"), s.flameStart);
    gl_.Uniform1f(location(program_, "u_flameRange"), std::max(s.flameRange, 1e-3f));
    gl_.Uniform1f(location(program_, "u_fireLight"), s.fireLight);
    gl_.Uniform1f(location(program_, "u_exposure"), s.exposure);
    gl_.Uniform1f(location(program_, "u_step"), 0.6f * domain_.voxel);
    gl_.Uniform1i(location(program_, "u_floor"), s.floor ? 1 : 0);
    gl_.Uniform3f(location(program_, "u_floorCenter"), 0.0f, 0.0f, 0.0f);
    gl_.Uniform1f(location(program_, "u_floorRadius"), std::max(4.0f, 2.5f * std::max(size.x, size.z)));
    gl_.Uniform3i(location(program_, "u_glowDims"), glowSize_[0], glowSize_[1], glowSize_[2]);
    const float block = static_cast<float>(glowBlock_) * domain_.voxel;
    gl_.Uniform3f(location(program_, "u_glowCell"), block, block, block);
    gl_.Uniform3f(location(program_, "u_backgroundTop"), 0.075f, 0.082f, 0.095f);
    gl_.Uniform3f(location(program_, "u_backgroundBottom"), 0.022f, 0.023f, 0.027f);

    gl_.ActiveTexture(TEXTURE0);
    gl_.BindTexture(TEXTURE_3D, fields_);
    gl_.Uniform1i(location(program_, "u_fields"), 0);
    gl_.ActiveTexture(TEXTURE1);
    gl_.BindTexture(TEXTURE_3D, light_);
    gl_.Uniform1i(location(program_, "u_sunlight"), 1);
    gl_.ActiveTexture(TEXTURE2);
    gl_.BindTexture(TEXTURE_3D, glow_);
    gl_.Uniform1i(location(program_, "u_glow"), 2);

    gl_.BindVertexArray(vao_);
    gl_.DrawArrays(TRIANGLES, 0, 3);

    // The guide lines, behind the solids where they pass behind them.
    if (lineCount_ > 0 && lineProgram_) {
        gl_.DepthFunc(LEQUAL);
        gl_.DepthMask(0);
        gl_.Enable(BLEND);
        gl_.BlendFunc(SRC_ALPHA, ONE_MINUS_SRC_ALPHA);
        gl_.UseProgram(lineProgram_);
        gl_.UniformMatrix4fv(location(lineProgram_, "u_viewProj"), 1, 0, viewProjection_.data());
        gl_.BindVertexArray(lineVao_);
        gl_.DrawArrays(LINES, 0, static_cast<GLsizei>(lineCount_));
        gl_.Disable(BLEND);
        gl_.DepthMask(1);
    }
    gl_.BindVertexArray(0);
    gl_.Disable(DEPTH_TEST);
    gl_.DepthFunc(LESS);

    gl_.ActiveTexture(TEXTURE2);
    gl_.BindTexture(TEXTURE_3D, 0);
    gl_.ActiveTexture(TEXTURE1);
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
