#include "pg/gl/Volume.h"

#include "pg/core/Instances.h"
#include "pg/core/Lod.h"
#include "pg/core/Parallel.h"
#include "pg/io/Exr.h"
#include "pg/io/Picture.h"
#include "pg/sim/Display.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace pg::gl {
namespace {

constexpr float kPi = 3.14159265358979323846f;

/// The box lo..hi grown to hold `p` -- made of it alone when `first`.
void grow(Vec3& lo, Vec3& hi, const Vec3& p, bool first) {
    lo = first ? p : glm::min(lo, p);
    hi = first ? p : glm::max(hi, p);
}
/// ... to hold the points of `data`, `stride` floats each, where they are
/// first; `first`: made of them alone.
void grow(Vec3& lo, Vec3& hi, const std::vector<float>& data, size_t stride, bool first) {
    for (size_t i = 0; i + 2 < data.size(); i += stride, first = false) grow(lo, hi, Vec3(data[i], data[i + 1], data[i + 2]), first);
}

const char* kFullScreen = R"(#version 330 core
out vec2 v_ndc;
void main() {
    // One triangle that covers the screen.
    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2) * 2.0 - 1.0;
    v_ndc = p;
    gl_Position = vec4(p, 0.0, 1.0);
}
)";

// The picture shown, its stair-stepped edges smoothed: FXAA (Timothy Lottes'
// first, simplest form) -- along each edge the luma finds, a blend of the
// pixels across it. Where the contrast is low, the pixel as it is: flat
// surfaces and the smoke keep their detail.
const char* kAntialiasFragment = R"(#version 330 core
in vec2 v_ndc;
out vec4 o_color;
uniform sampler2D u_picture;
uniform vec2 u_texel;
float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }
vec3 at(vec2 uv) { return textureLod(u_picture, uv, 0.0).rgb; }
void main() {
    vec2 uv = v_ndc * 0.5 + 0.5;
    vec4 m = textureLod(u_picture, uv, 0.0);
    float lm = luma(m.rgb);
    float nw = luma(at(uv + vec2(-1.0, -1.0) * u_texel)), ne = luma(at(uv + vec2(1.0, -1.0) * u_texel));
    float sw = luma(at(uv + vec2(-1.0, 1.0) * u_texel)), se = luma(at(uv + vec2(1.0, 1.0) * u_texel));
    float lo = min(lm, min(min(nw, ne), min(sw, se))), hi = max(lm, max(max(nw, ne), max(sw, se)));
    if (hi - lo < max(0.0312, 0.125 * hi)) {
        o_color = m;
        return;
    }
    vec2 dir = vec2(-((nw + ne) - (sw + se)), (nw + sw) - (ne + se));
    float reduce = max((nw + ne + sw + se) * 0.25 * (1.0 / 8.0), 1.0 / 128.0);
    dir = clamp(dir / (min(abs(dir.x), abs(dir.y)) + reduce), vec2(-8.0), vec2(8.0)) * u_texel;
    vec3 a = 0.5 * (at(uv + dir * (1.0 / 3.0 - 0.5)) + at(uv + dir * (2.0 / 3.0 - 0.5)));
    vec3 b = 0.5 * a + 0.25 * (at(uv - dir * 0.5) + at(uv + dir * 0.5));
    float lb = luma(b);
    o_color = vec4(lb < lo || lb > hi ? a : b, m.a);
}
)";

// What both passes need: the box, the solids, the fade at the open faces.
const char* kCommon = R"(
uniform vec3 u_boxMin, u_boxSize;
uniform vec3 u_texel;          // one cell, in texture coordinates
// The solids: each a shape placed in the world (pg/sim/Shape.h), a row of
// six texels of u_solids each -- centre and shape; its own axes, each with
// half its size along it; colour and highlight; a torus's ring and tube.
uniform int u_solidCount;
uniform sampler2D u_solids;
vec4 solidA(int i) { return texelFetch(u_solids, ivec2(0, i), 0); }  // centre, shape (0 sphere, 1 box, 2 cylinder, 3 cone, 4 torus)
vec4 solidB(int i) { return texelFetch(u_solids, ivec2(1, i), 0); }  // its x axis in the world, half its size along it
vec4 solidC(int i) { return texelFetch(u_solids, ivec2(2, i), 0); }  // y
vec4 solidD(int i) { return texelFetch(u_solids, ivec2(3, i), 0); }  // z
vec4 solidE(int i) { return texelFetch(u_solids, ivec2(4, i), 0); }  // colour, highlight: 0 none, 1 hovered, 2 selected
// A torus's ring and tube radius; what it is over a plate (sim::Matte); the
// radius of a ball round it.
vec4 solidF(int i) { return texelFetch(u_solids, ivec2(5, i), 0); }

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
    vec4 sa = solidA(i), sf = solidF(i);
    vec3 oc = o - sa.xyz;
    // Missing the ball round it, as most rays do in a scene of many:
    // nothing more to ask.
    float ba = dot(d, d), bb = dot(oc, d), bc = dot(oc, oc) - sf.w * sf.w;
    float bd = bb * bb - ba * bc;
    if (bd < 0.0 || -bb + sqrt(bd) < tMin * ba) return 1e30;
    vec4 sb = solidB(i), sc = solidC(i), sd = solidD(i);
    vec3 ax = sb.xyz, ay = sc.xyz, az = sd.xyz;
    vec3 h = vec3(sb.w, sc.w, sd.w);
    vec3 lo = vec3(dot(ax, oc), dot(ay, oc), dot(az, oc));
    vec3 ld = vec3(dot(ax, d), dot(ay, d), dot(az, d));
    int shape = int(sa.w + 0.5);
    if (shape == 4) {
        // A torus: sphere tracing its exact distance, z squeezed to x's scale.
        float k = h.x / h.z;
        vec3 to = vec3(lo.x, lo.y, lo.z * k), td = vec3(ld.x, ld.y, ld.z * k);
        float speed = length(td);
        float ring = sf.x, tube = sf.y;
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

// The meshes that cast shadows: their distance fields (pg/sim/Mesh.h), and
// which solid each is. A mesh is drawn by rasterising its triangles; for
// the shadows, rays march through its distance field.
uniform int u_meshShadows;
uniform sampler3D u_sdf0, u_sdf1, u_sdf2, u_sdf3;
uniform int u_meshSolid[4];
uniform vec4 u_meshScale[4];   // mesh units per world unit along the solid's axes; w: world units per mesh unit
uniform vec3 u_meshCenter[4];
uniform vec4 u_sdfLo[4];       // the grid's corner, and its cell (w), mesh units
uniform vec3 u_sdfCount[4];    // its points along each axis

// World distance to mesh `slot`: on the safe side where the solid is stretched.
float meshDistance(sampler3D sdf, int slot, vec3 p) {
    int i = u_meshSolid[slot];
    vec3 oc = p - solidA(i).xyz;
    vec3 local = vec3(dot(solidB(i).xyz, oc), dot(solidC(i).xyz, oc), dot(solidD(i).xyz, oc));
    vec3 m = local * u_meshScale[slot].xyz + u_meshCenter[slot];
    vec3 g = (m - u_sdfLo[slot].xyz) / u_sdfLo[slot].w;
    vec3 inside = clamp(g, vec3(0.0), u_sdfCount[slot] - 1.0);
    float d = textureLod(sdf, (inside + 0.5) / u_sdfCount[slot], 0.0).r;
    return (d + length(g - inside) * u_sdfLo[slot].w) * u_meshScale[slot].w;
}

// Does the ray from o along d (unit) pass through mesh `slot`? It starts
// a cell and a half out: the field is coarser than the triangles drawn,
// and a surface must not shadow itself.
bool meshBlocks(sampler3D sdf, int slot, vec3 o, vec3 d) {
    int i = u_meshSolid[slot];
    vec4 sb = solidB(i), sc = solidC(i), sd = solidD(i);
    vec3 h = vec3(sb.w, sc.w, sd.w);
    vec3 oc = o - solidA(i).xyz;
    vec3 lo = vec3(dot(sb.xyz, oc), dot(sc.xyz, oc), dot(sd.xyz, oc)) / h;
    vec3 ld = vec3(dot(sb.xyz, d), dot(sc.xyz, d), dot(sd.xyz, d)) / h;
    vec2 span = boxSpan(lo, ld, vec3(-1.05), vec3(1.05));
    if (span.x > span.y || span.y < 0.0) return false;
    float cell = u_sdfLo[slot].w * u_meshScale[slot].w;
    float t = max(span.x, 0.0) + 1.5 * cell;
    for (int s = 0; s < 64 && t < span.y; ++s) {
        float dist = meshDistance(sdf, slot, o + d * t);
        if (dist < 0.25 * cell) return true;
        t += max(dist, 0.5 * cell);
    }
    return false;
}

bool meshShadow(vec3 o, vec3 d) {
    if (u_meshShadows > 0 && meshBlocks(u_sdf0, 0, o, d)) return true;
    if (u_meshShadows > 1 && meshBlocks(u_sdf1, 1, o, d)) return true;
    if (u_meshShadows > 2 && meshBlocks(u_sdf2, 2, o, d)) return true;
    if (u_meshShadows > 3 && meshBlocks(u_sdf3, 3, o, d)) return true;
    return false;
}

// The nearest solid the ray from o along d meets at tMin or later: how far,
// the normal there and which solid. 1e30 when it meets none. Meshes are
// not among them: they are rasterised (u_meshG).
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
uniform float u_steamExtinction;  // ... of steam
uniform float u_step;          // world units

void main() {
    vec3 uvw = vec3(gl_FragCoord.xy, u_layer + 0.5) / u_size;
    vec3 p = u_boxMin + uvw * u_boxSize;
    vec3 n;
    int which;
    if (hitSolid(p, u_lightDir, 0.0, n, which) < 1e29 || meshShadow(p, u_lightDir)) {
        o_light = vec4(0.0);
        return;
    }
    // Through the smoke towards the light, half a step first -- a cell does
    // not shade itself -- give or take half a step, different in each cell:
    // started alike, the steps of neighbouring cells fall on the smoke alike
    // and show as rings round every billow of a fine grid.
    float far = boxSpan(p, u_lightDir, u_boxMin, u_boxMin + u_boxSize).y;
    float depth = 0.0;
    float jitter = fract(52.9829189 * fract(dot(vec3(gl_FragCoord.xy, u_layer), vec3(0.06711056, 0.00583715, 0.03752))));
    for (float t = jitter * u_step; t < far; t += u_step) {
        vec3 q = (p + u_lightDir * t - u_boxMin) / u_boxSize;
        vec4 f = textureLod(u_fields, q, 1.0);
        depth += (u_extinction * max(f.r, 0.0) + u_steamExtinction * max(f.a, 0.0)) * fadeAt(q);
    }
    o_light = vec4(exp(-depth * u_step));
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
layout(location = 0) out vec4 o_color;
// The passes, when they are drawn (VolumeRenderer::Passes): the depth of
// the surface along the view, how much of what is behind the smoke hides,
// what the surface is; and how far the pixel moves by the next frame.
layout(location = 1) out vec4 o_aux;
layout(location = 2) out vec4 o_motion;
uniform bool u_linear;         // light as it is: no tone curve, no gamma
uniform mat4 u_nextViewProj;   // the camera of the next frame
uniform vec2 u_viewport;       // pixels
uniform sampler2D u_meshAux;   // the meshes' motion (pixels) and what each is

uniform vec3 u_eye, u_right, u_up, u_forward;
uniform vec2 u_tanHalfFov;
uniform mat4 u_viewProj;
uniform bool u_hasGas;
uniform sampler3D u_fields;    // r: smoke, g: temperature, b: flame, a: steam
uniform sampler3D u_sunlight;  // r: share of the sunlight that gets through to here
uniform vec3 u_lightDir;       // towards the light
uniform vec3 u_light;          // its colour x intensity
uniform vec3 u_sky;
uniform vec3 u_albedo;
uniform vec3 u_steamAlbedo;
uniform float u_extinction;    // per unit of smoke per world unit
uniform float u_steamExtinction;  // ... of steam
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
uniform bool u_hasMeshes;
uniform sampler2D u_meshG;     // the rasterised meshes: normal (octahedral; the displayed geometry's translucency on its x), which solid, distance (< 0: none)

vec3 octDecode(vec2 f) {
    vec3 n = vec3(f, 1.0 - abs(f.x) - abs(f.y));
    float t = clamp(-n.z, 0.0, 1.0);
    n.x += n.x >= 0.0 ? -t : t;
    n.y += n.y >= 0.0 ? -t : t;
    return normalize(n);
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

// --- water ---------------------------------------------------------------------
uniform bool u_hasWater;
uniform sampler3D u_water;     // r: distance to the surface, 0 at -band to 1 at +band; g: foam
uniform vec3 u_waterMin, u_waterSize;
uniform float u_waterCell;     // a cell of the water's grid, world units
uniform float u_waterBand;
uniform vec3 u_waterColor;     // what deep water looks like
uniform vec3 u_waterSigma;     // how fast light fades in it, per metre, for red, green and blue
uniform float u_foam;

// The distance to the water's surface at p, world units, below 0 in it.
// Outside the water's grid there is none -- but a point on its side, where
// a ray comes in, counts as in: rounded a hair outside, a ray would take its
// first step from there as if in empty space, past water right at the side.
float waterAt(vec3 p) {
    vec3 uvw = (p - u_waterMin) / u_waterSize;
    if (any(lessThan(uvw, vec3(-1e-3))) || any(greaterThan(uvw, vec3(1.001)))) return u_waterBand;
    return (textureLod(u_water, clamp(uvw, 0.0, 1.0), 0.0).r * 2.0 - 1.0) * u_waterBand;
}

float foamAt(vec3 p) {
    return textureLod(u_water, clamp((p - u_waterMin) / u_waterSize, 0.0, 1.0), 0.0).g;
}

// The gradient close round p: a sheet of water is often thinner than two
// cells, and a wider difference would reach through to its other side.
vec3 waterNormal(vec3 p) {
    float e = 0.35 * u_waterCell;
    vec3 g = vec3(waterAt(p + vec3(e, 0.0, 0.0)) - waterAt(p - vec3(e, 0.0, 0.0)),
                  waterAt(p + vec3(0.0, e, 0.0)) - waterAt(p - vec3(0.0, e, 0.0)),
                  waterAt(p + vec3(0.0, 0.0, e)) - waterAt(p - vec3(0.0, 0.0, e)));
    return dot(g, g) > 1e-12 ? normalize(g) : vec3(0.0, 1.0, 0.0);
}

// Where a ray from o along d (unit) first meets the water between t0 and t1;
// 1e30 if it does not. Sphere tracing: a step as long as the distance says
// is free -- a little less, the field is an estimate -- then the secant
// between the last point outside and the first inside.
float hitWater(vec3 o, vec3 d, float t0, float t1) {
    vec2 span = boxSpan(o, d, u_waterMin, u_waterMin + u_waterSize);
    float t = max(span.x, t0);
    float end = min(span.y, t1);
    if (t >= end) return 1e30;
    float least = 0.35 * u_waterCell;
    float last = t, lastD = waterAt(o + d * t);
    if (lastD < 0.0) return t;
    // The grid ends a hair before `end`: water right against its side -- a
    // film on the wall of a tank -- is looked at there too.
    end -= 1e-4 * u_waterCell;
    // The nearest the ray came: a sheet thinner than a step may never show
    // a point inside it, but the ray comes close to it.
    float nearT = 1e30, nearD = 1e30;
    for (int i = 0; i < 400; ++i) {
        if (last >= end) break;
        t = min(last + max(0.9 * lastD, least), end);
        float dist = waterAt(o + d * t);
        if (dist < 0.0) {
            float a = last, b = t, da = lastD, db = dist;
            for (int k = 0; k < 4; ++k) {
                float m = a + (b - a) * da / (da - db);
                float dm = waterAt(o + d * m);
                if (dm < 0.0) { b = m; db = dm; } else { a = m; da = dm; }
            }
            return a + (b - a) * da / (da - db);
        }
        if (dist < nearD) {
            nearD = dist;
            nearT = t;
        }
        last = t;
        lastD = dist;
    }
    return nearD < 0.3 * u_waterCell ? nearT : 1e30;
}

// How far a ray from o along d, starting in the water, goes before it leaves
// it -- at most `most`.
float leaveWater(vec3 o, vec3 d, float most) {
    float least = 0.5 * u_waterCell;
    float t = least;
    for (int i = 0; i < 200 && t < most; ++i) {
        float dist = waterAt(o + d * t);
        if (dist > 0.0) return t;
        t += max(-0.8 * dist, least);
    }
    return min(t, most);
}

// The share of the sunlight that gets through the water on the way to p:
// water dims the light a little -- less than it dims a view, the light it
// bends comes back together (caustics). For a point on the water's own
// surface only the water past it counts: the light comes in through that
// surface, not along it, which a grazing ray through a thin sheet would.
float waterShade(vec3 p, bool onSurface) {
    if (!u_hasWater) return 1.0;
    vec2 span = boxSpan(p, u_lightDir, u_waterMin, u_waterMin + u_waterSize);
    float t0 = max(span.x, 0.0), t1 = span.y;
    if (t1 <= t0) return 1.0;
    float dt = max((t1 - t0) / 64.0, u_waterCell);
    float depth = 0.0;
    bool counting = !onSurface;
    for (float t = t0 + 0.5 * dt; t < t1; t += dt) {
        float dist = waterAt(p + u_lightDir * t);
        if (!counting) {
            counting = dist > 0.5 * u_waterBand;
            continue;
        }
        depth += dt * smoothstep(0.5 * u_waterBand, -0.5 * u_waterBand, dist);
    }
    return exp(-0.4 * dot(u_waterSigma, vec3(1.0 / 3.0)) * depth);
}

// The shadows of the displayed geometry and the pieces: the share of the
// sun's disc that reaches p past them, from their map seen from the sun --
// nine samples round the point, soft at the edge.
uniform bool u_hasGeoShadow;
uniform sampler2D u_geoShadow;
uniform mat4 u_lightViewProj;
uniform float u_geoShadowTexel;  // a texel of the map, in its units (0 to 1)
uniform float u_geoShadowBias;   // how much nearer the sun a caster must be, in its depth (0 to 1)
uniform float u_geoShadowLift;   // how far off a surface its shadow is looked up, world units

// At p, on a surface facing n: looked up that far off it, along n -- a
// surface the sun grazes would shadow itself in stripes from its own
// texels, however much nearer the sun a caster is asked to be.
float geoLit(vec3 p, vec3 n) {
    if (!u_hasGeoShadow) return 1.0;
    p += n * u_geoShadowLift;
    vec4 c = u_lightViewProj * vec4(p, 1.0);
    vec3 q = c.xyz * 0.5 + 0.5;
    if (any(lessThan(q.xy, vec2(0.0))) || any(greaterThan(q.xy, vec2(1.0)))) return 1.0;
    float depth = min(q.z, 1.0) - u_geoShadowBias;
    float lit = 0.0;
    for (int j = -1; j <= 1; ++j) {
        for (int i = -1; i <= 1; ++i) {
            float d = textureLod(u_geoShadow, q.xy + vec2(float(i), float(j)) * u_geoShadowTexel, 0.0).r;
            lit += depth <= d ? 1.0 : 0.0;
        }
    }
    return lit / 9.0;
}

// Sunlight at p, a hair off a surface facing `facing`: blocked by the
// solids and the geometry, dimmed by the water and the smoke.
float sunThrough(vec3 p, vec3 facing, bool onWater) {
    vec3 n;
    int which;
    if (hitSolid(p, u_lightDir, 1e-3, n, which) < 1e29 || meshShadow(p, u_lightDir)) return 0.0;
    float lit = geoLit(p, facing);
    if (lit <= 0.0) return 0.0;
    float light = waterShade(p, onWater) * lit;
    if (!u_hasGas) return light;
    vec2 span = boxSpan(p, u_lightDir, u_boxMin, u_boxMin + u_boxSize);
    float t0 = max(span.x, 0.0), t1 = span.y;
    if (t1 <= t0) return light;
    float dt = max((t1 - t0) / 48.0, 2.0 * u_step);
    float depth = 0.0;
    for (float t = t0 + 0.5 * dt; t < t1; t += dt) {
        vec3 q = (p + u_lightDir * t - u_boxMin) / u_boxSize;
        vec4 f = textureLod(u_fields, q, 1.0);
        depth += (u_extinction * max(f.r, 0.0) + u_steamExtinction * max(f.a, 0.0)) * fadeAt(q);
    }
    return light * exp(-depth * dt);
}

// Sunlight at a point p of a solid facing n.
float sunAt(vec3 p, vec3 n) { return sunThrough(p + n * 2e-3, n, false); }

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
    float sun = ndl > 0.0 ? sunAt(p, n) : 0.0;
    vec3 sky = u_sky * (0.6 + 0.4 * n.y);
    return albedo * (u_light * sun * ndl + sky + fireGlow(p, n));
}

// What a wet or shiny surface reflects along r: the sky -- brighter
// overhead -- above the horizon, the floor below it.
vec3 environment(vec3 r) {
    if (r.y < 0.0) return vec3(0.075) * (u_light * max(u_lightDir.y, 0.0) * 0.6 + u_sky);
    return u_sky * (1.4 + 2.2 * r.y) + u_light * 0.04;
}

// --- rain: what it wets -----------------------------------------------------------
uniform float u_wet;             // how wet the rain makes things, 0 to 1
uniform vec2 u_wetMin, u_wetMax; // where it rains: wet there, drying off a little way out

float wetAt(vec2 q) {
    vec2 out_ = max(max(u_wetMin - q, q - u_wetMax), vec2(0.0));
    return u_wet * (1.0 - smoothstep(0.0, 0.35, length(out_)));
}

// Wet, a surface is darker and shines with the sky: what that adds to
// `lit`, seen along `view` at p with normal n -- where the rain falls on it.
vec3 wetten(vec3 lit, vec3 p, vec3 n, vec3 view) {
    float wet = u_wet > 0.0 ? wetAt(p.xz) * smoothstep(0.1, 0.7, n.y) : 0.0;
    if (wet <= 0.0) return lit;
    float f = 0.02 + 0.98 * pow(1.0 - clamp(-dot(view, n), 0.0, 1.0), 5.0);
    return lit * (1.0 - 0.5 * wet) + environment(reflect(view, n)) * (f * wet * 0.8);
}

// An object: as shade(), with a soft highlight of the sun, and the rim of a
// selected (or hovered) one in the colour of the selection -- `mark` 1 for
// hovered, 2 for selected.
vec3 shadeSurface(vec3 p, vec3 n, vec3 view, vec3 albedo, float mark, float through) {
    float ndl = max(dot(n, u_lightDir), 0.0);
    float sun = ndl > 0.0 ? sunAt(p, n) : 0.0;
    vec3 sky = u_sky * (0.6 + 0.4 * n.y);
    vec3 half_ = normalize(u_lightDir - view);
    // A thin face -- a leaf, a blade -- lets `through` of the light through:
    // that much less from its side of the sun, and the sun behind it
    // glowing through it in its colour (as the renderers' translucent BSDF).
    float behind = through > 0.0 ? max(-dot(n, u_lightDir), 0.0) : 0.0;
    float sunBehind = behind > 0.0 ? sunAt(p, -n) : 0.0;
    vec3 c = wetten(albedo * (u_light * (sun * ndl * (1.0 - through) + sunBehind * behind * through) + sky + fireGlow(p, n)),
                    p, n, view) +
             u_light * sun * 0.12 * pow(max(dot(n, half_), 0.0), 40.0) * ndl;
    if (mark > 0.5) {
        float rim = pow(1.0 - abs(dot(n, view)), 2.0);
        c += vec3(1.0, 0.36, 0.08) * rim * (mark > 1.5 ? 1.4 : 0.6) + vec3(0.06, 0.025, 0.005) * (mark > 1.5 ? 1.0 : 0.0);
    }
    return c;
}

vec3 shadeSolid(vec3 p, vec3 n, vec3 view, int i) {
    vec4 e = solidE(i);
    return shadeSurface(p, n, view, e.rgb, e.w, 0.0);
}

// --- the plate: what the camera filmed, behind it all ----------------------------
uniform bool u_hasPlate;
uniform sampler2D u_plate;     // in the renderer's light: a shown picture went back through the view transform
uniform vec3 u_plateForward, u_plateRight, u_plateUp;  // the camera that filmed it
uniform vec2 u_plateTan;       // tan of half its view, across and up

// The plate along d -- false where the camera's frame does not reach.
bool plateAt(vec3 d, out vec3 colour) {
    colour = vec3(0.0);
    if (!u_hasPlate) return false;
    float z = dot(d, u_plateForward);
    if (z <= 1e-6) return false;
    vec2 uv = 0.5 + 0.5 * vec2(dot(d, u_plateRight), dot(d, u_plateUp)) / (z * u_plateTan);
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) return false;
    colour = textureLod(u_plate, vec2(uv.x, 1.0 - uv.y), 0.0).rgb;
    return true;
}

// What solid i is over the plate: 0 itself, 1 a holdout, 2 a catcher.
int matteOf(int i) { return i >= 0 && i < u_solidCount ? int(solidF(i).z + 0.5) : 0; }
uniform int u_floorMatte;      // ... and what the floor is

// The sun at p past the real things alone -- the holdouts and catchers --
// as the plate saw it.
float realSun(vec3 p) {
    vec3 n;
    for (int i = 0; i < u_solidCount; ++i) {
        if (matteOf(i) != 0 && hitShape(i, p, u_lightDir, 1e-3, n) < 1e29) return 0.0;
    }
    if (u_meshShadows > 0 && matteOf(u_meshSolid[0]) != 0 && meshBlocks(u_sdf0, 0, p, u_lightDir)) return 0.0;
    if (u_meshShadows > 1 && matteOf(u_meshSolid[1]) != 0 && meshBlocks(u_sdf1, 1, p, u_lightDir)) return 0.0;
    if (u_meshShadows > 2 && matteOf(u_meshSolid[2]) != 0 && meshBlocks(u_sdf2, 2, p, u_lightDir)) return 0.0;
    if (u_meshShadows > 3 && matteOf(u_meshSolid[3]) != 0 && meshBlocks(u_sdf3, 3, p, u_lightDir)) return 0.0;
    return 1.0;
}

// A shadow catcher at p, facing n: the plate there, lit as the real scene
// lit it, relit by the CG -- its shadows take the sun away, the fire adds
// its light. What the plate is multiplied by: the light with the CG over
// the light without it.
vec3 catcher(vec3 p, vec3 n) {
    float ndl = max(dot(n, u_lightDir), 0.0);
    float sunAll = ndl > 0.0 ? sunAt(p, n) : 0.0;
    float sunReal = ndl > 0.0 ? realSun(p + n * 2e-3) : 0.0;
    vec3 sky = u_sky * (0.6 + 0.4 * n.y);
    vec3 withCg = u_light * sunAll * ndl + sky + fireGlow(p, n);
    vec3 without = u_light * sunReal * ndl + sky;
    return withCg / max(without, vec3(1e-4));
}

// The floor: the ground's colour, and -- with the grid -- a line every
// 10 cm and a stronger one every metre, each as thin as the pixels allow
// and gone where they would crowd.
uniform vec3 u_ground;
uniform bool u_grid;
uniform bool u_skyBehind;

// The sky behind it all, outdoors: hazy and brightest towards the horizon,
// glowing round the sun. Below the horizon, where the floor has faded out,
// the haze at the horizon.
vec3 skyBehind(vec3 d) {
    float up = max(d.y, 0.0);
    float toSun = max(dot(d, u_lightDir), 0.0);
    vec3 haze = u_sky * (2.2 - 1.2 * sqrt(up)) + u_light * 0.06;
    return haze + u_light * (0.5 * pow(toSun, 48.0) + 0.12 * pow(toSun, 6.0));
}
vec3 floorAlbedo(vec2 q, vec2 width) {
    if (!u_grid) return u_ground;
    vec2 w = max(width, vec2(1e-6));
    vec2 minor = abs(fract(q * 10.0 - 0.5) - 0.5) / (w * 10.0);
    vec2 major = abs(fract(q - 0.5) - 0.5) / w;
    float crowd = clamp(1.0 - max(w.x, w.y) * 10.0 / 0.35, 0.0, 1.0);
    float lines = 0.025 * (1.0 - min(min(minor.x, minor.y), 1.0)) * crowd +
                  0.06 * (1.0 - min(min(major.x, major.y), 1.0)) * clamp(1.0 - max(w.x, w.y) / 0.35, 0.0, 1.0);
    return u_ground + vec3(lines);
}

// --- rain: ripples on the water ---------------------------------------------------
uniform bool u_hasRipples;
uniform sampler2D u_ripples;   // heights of the rings the drops make, world units
uniform vec2 u_rippleMin, u_rippleSize;
uniform float u_rippleCell;

// How the ripples tilt the water at p: their height's slope along x and z.
vec2 rippleSlope(vec3 p) {
    if (!u_hasRipples) return vec2(0.0);
    vec2 uv = (p.xz - u_rippleMin) / u_rippleSize;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) return vec2(0.0);
    vec2 e = vec2(u_rippleCell / u_rippleSize.x, 0.0), f = vec2(0.0, u_rippleCell / u_rippleSize.y);
    float hx = textureLod(u_ripples, uv + e, 0.0).r - textureLod(u_ripples, uv - e, 0.0).r;
    float hz = textureLod(u_ripples, uv + f, 0.0).r - textureLod(u_ripples, uv - f, 0.0).r;
    return vec2(hx, hz) / (2.0 * u_rippleCell);
}

// What is seen through the water along d from p, just inside it: the floor
// or an object -- or the sky, when the ray leaves the water first and meets
// nothing. `travelled`: how far it went through the water.
vec3 underWater(vec3 p, vec3 d, out float travelled) {
    float tFloor = d.y < -1e-6 ? -p.y / d.y : 1e30;
    if (!u_floor) tFloor = 1e30;
    vec3 n;
    int which;
    float tSolid = hitSolid(p, d, 1e-3, n, which);
    float tStop = min(tFloor, tSolid);
    travelled = leaveWater(p, d, min(tStop, 50.0));
    if (tStop >= 1e29) return environment(d);
    vec3 q = p + d * tStop;
    if (tSolid < tFloor) return shadeSolid(q, n, d, which);
    return shade(q, vec3(0.0, 1.0, 0.0), floorAlbedo(q.xz, vec2(0.004)));
}

// The water's surface at p, seen along d.
vec3 shadeWater(vec3 p, vec3 d) {
    vec3 n = waterNormal(p);
    if (dot(n, d) > 0.0) n = -n;
    // Rings from the rain, on what faces up.
    if (n.y > 0.3) {
        vec2 slope = rippleSlope(p);
        n = normalize(n - vec3(slope.x, 0.0, slope.y) * n.y);
    }
    float cosi = clamp(-dot(d, n), 0.0, 1.0);
    float fresnel = 0.02 + 0.98 * pow(1.0 - cosi, 5.0);
    float sun = sunThrough(p + n * 2e-3, n, true);
    // What it reflects: the sky and the sun's glint, or an object.
    vec3 r = reflect(d, n);
    vec3 reflected = environment(r) + u_light * sun * 30.0 * pow(max(dot(r, u_lightDir), 0.0), 800.0);
    vec3 sn;
    int which;
    float ts = hitSolid(p + n * 1e-3, r, 0.0, sn, which);
    if (ts < 1e29) reflected = shadeSolid(p + n * 1e-3 + r * ts, sn, r, which);
    // What it lets through: bent, fading with the way through the water, and
    // the water's own colour, from the light it scatters back.
    vec3 t = refract(d, n, 1.0 / 1.33);
    float travelled = 0.0;
    vec3 behind = dot(t, t) > 0.0 ? underWater(p - n * 2e-3, t, travelled) : vec3(0.0);
    vec3 through = exp(-u_waterSigma * travelled);
    vec3 ambient = u_sky * 1.5 + u_light * sun * max(u_lightDir.y, 0.0) * 0.35;
    vec3 c = mix(behind * through + u_waterColor * ambient * (1.0 - through), reflected, fresnel);
    // Foam and spray: white and rough, lit like the floor.
    float foam = clamp(foamAt(p) * u_foam, 0.0, 1.0);
    if (foam > 0.02) {
        vec3 white = vec3(0.85) * (u_light * sun * max(dot(n, u_lightDir), 0.0) + u_sky * (0.6 + 0.4 * n.y) * 1.5);
        c = mix(c, white, 0.9 * smoothstep(0.1, 1.0, foam));
    }
    return c;
}

// --- glass ---------------------------------------------------------------------
uniform bool u_hasGlass;
// The nearest face of glass turned to the eye, and the next: the normal
// (octahedral), the tint as a number -- below 0 on the face of a crack --
// and the distance (< 0: none).
uniform sampler2D u_glass0, u_glass1;

// What glass reflects along r: the sky as it shows behind it all, or the
// ground, lit by the sun and the sky.
vec3 glassSky(vec3 r) {
    if (r.y < 0.0 && u_floor) return u_ground * (u_light * max(u_lightDir.y, 0.0) + u_sky);
    return u_skyBehind ? skyBehind(r) : environment(r);
}

// A face of glass at p, facing n, seen along d, of the tint `tint`: the
// light it sends to the eye -- what it reflects -- and in `through` how
// much of what is behind it it lets through. A pane is a thin slab: light
// comes off both of its faces, and what goes through is tinted on its way,
// the more the flatter it goes. Into the face of a crack the view runs
// along the pane, through far more glass: dark and green -- and bright with
// the light the pane carries to its broken edge.
vec3 shadeGlass(vec3 p, vec3 n, vec3 d, vec3 tint, bool crack, out vec3 through) {
    float cosi = clamp(-dot(d, n), 0.0, 1.0);
    float f = 0.04 + 0.96 * pow(1.0 - cosi, 5.0);
    float r = 2.0 * f / (1.0 + f);  // off both faces, and back and forth between them
    float sun = sunThrough(p + n * 2e-3, n, false);
    vec3 rd = reflect(d, n);
    vec3 reflected = glassSky(rd) + u_light * sun * 25.0 * pow(max(dot(rd, u_lightDir), 0.0), 600.0);
    vec3 sn;
    int which;
    float ts = hitSolid(p + n * 1e-3, rd, 0.0, sn, which);
    if (ts < 1e29) reflected = shadeSolid(p + n * 1e-3 + rd * ts, sn, rd, which);
    if (crack) {
        vec3 deep = tint * tint;
        through = (1.0 - r) * deep * deep * 0.5;
        return r * reflected + (1.0 - r) * deep * deep * (u_sky * 0.8 + u_light * sun * 0.1);
    }
    // How far through the pane, in its thicknesses: bent into it (n = 1.5).
    float cost = sqrt(1.0 - (1.0 - cosi * cosi) / 2.25);
    through = (1.0 - r) * pow(mix(vec3(1.0), tint, 0.35), vec3(1.0 / cost));
    return r * reflected;
}

void main() {
    vec3 dir = normalize(u_forward + v_ndc.x * u_tanHalfFov.x * u_right + v_ndc.y * u_tanHalfFov.y * u_up);
    vec3 background = u_skyBehind ? u_exposure * skyBehind(dir)
                                  : mix(u_backgroundBottom, u_backgroundTop, clamp(v_ndc.y * 0.5 + 0.5, 0.0, 1.0));
    // The plate, where the camera's frame is: behind it all, as it was filmed.
    vec3 plate;
    bool onPlate = plateAt(dir, plate);
    if (onPlate) background = plate;

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
    // The meshes' buffer: a solid's index, or -- below 0 -- the displayed
    // geometry's colour, 8 bits a channel.
    bool displayed = false;
    float through = 0.0;  // how much light it lets through: a leaf, a blade
    vec3 displayColor = vec3(0.0);
    bool fromMesh = false;
    vec4 meshAux = vec4(0.0);
    if (u_hasMeshes) {
        vec4 g = texelFetch(u_meshG, ivec2(gl_FragCoord.xy), 0);
        if (g.w > 0.0 && g.w < tSolid) {
            tSolid = g.w;
            fromMesh = true;
            meshAux = texelFetch(u_meshAux, ivec2(gl_FragCoord.xy), 0);
            displayed = g.z < -0.5;
            // The displayed geometry's translucency, off the normal's x.
            float level = displayed ? floor((g.x + 2.0) / 4.0) : 0.0;
            through = level / 15.0;
            normal = octDecode(vec2(g.x - 4.0 * level, g.y));
            if (displayed) {
                float code = -g.z - 1.0;
                float b = floor(code / 65536.0);
                float gr = floor((code - b * 65536.0) / 256.0);
                displayColor = vec3(code - b * 65536.0 - gr * 256.0, gr, b) / 255.0;
            } else {
                which = int(g.z + 0.5);
            }
        }
    }
    vec3 surface = vec3(0.0);
    float cover = 0.0;  // how much of the pixel the solid covers
    float tEnd = 1e30;
    // What the surface is, for the masks: 0 nothing, 1 the floor, 2 the
    // displayed geometry, 3 the pieces, 4 an object, 5 the water.
    float surfaceClass = 0.0;
    // Over the plate, a holdout or a catcher is the real thing: the plate
    // shows there (relit, for a catcher), and what is behind it is hidden.
    vec3 relit = vec3(1.0);
    int matte = onPlate && !displayed ? matteOf(which) : 0;
    // Geometry lying on the floor -- a grid at y 0 -- is in front of it: the
    // distance its faces rasterise to and the floor's, met exactly, are a
    // hair apart either way. Without the floor nothing is behind it: a
    // terrain's valleys below y 0 show.
    float floorAt = !u_floor ? 1e30 : fromMesh ? tFloor * (1.0 + 2e-4) : tFloor;
    if (tSolid < floorAt && tSolid < 1e29 && matte != 0) {
        tEnd = tSolid;
        if (matte == 2) relit = catcher(u_eye + dir * tSolid, normal);
    } else if (tSolid < floorAt && tSolid < 1e29) {
        tEnd = tSolid;
        surface = displayed ? shadeSurface(u_eye + dir * tSolid, normal, dir, displayColor, 0.0, through)
                            : shadeSolid(u_eye + dir * tSolid, normal, dir, which);
        cover = 1.0;
        surfaceClass = fromMesh && meshAux.w > 0.0 ? meshAux.z : 4.0;
    } else if (u_floor && down) {
        tEnd = tFloor;
        fromMesh = false;
        float away = length(floorPoint.xz - u_floorCenter.xz) / u_floorRadius;
        cover = 1.0 - smoothstep(0.35, 1.0, away);
        surfaceClass = cover > 0.5 ? 1.0 : 0.0;
        int floorMatte = onPlate ? u_floorMatte : 0;
        if (floorMatte != 0) {
            // Over the plate, the ground it was filmed on: the plate shows
            // there, relit -- less and less where the floor fades out.
            if (floorMatte == 2) relit = mix(vec3(1.0), catcher(floorPoint, vec3(0.0, 1.0, 0.0)), cover);
            cover = 0.0;
        } else if (cover > 0.0) {
            vec3 up = vec3(0.0, 1.0, 0.0);
            surface = wetten(shade(floorPoint, up, floorAlbedo(floorPoint.xz, pixel)), floorPoint, up, dir);
        }
    }
    // The water, in front of it all.
    if (u_hasWater) {
        float tWater = hitWater(u_eye, dir, 0.0, tEnd);
        if (tWater < 1e29) {
            tEnd = tWater;
            surface = shadeWater(u_eye + dir * tWater, dir);
            cover = 1.0;
            surfaceClass = 5.0;
            fromMesh = false;
            relit = vec3(1.0);
        }
    }

    // The glass in front of where the ray stops -- the nearest two of its
    // faces turned to the eye: what each sends back, and what it lets
    // through of what is behind it, the gas and the surface.
    float glassT[2] = float[2](1e30, 1e30);
    vec3 glassLight[2] = vec3[2](vec3(0.0), vec3(0.0));
    vec3 glassThrough[2] = vec3[2](vec3(1.0), vec3(1.0));
    if (u_hasGlass) {
        ivec2 at = ivec2(gl_FragCoord.xy);
        vec4 layers[2] = vec4[2](texelFetch(u_glass0, at, 0), texelFetch(u_glass1, at, 0));
        for (int k = 0; k < 2; ++k) {
            vec4 g = layers[k];
            if (g.w <= 0.0 || g.w >= tEnd) break;
            float code = abs(g.z) - 1.0;
            float b = floor(code / 65536.0);
            float gr = floor((code - b * 65536.0) / 256.0);
            vec3 tint = vec3(code - b * 65536.0 - gr * 256.0, gr, b) / 255.0;
            glassT[k] = g.w;
            glassLight[k] = shadeGlass(u_eye + dir * g.w, octDecode(g.xy), dir, tint, g.z < 0.0, glassThrough[k]);
        }
    }
    int nextGlass = 0;
    vec3 glassPass = vec3(1.0);  // what the glass the ray came through lets through

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
            float t = start + float(i) * u_step;
            // The glass on the way here.
            while (nextGlass < 2 && glassT[nextGlass] <= t) {
                radiance += transmittance * glassPass * glassLight[nextGlass];
                glassPass *= glassThrough[nextGlass];
                ++nextGlass;
            }
            vec3 uvw = (u_eye + dir * t - u_boxMin) / u_boxSize;
            // Each sample moved by up to half a cell: a ray running along a
            // layer of cells would see the interpolation between them as
            // stripes; this way it is fine noise that anti-aliasing averages.
            vec4 here = textureLod(u_fields, uvw + jitter(float(i)) * u_texel, 0.0);
            float fade = fadeAt(uvw);
            float smoke = max(here.r, 0.0) * fade, heat = here.g, flame = max(here.b, 0.0) * fade;
            float steam = max(here.a, 0.0) * fade;
            if (smoke < 1e-4 && flame < 1e-4 && steam < 1e-4) continue;

            // In the flame the soot glows -- it is what makes the flame
            // yellow -- and hides less of what is behind it than when cooled.
            // Steam stops the light as white as it is.
            float sigmaSmoke = u_extinction * smoke / (1.0 + 4.0 * flame);
            float sigmaSteam = u_steamExtinction * steam;
            float sigma = sigmaSmoke + sigmaSteam;
            vec3 albedo = sigma > 1e-6 ? (u_albedo * sigmaSmoke + u_steamAlbedo * sigmaSteam) / sigma : u_albedo;
            vec3 emitted = u_flame * (1.0 - exp(-4.0 * flame)) * glowAt(heat);
            // What the smoke here scatters towards the eye. `around` averages
            // the fields over a few cells: thick smoke -- and steam, as much
            // as it is as thick -- nearby hides the sky, fire nearby lights it.
            vec4 around = textureLod(u_fields, uvw, 2.0);
            float sun = textureLod(u_sunlight, uvw, 0.0).r;
            float hidden = around.r + max(around.a, 0.0) * u_steamExtinction / max(u_extinction, 1e-6);
            vec3 lit = u_light * sun * phase + u_sky * exp(-u_occlusion * hidden) +
                       u_fireLight * (1.0 - exp(-4.0 * around.b)) * glowAt(around.g);
            vec3 source = albedo * lit * sigma + emitted;
            // The step integrated exactly: what it adds is dimmed by itself too.
            float a = exp(-sigma * u_step);
            radiance += transmittance * glassPass * (sigma > 1e-4 ? source * (1.0 - a) / sigma : source * u_step);
            transmittance *= a;
            if (transmittance < 0.004) break;
        }
    }
    // The glass past the gas, or where there is none.
    for (; nextGlass < 2 && glassT[nextGlass] < 1e29; ++nextGlass) {
        radiance += transmittance * glassPass * glassLight[nextGlass];
        glassPass *= glassThrough[nextGlass];
    }
    // The CG, and how much of the background shows through it.
    vec3 cg = u_exposure * (radiance + transmittance * glassPass * cover * surface);
    vec3 behind = transmittance * (1.0 - cover) * glassPass;
    vec3 colour = cg + behind * relit * background;
    if (u_linear && u_hasPlate) {
        // For compositing over the plate: the CG alone, with how much of the pixel it covers.
        o_color = vec4(cg, 1.0 - dot(behind, vec3(1.0 / 3.0)));
    } else {
        o_color = u_linear ? vec4(colour, 1.0) : vec4(pow(toneMap(colour), vec3(1.0 / 2.2)), 1.0);
    }
    // The passes. The motion: of the point seen -- far off for the sky --
    // as the camera moves; a mesh's as it moves too, from its buffer.
    vec3 seen = u_eye + dir * min(tEnd, 1e5);
    vec4 now = u_viewProj * vec4(seen, 1.0), next = u_nextViewProj * vec4(seen, 1.0);
    vec2 motion = (next.xy / next.w - now.xy / now.w) * 0.5 * u_viewport;
    if (fromMesh && meshAux.w > 0.0 && tEnd == tSolid) motion = meshAux.xy;
    float depth = tEnd < 1e29 && cover > 0.5 ? tEnd * dot(dir, u_forward) : -1.0;  // -1: none
    // The catchers' relighting rides along in what the passes leave free.
    o_aux = vec4(depth, 1.0 - transmittance, surfaceClass, relit.r);
    o_motion = vec4(motion, relit.g, relit.b);

    // The depth of the solid, for the guide lines drawn next.
    if (tEnd < 1e29) {
        vec4 clip = u_viewProj * vec4(u_eye + dir * tEnd, 1.0);
        gl_FragDepth = clamp(clip.z / clip.w * 0.5 + 0.5, 0.0, 1.0);
    } else {
        gl_FragDepth = 1.0;
    }
}
)";

// A mesh's triangles, placed like the solid it is: into a buffer the pass
// that shades everything reads -- the normal facing the eye, which solid,
// and how far along the ray.
const char* kMeshVertex = R"(#version 330 core
layout(location = 0) in vec3 a_position;  // the mesh's own space
layout(location = 1) in vec3 a_normal;
uniform mat4 u_viewProj, u_nextViewProj;
uniform vec3 u_center, u_axisX, u_axisY, u_axisZ;
uniform vec3 u_scale;       // world units per mesh unit along each own axis
uniform vec3 u_meshCenter;
out vec3 v_world, v_normal;
out vec4 v_now, v_next;     // where it is on the screen, and next frame: the camera's motion
void main() {
    vec3 local = (a_position - u_meshCenter) * u_scale;
    v_world = u_center + u_axisX * local.x + u_axisY * local.y + u_axisZ * local.z;
    vec3 n = a_normal / u_scale;  // the inverse transpose of the stretch
    v_normal = u_axisX * n.x + u_axisY * n.y + u_axisZ * n.z;
    gl_Position = u_viewProj * vec4(v_world, 1.0);
    v_now = gl_Position;
    v_next = u_nextViewProj * vec4(v_world, 1.0);
}
)";

const char* kMeshFragment = R"(#version 330 core
in vec3 v_world, v_normal;
in vec4 v_now, v_next;
layout(location = 0) out vec4 o_g;
layout(location = 1) out vec4 o_aux;  // the passes: motion in pixels, and 4 -- an object
uniform vec3 u_eye;
uniform float u_index;
uniform vec2 u_viewport;
vec2 octWrap(vec2 v) { return (1.0 - abs(v.yx)) * vec2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0); }
void main() {
    vec3 view = v_world - u_eye;
    vec3 n = normalize(v_normal);
    if (dot(n, view) > 0.0) n = -n;  // both sides: an open mesh shows its inside
    n /= abs(n.x) + abs(n.y) + abs(n.z);
    o_g = vec4(n.z >= 0.0 ? n.xy : octWrap(n.xy), u_index, length(view));
    o_aux = vec4((v_next.xy / v_next.w - v_now.xy / v_now.w) * 0.5 * u_viewport, 4.0, 1.0);
}
)";

// The displayed geometry's triangles, into the same buffer as the meshes:
// its colour in place of a solid's index, as a number below 0.
const char* kGeoVertex = R"(#version 330 core
layout(location = 0) in vec3 a_position;
layout(location = 1) in vec3 a_normal;
layout(location = 2) in vec3 a_color;
layout(location = 3) in vec3 a_velocity;  // world units a second
// An instance: where it stands and how big, how it is turned (a quaternion),
// its tint -- one of each where the geometry is not instanced: where it is,
// as it is.
layout(location = 4) in vec4 i_place;
layout(location = 5) in vec4 i_turn;
layout(location = 6) in vec4 i_tint;
layout(location = 7) in float a_through;  // how much light its face lets through: a leaf's, a blade's
// Where on its pictures (sim::DisplayMesh::textures): by uv (u, v, 0, 0),
// or from three sides by its place and metres a picture; and which (-1 none).
layout(location = 8) in vec4 a_tex;
layout(location = 9) in float a_picture;
uniform mat4 u_viewProj, u_nextViewProj;
uniform float u_frameTime;                // seconds to the next frame
out vec3 v_world, v_normal, v_color, v_tint, v_ownNormal;
out float v_through;
out vec4 v_tex;
flat out float v_picture, v_fade;
out vec4 v_now, v_next;                   // on the screen now, and where it moves by the next frame
vec3 turned(vec4 q, vec3 v) { vec3 t = 2.0 * cross(q.xyz, v); return v + q.w * t + cross(q.xyz, t); }
void main() {
    v_world = i_place.xyz + turned(i_turn, a_position * i_place.w);
    v_normal = turned(i_turn, a_normal);
    v_ownNormal = a_normal;
    v_color = a_color * i_tint.rgb;
    v_tint = i_tint.rgb;
    v_fade = i_tint.w;
    v_through = a_through;
    v_tex = a_tex;
    v_picture = a_picture;
    gl_Position = u_viewProj * vec4(v_world, 1.0);
    v_now = gl_Position;
    v_next = u_nextViewProj * vec4(v_world + turned(i_turn, a_velocity) * u_frameTime, 1.0);
}
)";

const char* kGeoFragment = R"(#version 330 core
in vec3 v_world, v_normal, v_color, v_tint, v_ownNormal;
in float v_through;
in vec4 v_tex;
flat in float v_picture, v_fade;
in vec4 v_now, v_next;
layout(location = 0) out vec4 o_g;
layout(location = 1) out vec4 o_aux;  // the passes: motion in pixels, and what it is
uniform vec3 u_eye;
uniform vec2 u_viewport;
uniform float u_class;                // 2 the displayed geometry, 3 the pieces
// The pictures laid on (render/Textures.h), a layer each: their colour and
// alpha, their normal maps; each one's mean colour and what it has -- 1
// Cd tints it, 2 a normal map, 4 an alpha.
uniform sampler2DArray u_pictures, u_normalMaps;
uniform vec4 u_pictureLook[16];
vec2 octWrap(vec2 v) { return (1.0 - abs(v.yx)) * vec2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0); }
vec4 picture(vec2 uv, float layer) { return texture(u_pictures, vec3(uv.x, 1.0 - uv.y, layer)); }
// From three sides, as the renderers lay it (TexturePicture::onSurface):
// as much from each as the surface faces it, each moved off the others.
vec4 threeSides(vec3 p, vec3 face, float metres, float layer) {
    float k = 1.0 / metres;
    vec3 w = face * face;
    w *= w;
    w /= max(w.x + w.y + w.z, 1e-6);
    return picture(vec2(p.z * k + 0.31, p.y * k + 0.17), layer) * w.x +
           picture(vec2(p.x * k + 0.53, p.z * k + 0.71), layer) * w.y + picture(vec2(p.x * k, p.y * k), layer) * w.z;
}
// How much of a cut-out picture is there, its alpha read from the smaller
// copies of it kept as thick as the picture's own: thin needles that
// average away in them otherwise vanish far off (Golus's sharpened alpha).
float cover(float alpha, vec2 uv) {
    vec2 dx = dFdx(uv * 512.0), dy = dFdy(uv * 512.0);
    float level = max(0.0, 0.5 * log2(max(dot(dx, dx), dot(dy, dy))));
    return alpha * (1.0 + 0.4 * level);
}
// A noise of the screen's pixels, 0 to 1: where a copy fading in or out is.
float dither() { return fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715)))); }
void main() {
    // A copy between two levels of detail: as much of it as its fade --
    // above 1, the rest of the pixels.
    if (v_fade < 1.0 && dither() >= v_fade) discard;
    if (v_fade > 1.0 && dither() < v_fade - 1.0) discard;
    vec3 view = v_world - u_eye;
    vec3 n = dot(v_normal, v_normal) > 1e-20 ? normalize(v_normal) : -normalize(view);
    if (dot(n, view) > 0.0) n = -n;  // both sides
    vec3 albedo = v_color;
    if (v_picture >= 0.0) {
        int layer = int(v_picture + 0.5);
        vec4 look = u_pictureLook[layer];
        int has = int(look.w + 0.5);
        bool byUv = v_tex.w == 0.0;
        vec4 pic = byUv ? picture(v_tex.xy, float(layer))
                        : threeSides(v_tex.xyz, normalize(v_ownNormal), abs(v_tex.w), float(layer));
        // Cut out where its alpha has none.
        if (byUv && (has & 4) != 0 && cover(pic.a, v_tex.xy) < 0.5) discard;
        albedo = min((has & 1) != 0 ? v_color * pic.rgb / max(look.rgb, vec3(1e-4)) : pic.rgb * v_tint, vec3(0.95));
        if (byUv && (has & 2) != 0) {
            // Bent by its normal map, in the frame the uv makes on the
            // surface here (Schüler's, from the screen's derivatives).
            vec3 dp1 = dFdx(v_world), dp2 = dFdy(v_world);
            vec2 uv = vec2(v_tex.x, v_tex.y), duv1 = dFdx(uv), duv2 = dFdy(uv);
            vec3 dp2perp = cross(dp2, n), dp1perp = cross(n, dp1);
            vec3 t = dp2perp * duv1.x + dp1perp * duv2.x;
            vec3 b = dp2perp * duv1.y + dp1perp * duv2.y;
            float scale = inversesqrt(max(max(dot(t, t), dot(b, b)), 1e-30));
            vec3 m = texture(u_normalMaps, vec3(uv.x, 1.0 - uv.y, float(layer))).xyz * 2.0 - 1.0;
            vec3 bent = t * scale * m.x + b * scale * m.y + n * m.z;
            if (dot(bent, bent) > 1e-12 && dot(bent, view) < 0.0) n = normalize(bent);
        }
    }
    n /= abs(n.x) + abs(n.y) + abs(n.z);
    vec3 c = floor(clamp(albedo, 0.0, 1.0) * 255.0 + 0.5);
    // How much light it lets through, in 15ths, four times over on the
    // normal's x (within -1 to 1): the colour fills the rest.
    vec2 o = n.z >= 0.0 ? n.xy : octWrap(n.xy);
    o.x += 4.0 * floor(clamp(v_through, 0.0, 1.0) * 15.0 + 0.5);
    o_g = vec4(o, -1.0 - (c.r + c.g * 256.0 + c.b * 65536.0), length(view));
    o_aux = vec4((v_next.xy / v_next.w - v_now.xy / v_now.w) * 0.5 * u_viewport, u_class, 1.0);
}
)";

// The displayed geometry and the pieces, seen from the sun: how far along
// its light each pixel's nearest triangle is, 0 to 1 -- what they shadow.
const char* kGeoShadowVertex = R"(#version 330 core
layout(location = 0) in vec3 a_position;
layout(location = 4) in vec4 i_place;  // an instance, as the geometry's program places it
layout(location = 5) in vec4 i_turn;
layout(location = 8) in vec4 a_tex;    // where on its pictures, as the geometry's program has it
layout(location = 9) in float a_picture;
uniform mat4 u_lightViewProj;
out vec2 v_uv;
flat out float v_cut;                  // its picture's layer where it is cut out by uv, else -1
uniform vec4 u_pictureLook[16];
vec3 turned(vec4 q, vec3 v) { vec3 t = 2.0 * cross(q.xyz, v); return v + q.w * t + cross(q.xyz, t); }
void main() {
    gl_Position = u_lightViewProj * vec4(i_place.xyz + turned(i_turn, a_position * i_place.w), 1.0);
    v_uv = a_tex.xy;
    int layer = int(a_picture + 0.5);
    v_cut = a_picture >= 0.0 && a_tex.w == 0.0 && (int(u_pictureLook[layer].w + 0.5) & 4) != 0 ? float(layer) : -1.0;
}
)";

const char* kGeoShadowFragment = R"(#version 330 core
in vec2 v_uv;
flat in float v_cut;
uniform sampler2DArray u_pictures;
out vec4 o_depth;
void main() {
    // No shadow where a leaf is cut out.
    if (v_cut >= 0.0) {
        vec2 dx = dFdx(v_uv * 512.0), dy = dFdy(v_uv * 512.0);
        float level = max(0.0, 0.5 * log2(max(dot(dx, dx), dot(dy, dy))));
        if (texture(u_pictures, vec3(v_uv.x, 1.0 - v_uv.y, v_cut)).a * (1.0 + 0.4 * level) < 0.5) discard;
    }
    o_depth = vec4(gl_FragCoord.z, 0.0, 0.0, 1.0);
}
)";

// A plant far away as a billboard: a card turned to the eye about +y,
// showing its picture from the side it is seen from -- of eight around it,
// each a G-buffer of the plant's own (VolumeRenderer::captureImpostor) --
// into the meshes' buffer as the geometry goes: its normals turned with the
// copy, its colours tinted.
const char* kImpostorVertex = R"(#version 330 core
layout(location = 0) in vec2 a_corner;  // -1 to 1 across and up the card
layout(location = 4) in vec4 i_place;
layout(location = 5) in vec4 i_turn;
layout(location = 6) in vec4 i_tint;
uniform mat4 u_viewProj;
uniform vec3 u_eye, u_center;           // the eye; the middle of the plant's box, its own
uniform float u_radius;                 // how far its corners are from it
uniform int u_views;
out vec3 v_world;
out vec2 v_uv;
flat out int v_view;
flat out vec4 v_turn;
flat out vec3 v_tint;
flat out float v_fade;
vec3 turned(vec4 q, vec3 v) { vec3 t = 2.0 * cross(q.xyz, v); return v + q.w * t + cross(q.xyz, t); }
void main() {
    vec3 middle = i_place.xyz + turned(i_turn, u_center * i_place.w);
    vec3 toEye = u_eye - middle;
    // Which picture: the one taken from nearest the way the eye is, in the
    // plant's own turn.
    vec3 own = turned(vec4(-i_turn.xyz, i_turn.w), toEye);
    float step = 6.2831853 / float(u_views);
    int k = int(floor(atan(own.x, own.z) / step + 0.5));
    v_view = ((k % u_views) + u_views) % u_views;
    vec3 level = vec3(toEye.x, 0.0, toEye.z);
    vec3 right = dot(level, level) > 1e-12 ? normalize(cross(vec3(0.0, 1.0, 0.0), level)) : vec3(1.0, 0.0, 0.0);
    v_world = middle + (right * a_corner.x + vec3(0.0, a_corner.y, 0.0)) * (u_radius * abs(i_place.w));
    v_uv = a_corner * 0.5 + 0.5;
    v_turn = i_turn;
    v_tint = i_tint.rgb;
    v_fade = i_tint.w;
    gl_Position = u_viewProj * vec4(v_world, 1.0);
}
)";

const char* kImpostorFragment = R"(#version 330 core
in vec3 v_world;
in vec2 v_uv;
flat in int v_view;
flat in vec4 v_turn;
flat in vec3 v_tint;
flat in float v_fade;
layout(location = 0) out vec4 o_g;
layout(location = 1) out vec4 o_aux;
uniform sampler2D u_atlas;              // the pictures side by side, each u_texels square
uniform int u_texels;
uniform vec3 u_eye;
uniform float u_class;
vec3 turned(vec4 q, vec3 v) { vec3 t = 2.0 * cross(q.xyz, v); return v + q.w * t + cross(q.xyz, t); }
vec2 octWrap(vec2 v) { return (1.0 - abs(v.yx)) * vec2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0); }
vec3 octDecode(vec2 f) {
    vec3 n = vec3(f, 1.0 - abs(f.x) - abs(f.y));
    float t = clamp(-n.z, 0.0, 1.0);
    n.x += n.x >= 0.0 ? -t : t;
    n.y += n.y >= 0.0 ? -t : t;
    return normalize(n);
}
float dither() { return fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715)))); }
void main() {
    if (v_fade < 1.0 && dither() >= v_fade) discard;
    if (v_fade > 1.0 && dither() < v_fade - 1.0) discard;
    ivec2 at = ivec2(clamp(v_uv, 0.0, 0.999) * float(u_texels)) + ivec2(v_view * u_texels, 0);
    vec4 g = texelFetch(u_atlas, at, 0);
    if (g.w <= 0.0) discard;  // nothing of the plant there
    // Its normal and how much light it lets through, turned with the copy.
    float level = floor((g.x + 2.0) / 4.0);
    vec3 n = turned(v_turn, octDecode(vec2(g.x - 4.0 * level, g.y)));
    n /= abs(n.x) + abs(n.y) + abs(n.z);
    vec2 o = n.z >= 0.0 ? n.xy : octWrap(n.xy);
    o.x += 4.0 * level;
    // Its colour, tinted as the copy is.
    float code = -g.z - 1.0;
    float b = floor(code / 65536.0), gr = floor((code - b * 65536.0) / 256.0);
    vec3 color = vec3(code - b * 65536.0 - gr * 256.0, gr, b) / 255.0 * v_tint;
    vec3 c = floor(clamp(color, 0.0, 1.0) * 255.0 + 0.5);
    o_g = vec4(o, -1.0 - (c.r + c.g * 256.0 + c.b * 65536.0), length(v_world - u_eye));
    o_aux = vec4(0.0, 0.0, u_class, 1.0);
}
)";

// The faces of the glass turned to the eye, into a buffer as the meshes go
// into theirs: the normal, the tint -- as a number, below 0 on the face of
// a crack -- and how far along the ray. Drawn twice: the nearest face, and
// then, peeled off it, the nearest behind that. A face turned away -- as
// its corners go round, whatever its normals -- is where a ray leaves a
// piece of glass, not where it comes into one.
const char* kGlassVertex = R"(#version 330 core
layout(location = 0) in vec3 a_position;
layout(location = 1) in vec3 a_normal;
layout(location = 2) in vec3 a_color;
layout(location = 3) in float a_kind;  // 1 a face of the pane, 2 of a crack
layout(location = 4) in vec3 a_face;   // the face's normal, as its corners go round
uniform mat4 u_viewProj;
out vec3 v_world, v_normal, v_color;
flat out vec3 v_face;
flat out float v_kind;
void main() {
    v_world = a_position;
    v_normal = a_normal;
    v_color = a_color;
    v_face = a_face;
    v_kind = a_kind;
    gl_Position = u_viewProj * vec4(a_position, 1.0);
}
)";

const char* kGlassFragment = R"(#version 330 core
in vec3 v_world, v_normal, v_color;
flat in vec3 v_face;
flat in float v_kind;
out vec4 o_g;
uniform vec3 u_eye;
uniform bool u_peel;         // the second layer: only what is behind the first
uniform sampler2D u_first;   // the first
vec2 octWrap(vec2 v) { return (1.0 - abs(v.yx)) * vec2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0); }
void main() {
    vec3 view = v_world - u_eye;
    if (dot(v_face, view) >= 0.0) discard;
    float far = length(view);
    if (u_peel) {
        float first = texelFetch(u_first, ivec2(gl_FragCoord.xy), 0).w;
        if (first < 0.0 || far <= first * (1.0 + 1e-5) + 1e-5) discard;
    }
    // Its normal -- smooth where the geometry gives one -- turned to the eye
    // where, at the edge of a round piece, it would look away.
    vec3 n = normalize(v_normal);
    vec3 back = -view / far;
    float facing = dot(n, back);
    if (facing < 0.02) n = normalize(n + back * (0.02 - facing));
    n /= abs(n.x) + abs(n.y) + abs(n.z);
    vec3 c = floor(clamp(v_color, 0.0, 1.0) * 255.0 + 0.5);
    float code = 1.0 + c.r + c.g * 256.0 + c.b * 65536.0;
    o_g = vec4(n.z >= 0.0 ? n.xy : octWrap(n.xy), v_kind > 1.5 ? -code : code, far);
}
)";

// The displayed geometry's loose points: round dots, shaded as little balls,
// as wide as their pscale where they have one -- else a few pixels. The
// pieces' -- their grit -- are chips of stone instead, each of a shape and a
// shade of its own; chips of glass -- a radius below 0 -- clear, glinting as
// they tumble. In the gas -- grit in the dust -- the smoke between the eye
// and a dot hides it, and the smoke between it and the sun shades it.
const char* kDotVertex = R"(#version 330 core
layout(location = 0) in vec3 a_position;
layout(location = 1) in vec3 a_color;
layout(location = 2) in float a_radius;
uniform mat4 u_viewProj;
uniform float u_pixelsPerUnit;  // pixels a world unit spans 1 unit in front of the eye
uniform float u_dot;            // pixels across a dot with no size
out vec3 v_color;
out vec3 v_world;
flat out uint v_seed;    // a chip's: from its size, which it keeps as it flies
flat out float v_glass;  // 1 for a chip of glass
void main() {
    gl_Position = u_viewProj * vec4(a_position, 1.0);
    float radius = abs(a_radius);
    float px = radius > 0.0 ? 2.0 * radius * u_pixelsPerUnit / max(gl_Position.w, 1e-4) : u_dot;
    gl_PointSize = clamp(px, 1.5, 64.0);
    v_color = a_color;
    v_world = a_position;
    v_seed = floatBitsToUint(radius);
    v_glass = a_radius < 0.0 ? 1.0 : 0.0;
}
)";

const char* kDotFragment = R"(#version 330 core
in vec3 v_color;
in vec3 v_world;
flat in uint v_seed;
flat in float v_glass;
out vec4 o_color;
uniform bool u_chips;      // chips of stone, not balls
uniform bool u_linear;     // light as it is: no tone curve, no gamma
uniform vec3 u_lightView;  // towards the sun, in the eye's frame: x right, y up, z back at the eye
uniform vec3 u_light, u_sky;
uniform float u_exposure;
uniform bool u_hasGas;
uniform sampler3D u_fields, u_sunlight;
uniform vec3 u_boxMin, u_boxSize, u_texel, u_eye;
uniform float u_extinction, u_steamExtinction, u_occlusion;
vec3 toneMap(vec3 x) { return clamp(x * (2.51 * x + 0.03) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0); }
bool inBox(vec3 uvw) { return all(greaterThanEqual(uvw, vec3(0.0))) && all(lessThanEqual(uvw, vec3(1.0))); }
float fadeAt(vec3 uvw) {  // as the volume fades at the open sides of its box
    vec3 cells = min(uvw, 1.0 - uvw) / u_texel;
    float side = min(cells.x, cells.z) / 6.0, top = (1.0 - uvw.y) / u_texel.y / 10.0;
    return smoothstep(0.0, 1.0, min(side, top));
}
float random(uint k) {  // the chip's k-th number, 0 to 1
    uint h = v_seed + k * 0x9e3779b9u;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return float(h >> 8) / 16777216.0;
}
// A chip of stone as big as the dot: the planes of `least` to two more
// breaks cut it out -- which way each faces and how far from the middle it
// is come from the chip's numbers -- and each is a face of it, leaning away
// from the eye from a top off the middle, where a face is turned to the eye.
// It turns as it flies. False outside it.
bool chip(vec2 q, int least, out vec3 n) {
    float turn = 6.2832 * random(0u) + dot(v_world, vec3(2.3, 1.7, 2.9));
    int breaks = least + int(random(1u) * 3.0);
    float sector = 6.2832 / float(breaks);
    vec2 top = vec2(random(2u), random(3u)) * 0.36 - 0.18;
    float nearest = 0.0, facing = 0.0;
    int face = 0;
    for (int k = 0; k < breaks; ++k) {
        float a = turn + (float(k) + 0.6 * (random(uint(10 + k)) - 0.5)) * sector;
        vec2 out_ = vec2(cos(a), sin(a));
        // How far towards this break, from the top: 1 at it.
        float far = dot(q - top, out_) / (0.5 + 0.45 * random(uint(20 + k)) - dot(top, out_));
        if (far > 1.0) return false;
        if (far > nearest) {
            nearest = far;
            face = k;
            facing = a;
        }
    }
    if (nearest < 0.2 + 0.45 * random(4u)) {
        n = normalize(vec3(0.5 * random(5u) - 0.25, 0.5 * random(6u) - 0.25, 1.0));
    } else {
        float lean = 0.4 + 1.1 * random(uint(30 + face));
        n = normalize(vec3(cos(facing) * lean, sin(facing) * lean, 1.0));
    }
    return true;
}
void main() {
    vec2 q = gl_PointCoord * 2.0 - 1.0;
    q.y = -q.y;
    float r2 = dot(q, q);
    if (r2 > 1.0) discard;
    vec3 n = vec3(q, sqrt(1.0 - r2));
    vec3 color = v_color;
    bool glass = v_glass > 0.5;
    if (u_chips) {
        // Glass breaks into slivers of three to five sides.
        if (!chip(q, glass ? 3 : 5, n)) discard;
        if (glass) {
            // ... flat: the whole of one a face, tilting as it tumbles.
            vec2 tilt = 0.8 * vec2(sin(dot(v_world, vec3(3.1, 1.3, 2.2)) + 6.2832 * random(7u)),
                                   cos(dot(v_world, vec3(1.7, 2.9, 1.1)) + 6.2832 * random(8u)));
            n = normalize(vec3(tilt, 1.0));
        } else {
            // Stones are not all of a colour: lighter and darker, some greyer.
            float grey = dot(v_color, vec3(0.3, 0.5, 0.2));
            color = mix(v_color, vec3(grey), 0.4 * random(40u)) * (0.7 + 0.55 * random(41u));
        }
    }
    // The smoke: what of the sun gets here, how much the sky is hidden, and
    // how much of the dot the smoke in front lets through.
    float sun = 1.0, sky = 1.0, seen = 1.0;
    if (u_hasGas) {
        vec3 here = (v_world - u_boxMin) / u_boxSize;
        if (inBox(here)) {
            sun = textureLod(u_sunlight, here, 0.0).r;
            vec4 around = textureLod(u_fields, here, 2.0);
            sky = exp(-u_occlusion * (max(around.r, 0.0) +
                                      max(around.a, 0.0) * u_steamExtinction / max(u_extinction, 1e-6)));
        }
        vec3 d = u_eye - v_world;
        const int steps = 32;
        float depth = 0.0;
        for (int i = 0; i < steps; ++i) {
            vec3 uvw = (v_world + d * ((float(i) + 0.5) / float(steps)) - u_boxMin) / u_boxSize;
            if (!inBox(uvw)) continue;
            vec4 f = textureLod(u_fields, uvw, 0.0);
            depth += (u_extinction * max(f.r, 0.0) + u_steamExtinction * max(f.a, 0.0)) * fadeAt(uvw);
        }
        seen = exp(-depth * length(d) / float(steps));
    }
    vec3 lit = color * (u_light * sun * max(dot(n, u_lightView), 0.0) + u_sky * sky * (0.7 + 0.5 * n.y));
    float alpha = seen;
    if (glass) {
        // Glass: what is behind it shows through, tinted; it sends back the
        // sky, more of it the flatter it is seen, and the sun off a face
        // turned just so.
        float f = 0.04 + 0.96 * pow(1.0 - clamp(n.z, 0.0, 1.0), 5.0);
        float glint = pow(max(dot(n, normalize(u_lightView + vec3(0.0, 0.0, 1.0))), 0.0), 240.0);
        lit = color * u_sky * sky * (1.0 + 2.0 * f) + u_light * sun * glint * 6.0;
        alpha = clamp(0.1 + 0.6 * f + glint, 0.0, 1.0) * seen;
    }
    o_color = vec4(u_linear ? u_exposure * lit : pow(toneMap(u_exposure * lit), vec3(1.0 / 2.2)), alpha);
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

// The marks of editing (Overlay): each a hair nearer the eye than where it
// is -- `u_pull` of the way -- so that what lies on a surface is drawn over
// it, and what is behind the surface is not.
const char* kOverlayVertex = R"(#version 330 core
layout(location = 0) in vec3 a_position;
layout(location = 1) in vec4 a_color;
uniform mat4 u_viewProj;
uniform vec3 u_eye;
uniform float u_pull;
out vec4 v_color;
void main() {
    v_color = a_color;
    gl_Position = u_viewProj * vec4(a_position + (u_eye - a_position) * u_pull, 1.0);
}
)";

const char* kOverlayFragment = R"(#version 330 core
in vec4 v_color;
uniform float u_fade;  // 1; less for what is drawn through the surface
out vec4 o_color;
void main() { o_color = vec4(v_color.rgb, v_color.a * u_fade); }
)";

// A dot, round, with a dark rim so that it shows on any colour. It covers a
// patch of the surface it lies on, the deeper the more slanted the surface
// is seen: it is drawn that much nearer the eye, all of it over the surface.
const char* kOverlayDotVertex = R"(#version 330 core
layout(location = 0) in vec3 a_position;
layout(location = 1) in vec4 a_color;
layout(location = 2) in float a_size;
layout(location = 3) in vec3 a_normal;
uniform mat4 u_viewProj;
uniform vec3 u_eye;
uniform float u_pull;
uniform float u_pixel;  // world units a pixel covers, a unit of distance away
out vec4 v_color;
void main() {
    v_color = a_color;
    vec3 toEye = u_eye - a_position;
    float dist = length(toEye);
    vec3 v = toEye / max(dist, 1e-6);
    float c = dot(a_normal, a_normal) > 0.25 ? abs(dot(normalize(a_normal), v)) : 0.7;
    float slant = sqrt(max(1.0 - c * c, 0.0)) / max(c, 0.15);
    float pull = u_pull * dist + 0.6 * a_size * u_pixel * dist * slant;
    gl_Position = u_viewProj * vec4(a_position + v * min(pull, 0.5 * dist), 1.0);
    gl_PointSize = a_size;
}
)";

const char* kOverlayDotFragment = R"(#version 330 core
in vec4 v_color;
uniform float u_fade;
out vec4 o_color;
void main() {
    vec2 c = gl_PointCoord * 2.0 - 1.0;
    float r = dot(c, c);
    if (r > 1.0) discard;
    o_color = r > 0.5 ? vec4(0.03, 0.03, 0.04, v_color.a * u_fade) : vec4(v_color.rgb, v_color.a * u_fade);
}
)";

// A line as wide as it says: two triangles, their corners pushed across the
// line on the screen -- lines of GL wider than a pixel are not there in a
// core profile.
const char* kOverlayWideVertex = R"(#version 330 core
layout(location = 0) in vec3 a_position;
layout(location = 1) in vec3 a_other;   // the line's other end
layout(location = 2) in float a_side;   // -1 or 1: which side of the line
layout(location = 3) in float a_width;  // pixels
layout(location = 4) in vec4 a_color;
uniform mat4 u_viewProj;
uniform vec3 u_eye;
uniform float u_pull;
uniform vec2 u_viewport;                // pixels
out vec4 v_color;
vec4 placed(vec3 p) { return u_viewProj * vec4(p + (u_eye - p) * u_pull, 1.0); }
void main() {
    v_color = a_color;
    vec4 here = placed(a_position), there = placed(a_other);
    vec2 a = here.xy / max(here.w, 1e-6), b = there.xy / max(there.w, 1e-6);
    vec2 along = (b - a) * u_viewport;
    along = dot(along, along) > 1e-12 ? normalize(along) : vec2(1.0, 0.0);
    vec2 across = vec2(-along.y, along.x) * a_side * a_width / u_viewport;
    gl_Position = here + vec4(across * here.w, 0.0, 0.0);
}
)";

// A drop as a streak: from where it is back along its velocity, as far as it
// falls in a share of a frame. A streak is drawn a line wide; a drop far
// away covers less of that line, and is fainter for it -- far off, the rain
// is a haze -- while one close by is as wide as it looks.
const char* kRainVertex = R"(#version 330 core
layout(location = 0) in vec3 a_position;
layout(location = 1) in vec3 a_velocity;
layout(location = 2) in vec3 a_corner;  // x: 0 tail, 1 head; y: -1 or 1 across; z: 1 for a droplet
uniform mat4 u_viewProj;
uniform float u_streakTime;             // seconds of fall a streak shows
uniform vec2 u_pixel;                   // a pixel, in clip units
uniform float u_width;                  // pixels
uniform float u_cover;                  // how far off a drop is as wide as the line
out float v_along;
out float v_alpha;
void main() {
    float droplet = a_corner.z;
    vec3 tail = a_position - a_velocity * (u_streakTime * (droplet > 0.5 ? 0.6 : 1.0));
    vec4 h = u_viewProj * vec4(a_position, 1.0), t = u_viewProj * vec4(tail, 1.0);
    if (h.w < 0.02 || t.w < 0.02) {  // behind the eye: nothing
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        v_along = 0.0;
        v_alpha = 0.0;
        return;
    }
    vec2 dp = (h.xy / h.w - t.xy / t.w) / u_pixel;
    vec2 across = dot(dp, dp) > 1e-6 ? normalize(vec2(-dp.y, dp.x)) : vec2(1.0, 0.0);
    vec2 along = dot(dp, dp) > 1e-6 ? normalize(dp) : vec2(0.0, 1.0);
    vec4 p = mix(t, h, a_corner.x);
    // A droplet is some half the size of a drop.
    float depth = 0.5 * (h.w + t.w);
    float cover = u_cover * (droplet > 0.5 ? 0.5 : 1.0) / depth;
    float w = u_width * clamp(cover, 1.0, 3.0);
    // Half a pixel past each end: a drop seen head on is still a dot.
    p.xy += (across * (0.5 * w * a_corner.y) + along * (a_corner.x - 0.5)) * u_pixel * p.w;
    gl_Position = p;
    v_along = a_corner.x;
    // One just in front of the lens would be out of focus: a blur, faint.
    v_alpha = clamp(cover, 0.12, 1.0) * smoothstep(0.15, 0.7, depth);
}
)";

const char* kRainFragment = R"(#version 330 core
in float v_along;
in float v_alpha;
out vec4 o_color;
uniform vec3 u_color;     // lit, tone mapped
uniform float u_opacity;
void main() {
    float fade = smoothstep(0.0, 0.4, v_along);  // the tail fades out
    o_color = vec4(u_color, u_opacity * fade * v_alpha);
}
)";

/// ACES as the view shader has it (Narkowicz), then gamma: a colour lit in
/// the scene as it comes out on screen.
Vec3 toScreen(const Vec3& x) {
    auto one = [](float v) {
        const float t = std::clamp(v * (2.51f * v + 0.03f) / (v * (2.43f * v + 0.59f) + 0.14f), 0.0f, 1.0f);
        return std::pow(t, 1.0f / 2.2f);
    };
    return {one(x.x), one(x.y), one(x.z)};
}

/// A mesh's triangles as the GPU draws them: three corners each, a position
/// and a normal per corner. A corner's normal averages the faces round its
/// vertex that bend less than 60 degrees from its own: round things come
/// out round, a box keeps its edges.
std::vector<float> meshVertices(const sim::TriangleMesh& m) {
    const std::vector<Vec3> normals = sim::cornerNormals(m.positions, m.triangles);
    std::vector<float> out;
    out.reserve(m.triangles.size() * 18);
    for (size_t f = 0; f < m.triangles.size(); ++f) {
        for (size_t c = 0; c < 3; ++c) {
            const Vec3& p = m.positions[m.triangles[f][c]];
            const Vec3& n = normals[f * 3 + c];
            out.insert(out.end(), {p.x, p.y, p.z, n.x, n.y, n.z});
        }
    }
    return out;
}

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
        case sim::Shape::Mesh:  // its box: the triangles would crowd the picture
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

sim::Domain sceneDomain(const sim::World& world) { return sim::sceneDomain(world); }

Lines sceneGuides(const sim::World* world, const std::vector<sim::Solid>& solids, const std::vector<int>& selected,
                  int domainNode, int waterNode, int rainNode, const sim::Camera* camera) {
    Lines lines;
    const sim::Scene* gas = world && world->hasGas ? &world->gas : nullptr;
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
    // The forces, each once, whichever solvers it acts in: the vortex's
    // cylinder, the attractor's sphere, the wind's arrows over the middle of
    // the scene.
    std::vector<sim::Force> forces = scene.forces;
    auto more = [&](const std::vector<sim::Force>& list) {
        for (const sim::Force& f : list) {
            const bool seen = f.node != 0 && std::any_of(forces.begin(), forces.end(),
                                                         [&](const sim::Force& g) { return g.node == f.node; });
            if (!seen) forces.push_back(f);
        }
    };
    if (world && world->hasWater) more(world->water.sanitized().forces);
    if (world && world->hasRain) more(world->rain.sanitized().forces);
    const sim::Domain whole = world && world->any() ? sim::sceneDomain(*world) : domain;
    const Vec3 middle = whole.origin() + whole.size() * 0.5f;
    for (const sim::Force& f : forces) {
        lines.owner = f.node;
        switch (f.kind) {
            case sim::ForceKind::Vortex: {
                const auto c = colour(0.35f, 0.85f, 1.0f, f.node);
                const float height = f.height > 0.0f ? f.height : whole.size().y;
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
                const float reach = 0.35f * length(whole.size());
                for (const float up : {0.3f, 0.6f}) {
                    const Vec3 at(middle.x, whole.size().y * up, middle.z);
                    lines.arrow(at - d * reach, at - d * (0.4f * reach), c.data());
                }
                break;
            }
            case sim::ForceKind::Turbulence:
            case sim::ForceKind::Drag: break;
        }
    }
    // The water: its domain, its sources, the jets of its flows.
    if (world && world->hasWater) {
        const sim::LiquidScene water = world->water.sanitized();
        const sim::Domain wd = water.solver.domain();
        const std::array<float, 4> tank = {0.45f, 0.7f, 1.0f, isSelected(waterNode) ? 0.8f : 0.45f};
        lines.owner = waterNode;
        lines.box(wd.origin(), wd.origin() + wd.size(), tank.data());
        for (const sim::WaterSource& w : water.sources) {
            lines.owner = w.node;
            const auto c = colour(0.3f, 0.7f, 1.0f, w.node);
            const sim::ShapeInstance shape = w.instance();
            lines.shape(shape, c.data());
            const Vec3 jet = shape.turn().apply(w.velocity);
            const float speed = length(jet);
            if (w.mode == sim::WaterMode::Flow && speed > 1e-4f) {
                const float r = 0.5f * length(w.size);
                lines.arrow(w.center, w.center + jet * ((r + 0.15f) / std::max(speed, 0.5f)), c.data());
            }
        }
    }
    // The rain: its cloud, and arrows the way the drops fall from it -- down,
    // slanting with the wind's steady speed.
    if (world && world->hasRain) {
        const sim::RainScene rain = world->rain.sanitized();
        const sim::RainSettings& r = rain.rain;
        lines.owner = rainNode;
        const auto c = colour(0.72f, 0.8f, 0.95f, rainNode);
        const Vec3 half = r.size * 0.5f;
        lines.box(r.center - half, r.center + half, c.data());
        Vec3 wind;
        for (const sim::Force& f : rain.forces) {
            if (f.kind == sim::ForceKind::Wind) wind += normalize(f.direction) * f.speed;
        }
        const Vec3 fall = normalize(wind + Vec3(0.0f, -r.speed, 0.0f));
        const float base = r.center.y - half.y;
        const float reach = std::min(0.6f, 0.5f * std::max(base, 0.0f)) / std::max(-fall.y, 0.2f);
        if (reach > 0.01f) {
            for (const Vec3 at : {Vec3(), Vec3(-0.3f, 0.0f, -0.3f), Vec3(0.3f, 0.0f, -0.3f), Vec3(-0.3f, 0.0f, 0.3f),
                                  Vec3(0.3f, 0.0f, 0.3f)}) {
                const Vec3 from(r.center.x + at.x * r.size.x, base, r.center.z + at.z * r.size.z);
                lines.arrow(from, from + fall * reach, c.data());
            }
        }
    }
    // The camera: a pyramid from where it stands to its picture, 40 cm out,
    // and a triangle over the picture's top.
    if (camera) {
        lines.owner = camera->node;
        const auto c = colour(0.85f, 0.85f, 0.9f, camera->node);
        const float out = 0.4f, h = out * std::tan(0.5f * camera->fovY() * kPi / 180.0f), w = h * camera->aspect();
        const Vec3 p = camera->position, f = camera->forward() * out, r = camera->right() * w, u = camera->up() * h;
        const Vec3 corners[4] = {p + f - r - u, p + f + r - u, p + f + r + u, p + f - r + u};
        for (int i = 0; i < 4; ++i) {
            lines.segment(p, corners[i], c.data());
            lines.segment(corners[i], corners[(i + 1) % 4], c.data());
        }
        const Vec3 top = p + f + u;
        lines.segment(top - r * 0.4f + u * 0.12f, top + u * 0.5f, c.data());
        lines.segment(top + u * 0.5f, top + r * 0.4f + u * 0.12f, c.data());
        lines.segment(top + r * 0.4f + u * 0.12f, top - r * 0.4f + u * 0.12f, c.data());
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
    for (GLuint p : {program_, shadowProgram_, glowProgram_, lineProgram_, meshProgram_, rainProgram_, geoProgram_,
                     dotProgram_, geoShadowProgram_, glassProgram_, overlayProgram_, overlayDotProgram_, overlayWideProgram_,
                     impostorProgram_, antialiasProgram_}) {
        if (p) gl_.DeleteProgram(p);
    }
    if (aaFbo_) gl_.DeleteFramebuffers(1, &aaFbo_);
    if (aaTex_) gl_.DeleteTextures(1, &aaTex_);
    if (impostorQuad_) gl_.DeleteBuffers(1, &impostorQuad_);
    for (int l = 0; l < kOverlayLayers; ++l) {
        for (int k = 0; k < 4; ++k) {
            if (overlayVao_[l][k]) gl_.DeleteVertexArrays(1, &overlayVao_[l][k]);
            if (overlayBuffer_[l][k]) gl_.DeleteBuffers(1, &overlayBuffer_[l][k]);
        }
        if (marksVao_[l]) gl_.DeleteVertexArrays(1, &marksVao_[l]);
        for (GLuint b : marksBuffer_[l]) {
            if (b) gl_.DeleteBuffers(1, &b);
        }
    }
    if (glassFbo_) gl_.DeleteFramebuffers(1, &glassFbo_);
    for (GLuint t : glassTex_) {
        if (t) gl_.DeleteTextures(1, &t);
    }
    if (glassDepth_) gl_.DeleteRenderbuffers(1, &glassDepth_);
    if (glassVao_) gl_.DeleteVertexArrays(1, &glassVao_);
    if (glassBuffer_) gl_.DeleteBuffers(1, &glassBuffer_);
    if (solidsTex_) gl_.DeleteTextures(1, &solidsTex_);
    if (geoShadowFbo_) gl_.DeleteFramebuffers(1, &geoShadowFbo_);
    if (geoShadowTex_) gl_.DeleteTextures(1, &geoShadowTex_);
    if (geoShadowDepth_) gl_.DeleteRenderbuffers(1, &geoShadowDepth_);
    for (InstancedGpu& gpu : instanced_) releaseInstanced(gpu);
    for (GLuint a : {geoVao_, dotVao_, curveVao_, shownVao_}) {
        if (a) gl_.DeleteVertexArrays(1, &a);
    }
    for (GLuint b : {geoBuffer_, dotBuffer_, curveBuffer_, geoVelocityBuffer_, shownPlaces_, shownColors_, shownVelocities_,
                     shownIndices_, shownThrough_, shownTextures_}) {
        if (b) gl_.DeleteBuffers(1, &b);
    }
    // The pictures' arrays are textures, not buffers.
    for (GLuint t : {auxTex_[0], auxTex_[1], gAux_, plateTex_, picturesTex_, normalMapsTex_}) {
        if (t) gl_.DeleteTextures(1, &t);
    }
    if (rainVao_) gl_.DeleteVertexArrays(1, &rainVao_);
    if (rainBuffer_) gl_.DeleteBuffers(1, &rainBuffer_);
    if (ripples_) gl_.DeleteTextures(1, &ripples_);
    for (MeshGpu& m : meshes_) {
        gl_.DeleteVertexArrays(1, &m.vao);
        gl_.DeleteBuffers(1, &m.vbo);
        gl_.DeleteTextures(1, &m.sdf);
    }
    if (gFbo_) gl_.DeleteFramebuffers(1, &gFbo_);
    if (gTex_) gl_.DeleteTextures(1, &gTex_);
    if (gDepth_) gl_.DeleteRenderbuffers(1, &gDepth_);
    gl_.DeleteVertexArrays(1, &vao_);
    gl_.DeleteVertexArrays(1, &lineVao_);
    gl_.DeleteBuffers(1, &lineBuffer_);
    for (GLuint t : {fields_, light_, glow_, colorTex_, water_}) {
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
    const GLuint meshes = lines ? buildProgram(gl_, kMeshVertex, kMeshFragment, log) : 0;
    const GLuint rain = meshes ? buildProgram(gl_, kRainVertex, kRainFragment, log) : 0;
    const GLuint geo = rain ? buildProgram(gl_, kGeoVertex, kGeoFragment, log) : 0;
    const GLuint dots = geo ? buildProgram(gl_, kDotVertex, kDotFragment, log) : 0;
    const GLuint geoShadow = dots ? buildProgram(gl_, kGeoShadowVertex, kGeoShadowFragment, log) : 0;
    // Billboards are a nicety: without them, the plants' meshes far away.
    std::string impostorLog;
    const GLuint impostor = geoShadow ? buildProgram(gl_, kImpostorVertex, kImpostorFragment, impostorLog) : 0;
    const GLuint glass = geoShadow ? buildProgram(gl_, kGlassVertex, kGlassFragment, log) : 0;
    const GLuint overlay = glass ? buildProgram(gl_, kOverlayVertex, kOverlayFragment, log) : 0;
    const GLuint overlayDots = overlay ? buildProgram(gl_, kOverlayDotVertex, kOverlayDotFragment, log) : 0;
    const GLuint overlayWide = overlayDots ? buildProgram(gl_, kOverlayWideVertex, kOverlayFragment, log) : 0;
    if (!overlayWide) {
        for (GLuint p : {view, shadow, glow, lines, meshes, rain, geo, dots, geoShadow, glass, overlay, overlayDots}) {
            if (p) gl_.DeleteProgram(p);
        }
        return false;
    }
    for (GLuint p : {program_, shadowProgram_, glowProgram_, lineProgram_, meshProgram_, rainProgram_, geoProgram_,
                     dotProgram_, geoShadowProgram_, glassProgram_, overlayProgram_, overlayDotProgram_, overlayWideProgram_}) {
        if (p) gl_.DeleteProgram(p);
    }
    overlayProgram_ = overlay;
    overlayDotProgram_ = overlayDots;
    overlayWideProgram_ = overlayWide;
    program_ = view;
    shadowProgram_ = shadow;
    glowProgram_ = glow;
    lineProgram_ = lines;
    meshProgram_ = meshes;
    rainProgram_ = rain;
    geoProgram_ = geo;
    dotProgram_ = dots;
    geoShadowProgram_ = geoShadow;
    if (impostorProgram_) gl_.DeleteProgram(impostorProgram_);
    impostorProgram_ = impostor;
    // So is the smoothing: without it, the picture as drawn.
    std::string antialiasLog;
    if (antialiasProgram_) gl_.DeleteProgram(antialiasProgram_);
    antialiasProgram_ = buildProgram(gl_, kFullScreen, kAntialiasFragment, antialiasLog);
    glassProgram_ = glass;
    lightingDirty_ = true;
    geoShadowDirty_ = true;
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

void VolumeRenderer::setFrame(const sim::Frame& frame, unsigned layers) {
    setFrame(frame, *sim::prepareVolumes(frame, texelBudget, layers));
}

void VolumeRenderer::setFrame(const sim::Frame& frame, const sim::PreparedVolumes& prepared) {
    if (prepared.layers & kWater) setWater(frame.water, prepared);
    else hasWater_ = false;
    if (prepared.layers & kRain) {
        setRain(frame.rain, prepared);
    } else {
        hasRain_ = false;
        hasRipples_ = false;
    }
    if (prepared.gas.empty()) {
        hasFrame_ = false;  // no gas in this frame
        return;
    }
    const sim::Domain& grid = prepared.gasGrid;
    const int nx = grid.cells[0], ny = grid.cells[1], nz = grid.cells[2];
    const std::vector<uint16_t>& fields = prepared.gas;
    setDomain(grid);
    if (!fields_) gl_.GenTextures(1, &fields_);
    gl_.ActiveTexture(TEXTURE0);
    gl_.BindTexture(TEXTURE_3D, fields_);
    gl_.PixelStorei(UNPACK_ALIGNMENT, 2);
    if (size_[0] != nx || size_[1] != ny || size_[2] != nz) {
        gl_.TexImage3D(TEXTURE_3D, 0, static_cast<GLint>(RGBA16F), nx, ny, nz, 0, RGBA, HALF_FLOAT, fields.data());
        gl_.TexParameteri(TEXTURE_3D, TEXTURE_MIN_FILTER, LINEAR_MIPMAP_LINEAR);
        gl_.TexParameteri(TEXTURE_3D, TEXTURE_MAG_FILTER, LINEAR);
        for (GLenum wrap : {TEXTURE_WRAP_S, TEXTURE_WRAP_T, TEXTURE_WRAP_R}) {
            gl_.TexParameteri(TEXTURE_3D, wrap, CLAMP_TO_EDGE);
        }
        size_[0] = nx;
        size_[1] = ny;
        size_[2] = nz;
    } else {
        gl_.TexSubImage3D(TEXTURE_3D, 0, 0, 0, 0, nx, ny, nz, RGBA, HALF_FLOAT, fields.data());
    }
    gl_.GenerateMipmap(TEXTURE_3D);
    gl_.BindTexture(TEXTURE_3D, 0);
    gl_.PixelStorei(UNPACK_ALIGNMENT, 4);
    hasFrame_ = true;
    lightingDirty_ = true;
}

void VolumeRenderer::setWater(const sim::WaterFrame& water, const sim::PreparedVolumes& prepared) {
    if (prepared.water.empty()) {
        hasWater_ = false;
        return;
    }
    const sim::Domain& grid = prepared.waterGrid;
    const std::vector<uint8_t>* texels = &prepared.water;
    const int nx = grid.cells[0], ny = grid.cells[1], nz = grid.cells[2];
    if (!water_) gl_.GenTextures(1, &water_);
    gl_.ActiveTexture(TEXTURE0);
    gl_.BindTexture(TEXTURE_3D, water_);
    gl_.PixelStorei(UNPACK_ALIGNMENT, 2);
    if (waterSize_[0] != nx || waterSize_[1] != ny || waterSize_[2] != nz) {
        gl_.TexImage3D(TEXTURE_3D, 0, static_cast<GLint>(RG8), nx, ny, nz, 0, RG, UNSIGNED_BYTE, texels->data());
        gl_.TexParameteri(TEXTURE_3D, TEXTURE_MIN_FILTER, LINEAR);
        gl_.TexParameteri(TEXTURE_3D, TEXTURE_MAG_FILTER, LINEAR);
        for (GLenum wrap : {TEXTURE_WRAP_S, TEXTURE_WRAP_T, TEXTURE_WRAP_R}) gl_.TexParameteri(TEXTURE_3D, wrap, CLAMP_TO_EDGE);
        waterSize_[0] = nx;
        waterSize_[1] = ny;
        waterSize_[2] = nz;
    } else {
        gl_.TexSubImage3D(TEXTURE_3D, 0, 0, 0, 0, nx, ny, nz, RG, UNSIGNED_BYTE, texels->data());
    }
    gl_.BindTexture(TEXTURE_3D, 0);
    gl_.PixelStorei(UNPACK_ALIGNMENT, 4);
    waterDomain_ = grid;
    waterBand_ = water.band;
    hasWater_ = true;
}

void VolumeRenderer::setRain(const sim::RainFrame& rain, const sim::PreparedVolumes& prepared) {
    hasRain_ = !prepared.rain.empty();
    rainTimeStep_ = rain.timeStep;
    rainVertices_ = 0;
    if (hasRain_) {
        // The floor is wet where the drops are.
        for (int a = 0; a < 2; ++a) {
            wetMin_[a] = prepared.wetMin[a];
            wetMax_[a] = prepared.wetMax[a];
        }
        const std::vector<float>& v = prepared.rain;
        grow(rainBox_.lo, rainBox_.hi, v, 9, true);
        if (!rainVao_) {
            gl_.GenVertexArrays(1, &rainVao_);
            gl_.GenBuffers(1, &rainBuffer_);
            gl_.BindVertexArray(rainVao_);
            gl_.BindBuffer(ARRAY_BUFFER, rainBuffer_);
            for (GLuint a = 0; a < 3; ++a) {
                gl_.EnableVertexAttribArray(a);
                gl_.VertexAttribPointer(a, 3, FLOAT, 0, 9 * sizeof(float), reinterpret_cast<const void*>(3 * a * sizeof(float)));
            }
            gl_.BindVertexArray(0);
        }
        gl_.BindBuffer(ARRAY_BUFFER, rainBuffer_);
        gl_.BufferData(ARRAY_BUFFER, static_cast<GLsizeiptr>(v.size() * sizeof(float)), v.data(), STATIC_DRAW);
        gl_.BindBuffer(ARRAY_BUFFER, 0);
        rainVertices_ = static_cast<GLsizei>(v.size() / 9);
    }
    hasRipples_ = !rain.ripples.empty() && rain.rippleCells[0] > 0 && rain.rippleCells[1] > 0;
    if (hasRipples_) {
        if (!ripples_) gl_.GenTextures(1, &ripples_);
        gl_.ActiveTexture(TEXTURE0);
        gl_.BindTexture(TEXTURE_2D, ripples_);
        gl_.PixelStorei(UNPACK_ALIGNMENT, 2);
        gl_.TexImage2D(TEXTURE_2D, 0, static_cast<GLint>(R16F), rain.rippleCells[0], rain.rippleCells[1], 0, RED, HALF_FLOAT,
                       rain.ripples.data());
        gl_.PixelStorei(UNPACK_ALIGNMENT, 4);
        gl_.TexParameteri(TEXTURE_2D, TEXTURE_MIN_FILTER, LINEAR);
        gl_.TexParameteri(TEXTURE_2D, TEXTURE_MAG_FILTER, LINEAR);
        gl_.TexParameteri(TEXTURE_2D, TEXTURE_WRAP_S, CLAMP_TO_EDGE);
        gl_.TexParameteri(TEXTURE_2D, TEXTURE_WRAP_T, CLAMP_TO_EDGE);
        gl_.BindTexture(TEXTURE_2D, 0);
        rippleMin_ = rain.rippleOrigin;
        rippleCell_ = rain.rippleCell;
        rippleSize_[0] = rain.rippleCells[0];
        rippleSize_[1] = rain.rippleCells[1];
    }
}

void VolumeRenderer::drawRain(int width, int height, const Vec3& eye) {
    (void)eye;
    if (!hasRain_ || !rainProgram_ || rainVertices_ == 0) return;
    const sim::Look& s = look;
    // The drops are lit by the sky, and a little by the sun they fall
    // through; as the rest, then tone mapped.
    const Vec3 sky = s.skyColor * s.skyIntensity, sun = s.lightColor * s.lightIntensity;
    const Vec3 lit = s.rainColor * ((sky * 3.0f + sun * 0.25f) * s.exposure);
    const Vec3 c = passes.on ? lit : toScreen(lit);  // the passes: light as it is
    gl_.Enable(DEPTH_TEST);
    gl_.DepthFunc(LEQUAL);
    gl_.DepthMask(0);
    gl_.Enable(BLEND);
    gl_.BlendFunc(SRC_ALPHA, ONE_MINUS_SRC_ALPHA);
    gl_.UseProgram(rainProgram_);
    gl_.UniformMatrix4fv(location(rainProgram_, "u_viewProj"), 1, 0, glm::value_ptr(viewProjection_));
    gl_.Uniform1f(location(rainProgram_, "u_streakTime"), s.rainStreak * rainTimeStep_);
    gl_.Uniform2f(location(rainProgram_, "u_pixel"), 2.0f / static_cast<float>(width), 2.0f / static_cast<float>(height));
    // As wide on a big image as on a small one: some 1.3 px at 600 high.
    const float line = std::max(1.0f, 1.3f * static_cast<float>(height) / 600.0f);
    gl_.Uniform1f(location(rainProgram_, "u_width"), line);
    // A drop 2.5 mm across is as wide as that line this far off.
    const float pixelAt1m = 2.0f * std::tan(0.5f * orbit.fovY * kPi / 180.0f) / static_cast<float>(height);
    gl_.Uniform1f(location(rainProgram_, "u_cover"), 0.0025f / (pixelAt1m * line));
    gl_.Uniform3f(location(rainProgram_, "u_color"), c.x, c.y, c.z);
    gl_.Uniform1f(location(rainProgram_, "u_opacity"), s.rainOpacity);
    gl_.BindVertexArray(rainVao_);
    gl_.DrawArrays(TRIANGLES, 0, rainVertices_);
    gl_.BindVertexArray(0);
    gl_.Disable(BLEND);
    gl_.DepthMask(1);
}

void VolumeRenderer::clearFrame() {
    hasFrame_ = false;
    hasWater_ = false;
    hasRain_ = false;
    hasRipples_ = false;
}

void VolumeRenderer::setSolids(const std::vector<sim::Solid>& solids) {
    std::vector<sim::Solid> kept(solids.begin(), solids.begin() + std::min<size_t>(solids.size(), kMaxSolids));
    // The shadows in the smoke hang on the bodies, not on their colours.
    bool moved = kept.size() != solids_.size();
    for (size_t i = 0; !moved && i < kept.size(); ++i) moved = !(kept[i].body == solids_[i].body);
    solids_ = std::move(kept);
    if (moved) lightingDirty_ = true;
    syncMeshes();
}

void VolumeRenderer::syncMeshes() {
    std::vector<MeshGpu> kept;
    anyMesh_ = false;
    for (const sim::Solid& solid : solids_) {
        const auto& mesh = solid.body.mesh;
        if (solid.body.shape != sim::Shape::Mesh || !mesh) continue;
        anyMesh_ = true;
        if (std::any_of(kept.begin(), kept.end(), [&](const MeshGpu& m) { return m.mesh == mesh; })) continue;
        const auto had = std::find_if(meshes_.begin(), meshes_.end(), [&](const MeshGpu& m) { return m.mesh == mesh; });
        if (had != meshes_.end()) {
            kept.push_back(*had);
            had->mesh.reset();  // moved: not to be deleted below
            continue;
        }
        // New: its triangles, and its distance field as half floats.
        MeshGpu gpu;
        gpu.mesh = mesh;
        const std::vector<float> data = meshVertices(mesh->mesh());
        gpu.vertices = static_cast<GLsizei>(data.size() / 6);
        gl_.GenVertexArrays(1, &gpu.vao);
        gl_.GenBuffers(1, &gpu.vbo);
        gl_.BindVertexArray(gpu.vao);
        gl_.BindBuffer(ARRAY_BUFFER, gpu.vbo);
        gl_.BufferData(ARRAY_BUFFER, static_cast<GLsizeiptr>(data.size() * sizeof(float)), data.data(), STATIC_DRAW);
        gl_.EnableVertexAttribArray(0);
        gl_.VertexAttribPointer(0, 3, FLOAT, 0, 6 * sizeof(float), nullptr);
        gl_.EnableVertexAttribArray(1);
        gl_.VertexAttribPointer(1, 3, FLOAT, 0, 6 * sizeof(float), reinterpret_cast<const void*>(3 * sizeof(float)));
        gl_.BindVertexArray(0);
        gl_.BindBuffer(ARRAY_BUFFER, 0);
        std::vector<uint16_t> field(mesh->field().size());
        for (size_t i = 0; i < field.size(); ++i) field[i] = sim::toHalf(mesh->field()[i]);
        gl_.GenTextures(1, &gpu.sdf);
        gl_.ActiveTexture(TEXTURE0);
        gl_.BindTexture(TEXTURE_3D, gpu.sdf);
        gl_.PixelStorei(UNPACK_ALIGNMENT, 2);
        gl_.TexImage3D(TEXTURE_3D, 0, static_cast<GLint>(R16F), mesh->points(0), mesh->points(1), mesh->points(2), 0, RED,
                       HALF_FLOAT, field.data());
        gl_.PixelStorei(UNPACK_ALIGNMENT, 4);
        gl_.TexParameteri(TEXTURE_3D, TEXTURE_MIN_FILTER, LINEAR);
        gl_.TexParameteri(TEXTURE_3D, TEXTURE_MAG_FILTER, LINEAR);
        for (GLenum wrap : {TEXTURE_WRAP_S, TEXTURE_WRAP_T, TEXTURE_WRAP_R}) gl_.TexParameteri(TEXTURE_3D, wrap, CLAMP_TO_EDGE);
        gl_.BindTexture(TEXTURE_3D, 0);
        kept.push_back(gpu);
    }
    for (MeshGpu& m : meshes_) {
        if (!m.mesh) continue;  // kept
        gl_.DeleteVertexArrays(1, &m.vao);
        gl_.DeleteBuffers(1, &m.vbo);
        gl_.DeleteTextures(1, &m.sdf);
    }
    meshes_ = std::move(kept);
}

void VolumeRenderer::renderMeshes(int width, int height, const Vec3& eye) {
    // The plants' copies at the levels of detail they look big enough for,
    // and the billboards they need pictured -- before the buffer is bound.
    seeFrom(eye);
    prepareImpostors();
    const bool resized = width != gWidth_ || height != gHeight_;
    if (!gFbo_) {
        gl_.GenFramebuffers(1, &gFbo_);
        gl_.GenTextures(1, &gTex_);
        gl_.GenRenderbuffers(1, &gDepth_);
    }
    if (width != gWidth_ || height != gHeight_) {
        gWidth_ = width;
        gHeight_ = height;
        gl_.BindTexture(TEXTURE_2D, gTex_);
        gl_.TexImage2D(TEXTURE_2D, 0, static_cast<GLint>(RGBA32F), width, height, 0, RGBA, FLOAT, nullptr);
        gl_.TexParameteri(TEXTURE_2D, TEXTURE_MIN_FILTER, 0x2600);  // NEAREST: each pixel its own
        gl_.TexParameteri(TEXTURE_2D, TEXTURE_MAG_FILTER, 0x2600);
        gl_.BindTexture(TEXTURE_2D, 0);
        gl_.BindRenderbuffer(RENDERBUFFER, gDepth_);
        gl_.RenderbufferStorage(RENDERBUFFER, DEPTH_COMPONENT24, width, height);
        gl_.BindRenderbuffer(RENDERBUFFER, 0);
        gl_.BindFramebuffer(FRAMEBUFFER, gFbo_);
        gl_.FramebufferTexture2D(FRAMEBUFFER, COLOR_ATTACHMENT0, TEXTURE_2D, gTex_, 0);
        gl_.FramebufferRenderbuffer(FRAMEBUFFER, DEPTH_ATTACHMENT, RENDERBUFFER, gDepth_);
    }
    // The passes: each mesh's motion to the next frame and what it is, beside.
    if (passes.on && (!gAux_ || resized || !gPasses_)) {
        if (!gAux_) gl_.GenTextures(1, &gAux_);
        gl_.BindTexture(TEXTURE_2D, gAux_);
        gl_.TexImage2D(TEXTURE_2D, 0, static_cast<GLint>(RGBA32F), width, height, 0, RGBA, FLOAT, nullptr);
        gl_.TexParameteri(TEXTURE_2D, TEXTURE_MIN_FILTER, 0x2600);  // NEAREST
        gl_.TexParameteri(TEXTURE_2D, TEXTURE_MAG_FILTER, 0x2600);
        gl_.BindTexture(TEXTURE_2D, 0);
    }
    gl_.BindFramebuffer(FRAMEBUFFER, gFbo_);
    gl_.FramebufferTexture2D(FRAMEBUFFER, COLOR_ATTACHMENT1, TEXTURE_2D, passes.on ? gAux_ : 0, 0);
    const GLenum buffers[2] = {COLOR_ATTACHMENT0, COLOR_ATTACHMENT1};
    gl_.DrawBuffers(passes.on ? 2 : 1, buffers);
    gPasses_ = passes.on;
    gl_.Viewport(0, 0, width, height);
    gl_.ClearColor(0.0f, 0.0f, 0.0f, -1.0f);  // w < 0: no mesh here
    gl_.Clear(COLOR_BUFFER_BIT | DEPTH_BUFFER_BIT);
    gl_.Enable(DEPTH_TEST);
    gl_.DepthFunc(LESS);
    gl_.DepthMask(1);
    gl_.Disable(BLEND);
    gl_.Disable(CULL_FACE);
    gl_.UseProgram(meshProgram_);
    gl_.UniformMatrix4fv(location(meshProgram_, "u_viewProj"), 1, 0, glm::value_ptr(viewProjection_));
    gl_.UniformMatrix4fv(location(meshProgram_, "u_nextViewProj"), 1, 0, glm::value_ptr(nextViewProjection_));
    gl_.Uniform2f(location(meshProgram_, "u_viewport"), static_cast<float>(width), static_cast<float>(height));
    gl_.Uniform3f(location(meshProgram_, "u_eye"), eye.x, eye.y, eye.z);
    for (size_t i = 0; i < solids_.size(); ++i) {
        const sim::Collider& body = solids_[i].body;
        if (body.shape != sim::Shape::Mesh || !body.mesh) continue;
        const auto gpu = std::find_if(meshes_.begin(), meshes_.end(), [&](const MeshGpu& m) { return m.mesh == body.mesh; });
        if (gpu == meshes_.end()) continue;
        const sim::ShapeInstance shape = body.instance();
        const Vec3& h = shape.half();
        const Vec3& mh = body.mesh->half();
        const Vec3& mc = body.mesh->center();
        gl_.Uniform3f(location(meshProgram_, "u_center"), shape.center().x, shape.center().y, shape.center().z);
        gl_.Uniform3f(location(meshProgram_, "u_axisX"), shape.turn().x.x, shape.turn().x.y, shape.turn().x.z);
        gl_.Uniform3f(location(meshProgram_, "u_axisY"), shape.turn().y.x, shape.turn().y.y, shape.turn().y.z);
        gl_.Uniform3f(location(meshProgram_, "u_axisZ"), shape.turn().z.x, shape.turn().z.y, shape.turn().z.z);
        gl_.Uniform3f(location(meshProgram_, "u_scale"), h.x / mh.x, h.y / mh.y, h.z / mh.z);
        gl_.Uniform3f(location(meshProgram_, "u_meshCenter"), mc.x, mc.y, mc.z);
        gl_.Uniform1f(location(meshProgram_, "u_index"), static_cast<float>(i));
        gl_.BindVertexArray(gpu->vao);
        gl_.DrawArrays(TRIANGLES, 0, gpu->vertices);
    }
    if ((geoVertices_ > 0 || shownElements_ > 0 || hasInstances()) && geoProgram_) {
        gl_.UseProgram(geoProgram_);
        placeUninstanced();
        gl_.UniformMatrix4fv(location(geoProgram_, "u_viewProj"), 1, 0, glm::value_ptr(viewProjection_));
        gl_.UniformMatrix4fv(location(geoProgram_, "u_nextViewProj"), 1, 0, glm::value_ptr(nextViewProjection_));
        gl_.Uniform1f(location(geoProgram_, "u_frameTime"), passes.on ? passes.frameTime : 0.0f);
        gl_.Uniform2f(location(geoProgram_, "u_viewport"), static_cast<float>(width), static_cast<float>(height));
        gl_.Uniform3f(location(geoProgram_, "u_eye"), eye.x, eye.y, eye.z);
        bindPictures(geoProgram_);
        // The displayed geometry, then the pieces: what each is, for the masks.
        gl_.Uniform1f(location(geoProgram_, "u_class"), static_cast<float>(Surface::Geometry));
        if (shownElements_ > 0) {
            gl_.BindVertexArray(shownVao_);
            gl_.DrawElements(TRIANGLES, shownElements_, UNSIGNED_INT, nullptr);
        }
        drawInstances(false);
        gl_.Uniform1f(location(geoProgram_, "u_class"), static_cast<float>(Surface::Pieces));
        if (geoVertices_ > 0) {
            gl_.BindVertexArray(geoVao_);
            gl_.DrawArrays(TRIANGLES, 0, geoVertices_);
        }
        drawImpostors(eye);
    }
    gl_.BindVertexArray(0);
    gl_.UseProgram(0);
    gl_.BindFramebuffer(FRAMEBUFFER, 0);
}

void VolumeRenderer::renderGlass(int width, int height, const Vec3& eye) {
    if (!glassFbo_) {
        gl_.GenFramebuffers(1, &glassFbo_);
        gl_.GenTextures(2, glassTex_);
        gl_.GenRenderbuffers(1, &glassDepth_);
    }
    if (width != glassWidth_ || height != glassHeight_) {
        glassWidth_ = width;
        glassHeight_ = height;
        for (const GLuint t : glassTex_) {
            gl_.BindTexture(TEXTURE_2D, t);
            gl_.TexImage2D(TEXTURE_2D, 0, static_cast<GLint>(RGBA32F), width, height, 0, RGBA, FLOAT, nullptr);
            gl_.TexParameteri(TEXTURE_2D, TEXTURE_MIN_FILTER, 0x2600);  // NEAREST: each pixel its own
            gl_.TexParameteri(TEXTURE_2D, TEXTURE_MAG_FILTER, 0x2600);
        }
        gl_.BindTexture(TEXTURE_2D, 0);
        gl_.BindRenderbuffer(RENDERBUFFER, glassDepth_);
        gl_.RenderbufferStorage(RENDERBUFFER, DEPTH_COMPONENT24, width, height);
        gl_.BindRenderbuffer(RENDERBUFFER, 0);
        gl_.BindFramebuffer(FRAMEBUFFER, glassFbo_);
        gl_.FramebufferRenderbuffer(FRAMEBUFFER, DEPTH_ATTACHMENT, RENDERBUFFER, glassDepth_);
    }
    gl_.BindFramebuffer(FRAMEBUFFER, glassFbo_);
    gl_.Viewport(0, 0, width, height);
    gl_.Enable(DEPTH_TEST);
    gl_.DepthFunc(LESS);
    gl_.DepthMask(1);
    gl_.ColorMask(1, 1, 1, 1);
    gl_.Disable(BLEND);
    gl_.Disable(CULL_FACE);
    gl_.UseProgram(glassProgram_);
    gl_.UniformMatrix4fv(location(glassProgram_, "u_viewProj"), 1, 0, glm::value_ptr(viewProjection_));
    gl_.Uniform3f(location(glassProgram_, "u_eye"), eye.x, eye.y, eye.z);
    gl_.Uniform1i(location(glassProgram_, "u_first"), 13);
    gl_.BindVertexArray(glassVao_);
    const GLenum buffer = COLOR_ATTACHMENT0;
    // The nearest face, then the nearest behind it: the first layer read
    // on unit 13 while the second is drawn.
    for (int layer = 0; layer < 2; ++layer) {
        gl_.FramebufferTexture2D(FRAMEBUFFER, COLOR_ATTACHMENT0, TEXTURE_2D, glassTex_[layer], 0);
        gl_.DrawBuffers(1, &buffer);
        gl_.ClearColor(0.0f, 0.0f, 0.0f, -1.0f);  // w < 0: no glass here
        gl_.Clear(COLOR_BUFFER_BIT | DEPTH_BUFFER_BIT);
        gl_.Uniform1i(location(glassProgram_, "u_peel"), layer);
        gl_.ActiveTexture(TEXTURE0 + 13);
        gl_.BindTexture(TEXTURE_2D, layer == 1 ? glassTex_[0] : 0);
        gl_.DrawArrays(TRIANGLES, 0, glassVertices_);
    }
    gl_.BindTexture(TEXTURE_2D, 0);
    gl_.ActiveTexture(TEXTURE0);
    gl_.BindVertexArray(0);
    gl_.UseProgram(0);
    gl_.BindFramebuffer(FRAMEBUFFER, 0);
}

void VolumeRenderer::setGeometry(const GeometryPtr& geometry) {
    if (geometry == geometry_) return;
    setPrepared(preparer_.prepare(geometry));
}

void VolumeRenderer::setPrepared(std::shared_ptr<const sim::PreparedGeometry> prepared) {
    if (prepared == prepared_) return;
    prepared_ = std::move(prepared);
    geometry_ = prepared_ ? prepared_->geometry : nullptr;
    // What stands on its points: where, sent again; what, sent only when new.
    uploadInstances();
    // Its polygons indexed. Of the topology in the buffers, the vertices'
    // places alone go to the GPU again -- and with nothing else to draw,
    // that is all.
    const bool moved = prepared_ && shownVao_ && prepared_->topology == sentTopology_;
    sentTopology_ = prepared_ ? prepared_->topology : 0;
    if (moved && !prepared_->rest) {
        uploadShownMesh(false);
        updateGeometryBounds();
        geoShadowDirty_ = true;
        return;
    }
    uploadShownMesh(!moved);
    uploadGeometry();
}

const sim::DisplayGeometry& VolumeRenderer::shownDisplay() const {
    static const sim::DisplayGeometry none;
    return prepared_ ? prepared_->display : none;
}

const sim::DisplayInstances& VolumeRenderer::instances() const {
    static const sim::DisplayInstances none;
    return prepared_ ? prepared_->instances : none;
}

void VolumeRenderer::uploadShownMesh(bool all) {
    static const sim::DisplayMesh none;
    const sim::DisplayMesh& m = prepared_ && prepared_->mesh ? *prepared_->mesh : none;
    if (!shownVao_) {
        gl_.GenVertexArrays(1, &shownVao_);
        GLuint buffers[6] = {};
        gl_.GenBuffers(6, buffers);
        shownPlaces_ = buffers[0];
        shownColors_ = buffers[1];
        shownVelocities_ = buffers[2];
        shownIndices_ = buffers[3];
        shownThrough_ = buffers[4];
        shownTextures_ = buffers[5];
        all = true;
    }
    auto bytes = [](const auto& v) { return static_cast<GLsizeiptr>(v.size() * sizeof(v[0])); };
    const GLsizei six = 6 * static_cast<GLsizei>(sizeof(float)), three = 3 * static_cast<GLsizei>(sizeof(float));
    gl_.BindVertexArray(shownVao_);
    gl_.BindBuffer(ARRAY_BUFFER, shownPlaces_);
    if (all) {
        gl_.BufferData(ARRAY_BUFFER, bytes(m.places), m.places.data(), DYNAMIC_DRAW);
        gl_.EnableVertexAttribArray(0);
        gl_.VertexAttribPointer(0, 3, FLOAT, 0, six, nullptr);
        gl_.EnableVertexAttribArray(1);
        gl_.VertexAttribPointer(1, 3, FLOAT, 0, six, reinterpret_cast<const void*>(3 * sizeof(float)));
        gl_.BindBuffer(ARRAY_BUFFER, shownColors_);
        gl_.BufferData(ARRAY_BUFFER, bytes(m.colors), m.colors.data(), STATIC_DRAW);
        gl_.EnableVertexAttribArray(2);
        gl_.VertexAttribPointer(2, 3, FLOAT, 0, three, nullptr);
        throughArray(shownThrough_, m.translucency);
        textureArray(shownTextures_, m);
        // The triangles: bound with the vertex array, and kept by it.
        gl_.BindBuffer(ELEMENT_ARRAY_BUFFER, shownIndices_);
        gl_.BufferData(ELEMENT_ARRAY_BUFFER, bytes(m.indices), m.indices.data(), STATIC_DRAW);
    } else if (!m.places.empty()) {
        gl_.BufferSubData(ARRAY_BUFFER, 0, bytes(m.places), m.places.data());
    }
    if (m.velocities.empty()) {
        gl_.DisableVertexAttribArray(3);
        gl_.VertexAttrib3f(3, 0.0f, 0.0f, 0.0f);
    } else {
        gl_.BindBuffer(ARRAY_BUFFER, shownVelocities_);
        if (all) gl_.BufferData(ARRAY_BUFFER, bytes(m.velocities), m.velocities.data(), DYNAMIC_DRAW);
        else gl_.BufferSubData(ARRAY_BUFFER, 0, bytes(m.velocities), m.velocities.data());
        gl_.EnableVertexAttribArray(3);
        gl_.VertexAttribPointer(3, 3, FLOAT, 0, three, nullptr);
    }
    gl_.BindVertexArray(0);
    gl_.BindBuffer(ARRAY_BUFFER, 0);
    shownElements_ = static_cast<GLsizei>(m.indices.size());
}

void VolumeRenderer::updateGeometryBounds() {
    Vec3 lo = shownDisplay().lo, hi = shownDisplay().hi;
    for (int a = 0; a < 3; ++a) {
        lo[a] = std::min({lo[a], piecesDisplay().lo[a], instances().lo[a]});
        hi[a] = std::max({hi[a], piecesDisplay().hi[a], instances().hi[a]});
    }
    hasGeoBounds_ = lo.x <= hi.x;
    geoLo_ = lo;
    geoHi_ = hi;
}

bool VolumeRenderer::hasInstances() const {
    for (const InstancedGpu& gpu : instanced_) {
        if (gpu.instances > 0 && gpu.elements > 0) return true;
    }
    return false;
}

void VolumeRenderer::releaseInstanced(InstancedGpu& gpu) {
    if (gpu.vao) gl_.DeleteVertexArrays(1, &gpu.vao);
    for (GLuint b : {gpu.places, gpu.colors, gpu.indices, gpu.placements, gpu.through, gpu.textures}) {
        if (b) gl_.DeleteBuffers(1, &b);
    }
    if (gpu.impostorVao) gl_.DeleteVertexArrays(1, &gpu.impostorVao);
    if (gpu.atlas) gl_.DeleteTextures(1, &gpu.atlas);
    gpu = InstancedGpu();
}

void VolumeRenderer::uploadInstances() {
    auto bytes = [](const auto& v) { return static_cast<GLsizeiptr>(v.size() * sizeof(v[0])); };
    static const std::vector<sim::PreparedGeometry::Prototype> none;
    const std::vector<sim::PreparedGeometry::Prototype>& prototypes = prepared_ ? prepared_->prototypes : none;
    std::vector<InstancedGpu> kept;
    kept.reserve(prototypes.size());
    // A plant at each level of detail, anything else as it is.
    for (const sim::PreparedGeometry::Prototype& p : prototypes) {
        const GeometryPtr& prototype = instances().prototypes[p.which];
        const int level = p.level;
        InstancedGpu gpu;
        const auto was = std::find_if(instanced_.begin(), instanced_.end(), [&](const InstancedGpu& g) {
            return g.prototype == prototype && g.level == level && g.vao;
        });
        if (was != instanced_.end()) {
            gpu = std::move(*was);
            *was = InstancedGpu();
        } else {
            // Its polygons, as prepared -- what stands on its own points
            // made copies of first; thinned for far away.
            gpu.prototype = prototype;
            gpu.level = level;
            const sim::DisplayMesh& m = *p.mesh;
            gl_.GenVertexArrays(1, &gpu.vao);
            GLuint buffers[6] = {};
            gl_.GenBuffers(6, buffers);
            gpu.places = buffers[0];
            gpu.colors = buffers[1];
            gpu.indices = buffers[2];
            gpu.placements = buffers[3];
            gpu.through = buffers[4];
            gpu.textures = buffers[5];
            const GLsizei six = 6 * static_cast<GLsizei>(sizeof(float)), three = 3 * static_cast<GLsizei>(sizeof(float));
            gl_.BindVertexArray(gpu.vao);
            gl_.BindBuffer(ARRAY_BUFFER, gpu.places);
            gl_.BufferData(ARRAY_BUFFER, bytes(m.places), m.places.data(), STATIC_DRAW);
            gl_.EnableVertexAttribArray(0);
            gl_.VertexAttribPointer(0, 3, FLOAT, 0, six, nullptr);
            gl_.EnableVertexAttribArray(1);
            gl_.VertexAttribPointer(1, 3, FLOAT, 0, six, reinterpret_cast<const void*>(3 * sizeof(float)));
            gl_.BindBuffer(ARRAY_BUFFER, gpu.colors);
            gl_.BufferData(ARRAY_BUFFER, bytes(m.colors), m.colors.data(), STATIC_DRAW);
            gl_.EnableVertexAttribArray(2);
            gl_.VertexAttribPointer(2, 3, FLOAT, 0, three, nullptr);
            gl_.DisableVertexAttribArray(3);  // not moving: the velocity everything without its own reads
            throughArray(gpu.through, m.translucency);
            textureArray(gpu.textures, m);
            gl_.BindBuffer(ELEMENT_ARRAY_BUFFER, gpu.indices);
            gl_.BufferData(ELEMENT_ARRAY_BUFFER, bytes(m.indices), m.indices.data(), STATIC_DRAW);
            // Where each instance goes: three vectors of it, one set an instance.
            gl_.BindBuffer(ARRAY_BUFFER, gpu.placements);
            const GLsizei stride = static_cast<GLsizei>(sim::DisplayInstances::kFloats * sizeof(float));
            for (GLuint a = 0; a < 3; ++a) {
                gl_.EnableVertexAttribArray(4 + a);
                gl_.VertexAttribPointer(4 + a, 4, FLOAT, 0, stride, reinterpret_cast<const void*>(size_t{a} * 4 * sizeof(float)));
                gl_.VertexAttribDivisor(4 + a, 1);
            }
            gl_.BindVertexArray(0);
            gpu.elements = static_cast<GLsizei>(m.indices.size());
        }
        gpu.which = p.which;
        gpu.levels = p.levels;
        kept.push_back(std::move(gpu));
    }
    // What no point stands for any more goes.
    for (InstancedGpu& gpu : instanced_) releaseInstanced(gpu);
    instanced_ = std::move(kept);
    placeByDetail();
}

void VolumeRenderer::placeByDetail(bool moved) {
    auto bytes = [](const auto& v) { return static_cast<GLsizeiptr>(v.size() * sizeof(v[0])); };
    const bool sorting = seenView_ != Mat4(0.0f);
    const sim::ViewPlanes planes = sim::viewPlanesOf(seenView_);
    // What each copy is drawn as, worked out for each prototype at once --
    // in parallel, the prototypes being many and their copies few, as a
    // plant's in the wind bent into several shapes -- then sent, in order.
    struct Placed {
        std::vector<float> placements;
        size_t seen = 0;
    };
    std::vector<Placed> placed(instanced_.size());
    // A task a prototype: its levels one after another, sharing them out once.
    std::vector<size_t> groups;
    for (size_t g = 0; g < instanced_.size(); ++g) {
        if (g == 0 || instanced_[g].which != instanced_[g - 1].which) groups.push_back(g);
    }
    groups.push_back(instanced_.size());
    pg::parallelFor(groups.size() - 1, 1, [&](size_t first, size_t last) {
        std::array<std::vector<float>, sim::kDetailLevels> parts;
        size_t parted = static_cast<size_t>(-1);
        for (size_t g = groups[first]; g < groups[last]; ++g) {
            const InstancedGpu& gpu = instanced_[g];
            const std::vector<float>& all = instances().placements[gpu.which];
            const std::vector<float>* placements = &all;
            if (gpu.levels > 1 && detailEyeSet_) {
                // Each copy at the level of detail it looks big enough for.
                if (parted != gpu.which) {
                    parts = sim::placementsByDetail(all, instances().centers[gpu.which], instances().radii[gpu.which],
                                                    detailEye_);
                    parted = gpu.which;
                }
                placements = &parts[static_cast<size_t>(gpu.level)];
            } else if (gpu.level > 0) {
                continue;  // the eye not known yet: all of them in full
            }
            // Those the camera sees first: drawn for it, the rest for the shadows.
            Placed& p = placed[g];
            if (sorting && !placements->empty()) {
                p.seen = sim::seenFirst(*placements, p.placements, instances().centers[gpu.which],
                                        instances().radii[gpu.which], planes);
            } else {
                p.placements = *placements;
                p.seen = p.placements.size() / sim::DisplayInstances::kFloats;
            }
        }
    });
    for (size_t g = 0; g < instanced_.size(); ++g) {
        InstancedGpu& gpu = instanced_[g];
        const std::vector<float>& placements = placed[g].placements;
        gl_.BindBuffer(ARRAY_BUFFER, gpu.placements);
        if (placements.size() > gpu.capacity) {
            gl_.BufferData(ARRAY_BUFFER, bytes(placements), placements.data(), DYNAMIC_DRAW);
            gpu.capacity = placements.size();
        } else if (!placements.empty()) {
            gl_.BufferSubData(ARRAY_BUFFER, 0, bytes(placements), placements.data());
        }
        gpu.instances = static_cast<GLsizei>(placements.size() / sim::DisplayInstances::kFloats);
        gpu.seen = static_cast<GLsizei>(placed[g].seen);
    }
    gl_.BindBuffer(ARRAY_BUFFER, 0);
    if (moved) geoShadowDirty_ = true;
}

void VolumeRenderer::seeFrom(const Vec3& eye) {
    // Again where the eye has moved a little: what it sees close changes.
    const bool moved = !detailEyeSet_ || length(eye - detailEye_) >= 0.1f;
    // ... and where it looks elsewhere: what it sees at all.
    const bool turned = viewProjection_ != seenView_;
    if (!moved && !turned) return;
    if (moved) {
        detailEye_ = eye;
        detailEyeSet_ = true;
    }
    seenView_ = viewProjection_;
    if (instanced_.empty()) return;
    const bool detail = std::any_of(instanced_.begin(), instanced_.end(), [](const InstancedGpu& g) { return g.levels > 1; });
    placeByDetail(moved && detail);
}

void VolumeRenderer::drawInstances(bool shadow) {
    gl_.VertexAttrib3f(3, 0.0f, 0.0f, 0.0f);
    for (const InstancedGpu& gpu : instanced_) {
        if (gpu.instances == 0 || gpu.elements == 0) continue;
        if (!shadow && gpu.atlas && impostorProgram_) continue;  // a billboard: drawImpostors
        // The camera's: those it sees; the sun's: every one, for the shadows
        // cast into the view from outside it.
        const GLsizei count = shadow ? gpu.instances : gpu.seen;
        if (count == 0) continue;
        gl_.BindVertexArray(gpu.vao);
        gl_.DrawElementsInstanced(TRIANGLES, gpu.elements, UNSIGNED_INT, nullptr, count);
    }
    gl_.BindVertexArray(0);
}

void VolumeRenderer::captureImpostor(InstancedGpu& gpu, const InstancedGpu& full, size_t which) {
    const int texels = kImpostorTexels, views = kImpostorViews;
    const Vec3 center = instances().centers[which];
    const float radius = std::max(instances().radii[which], 1e-4f);
    gl_.GenTextures(1, &gpu.atlas);
    gl_.BindTexture(TEXTURE_2D, gpu.atlas);
    gl_.TexImage2D(TEXTURE_2D, 0, static_cast<GLint>(RGBA32F), texels * views, texels, 0, RGBA, FLOAT, nullptr);
    gl_.TexParameteri(TEXTURE_2D, TEXTURE_MIN_FILTER, NEAREST);
    gl_.TexParameteri(TEXTURE_2D, TEXTURE_MAG_FILTER, NEAREST);
    gl_.BindTexture(TEXTURE_2D, 0);
    GLuint fbo = 0, depth = 0;
    gl_.GenRenderbuffers(1, &depth);
    gl_.BindRenderbuffer(RENDERBUFFER, depth);
    gl_.RenderbufferStorage(RENDERBUFFER, DEPTH_COMPONENT24, texels * views, texels);
    gl_.BindRenderbuffer(RENDERBUFFER, 0);
    gl_.GenFramebuffers(1, &fbo);
    gl_.BindFramebuffer(FRAMEBUFFER, fbo);
    gl_.FramebufferTexture2D(FRAMEBUFFER, COLOR_ATTACHMENT0, TEXTURE_2D, gpu.atlas, 0);
    gl_.FramebufferRenderbuffer(FRAMEBUFFER, DEPTH_ATTACHMENT, RENDERBUFFER, depth);
    const GLenum one = COLOR_ATTACHMENT0;
    gl_.DrawBuffers(1, &one);
    const bool complete = gl_.CheckFramebufferStatus(FRAMEBUFFER) == FRAMEBUFFER_COMPLETE;
    if (complete) {
        gl_.Viewport(0, 0, texels * views, texels);
        gl_.ClearColor(0.0f, 0.0f, 0.0f, -1.0f);  // w < 0: nothing there
        gl_.Clear(COLOR_BUFFER_BIT | DEPTH_BUFFER_BIT);
        gl_.Enable(DEPTH_TEST);
        gl_.DepthFunc(LESS);
        gl_.DepthMask(1);
        gl_.Disable(BLEND);
        gl_.Disable(CULL_FACE);
        gl_.UseProgram(geoProgram_);
        placeUninstanced();
        bindPictures(geoProgram_);
        gl_.Uniform1f(location(geoProgram_, "u_frameTime"), 0.0f);
        gl_.Uniform2f(location(geoProgram_, "u_viewport"), static_cast<float>(texels), static_cast<float>(texels));
        gl_.Uniform1f(location(geoProgram_, "u_class"), static_cast<float>(Surface::Geometry));
        gl_.BindVertexArray(full.vao);
        // The plant as it is, not placed as its copies are.
        for (GLuint a = 4; a < 7; ++a) gl_.DisableVertexAttribArray(a);
        for (int k = 0; k < views; ++k) {
            // From the side at k eighths round, square on, the box filling it.
            const float angle = 2.0f * kPi * static_cast<float>(k) / static_cast<float>(views);
            const Vec3 side(std::sin(angle), 0.0f, std::cos(angle));
            const Vec3 eye = center + side * (3.0f * radius);
            const Mat4 view = glm::lookAt(eye, center, Vec3(0.0f, 1.0f, 0.0f));
            const Mat4 proj = glm::ortho(-radius, radius, -radius, radius, 0.5f * radius, 5.0f * radius);
            const Mat4 viewProj = proj * view;
            gl_.Viewport(k * texels, 0, texels, texels);
            gl_.UniformMatrix4fv(location(geoProgram_, "u_viewProj"), 1, 0, glm::value_ptr(viewProj));
            gl_.UniformMatrix4fv(location(geoProgram_, "u_nextViewProj"), 1, 0, glm::value_ptr(viewProj));
            gl_.Uniform3f(location(geoProgram_, "u_eye"), eye.x, eye.y, eye.z);
            gl_.DrawElements(TRIANGLES, full.elements, UNSIGNED_INT, nullptr);
        }
        for (GLuint a = 4; a < 7; ++a) gl_.EnableVertexAttribArray(a);
        gl_.BindVertexArray(0);
        // Its card: a square, and where its copies are.
        if (!impostorQuad_) {
            const float corners[12] = {-1.0f, -1.0f, 1.0f, -1.0f, 1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, -1.0f, 1.0f};
            gl_.GenBuffers(1, &impostorQuad_);
            gl_.BindBuffer(ARRAY_BUFFER, impostorQuad_);
            gl_.BufferData(ARRAY_BUFFER, sizeof corners, corners, STATIC_DRAW);
        }
        gl_.GenVertexArrays(1, &gpu.impostorVao);
        gl_.BindVertexArray(gpu.impostorVao);
        gl_.BindBuffer(ARRAY_BUFFER, impostorQuad_);
        gl_.EnableVertexAttribArray(0);
        gl_.VertexAttribPointer(0, 2, FLOAT, 0, 2 * static_cast<GLsizei>(sizeof(float)), nullptr);
        gl_.BindBuffer(ARRAY_BUFFER, gpu.placements);
        const GLsizei stride = static_cast<GLsizei>(sim::DisplayInstances::kFloats * sizeof(float));
        for (GLuint a = 0; a < 3; ++a) {
            gl_.EnableVertexAttribArray(4 + a);
            gl_.VertexAttribPointer(4 + a, 4, FLOAT, 0, stride, reinterpret_cast<const void*>(size_t{a} * 4 * sizeof(float)));
            gl_.VertexAttribDivisor(4 + a, 1);
        }
        gl_.BindVertexArray(0);
        gl_.BindBuffer(ARRAY_BUFFER, 0);
    } else {
        gl_.DeleteTextures(1, &gpu.atlas);
        gpu.atlas = 0;
    }
    gl_.BindFramebuffer(FRAMEBUFFER, 0);
    gl_.DeleteFramebuffers(1, &fbo);
    gl_.DeleteRenderbuffers(1, &depth);
}

void VolumeRenderer::prepareImpostors() {
    if (!impostorProgram_) return;
    int pictured = 0;
    for (const InstancedGpu& gpu : instanced_) pictured += gpu.atlas ? 1 : 0;
    for (InstancedGpu& gpu : instanced_) {
        // A billboard is pictured when a copy is first far enough for it --
        // no more than kMostImpostors: past them, the plant's mesh.
        if (gpu.levels < 2 || gpu.level != gpu.levels - 1 || gpu.atlas || gpu.pictureTried || gpu.instances == 0) continue;
        if (pictured >= kMostImpostors) break;
        gpu.pictureTried = true;
        const auto full = std::find_if(instanced_.begin(), instanced_.end(), [&](const InstancedGpu& g) {
            return g.prototype == gpu.prototype && g.level == 0 && g.vao;
        });
        if (full == instanced_.end()) continue;
        captureImpostor(gpu, *full, gpu.which);
        pictured += gpu.atlas ? 1 : 0;
    }
}

void VolumeRenderer::drawImpostors(const Vec3& eye) {
    if (!impostorProgram_) return;
    bool any = false;
    for (const InstancedGpu& gpu : instanced_) any = any || (gpu.atlas && gpu.seen > 0);
    if (!any) return;
    gl_.UseProgram(impostorProgram_);
    gl_.UniformMatrix4fv(location(impostorProgram_, "u_viewProj"), 1, 0, glm::value_ptr(viewProjection_));
    gl_.Uniform3f(location(impostorProgram_, "u_eye"), eye.x, eye.y, eye.z);
    gl_.Uniform1i(location(impostorProgram_, "u_views"), kImpostorViews);
    gl_.Uniform1i(location(impostorProgram_, "u_texels"), kImpostorTexels);
    gl_.Uniform1f(location(impostorProgram_, "u_class"), static_cast<float>(Surface::Geometry));
    gl_.Uniform1i(location(impostorProgram_, "u_atlas"), 13);
    gl_.ActiveTexture(TEXTURE0 + 13);
    for (const InstancedGpu& gpu : instanced_) {
        if (!gpu.atlas || gpu.seen == 0) continue;
        const Vec3& c = instances().centers[gpu.which];
        gl_.Uniform3f(location(impostorProgram_, "u_center"), c.x, c.y, c.z);
        gl_.Uniform1f(location(impostorProgram_, "u_radius"), instances().radii[gpu.which]);
        gl_.BindTexture(TEXTURE_2D, gpu.atlas);
        gl_.BindVertexArray(gpu.impostorVao);
        gl_.DrawArraysInstanced(TRIANGLES, 0, 6, gpu.seen);
    }
    gl_.BindVertexArray(0);
    gl_.BindTexture(TEXTURE_2D, 0);
    gl_.ActiveTexture(TEXTURE0);
}

void VolumeRenderer::placeUninstanced() {
    gl_.VertexAttrib4f(4, 0.0f, 0.0f, 0.0f, 1.0f);  // here, as big as it is
    gl_.VertexAttrib4f(5, 0.0f, 0.0f, 0.0f, 1.0f);  // not turned
    gl_.VertexAttrib4f(6, 1.0f, 1.0f, 1.0f, 1.0f);  // its own colour
    gl_.VertexAttrib1f(7, 0.0f);                    // letting no light through, unless it says
    gl_.VertexAttrib4f(8, 0.0f, 0.0f, 0.0f, 0.0f);  // no pictures, unless it has
    gl_.VertexAttrib1f(9, -1.0f);
}

void VolumeRenderer::textureArray(GLuint buffer, const sim::DisplayMesh& mesh) {
    if (mesh.textures.empty()) {
        gl_.DisableVertexAttribArray(8);
        gl_.DisableVertexAttribArray(9);
        return;
    }
    // Its pictures' layers in place of its own numbers for them.
    std::vector<int> layer(mesh.pictures.size());
    for (size_t i = 0; i < layer.size(); ++i) layer[i] = layerOf(mesh.pictures[i]);
    std::vector<float> data = mesh.textures;
    for (size_t v = 4; v < data.size(); v += 5) {
        const int own = static_cast<int>(data[v]);
        data[v] = own >= 0 && static_cast<size_t>(own) < layer.size() ? static_cast<float>(layer[static_cast<size_t>(own)]) : -1.0f;
    }
    gl_.BindBuffer(ARRAY_BUFFER, buffer);
    gl_.BufferData(ARRAY_BUFFER, static_cast<GLsizeiptr>(data.size() * sizeof(float)), data.data(), STATIC_DRAW);
    const GLsizei five = 5 * static_cast<GLsizei>(sizeof(float));
    gl_.EnableVertexAttribArray(8);
    gl_.VertexAttribPointer(8, 4, FLOAT, 0, five, nullptr);
    gl_.EnableVertexAttribArray(9);
    gl_.VertexAttribPointer(9, 1, FLOAT, 0, five, reinterpret_cast<const void*>(4 * sizeof(float)));
}

int VolumeRenderer::layerOf(const sim::DisplayPicture& picture) {
    const auto known = std::find(layers_.begin(), layers_.end(), picture);
    if (known != layers_.end()) return static_cast<int>(known - layers_.begin());
    if (static_cast<int>(layers_.size()) >= kPictureLayers) return -1;
    std::shared_ptr<const sim::PictureBytes> bytes = prepared_ ? prepared_->bytesOf(picture) : nullptr;
    if (!bytes) bytes = sim::pictureBytes(picture, kPictureSize);
    if (!bytes) return -1;
    layers_.push_back(picture);
    layerBytes_.push_back(std::move(bytes));
    return static_cast<int>(layers_.size()) - 1;
}

void VolumeRenderer::bindPictures(GLuint program) {
    const int count = static_cast<int>(layers_.size());
    if (count > picturesMade_) {
        // Grown: made again, every layer, with its smaller copies.
        const int size = kPictureSize;
        if (!picturesTex_) gl_.GenTextures(1, &picturesTex_);
        if (!normalMapsTex_) gl_.GenTextures(1, &normalMapsTex_);
        gl_.PixelStorei(UNPACK_ALIGNMENT, 1);
        for (int which = 0; which < 2; ++which) {
            gl_.BindTexture(TEXTURE_2D_ARRAY, which == 0 ? picturesTex_ : normalMapsTex_);
            gl_.TexImage3D(TEXTURE_2D_ARRAY, 0, static_cast<GLint>(which == 0 ? SRGB8_ALPHA8 : RGBA8), size, size, count, 0,
                           RGBA, UNSIGNED_BYTE, nullptr);
            for (int l = 0; l < count; ++l) {
                const sim::PictureBytes& made = *layerBytes_[static_cast<size_t>(l)];
                const std::vector<uint8_t>& bytes = which == 0 ? made.color : made.normal;
                gl_.TexSubImage3D(TEXTURE_2D_ARRAY, 0, 0, 0, l, size, size, 1, RGBA, UNSIGNED_BYTE, bytes.data());
            }
            gl_.GenerateMipmap(TEXTURE_2D_ARRAY);
            gl_.TexParameteri(TEXTURE_2D_ARRAY, TEXTURE_MIN_FILTER, LINEAR_MIPMAP_LINEAR);
            gl_.TexParameteri(TEXTURE_2D_ARRAY, TEXTURE_MAG_FILTER, LINEAR);
            gl_.TexParameteri(TEXTURE_2D_ARRAY, TEXTURE_WRAP_S, REPEAT);
            gl_.TexParameteri(TEXTURE_2D_ARRAY, TEXTURE_WRAP_T, REPEAT);
        }
        gl_.BindTexture(TEXTURE_2D_ARRAY, 0);
        picturesMade_ = count;
    }
    gl_.ActiveTexture(TEXTURE0 + 15);
    gl_.BindTexture(TEXTURE_2D_ARRAY, count > 0 ? picturesTex_ : 0);
    gl_.ActiveTexture(TEXTURE0 + 14);
    gl_.BindTexture(TEXTURE_2D_ARRAY, count > 0 ? normalMapsTex_ : 0);
    gl_.ActiveTexture(TEXTURE0);
    gl_.Uniform1i(location(program, "u_pictures"), 15);
    gl_.Uniform1i(location(program, "u_normalMaps"), 14);
    std::array<float, 4 * kPictureLayers> looks{};
    for (int l = 0; l < count; ++l) {
        const sim::DisplayPicture& p = layers_[static_cast<size_t>(l)];
        const int has = (p.tint ? 1 : 0) | (p.normal.empty() ? 0 : 2) | (p.alpha.empty() ? 0 : 4);
        looks[static_cast<size_t>(l) * 4] = p.mean.x;
        looks[static_cast<size_t>(l) * 4 + 1] = p.mean.y;
        looks[static_cast<size_t>(l) * 4 + 2] = p.mean.z;
        looks[static_cast<size_t>(l) * 4 + 3] = static_cast<float>(has);
    }
    gl_.Uniform4fv(location(program, "u_pictureLook"), kPictureLayers, looks.data());
}

void VolumeRenderer::throughArray(GLuint buffer, const std::vector<float>& translucency) {
    if (translucency.empty()) {
        gl_.DisableVertexAttribArray(7);
        return;
    }
    gl_.BindBuffer(ARRAY_BUFFER, buffer);
    gl_.BufferData(ARRAY_BUFFER, static_cast<GLsizeiptr>(translucency.size() * sizeof(float)), translucency.data(),
                   STATIC_DRAW);
    gl_.EnableVertexAttribArray(7);
    gl_.VertexAttribPointer(7, 1, FLOAT, 0, static_cast<GLsizei>(sizeof(float)), nullptr);
}

void VolumeRenderer::setPieces(const GeometryPtr& pieces) {
    if (pieces == pieces_) return;
    pieces_ = pieces;
    auto made = std::make_shared<sim::PreparedBodies>();
    if (pieces) made->display = sim::displayOf(*pieces);
    preparedPieces_ = std::move(made);
    uploadGeometry();
}

void VolumeRenderer::setPreparedPieces(std::shared_ptr<const sim::PreparedBodies> pieces) {
    if (pieces == preparedPieces_ && !pieces_) return;
    pieces_ = nullptr;
    preparedPieces_ = std::move(pieces);
    uploadGeometry();
}

const sim::DisplayGeometry& VolumeRenderer::piecesDisplay() const {
    static const sim::DisplayGeometry none;
    return preparedPieces_ ? preparedPieces_->display : none;
}

void VolumeRenderer::uploadGeometry() {
    // The displayed geometry's and the pieces', one after the other in each
    // buffer -- sent as they are, not put together first.
    const sim::DisplayGeometry& s = shownDisplay();
    const sim::DisplayGeometry& p = piecesDisplay();
    updateGeometryBounds();
    // Each pair of arrays into its buffer, with the layout of its attributes: {location, floats}.
    auto upload = [&](GLuint& vao, GLuint& buffer, const std::vector<float>& first, const std::vector<float>& second,
                      std::initializer_list<std::pair<int, int>> layout) {
        if (!vao) {
            gl_.GenVertexArrays(1, &vao);
            gl_.GenBuffers(1, &buffer);
        }
        int stride = 0;
        for (const auto& [where, floats] : layout) stride += floats;
        gl_.BindVertexArray(vao);
        gl_.BindBuffer(ARRAY_BUFFER, buffer);
        const auto a = static_cast<GLsizeiptr>(first.size() * sizeof(float));
        const auto b = static_cast<GLsizeiptr>(second.size() * sizeof(float));
        gl_.BufferData(ARRAY_BUFFER, a + b, nullptr, STATIC_DRAW);
        if (a > 0) gl_.BufferSubData(ARRAY_BUFFER, 0, a, first.data());
        if (b > 0) gl_.BufferSubData(ARRAY_BUFFER, a, b, second.data());
        int offset = 0;
        for (const auto& [where, floats] : layout) {
            gl_.EnableVertexAttribArray(static_cast<GLuint>(where));
            gl_.VertexAttribPointer(static_cast<GLuint>(where), floats, FLOAT, 0, stride * static_cast<GLsizei>(sizeof(float)),
                                    reinterpret_cast<const void*>(static_cast<size_t>(offset) * sizeof(float)));
            offset += floats;
        }
        gl_.BindVertexArray(0);
        gl_.BindBuffer(ARRAY_BUFFER, 0);
    };
    upload(geoVao_, geoBuffer_, s.triangles, p.triangles, {{0, 3}, {1, 3}, {2, 3}});
    // The corners' velocities, on attribute 3 -- 0 for the part that has none.
    const size_t corners = (s.triangles.size() + p.triangles.size()) / 9;
    std::vector<float> velocities;
    if (!s.velocities.empty() || !p.velocities.empty()) {
        velocities = s.velocities;
        velocities.resize(s.triangles.size() / 3, 0.0f);
        velocities.insert(velocities.end(), p.velocities.begin(), p.velocities.end());
        velocities.resize(corners * 3, 0.0f);
    }
    gl_.BindVertexArray(geoVao_);
    if (velocities.empty()) {
        gl_.DisableVertexAttribArray(3);
        gl_.VertexAttrib3f(3, 0.0f, 0.0f, 0.0f);
    } else {
        if (!geoVelocityBuffer_) gl_.GenBuffers(1, &geoVelocityBuffer_);
        gl_.BindBuffer(ARRAY_BUFFER, geoVelocityBuffer_);
        gl_.BufferData(ARRAY_BUFFER, static_cast<GLsizeiptr>(velocities.size() * sizeof(float)), velocities.data(), STATIC_DRAW);
        gl_.EnableVertexAttribArray(3);
        gl_.VertexAttribPointer(3, 3, FLOAT, 0, 3 * static_cast<GLsizei>(sizeof(float)), nullptr);
        gl_.BindBuffer(ARRAY_BUFFER, 0);
    }
    gl_.BindVertexArray(0);
    upload(dotVao_, dotBuffer_, s.dots, p.dots, {{0, 3}, {1, 3}, {2, 1}});
    upload(curveVao_, curveBuffer_, s.lines, p.lines, {{0, 3}, {1, 4}});
    upload(glassVao_, glassBuffer_, s.glass, p.glass, {{0, 3}, {1, 3}, {2, 3}, {3, 1}, {4, 3}});
    glassVertices_ = static_cast<GLsizei>((s.glassCount() + p.glassCount()) * 3);
    geoVertices_ = static_cast<GLsizei>(corners);
    geoShadowDirty_ = true;
    dots_ = static_cast<GLsizei>(s.dotCount() + p.dotCount());
    gritDots_ = static_cast<GLsizei>(p.dotCount());
    curveVertices_ = static_cast<GLsizei>((s.lines.size() + p.lines.size()) / 7);
}

void VolumeRenderer::updateGeoShadow(const Vec3& light) {
    if (!geoShadowProgram_ || (geoVertices_ == 0 && shownElements_ == 0 && !hasInstances()) || !hasGeoBounds_) {
        hasGeoShadow_ = false;
        return;
    }
    if (!geoShadowDirty_ && hasGeoShadow_ && light == geoShadowLight_) return;
    geoShadowDirty_ = false;
    geoShadowLight_ = light;
    const int size = kGeoShadowSize;
    if (!geoShadowFbo_) {
        gl_.GenTextures(1, &geoShadowTex_);
        gl_.BindTexture(TEXTURE_2D, geoShadowTex_);
        gl_.TexImage2D(TEXTURE_2D, 0, static_cast<GLint>(0x822E), size, size, 0, RED, FLOAT, nullptr);  // R32F
        gl_.TexParameteri(TEXTURE_2D, TEXTURE_MIN_FILTER, 0x2600);  // NEAREST: the samples filter it
        gl_.TexParameteri(TEXTURE_2D, TEXTURE_MAG_FILTER, 0x2600);
        gl_.TexParameteri(TEXTURE_2D, TEXTURE_WRAP_S, 0x812F);      // CLAMP_TO_EDGE
        gl_.TexParameteri(TEXTURE_2D, TEXTURE_WRAP_T, 0x812F);
        gl_.BindTexture(TEXTURE_2D, 0);
        gl_.GenRenderbuffers(1, &geoShadowDepth_);
        gl_.BindRenderbuffer(RENDERBUFFER, geoShadowDepth_);
        gl_.RenderbufferStorage(RENDERBUFFER, DEPTH_COMPONENT24, size, size);
        gl_.BindRenderbuffer(RENDERBUFFER, 0);
        gl_.GenFramebuffers(1, &geoShadowFbo_);
        gl_.BindFramebuffer(FRAMEBUFFER, geoShadowFbo_);
        gl_.FramebufferTexture2D(FRAMEBUFFER, COLOR_ATTACHMENT0, TEXTURE_2D, geoShadowTex_, 0);
        gl_.FramebufferRenderbuffer(FRAMEBUFFER, DEPTH_ATTACHMENT, RENDERBUFFER, geoShadowDepth_);
        const bool complete = gl_.CheckFramebufferStatus(FRAMEBUFFER) == FRAMEBUFFER_COMPLETE;
        gl_.BindFramebuffer(FRAMEBUFFER, 0);
        if (!complete) {
            gl_.DeleteFramebuffers(1, &geoShadowFbo_);
            geoShadowFbo_ = 0;
            geoShadowProgram_ = 0;  // no shadows of geometry on this GPU
            hasGeoShadow_ = false;
            return;
        }
    }
    // The sun's view: along its light, square onto the box round the geometry.
    const Vec3 f = -light;
    const Vec3 up = std::fabs(f.y) > 0.99f ? Vec3(1.0f, 0.0f, 0.0f) : Vec3(0.0f, 1.0f, 0.0f);
    const Vec3 sx = normalize(cross(f, up)), sy = cross(sx, f);
    Vec3 lo(1e30f, 1e30f, 1e30f), hi(-1e30f, -1e30f, -1e30f);
    for (int i = 0; i < 8; ++i) {
        const Vec3 c(i & 1 ? geoHi_.x : geoLo_.x, i & 2 ? geoHi_.y : geoLo_.y, i & 4 ? geoHi_.z : geoLo_.z);
        const Vec3 q(dot(c, sx), dot(c, sy), dot(c, f));
        for (int a = 0; a < 3; ++a) {
            lo[a] = std::min(lo[a], q[a]);
            hi[a] = std::max(hi[a], q[a]);
        }
    }
    const float margin = 0.02f * std::max({hi.x - lo.x, hi.y - lo.y, 1e-3f});
    for (int a = 0; a < 3; ++a) {
        lo[a] -= margin;
        hi[a] += margin;
    }
    const float ax = 2.0f / (hi.x - lo.x), ay = 2.0f / (hi.y - lo.y), az = 2.0f / (hi.z - lo.z);
    // Rows: the sun's view axes, scaled to the box -- GLM's columns index first.
    Mat4 m(0.0f);
    m[0][0] = sx.x * ax; m[1][0] = sx.y * ax; m[2][0] = sx.z * ax; m[3][0] = -lo.x * ax - 1.0f;
    m[0][1] = sy.x * ay; m[1][1] = sy.y * ay; m[2][1] = sy.z * ay; m[3][1] = -lo.y * ay - 1.0f;
    m[0][2] = f.x * az;  m[1][2] = f.y * az;  m[2][2] = f.z * az;  m[3][2] = -lo.z * az - 1.0f;
    m[3][3] = 1.0f;
    lightViewProj_ = m;
    // Two texels of it, as a depth: what a surface may be off its own triangles.
    const float texel = std::max(hi.x - lo.x, hi.y - lo.y) / static_cast<float>(size);
    geoShadowBias_ = 2.5f * texel / (hi.z - lo.z) + 1e-4f;
    // And a surface looks its shadow up a texel and a half off itself: past
    // the texels it fills itself, however the sun grazes it.
    geoShadowLift_ = 1.5f * texel;

    gl_.BindFramebuffer(FRAMEBUFFER, geoShadowFbo_);
    gl_.Viewport(0, 0, size, size);
    gl_.ColorMask(1, 1, 1, 1);
    gl_.DepthMask(1);
    gl_.ClearColor(1.0f, 1.0f, 1.0f, 1.0f);  // nothing in the way: as far as can be
    gl_.Clear(COLOR_BUFFER_BIT | DEPTH_BUFFER_BIT);
    gl_.Enable(DEPTH_TEST);
    gl_.DepthFunc(LESS);
    gl_.Disable(BLEND);
    gl_.Disable(CULL_FACE);
    gl_.UseProgram(geoShadowProgram_);
    gl_.UniformMatrix4fv(location(geoShadowProgram_, "u_lightViewProj"), 1, 0, glm::value_ptr(lightViewProj_));
    bindPictures(geoShadowProgram_);
    placeUninstanced();
    if (shownElements_ > 0) {
        gl_.BindVertexArray(shownVao_);
        gl_.DrawElements(TRIANGLES, shownElements_, UNSIGNED_INT, nullptr);
    }
    drawInstances(true);
    if (geoVertices_ > 0) {
        gl_.BindVertexArray(geoVao_);
        gl_.DrawArrays(TRIANGLES, 0, geoVertices_);
    }
    gl_.BindVertexArray(0);
    gl_.UseProgram(0);
    gl_.BindFramebuffer(FRAMEBUFFER, 0);
    hasGeoShadow_ = true;
}

bool VolumeRenderer::geometryBounds(Vec3& lo, Vec3& hi) const {
    if ((!geometry_ && !preparedPieces_) || !hasGeoBounds_) return false;
    lo = geoLo_;
    hi = geoHi_;
    return true;
}

void VolumeRenderer::drawGeometry(int width, int height) {
    if ((dots_ == 0 || !dotProgram_) && (curveVertices_ == 0 || !lineProgram_)) return;
    gl_.Enable(DEPTH_TEST);
    gl_.DepthFunc(LEQUAL);
    if (dots_ > 0 && dotProgram_) {
        const sim::Look& s = look;
        Vec3 f, r, u;
        orbit.axes(f, r, u);
        const Vec3 light = normalize(s.lightDirection());
        gl_.DepthMask(1);
        gl_.Enable(PROGRAM_POINT_SIZE);
        gl_.UseProgram(dotProgram_);
        gl_.UniformMatrix4fv(location(dotProgram_, "u_viewProj"), 1, 0, glm::value_ptr(viewProjection_));
        const float tanHalf = std::tan(orbit.fovY * kPi / 360.0f);
        gl_.Uniform1f(location(dotProgram_, "u_pixelsPerUnit"), 0.5f * static_cast<float>(height) / tanHalf);
        gl_.Uniform1f(location(dotProgram_, "u_dot"), std::max(3.0f, 3.0f * static_cast<float>(height) / 700.0f));
        gl_.Uniform3f(location(dotProgram_, "u_lightView"), dot(light, r), dot(light, u), -dot(light, f));
        gl_.Uniform3f(location(dotProgram_, "u_light"), s.lightColor.x * s.lightIntensity, s.lightColor.y * s.lightIntensity,
                      s.lightColor.z * s.lightIntensity);
        gl_.Uniform3f(location(dotProgram_, "u_sky"), s.skyColor.x * s.skyIntensity, s.skyColor.y * s.skyIntensity,
                      s.skyColor.z * s.skyIntensity);
        gl_.Uniform1f(location(dotProgram_, "u_exposure"), s.exposure);
        gl_.Uniform1i(location(dotProgram_, "u_linear"), passes.on ? 1 : 0);
        // Through the smoke: the fields and the sunlight in them, where there is gas.
        gl_.Uniform1i(location(dotProgram_, "u_hasGas"), hasFrame_ ? 1 : 0);
        const Vec3 lo = domain_.origin(), size = domain_.size();
        const Vec3 eye = orbit.eye();
        gl_.Uniform3f(location(dotProgram_, "u_boxMin"), lo.x, lo.y, lo.z);
        gl_.Uniform3f(location(dotProgram_, "u_boxSize"), size.x, size.y, size.z);
        gl_.Uniform3f(location(dotProgram_, "u_texel"), 1.0f / static_cast<float>(domain_.cells[0]),
                      1.0f / static_cast<float>(domain_.cells[1]), 1.0f / static_cast<float>(domain_.cells[2]));
        gl_.Uniform3f(location(dotProgram_, "u_eye"), eye[0], eye[1], eye[2]);
        gl_.Uniform1f(location(dotProgram_, "u_extinction"), s.smokeDensity);
        gl_.Uniform1f(location(dotProgram_, "u_steamExtinction"), s.steamDensity);
        gl_.Uniform1f(location(dotProgram_, "u_occlusion"), s.occlusion);
        gl_.ActiveTexture(TEXTURE0);
        gl_.BindTexture(TEXTURE_3D, fields_);
        gl_.Uniform1i(location(dotProgram_, "u_fields"), 0);
        gl_.ActiveTexture(TEXTURE1);
        gl_.BindTexture(TEXTURE_3D, light_);
        gl_.Uniform1i(location(dotProgram_, "u_sunlight"), 1);
        // A dot deep in the smoke is as good as gone: blended over what the
        // smoke drew there.
        gl_.Enable(BLEND);
        gl_.BlendFunc(SRC_ALPHA, ONE_MINUS_SRC_ALPHA);
        gl_.BindVertexArray(dotVao_);
        // The displayed geometry's points as balls; the pieces' -- their
        // grit -- as chips of stone.
        const GLsizei balls = dots_ - gritDots_;
        gl_.Uniform1i(location(dotProgram_, "u_chips"), 0);
        if (balls > 0) gl_.DrawArrays(POINTS, 0, balls);
        gl_.Uniform1i(location(dotProgram_, "u_chips"), 1);
        if (gritDots_ > 0) gl_.DrawArrays(POINTS, balls, gritDots_);
        gl_.Disable(BLEND);
        gl_.Disable(PROGRAM_POINT_SIZE);
    }
    if (curveVertices_ > 0 && lineProgram_) {
        gl_.DepthMask(0);
        gl_.Enable(BLEND);
        gl_.BlendFunc(SRC_ALPHA, ONE_MINUS_SRC_ALPHA);
        gl_.UseProgram(lineProgram_);
        gl_.UniformMatrix4fv(location(lineProgram_, "u_viewProj"), 1, 0, glm::value_ptr(viewProjection_));
        gl_.BindVertexArray(curveVao_);
        gl_.DrawArrays(LINES, 0, curveVertices_);
        gl_.Disable(BLEND);
    }
    gl_.BindVertexArray(0);
    gl_.DepthMask(1);
    (void)width;
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
    linesBox_ = Box();
    for (size_t i = 0; i < lines.vertices.size(); ++i) {
        const float* p = lines.vertices[i].position;
        grow(linesBox_.lo, linesBox_.hi, Vec3(p[0], p[1], p[2]), i == 0);
    }
}

void Overlay::dot(const Vec3& p, const Vec4& color, float pixels, const Vec3& normal) {
    dots.insert(dots.end(), {p.x, p.y, p.z, color.x, color.y, color.z, color.w, pixels, normal.x, normal.y, normal.z});
}

void Overlay::line(const Vec3& a, const Vec3& b, const Vec4& color) {
    lines.insert(lines.end(), {a.x, a.y, a.z, color.x, color.y, color.z, color.w, b.x, b.y, b.z, color.x, color.y, color.z, color.w});
}

void Overlay::wideLine(const Vec3& a, const Vec3& b, const Vec4& color, float pixels) {
    wide.insert(wide.end(), {a.x, a.y, a.z, color.x, color.y, color.z, color.w, pixels, b.x, b.y, b.z, color.x, color.y,
                             color.z, color.w, pixels});
}

void Overlay::face(const Vec3& a, const Vec3& b, const Vec3& c, const Vec4& color) { face(a, b, c, color, color, color); }

void Overlay::face(const Vec3& a, const Vec3& b, const Vec3& c, const Vec4& ca, const Vec4& cb, const Vec4& cc) {
    faces.insert(faces.end(), {a.x, a.y, a.z, ca.x, ca.y, ca.z, ca.w, b.x, b.y, b.z, cb.x, cb.y, cb.z, cb.w, c.x, c.y, c.z,
                               cc.x, cc.y, cc.z, cc.w});
}

void VolumeRenderer::setOverlay(const Overlay& overlay, int layer) {
    if (layer < 0 || layer >= kOverlayLayers) return;
    Box& box = overlayBox_[layer];
    box = Box();
    bool first = true;
    for (const auto& [data, stride] : {std::pair{&overlay.faces, size_t(7)}, std::pair{&overlay.lines, size_t(7)},
                                       std::pair{&overlay.dots, size_t(11)}, std::pair{&overlay.wide, size_t(8)}}) {
        grow(box.lo, box.hi, *data, stride, first);
        first = first && data->empty();
    }
    const Overlay::Marks& marks = overlay.marks;
    const size_t markPoints = marks.points ? marks.points->size() : 0;
    for (size_t i = 0; i < markPoints; ++i, first = false) grow(box.lo, box.hi, (*marks.points)[i], first);
    setMarks(marks, layer);
    GLuint* vaos = overlayVao_[layer];
    GLuint* buffers = overlayBuffer_[layer];
    GLsizei* counts = overlayCount_[layer];
    // The wide lines as the triangles they are drawn as: six corners each,
    // every corner knowing the line's other end and its side.
    std::vector<float> wide;
    wide.reserve(overlay.wide.size() / 16 * 6 * 12);
    for (size_t i = 0; i + 16 <= overlay.wide.size(); i += 16) {
        const float* a = &overlay.wide[i];
        const float* b = &overlay.wide[i + 8];
        auto corner = [&](const float* end, const float* other, float side) {
            wide.insert(wide.end(), {end[0], end[1], end[2], other[0], other[1], other[2], side, end[7], end[3], end[4],
                                     end[5], end[6]});
        };
        // Across b the other way round is the same side as across a.
        corner(a, b, -1.0f);
        corner(a, b, 1.0f);
        corner(b, a, -1.0f);
        corner(a, b, -1.0f);
        corner(b, a, -1.0f);
        corner(b, a, 1.0f);
    }
    const std::vector<float>* data[4] = {&overlay.faces, &overlay.lines, &overlay.dots, &wide};
    const int floats[4] = {7, 7, 11, 12};
    for (int k = 0; k < 4; ++k) {
        if (!vaos[k]) {
            if (data[k]->empty()) continue;
            gl_.GenVertexArrays(1, &vaos[k]);
            gl_.GenBuffers(1, &buffers[k]);
            gl_.BindVertexArray(vaos[k]);
            gl_.BindBuffer(ARRAY_BUFFER, buffers[k]);
            const GLsizei stride = static_cast<GLsizei>(floats[k] * sizeof(float));
            auto attribute = [&](GLuint index, GLint size, int offset) {
                gl_.EnableVertexAttribArray(index);
                gl_.VertexAttribPointer(index, size, FLOAT, 0, stride,
                                        reinterpret_cast<const void*>(static_cast<size_t>(offset) * sizeof(float)));
            };
            if (k < 2) {  // position, colour
                attribute(0, 3, 0);
                attribute(1, 4, 3);
            } else if (k == 2) {  // position, colour, size, normal
                attribute(0, 3, 0);
                attribute(1, 4, 3);
                attribute(2, 1, 7);
                attribute(3, 3, 8);
            } else {  // position, the other end, side, width, colour
                attribute(0, 3, 0);
                attribute(1, 3, 3);
                attribute(2, 1, 6);
                attribute(3, 1, 7);
                attribute(4, 4, 8);
            }
            gl_.BindVertexArray(0);
        }
        gl_.BindBuffer(ARRAY_BUFFER, buffers[k]);
        gl_.BufferData(ARRAY_BUFFER, static_cast<GLsizeiptr>(data[k]->size() * sizeof(float)), data[k]->data(), STATIC_DRAW);
        counts[k] = static_cast<GLsizei>(data[k]->size() / static_cast<size_t>(floats[k]));
    }
    gl_.BindBuffer(ARRAY_BUFFER, 0);
}

void VolumeRenderer::setMarks(const Overlay::Marks& marks, int layer) {
    const size_t n = marks.points ? marks.points->size() : 0;
    const bool normals = marks.normals && marks.normals->size() == n;
    const size_t edges = marks.edges ? marks.edges->size() : 0;
    marksEnds_[layer] = 0;
    marksDots_[layer] = 0;
    if (n == 0) return;
    GLuint* buffers = marksBuffer_[layer];
    if (!marksVao_[layer]) {
        gl_.GenVertexArrays(1, &marksVao_[layer]);
        gl_.GenBuffers(3, buffers);
    }
    // The points shared by the wire and the dots; the colour and the size
    // the same for all, given as constant attributes when drawn.
    gl_.BindVertexArray(marksVao_[layer]);
    gl_.BindBuffer(ARRAY_BUFFER, buffers[0]);
    gl_.BufferData(ARRAY_BUFFER, static_cast<GLsizeiptr>(n * sizeof(Vec3)), marks.points->data(), STATIC_DRAW);
    gl_.EnableVertexAttribArray(0);
    gl_.VertexAttribPointer(0, 3, FLOAT, 0, sizeof(Vec3), nullptr);
    gl_.DisableVertexAttribArray(1);
    gl_.DisableVertexAttribArray(2);
    if (normals) {
        gl_.BindBuffer(ARRAY_BUFFER, buffers[1]);
        gl_.BufferData(ARRAY_BUFFER, static_cast<GLsizeiptr>(n * sizeof(Vec3)), marks.normals->data(), STATIC_DRAW);
        gl_.EnableVertexAttribArray(3);
        gl_.VertexAttribPointer(3, 3, FLOAT, 0, sizeof(Vec3), nullptr);
    } else {
        gl_.DisableVertexAttribArray(3);
    }
    // The edges' ends, two indices each.
    std::vector<uint32_t> ends;
    ends.reserve(2 * edges);
    for (size_t e = 0; e < edges; ++e) {
        const auto& [a, b] = (*marks.edges)[e];
        if (a >= n || b >= n) continue;
        ends.push_back(a);
        ends.push_back(b);
    }
    gl_.BindBuffer(ELEMENT_ARRAY_BUFFER, buffers[2]);
    gl_.BufferData(ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(ends.size() * sizeof(uint32_t)), ends.data(), STATIC_DRAW);
    gl_.BindVertexArray(0);
    gl_.BindBuffer(ARRAY_BUFFER, 0);
    marksEnds_[layer] = static_cast<GLsizei>(ends.size());
    marksDots_[layer] = marks.dotPixels > 0.0f ? static_cast<GLsizei>(n) : 0;
    marksWire_[layer] = marks.wireColor;
    marksDot_[layer] = marks.dotColor;
    marksPixels_[layer] = marks.dotPixels;
}

void VolumeRenderer::drawOverlay(int width, int height, const Vec3& eye) {
    GLsizei any = 0;
    for (int l = 0; l < kOverlayLayers; ++l) {
        for (int k = 0; k < 4; ++k) any += overlayCount_[l][k];
        any += marksEnds_[l] + marksDots_[l];
    }
    if (!overlayProgram_ || any == 0) return;
    gl_.Enable(DEPTH_TEST);
    gl_.DepthMask(0);
    gl_.Enable(BLEND);
    gl_.BlendFunc(SRC_ALPHA, ONE_MINUS_SRC_ALPHA);
    float fade = 1.0f;
    auto use = [&](GLuint program, float pull) {
        gl_.UseProgram(program);
        gl_.UniformMatrix4fv(location(program, "u_viewProj"), 1, 0, glm::value_ptr(viewProjection_));
        gl_.Uniform3f(location(program, "u_eye"), eye.x, eye.y, eye.z);
        gl_.Uniform1f(location(program, "u_pull"), pull);
        gl_.Uniform1f(location(program, "u_fade"), fade);
    };
    auto layers = [&]() {
        for (int l = 0; l < kOverlayLayers; ++l) {
            const GLsizei* counts = overlayCount_[l];
            // Faces first, then the lines over them, the dots over those.
            if (counts[0] > 0) {
                use(overlayProgram_, 0.002f);
                gl_.BindVertexArray(overlayVao_[l][0]);
                gl_.DrawArrays(TRIANGLES, 0, counts[0]);
            }
            if (counts[1] > 0) {
                use(overlayProgram_, 0.003f);
                gl_.BindVertexArray(overlayVao_[l][1]);
                gl_.DrawArrays(LINES, 0, counts[1]);
            }
            if (marksEnds_[l] > 0) {
                use(overlayProgram_, 0.003f);
                const Vec4& c = marksWire_[l];
                gl_.BindVertexArray(marksVao_[l]);
                gl_.VertexAttrib4f(1, c.x, c.y, c.z, c.w);
                gl_.DrawElements(LINES, marksEnds_[l], UNSIGNED_INT, nullptr);
            }
            if (counts[3] > 0) {
                use(overlayWideProgram_, 0.0035f);
                gl_.Uniform2f(location(overlayWideProgram_, "u_viewport"), static_cast<float>(width),
                              static_cast<float>(height));
                gl_.BindVertexArray(overlayVao_[l][3]);
                gl_.DrawArrays(TRIANGLES, 0, counts[3]);
            }
            if (counts[2] > 0) {
                use(overlayDotProgram_, 0.003f);
                gl_.Uniform1f(location(overlayDotProgram_, "u_pixel"),
                              2.0f * std::tan(orbit.fovY * kPi / 360.0f) / static_cast<float>(std::max(height, 1)));
                gl_.Enable(PROGRAM_POINT_SIZE);
                gl_.BindVertexArray(overlayVao_[l][2]);
                gl_.DrawArrays(POINTS, 0, counts[2]);
                gl_.Disable(PROGRAM_POINT_SIZE);
            }
            if (marksDots_[l] > 0) {
                use(overlayDotProgram_, 0.003f);
                gl_.Uniform1f(location(overlayDotProgram_, "u_pixel"),
                              2.0f * std::tan(orbit.fovY * kPi / 360.0f) / static_cast<float>(std::max(height, 1)));
                const Vec4& c = marksDot_[l];
                gl_.BindVertexArray(marksVao_[l]);
                gl_.VertexAttrib4f(1, c.x, c.y, c.z, c.w);
                gl_.VertexAttrib1f(2, marksPixels_[l]);
                gl_.VertexAttrib3f(3, 0.0f, 0.0f, 0.0f);  // where the points have no normals
                gl_.Enable(PROGRAM_POINT_SIZE);
                gl_.DrawArrays(POINTS, 0, marksDots_[l]);
                gl_.Disable(PROGRAM_POINT_SIZE);
            }
        }
    };
    // What the surface hides, first and faint, when it is asked for; then
    // what it does not.
    if (overlayHidden_ > 0.0f) {
        gl_.DepthFunc(GREATER);
        fade = overlayHidden_;
        layers();
    }
    gl_.DepthFunc(LEQUAL);
    fade = 1.0f;
    layers();
    gl_.BindVertexArray(0);
    gl_.Disable(BLEND);
    gl_.DepthMask(1);
}

void VolumeRenderer::setSceneUniforms(GLuint program) {
    // The solids, a row of six texels each (kCommon).
    const int n = static_cast<int>(solids_.size());
    std::vector<float> rows(static_cast<size_t>(24 * std::max(n, 1)), 0.0f);
    for (int i = 0; i < n; ++i) {
        const sim::Solid& solid = solids_[static_cast<size_t>(i)];
        const sim::ShapeInstance s = solid.body.instance();
        const Vec3& h = s.half();
        auto put = [&](int texel, const Vec3& v, float w) {
            float* to = rows.data() + 24 * i + 4 * texel;
            to[0] = v.x;
            to[1] = v.y;
            to[2] = v.z;
            to[3] = w;
        };
        put(0, s.center(), static_cast<float>(s.shape()));
        put(1, s.turn().x, h.x);
        put(2, s.turn().y, h.y);
        put(3, s.turn().z, h.z);
        const int node = solid.body.node;
        const bool selected = node != 0 && std::find(selected_.begin(), selected_.end(), node) != selected_.end();
        put(4, solid.color, selected ? 2.0f : node != 0 && node == hovered_ ? 1.0f : 0.0f);
        // The ball round its box holds it, whatever its shape.
        put(5, Vec3(s.ring(), s.tube(), static_cast<float>(solid.matte)), 1.001f * length(h));
    }
    if (!solidsTex_) gl_.GenTextures(1, &solidsTex_);
    gl_.ActiveTexture(TEXTURE0 + kSolidsUnit);
    gl_.BindTexture(TEXTURE_2D, solidsTex_);
    gl_.TexImage2D(TEXTURE_2D, 0, static_cast<GLint>(RGBA32F), 6, std::max(n, 1), 0, RGBA, FLOAT, rows.data());
    gl_.TexParameteri(TEXTURE_2D, TEXTURE_MIN_FILTER, NEAREST);
    gl_.TexParameteri(TEXTURE_2D, TEXTURE_MAG_FILTER, NEAREST);
    gl_.Uniform1i(location(program, "u_solids"), kSolidsUnit);
    // The meshes' distance fields, on texture units 4 to 7.
    int slots = 0;
    int meshSolid[kMaxMeshShadows] = {};
    float scale[4 * kMaxMeshShadows] = {}, centers[3 * kMaxMeshShadows] = {}, grid[4 * kMaxMeshShadows] = {},
          counts[3 * kMaxMeshShadows] = {};
    for (int i = 0; i < n && slots < kMaxMeshShadows; ++i) {
        const sim::Collider& body = solids_[static_cast<size_t>(i)].body;
        if (body.shape != sim::Shape::Mesh || !body.mesh) continue;
        const auto gpu = std::find_if(meshes_.begin(), meshes_.end(), [&](const MeshGpu& m) { return m.mesh == body.mesh; });
        if (gpu == meshes_.end()) continue;
        const sim::MeshShape& m = *body.mesh;
        const Vec3 h = body.instance().half();
        meshSolid[slots] = i;
        const Vec3 toMesh(m.half().x / h.x, m.half().y / h.y, m.half().z / h.z);
        const float fromMesh = std::min({h.x / m.half().x, h.y / m.half().y, h.z / m.half().z});
        for (int a = 0; a < 3; ++a) {
            scale[4 * slots + a] = toMesh[a];
            centers[3 * slots + a] = m.center()[a];
            grid[4 * slots + a] = m.gridLo()[a];
            counts[3 * slots + a] = static_cast<float>(m.points(a));
        }
        scale[4 * slots + 3] = fromMesh;
        grid[4 * slots + 3] = m.cell();
        gl_.ActiveTexture(TEXTURE4 + static_cast<GLenum>(slots));
        gl_.BindTexture(TEXTURE_3D, gpu->sdf);
        ++slots;
    }
    gl_.ActiveTexture(TEXTURE0);
    gl_.Uniform1i(location(program, "u_meshShadows"), slots);
    gl_.Uniform1iv(location(program, "u_meshSolid"), kMaxMeshShadows, meshSolid);
    gl_.Uniform4fv(location(program, "u_meshScale"), kMaxMeshShadows, scale);
    gl_.Uniform3fv(location(program, "u_meshCenter"), kMaxMeshShadows, centers);
    gl_.Uniform4fv(location(program, "u_sdfLo"), kMaxMeshShadows, grid);
    gl_.Uniform3fv(location(program, "u_sdfCount"), kMaxMeshShadows, counts);
    const char* samplers[kMaxMeshShadows] = {"u_sdf0", "u_sdf1", "u_sdf2", "u_sdf3"};
    for (int k = 0; k < kMaxMeshShadows; ++k) gl_.Uniform1i(location(program, samplers[k]), 4 + k);

    gl_.Uniform1i(location(program, "u_solidCount"), n);
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
    const LightingKey key{look.lightDirection(), look.smokeDensity, look.steamDensity, look.flameIntensity,
                          look.flameStart, look.flameRange};
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
    gl_.Uniform1f(location(shadowProgram_, "u_steamExtinction"), look.steamDensity);
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
    if (fbo_ && width == width_ && height == height_ && targetPasses_ == passes.on) return;
    if (!fbo_) {
        gl_.GenFramebuffers(1, &fbo_);
        gl_.GenTextures(1, &colorTex_);
        gl_.GenRenderbuffers(1, &depthBuffer_);
    }
    width_ = width;
    height_ = height;
    targetPasses_ = passes.on;
    // The passes: the picture in linear light, beyond white -- 16-bit
    // floats -- and two targets beside it; else 8 bits, tone mapped.
    gl_.BindTexture(TEXTURE_2D, colorTex_);
    if (passes.on) {
        gl_.TexImage2D(TEXTURE_2D, 0, static_cast<GLint>(RGBA16F), width, height, 0, RGBA, FLOAT, nullptr);
    } else {
        gl_.TexImage2D(TEXTURE_2D, 0, static_cast<GLint>(RGBA8), width, height, 0, RGBA, UNSIGNED_BYTE, nullptr);
    }
    gl_.TexParameteri(TEXTURE_2D, TEXTURE_MIN_FILTER, LINEAR);
    gl_.TexParameteri(TEXTURE_2D, TEXTURE_MAG_FILTER, LINEAR);
    if (passes.on) {
        if (!auxTex_[0]) gl_.GenTextures(2, auxTex_);
        for (const GLuint t : auxTex_) {
            gl_.BindTexture(TEXTURE_2D, t);
            gl_.TexImage2D(TEXTURE_2D, 0, static_cast<GLint>(RGBA32F), width, height, 0, RGBA, FLOAT, nullptr);
            gl_.TexParameteri(TEXTURE_2D, TEXTURE_MIN_FILTER, 0x2600);  // NEAREST
            gl_.TexParameteri(TEXTURE_2D, TEXTURE_MAG_FILTER, 0x2600);
        }
    }
    gl_.BindTexture(TEXTURE_2D, 0);
    gl_.BindRenderbuffer(RENDERBUFFER, depthBuffer_);
    gl_.RenderbufferStorage(RENDERBUFFER, DEPTH_COMPONENT24, width, height);
    gl_.BindRenderbuffer(RENDERBUFFER, 0);
    gl_.BindFramebuffer(FRAMEBUFFER, fbo_);
    gl_.FramebufferTexture2D(FRAMEBUFFER, COLOR_ATTACHMENT0, TEXTURE_2D, colorTex_, 0);
    gl_.FramebufferTexture2D(FRAMEBUFFER, COLOR_ATTACHMENT1, TEXTURE_2D, passes.on ? auxTex_[0] : 0, 0);
    gl_.FramebufferTexture2D(FRAMEBUFFER, COLOR_ATTACHMENT2, TEXTURE_2D, passes.on ? auxTex_[1] : 0, 0);
    gl_.FramebufferRenderbuffer(FRAMEBUFFER, DEPTH_ATTACHMENT, RENDERBUFFER, depthBuffer_);
    const GLenum buffers[3] = {COLOR_ATTACHMENT0, COLOR_ATTACHMENT1, COLOR_ATTACHMENT2};
    gl_.DrawBuffers(passes.on ? 3 : 1, buffers);
    gl_.BindFramebuffer(FRAMEBUFFER, 0);
}

Orbit orbitThrough(const sim::Camera& camera, float distance) {
    constexpr float kDegrees = 180.0f / kPi;
    const Vec3 f = camera.forward();
    Orbit o;
    // The orbit's eye is its target back along the view: pitch and yaw
    // from where the camera looks.
    o.pitch = std::asin(std::clamp(-f.y, -1.0f, 1.0f)) * kDegrees;
    o.yaw = std::atan2(-f.x, -f.z) * kDegrees;
    o.distance = std::max(distance, 0.01f);
    const Vec3 target = camera.position + f * o.distance;
    o.target = target;
    o.fovY = camera.fovY();
    // The roll: how far the camera's right is turned from the level one.
    Vec3 level, right, up;
    o.axes(level, right, up);
    const Vec3 r = camera.right();
    o.roll = std::atan2(dot(r, up), dot(r, right)) * kDegrees;
    return o;
}

sim::Camera cameraFrom(const Orbit& orbit, sim::Camera camera) {
    Vec3 forward, right, up;
    orbit.axes(forward, right, up);
    camera.position = orbit.eye();
    camera.rotation = sim::Camera::rotationFor(forward, up, camera.rotation);
    return camera;
}

Orbit VolumeRenderer::viewOf(const sim::Domain& domain) {
    const Vec3 size = domain.size();
    Orbit o;
    o.yaw = 35.0f;
    o.pitch = 12.0f;
    // Far enough for the sphere round the domain to fit the view, a little tighter.
    o.distance = 0.92f * 0.5f * length(size) / std::sin(kFovY * kPi / 360.0f);
    o.target = Vec3(0.0f, 0.46f * size.y, 0.0f);
    return o;
}

void VolumeRenderer::clipPlanes(const Vec3& eye, float& zNear, float& zFar) const {
    // The farthest of what is drawn: the corners of the boxes round it --
    // the geometry, the pieces and what stands on the points; the gas and
    // the water; the solids; the guides, the overlay, the rain.
    float farthest = 0.0f;
    auto reach = [&](const Vec3& lo, const Vec3& hi) { farthest = std::max(farthest, farthestCorner(eye, lo, hi)); };
    if (hasGeoBounds_) reach(geoLo_, geoHi_);
    reach(domain_.origin(), domain_.origin() + domain_.size());
    if (hasWater_) reach(waterDomain_.origin(), waterDomain_.origin() + waterDomain_.size());
    for (const sim::Solid& s : solids_) {
        // A sphere round it, however it is turned: its size along its axes.
        const float r = length(s.body.size);
        reach(s.body.center - Vec3(r), s.body.center + Vec3(r));
    }
    if (lineCount_ > 0) reach(linesBox_.lo, linesBox_.hi);
    for (const Box& box : overlayBox_) reach(box.lo, box.hi);
    if (hasRain_) reach(rainBox_.lo, rainBox_.hi);
    gl::clipPlanes(farthest, zNear, zFar);
}

void VolumeRenderer::render(int width, int height) {
    ensureTarget(width, height);
    updateLighting();
    // The camera: where it is, and the directions of the screen's axes.
    const Vec3 e = orbit.eye();
    Vec3 forward, right, up;
    orbit.axes(forward, right, up);
    const float aspect = static_cast<float>(width) / static_cast<float>(height);
    float zNear = kNearClip, zFar = kFarClip;
    clipPlanes(e, zNear, zFar);
    viewProjection_ = orbit.viewProjection(aspect, zNear, zFar);
    // The view of the next frame, for the passes' motion; the same when the
    // camera stands still.
    nextViewProjection_ = viewProjection_;
    if (passes.on && passes.moving) nextViewProjection_ = passes.next.viewProjection(aspect, zNear, zFar);
    // The shadows of the geometry: its map from the sun, when it or the sun moved.
    updateGeoShadow(normalize(look.lightDirection()));
    // The meshes first, into their own buffer, seen by the same camera.
    const bool meshes = (anyMesh_ || geoVertices_ > 0 || shownElements_ > 0 || hasInstances()) && meshProgram_;
    if (meshes) renderMeshes(width, height, e);
    // ... and the glass into its own, as two layers.
    const bool glass = glassVertices_ > 0 && glassProgram_;
    if (glass) renderGlass(width, height, e);
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

    const float tanHalf = std::tan(orbit.fovY * kPi / 360.0f);

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
    gl_.UniformMatrix4fv(location(program_, "u_viewProj"), 1, 0, glm::value_ptr(viewProjection_));
    gl_.Uniform1i(location(program_, "u_hasGas"), hasFrame_ ? 1 : 0);
    gl_.Uniform3f(location(program_, "u_lightDir"), light.x, light.y, light.z);
    gl_.Uniform3f(location(program_, "u_light"), s.lightColor.x * s.lightIntensity, s.lightColor.y * s.lightIntensity,
                  s.lightColor.z * s.lightIntensity);
    gl_.Uniform3f(location(program_, "u_sky"), s.skyColor.x * s.skyIntensity, s.skyColor.y * s.skyIntensity,
                  s.skyColor.z * s.skyIntensity);
    gl_.Uniform3f(location(program_, "u_albedo"), s.smokeColor.x, s.smokeColor.y, s.smokeColor.z);
    gl_.Uniform3f(location(program_, "u_steamAlbedo"), s.steamColor.x, s.steamColor.y, s.steamColor.z);
    gl_.Uniform1f(location(program_, "u_extinction"), s.smokeDensity);
    gl_.Uniform1f(location(program_, "u_steamExtinction"), s.steamDensity);
    gl_.Uniform1f(location(program_, "u_occlusion"), s.occlusion);
    gl_.Uniform1f(location(program_, "u_flame"), s.flameIntensity);
    gl_.Uniform1f(location(program_, "u_flameStart"), s.flameStart);
    gl_.Uniform1f(location(program_, "u_flameRange"), std::max(s.flameRange, 1e-3f));
    gl_.Uniform1f(location(program_, "u_fireLight"), s.fireLight);
    gl_.Uniform1f(location(program_, "u_exposure"), s.exposure);
    gl_.Uniform1f(location(program_, "u_step"), 0.6f * domain_.voxel);
    gl_.Uniform1i(location(program_, "u_floor"), s.floor ? 1 : 0);
    gl_.Uniform1i(location(program_, "u_floorMatte"), static_cast<int>(s.floorMatte));
    gl_.Uniform3f(location(program_, "u_ground"), s.groundColor.x, s.groundColor.y, s.groundColor.z);
    gl_.Uniform1i(location(program_, "u_grid"), s.grid ? 1 : 0);
    gl_.Uniform1i(location(program_, "u_skyBehind"), s.skyBehind ? 1 : 0);
    // The shadows of the geometry, from their map.
    const bool geoShadow = hasGeoShadow_ && (geoVertices_ > 0 || shownElements_ > 0 || hasInstances());
    gl_.Uniform1i(location(program_, "u_hasGeoShadow"), geoShadow ? 1 : 0);
    gl_.UniformMatrix4fv(location(program_, "u_lightViewProj"), 1, 0, glm::value_ptr(lightViewProj_));
    gl_.Uniform1f(location(program_, "u_geoShadowTexel"), 1.0f / static_cast<float>(kGeoShadowSize));
    gl_.Uniform1f(location(program_, "u_geoShadowBias"), geoShadowBias_);
    gl_.Uniform1f(location(program_, "u_geoShadowLift"), geoShadowLift_);
    gl_.ActiveTexture(TEXTURE10);
    gl_.BindTexture(TEXTURE_2D, geoShadow ? geoShadowTex_ : 0);
    gl_.Uniform1i(location(program_, "u_geoShadow"), 10);
    gl_.Uniform3f(location(program_, "u_floorCenter"), 0.0f, 0.0f, 0.0f);
    // The floor fades out far away: past the domain, past the geometry, as far as the eye is off.
    float reach = 2.5f * std::max(size.x, size.z);
    if (hasGeoBounds_) reach = std::max(reach, 2.5f * std::max(geoHi_.x - geoLo_.x, geoHi_.z - geoLo_.z));
    reach = std::max(reach, 3.0f * length(e));
    gl_.Uniform1f(location(program_, "u_floorRadius"), std::max(4.0f, reach));
    gl_.Uniform3i(location(program_, "u_glowDims"), glowSize_[0], glowSize_[1], glowSize_[2]);
    const float block = static_cast<float>(glowBlock_) * domain_.voxel;
    gl_.Uniform3f(location(program_, "u_glowCell"), block, block, block);
    gl_.Uniform3f(location(program_, "u_backgroundTop"), 0.075f, 0.082f, 0.095f);
    gl_.Uniform3f(location(program_, "u_backgroundBottom"), 0.022f, 0.023f, 0.027f);
    // The passes: linear light, and the motion to the next frame.
    gl_.Uniform1i(location(program_, "u_linear"), passes.on ? 1 : 0);
    gl_.UniformMatrix4fv(location(program_, "u_nextViewProj"), 1, 0, glm::value_ptr(nextViewProjection_));
    gl_.Uniform2f(location(program_, "u_viewport"), static_cast<float>(width), static_cast<float>(height));
    gl_.ActiveTexture(TEXTURE0 + 11);
    gl_.BindTexture(TEXTURE_2D, meshes && passes.on ? gAux_ : 0);
    gl_.Uniform1i(location(program_, "u_meshAux"), 11);

    gl_.ActiveTexture(TEXTURE0);
    gl_.BindTexture(TEXTURE_3D, fields_);
    gl_.Uniform1i(location(program_, "u_fields"), 0);
    gl_.ActiveTexture(TEXTURE1);
    gl_.BindTexture(TEXTURE_3D, light_);
    gl_.Uniform1i(location(program_, "u_sunlight"), 1);
    gl_.ActiveTexture(TEXTURE2);
    gl_.BindTexture(TEXTURE_3D, glow_);
    gl_.Uniform1i(location(program_, "u_glow"), 2);
    gl_.ActiveTexture(TEXTURE3);
    gl_.BindTexture(TEXTURE_2D, meshes ? gTex_ : 0);
    gl_.Uniform1i(location(program_, "u_meshG"), 3);
    gl_.Uniform1i(location(program_, "u_hasMeshes"), meshes ? 1 : 0);
    // The water, on unit 8: how far light gets into it follows from its
    // colour -- what a colour keeps, it loses slowly -- and its clarity.
    gl_.ActiveTexture(TEXTURE8);
    gl_.BindTexture(TEXTURE_3D, hasWater_ ? water_ : 0);
    gl_.Uniform1i(location(program_, "u_water"), 8);
    gl_.Uniform1i(location(program_, "u_hasWater"), hasWater_ && s.waterSurface ? 1 : 0);
    {
        const Vec3 lo = waterDomain_.origin(), extent = waterDomain_.size();
        gl_.Uniform3f(location(program_, "u_waterMin"), lo.x, lo.y, lo.z);
        gl_.Uniform3f(location(program_, "u_waterSize"), extent.x, extent.y, extent.z);
        gl_.Uniform1f(location(program_, "u_waterCell"), waterDomain_.voxel);
        gl_.Uniform1f(location(program_, "u_waterBand"), std::max(waterBand_, 1e-4f));
        const Vec3& c = s.waterColor;
        gl_.Uniform3f(location(program_, "u_waterColor"), c.x, c.y, c.z);
        const float per = 3.0f / std::max(s.waterClarity, 0.01f);
        gl_.Uniform3f(location(program_, "u_waterSigma"), (1.0f - 0.85f * c.x) * per, (1.0f - 0.85f * c.y) * per,
                      (1.0f - 0.85f * c.z) * per);
        gl_.Uniform1f(location(program_, "u_foam"), s.foam);
    }
    // The rain's ripples, on unit 9, and the wet floor.
    gl_.ActiveTexture(TEXTURE9);
    gl_.BindTexture(TEXTURE_2D, hasRipples_ ? ripples_ : 0);
    gl_.Uniform1i(location(program_, "u_ripples"), 9);
    gl_.Uniform1i(location(program_, "u_hasRipples"), hasRipples_ ? 1 : 0);
    gl_.Uniform2f(location(program_, "u_rippleMin"), rippleMin_.x, rippleMin_.z);
    gl_.Uniform2f(location(program_, "u_rippleSize"), rippleCell_ * static_cast<float>(rippleSize_[0]),
                  rippleCell_ * static_cast<float>(rippleSize_[1]));
    gl_.Uniform1f(location(program_, "u_rippleCell"), std::max(rippleCell_, 1e-4f));
    gl_.Uniform1f(location(program_, "u_wet"), hasRain_ && wetMin_[0] <= wetMax_[0] ? s.wetness : 0.0f);
    gl_.Uniform2f(location(program_, "u_wetMin"), wetMin_[0], wetMin_[1]);
    gl_.Uniform2f(location(program_, "u_wetMax"), wetMax_[0], wetMax_[1]);

    // The plate, on unit 12, and the camera that filmed it -- an empty one
    // seen wherever the camera looks.
    if (blankPlate_) {
        plateForward_ = forward;
        plateRight_ = right;
        plateUp_ = up;
        plateTan_[0] = plateTan_[1] = 1e6f;
    }
    const bool plate = hasPlate();
    gl_.ActiveTexture(TEXTURE0 + 12);
    gl_.BindTexture(TEXTURE_2D, plate ? plateTex_ : 0);
    gl_.Uniform1i(location(program_, "u_plate"), 12);
    gl_.Uniform1i(location(program_, "u_hasPlate"), plate ? 1 : 0);
    gl_.Uniform3f(location(program_, "u_plateForward"), plateForward_.x, plateForward_.y, plateForward_.z);
    gl_.Uniform3f(location(program_, "u_plateRight"), plateRight_.x, plateRight_.y, plateRight_.z);
    gl_.Uniform3f(location(program_, "u_plateUp"), plateUp_.x, plateUp_.y, plateUp_.z);
    gl_.Uniform2f(location(program_, "u_plateTan"), plateTan_[0], plateTan_[1]);
    // The glass, on units 13 and 14: its nearest face turned to the eye, and the next.
    for (int layer = 0; layer < 2; ++layer) {
        gl_.ActiveTexture(TEXTURE0 + 13 + static_cast<GLenum>(layer));
        gl_.BindTexture(TEXTURE_2D, glass ? glassTex_[layer] : 0);
    }
    gl_.Uniform1i(location(program_, "u_glass0"), 13);
    gl_.Uniform1i(location(program_, "u_glass1"), 14);
    gl_.Uniform1i(location(program_, "u_hasGlass"), glass ? 1 : 0);

    gl_.BindVertexArray(vao_);
    gl_.DrawArrays(TRIANGLES, 0, 3);
    // What is drawn over it -- the dots, the rain, the guides -- goes into
    // the picture alone: the passes are the main pass's.
    const GLenum picture = COLOR_ATTACHMENT0;
    if (passes.on) gl_.DrawBuffers(1, &picture);
    drawGeometry(width, height);
    drawRain(width, height, e);
    drawOverlay(width, height, e);

    // The guide lines, behind the solids where they pass behind them.
    if (lineCount_ > 0 && lineProgram_) {
        gl_.DepthFunc(LEQUAL);
        gl_.DepthMask(0);
        gl_.Enable(BLEND);
        gl_.BlendFunc(SRC_ALPHA, ONE_MINUS_SRC_ALPHA);
        gl_.UseProgram(lineProgram_);
        gl_.UniformMatrix4fv(location(lineProgram_, "u_viewProj"), 1, 0, glm::value_ptr(viewProjection_));
        gl_.BindVertexArray(lineVao_);
        gl_.DrawArrays(LINES, 0, static_cast<GLsizei>(lineCount_));
        gl_.Disable(BLEND);
        gl_.DepthMask(1);
    }
    gl_.BindVertexArray(0);
    gl_.Disable(DEPTH_TEST);
    gl_.DepthFunc(LESS);
    if (passes.on) {
        const GLenum all[3] = {COLOR_ATTACHMENT0, COLOR_ATTACHMENT1, COLOR_ATTACHMENT2};
        gl_.DrawBuffers(3, all);
    }

    for (int unit = 11; unit <= 14; ++unit) {
        gl_.ActiveTexture(TEXTURE0 + static_cast<GLenum>(unit));
        gl_.BindTexture(TEXTURE_2D, 0);
    }
    for (int unit = 4; unit < 4 + kMaxMeshShadows; ++unit) {
        gl_.ActiveTexture(TEXTURE0 + static_cast<GLenum>(unit));
        gl_.BindTexture(TEXTURE_3D, 0);
    }
    gl_.ActiveTexture(TEXTURE8);
    gl_.BindTexture(TEXTURE_3D, 0);
    gl_.ActiveTexture(TEXTURE9);
    gl_.BindTexture(TEXTURE_2D, 0);
    gl_.ActiveTexture(TEXTURE10);
    gl_.BindTexture(TEXTURE_2D, 0);
    gl_.ActiveTexture(TEXTURE3);
    gl_.BindTexture(TEXTURE_2D, 0);
    gl_.ActiveTexture(TEXTURE2);
    gl_.BindTexture(TEXTURE_3D, 0);
    gl_.ActiveTexture(TEXTURE1);
    gl_.BindTexture(TEXTURE_3D, 0);
    gl_.ActiveTexture(TEXTURE0);
    gl_.BindTexture(TEXTURE_3D, 0);
    gl_.UseProgram(0);
    gl_.BindFramebuffer(FRAMEBUFFER, 0);
    smooth();
}

void VolumeRenderer::smooth() {
    // The picture shown, not the passes: those are read as drawn, and a
    // render is read at twice the size, averaged down (readPixels).
    smoothed_ = false;
    if (!antialias || passes.on || !antialiasProgram_ || width_ <= 0 || height_ <= 0) return;
    if (!aaFbo_) {
        gl_.GenFramebuffers(1, &aaFbo_);
        gl_.GenTextures(1, &aaTex_);
    }
    if (aaWidth_ != width_ || aaHeight_ != height_) {
        aaWidth_ = width_;
        aaHeight_ = height_;
        gl_.BindTexture(TEXTURE_2D, aaTex_);
        gl_.TexImage2D(TEXTURE_2D, 0, static_cast<GLint>(RGBA8), width_, height_, 0, RGBA, UNSIGNED_BYTE, nullptr);
        gl_.TexParameteri(TEXTURE_2D, TEXTURE_MIN_FILTER, LINEAR);
        gl_.TexParameteri(TEXTURE_2D, TEXTURE_MAG_FILTER, LINEAR);
        gl_.BindTexture(TEXTURE_2D, 0);
        gl_.BindFramebuffer(FRAMEBUFFER, aaFbo_);
        gl_.FramebufferTexture2D(FRAMEBUFFER, COLOR_ATTACHMENT0, TEXTURE_2D, aaTex_, 0);
    }
    gl_.BindFramebuffer(FRAMEBUFFER, aaFbo_);
    const GLenum one = COLOR_ATTACHMENT0;
    gl_.DrawBuffers(1, &one);
    gl_.Viewport(0, 0, width_, height_);
    gl_.Disable(DEPTH_TEST);
    gl_.Disable(BLEND);
    gl_.UseProgram(antialiasProgram_);
    gl_.ActiveTexture(TEXTURE0);
    gl_.BindTexture(TEXTURE_2D, colorTex_);
    gl_.Uniform1i(location(antialiasProgram_, "u_picture"), 0);
    gl_.Uniform2f(location(antialiasProgram_, "u_texel"), 1.0f / static_cast<float>(width_), 1.0f / static_cast<float>(height_));
    gl_.BindVertexArray(vao_);
    gl_.DrawArrays(TRIANGLES, 0, 3);
    gl_.BindVertexArray(0);
    gl_.BindTexture(TEXTURE_2D, 0);
    gl_.UseProgram(0);
    gl_.BindFramebuffer(FRAMEBUFFER, 0);
    smoothed_ = true;
}

namespace {

/// A value as a picture shows it back to the light that the renderer's
/// view transform (ACES, Narkowicz's fit, then 1 / 2.2) shows as it.
float unshown(float v) {
    const double y = std::pow(std::clamp(static_cast<double>(v), 0.0, 1.0), 2.2);
    const double a = 2.43 * y - 2.51, b = 0.59 * y - 0.03, c = 0.14 * y;
    return static_cast<float>((-b - std::sqrt(std::max(b * b - 4.0 * a * c, 0.0))) / (2.0 * a));
}

}  // namespace

bool VolumeRenderer::setPlate(const std::string& file, const sim::Camera& camera, std::string& error) {
    if (file.empty()) {
        clearPlate();
        return true;
    }
    blankPlate_ = false;
    const sim::Camera c = camera.sanitized();
    plateForward_ = c.forward();
    plateRight_ = c.right();
    plateUp_ = c.up();
    const float tanHalf = std::tan(c.fovY() * kPi / 360.0f);
    plateTan_[0] = tanHalf * c.aspect();
    plateTan_[1] = tanHalf;
    if (file == plateFile_ && plateTex_) {
        plateOn_ = true;
        return true;
    }
    io::Picture picture;
    if (!io::readPicture(file, picture, error)) {
        clearPlate();
        return false;
    }
    // Into the renderer's light: a shown picture back through the view
    // transform -- most of its values are 8 bits, k / 255, looked up.
    static const std::array<float, 256> eight = [] {
        std::array<float, 256> t{};
        for (size_t k = 0; k < t.size(); ++k) t[k] = unshown(static_cast<float>(k) / 255.0f);
        return t;
    }();
    for (size_t i = 0; i + 3 < picture.rgba.size(); i += 4) {
        for (size_t k = 0; k < 3; ++k) {
            float& v = picture.rgba[i + k];
            if (picture.linear) {
                v = std::isfinite(v) ? std::clamp(v, 0.0f, 65504.0f) : 0.0f;
                continue;
            }
            const float level = v * 255.0f;
            const float nearest = std::round(level);
            v = std::fabs(level - nearest) < 1e-3f && nearest >= 0.0f && nearest <= 255.0f
                    ? eight[static_cast<size_t>(nearest)]
                    : unshown(v);
        }
    }
    if (!plateTex_) gl_.GenTextures(1, &plateTex_);
    gl_.ActiveTexture(TEXTURE0);
    gl_.BindTexture(TEXTURE_2D, plateTex_);
    gl_.PixelStorei(UNPACK_ALIGNMENT, 4);
    gl_.TexImage2D(TEXTURE_2D, 0, static_cast<GLint>(RGBA16F), picture.width, picture.height, 0, RGBA, FLOAT,
                   picture.rgba.data());
    // Pixel for pixel where it is the picture's size: the plate comes out
    // as it went in; filtered where it is scaled.
    const GLint filter = picture.width == c.width && picture.height == c.height ? NEAREST : LINEAR;
    gl_.TexParameteri(TEXTURE_2D, TEXTURE_MIN_FILTER, filter);
    gl_.TexParameteri(TEXTURE_2D, TEXTURE_MAG_FILTER, filter);
    gl_.TexParameteri(TEXTURE_2D, TEXTURE_WRAP_S, CLAMP_TO_EDGE);
    gl_.TexParameteri(TEXTURE_2D, TEXTURE_WRAP_T, CLAMP_TO_EDGE);
    gl_.BindTexture(TEXTURE_2D, 0);
    plateFile_ = file;
    plateOn_ = true;
    return true;
}

void VolumeRenderer::clearPlate() {
    plateOn_ = false;
    blankPlate_ = false;
}

void VolumeRenderer::setBlankPlate() {
    if (!blankPlate_ || !plateTex_) {
        const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        if (!plateTex_) gl_.GenTextures(1, &plateTex_);
        gl_.ActiveTexture(TEXTURE0);
        gl_.BindTexture(TEXTURE_2D, plateTex_);
        gl_.TexImage2D(TEXTURE_2D, 0, static_cast<GLint>(RGBA32F), 1, 1, 0, RGBA, FLOAT, black);
        gl_.TexParameteri(TEXTURE_2D, TEXTURE_MIN_FILTER, LINEAR);
        gl_.TexParameteri(TEXTURE_2D, TEXTURE_MAG_FILTER, LINEAR);
        gl_.BindTexture(TEXTURE_2D, 0);
        plateFile_.clear();  // a file set next is read again
    }
    blankPlate_ = true;
    plateOn_ = true;
}

std::vector<uint8_t> VolumeRenderer::readTransparent(int factor) const {
    const PassImage p = readPasses(factor);
    const size_t n = static_cast<size_t>(p.width) * static_cast<size_t>(p.height);
    std::vector<uint8_t> out(4 * n, 0);
    if (p.rgba.size() < 4 * n) return out;
    auto shown = [](float v) {  // the shaders' toneMap, then 1 / 2.2
        v = std::max(v, 0.0f);
        const float t = std::clamp(v * (2.51f * v + 0.03f) / (v * (2.43f * v + 0.59f) + 0.14f), 0.0f, 1.0f);
        return static_cast<uint8_t>(std::lround(std::pow(t, 1.0f / 2.2f) * 255.0f));
    };
    for (size_t i = 0; i < n; ++i) {
        // Covered by the CG, and by the shadows a catcher takes from what
        // would be behind it -- nothing, here.
        const float cover = std::clamp(p.rgba[4 * i + 3], 0.0f, 1.0f);
        float shadow = 0.0f;
        if (p.relit.size() >= 3 * n) {
            shadow = std::clamp(1.0f - (p.relit[3 * i] + p.relit[3 * i + 1] + p.relit[3 * i + 2]) / 3.0f, 0.0f, 1.0f);
        }
        const float a = cover + (1.0f - cover) * shadow;
        for (int c = 0; c < 3; ++c) out[4 * i + static_cast<size_t>(c)] = a > 1e-4f ? shown(p.rgba[4 * i + static_cast<size_t>(c)] / a) : 0;
        out[4 * i + 3] = static_cast<uint8_t>(std::lround(a * 255.0f));
    }
    return out;
}

std::vector<uint8_t> VolumeRenderer::readPixels(int factor) const {
    return readRgb(gl_, fbo_, width_, height_, factor);
}

VolumeRenderer::PassImage VolumeRenderer::readPasses(int factor) const {
    PassImage out;
    if (!fbo_ || !targetPasses_) return out;
    factor = std::max(factor, 1);
    const size_t W = static_cast<size_t>(width_), H = static_cast<size_t>(height_), n = W * H;
    std::vector<float> color(4 * n), aux(4 * n), motion(4 * n);
    gl_.BindFramebuffer(FRAMEBUFFER, fbo_);
    gl_.PixelStorei(PACK_ALIGNMENT, 4);
    const std::pair<GLenum, std::vector<float>*> reads[3] = {
        {COLOR_ATTACHMENT0, &color}, {COLOR_ATTACHMENT1, &aux}, {COLOR_ATTACHMENT2, &motion}};
    for (const auto& [attachment, into] : reads) {
        gl_.ReadBuffer(attachment);
        gl_.ReadPixels(0, 0, width_, height_, RGBA, FLOAT, into->data());
    }
    gl_.ReadBuffer(COLOR_ATTACHMENT0);
    gl_.BindFramebuffer(FRAMEBUFFER, 0);

    // GL's lines run bottom to top, a picture's top to bottom; factor x
    // factor pixels make one: averaged, the depth the nearest, each mask
    // the share of them that are of it, the motion in the picture's pixels.
    const int w = width_ / factor, h = height_ / factor;
    out.width = w;
    out.height = h;
    const size_t m = static_cast<size_t>(w) * static_cast<size_t>(h);
    out.rgba.assign(4 * m, 0.0f);
    out.depth.assign(m, std::numeric_limits<float>::infinity());
    out.smoke.assign(m, 0.0f);
    out.motion.assign(2 * m, 0.0f);
    out.plate = plateOn_;
    if (out.plate) out.relit.assign(3 * m, 0.0f);
    for (auto& mask : out.masks) mask.assign(m, 0.0f);
    const float share = 1.0f / static_cast<float>(factor * factor);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const size_t o = static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x);
            for (int dy = 0; dy < factor; ++dy) {
                for (int dx = 0; dx < factor; ++dx) {
                    const size_t sy = H - 1 - static_cast<size_t>(y * factor + dy), sx = static_cast<size_t>(x * factor + dx);
                    const size_t i = sy * W + sx;
                    for (int c = 0; c < 4; ++c) out.rgba[4 * o + static_cast<size_t>(c)] += share * color[4 * i + static_cast<size_t>(c)];
                    const float depth = aux[4 * i];
                    if (depth >= 0.0f) out.depth[o] = std::min(out.depth[o], depth);
                    out.smoke[o] += share * std::clamp(aux[4 * i + 1], 0.0f, 1.0f);
                    const int surface = static_cast<int>(std::lround(aux[4 * i + 2]));
                    if (surface >= 0 && surface < static_cast<int>(Surface::Count)) out.masks[surface][o] += share;
                    out.motion[2 * o] += share * motion[4 * i] / static_cast<float>(factor);
                    out.motion[2 * o + 1] += share * motion[4 * i + 1] / static_cast<float>(factor);
                    if (out.plate) {
                        out.relit[3 * o] += share * aux[4 * i + 3];
                        out.relit[3 * o + 1] += share * motion[4 * i + 2];
                        out.relit[3 * o + 2] += share * motion[4 * i + 3];
                    }
                }
            }
        }
    }
    return out;
}

bool writePassesExr(const VolumeRenderer& renderer, const std::string& path, const std::string& comment,
                    std::string& error, LinearSpace space) {
    using Surface = VolumeRenderer::Surface;
    VolumeRenderer::PassImage p = renderer.readPasses(2);
    if (p.width == 0) {
        error = path + ": nothing was rendered with the passes";
        return false;
    }
    io::ExrImage img;
    img.width = p.width;
    img.height = p.height;
    img.hasChromaticities = true;
    img.chromaticities = chromaticitiesOf(space);
    const size_t n = static_cast<size_t>(p.width) * static_cast<size_t>(p.height);
    if (space != LinearSpace::Rec709) {
        for (size_t i = 0; i < n; ++i) {
            float* c = &p.rgba[4 * i];
            const Vec3 to = fromRec709(Vec3(c[0], c[1], c[2]), space);
            c[0] = to.x;
            c[1] = to.y;
            c[2] = to.z;
        }
    }
    // Transparent: the alpha the shadows on the catchers too -- there is no
    // plate for them to darken.
    if (renderer.look.transparent && p.relit.size() >= 3 * n) {
        for (size_t i = 0; i < n; ++i) {
            const float cover = std::clamp(p.rgba[4 * i + 3], 0.0f, 1.0f);
            const float shadow =
                std::clamp(1.0f - (p.relit[3 * i] + p.relit[3 * i + 1] + p.relit[3 * i + 2]) / 3.0f, 0.0f, 1.0f);
            p.rgba[4 * i + 3] = cover + (1.0f - cover) * shadow;
        }
    }
    const char* rgba[4] = {"R", "G", "B", "A"};
    for (int c = 0; c < 4; ++c) {
        io::ExrChannel ch{rgba[c], true, std::vector<float>(n)};
        for (size_t i = 0; i < n; ++i) ch.values[i] = p.rgba[4 * i + static_cast<size_t>(c)];
        img.channels.push_back(std::move(ch));
    }
    img.channels.push_back({"Z", false, std::move(p.depth)});
    io::ExrChannel u{"forward.u", false, std::vector<float>(n)}, v{"forward.v", false, std::vector<float>(n)};
    for (size_t i = 0; i < n; ++i) {
        u.values[i] = p.motion[2 * i];
        v.values[i] = p.motion[2 * i + 1];
    }
    img.channels.push_back(std::move(u));
    img.channels.push_back(std::move(v));
    const std::pair<Surface, const char*> masks[] = {{Surface::Floor, "mask.floor"},   {Surface::Geometry, "mask.geometry"},
                                                     {Surface::Pieces, "mask.pieces"}, {Surface::Objects, "mask.objects"},
                                                     {Surface::Water, "mask.water"}};
    for (const auto& [surface, name] : masks) img.channels.push_back({name, true, std::move(p.masks[static_cast<int>(surface)])});
    img.channels.push_back({"mask.smoke", true, std::move(p.smoke)});
    if (p.plate) {
        // Over a plate the picture is the CG alone, and this what the plate
        // is multiplied by where catchers relight it: plate x catcher x (1 - A) + RGB.
        const char* rgb[3] = {"catcher.R", "catcher.G", "catcher.B"};
        for (int c = 0; c < 3; ++c) {
            io::ExrChannel ch{rgb[c], true, std::vector<float>(n)};
            for (size_t i = 0; i < n; ++i) ch.values[i] = p.relit[3 * i + static_cast<size_t>(c)];
            img.channels.push_back(std::move(ch));
        }
    }
    img.strings = {{"comments", comment}, {"owner", "Prototype"}};
    return io::writeExr(img, path, error);
}

}  // namespace pg::gl
