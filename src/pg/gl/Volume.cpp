#include "pg/gl/Volume.h"

#include "pg/sim/Display.h"

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
    vec3 oc = p - u_solidA[i].xyz;
    vec3 local = vec3(dot(u_solidB[i].xyz, oc), dot(u_solidC[i].xyz, oc), dot(u_solidD[i].xyz, oc));
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
    vec3 h = vec3(u_solidB[i].w, u_solidC[i].w, u_solidD[i].w);
    vec3 oc = o - u_solidA[i].xyz;
    vec3 lo = vec3(dot(u_solidB[i].xyz, oc), dot(u_solidC[i].xyz, oc), dot(u_solidD[i].xyz, oc)) / h;
    vec3 ld = vec3(dot(u_solidB[i].xyz, d), dot(u_solidC[i].xyz, d), dot(u_solidD[i].xyz, d)) / h;
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
uniform bool u_hasMeshes;
uniform sampler2D u_meshG;     // the rasterised meshes: normal (octahedral), which solid, distance (< 0: none)

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

// Sunlight at p: blocked by the solids, dimmed by the water and the smoke.
float sunThrough(vec3 p, bool onWater) {
    vec3 n;
    int which;
    if (hitSolid(p, u_lightDir, 1e-3, n, which) < 1e29 || meshShadow(p, u_lightDir)) return 0.0;
    float light = waterShade(p, onWater);
    if (!u_hasGas) return light;
    vec2 span = boxSpan(p, u_lightDir, u_boxMin, u_boxMin + u_boxSize);
    float t0 = max(span.x, 0.0), t1 = span.y;
    if (t1 <= t0) return 1.0;
    float dt = max((t1 - t0) / 48.0, 2.0 * u_step);
    float depth = 0.0;
    for (float t = t0 + 0.5 * dt; t < t1; t += dt) {
        vec3 q = (p + u_lightDir * t - u_boxMin) / u_boxSize;
        depth += textureLod(u_fields, q, 1.0).r * fadeAt(q);
    }
    return light * exp(-u_extinction * depth * dt);
}

// Sunlight at a point of a solid.
float sunAt(vec3 p) { return sunThrough(p, false); }

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
vec3 shadeSurface(vec3 p, vec3 n, vec3 view, vec3 albedo, float mark) {
    float ndl = max(dot(n, u_lightDir), 0.0);
    float sun = ndl > 0.0 ? sunAt(p + n * 2e-3) : 0.0;
    vec3 sky = u_sky * (0.6 + 0.4 * n.y);
    vec3 half_ = normalize(u_lightDir - view);
    vec3 c = wetten(albedo * (u_light * sun * ndl + sky + fireGlow(p, n)), p, n, view) +
             u_light * sun * 0.12 * pow(max(dot(n, half_), 0.0), 40.0) * ndl;
    if (mark > 0.5) {
        float rim = pow(1.0 - abs(dot(n, view)), 2.0);
        c += vec3(1.0, 0.36, 0.08) * rim * (mark > 1.5 ? 1.4 : 0.6) + vec3(0.06, 0.025, 0.005) * (mark > 1.5 ? 1.0 : 0.0);
    }
    return c;
}

vec3 shadeSolid(vec3 p, vec3 n, vec3 view, int i) { return shadeSurface(p, n, view, u_solidE[i].rgb, u_solidE[i].w); }

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
    float sun = sunThrough(p + n * 2e-3, true);
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
    // The meshes' buffer: a solid's index, or -- below 0 -- the displayed
    // geometry's colour, 8 bits a channel.
    bool displayed = false;
    vec3 displayColor = vec3(0.0);
    if (u_hasMeshes) {
        vec4 g = texelFetch(u_meshG, ivec2(gl_FragCoord.xy), 0);
        if (g.w > 0.0 && g.w < tSolid) {
            tSolid = g.w;
            normal = octDecode(g.xy);
            displayed = g.z < -0.5;
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
    if (tSolid < tFloor && tSolid < 1e29) {
        tEnd = tSolid;
        surface = displayed ? shadeSurface(u_eye + dir * tSolid, normal, dir, displayColor, 0.0)
                            : shadeSolid(u_eye + dir * tSolid, normal, dir, which);
        cover = 1.0;
    } else if (u_floor && down) {
        tEnd = tFloor;
        float away = length(floorPoint.xz - u_floorCenter.xz) / u_floorRadius;
        cover = 1.0 - smoothstep(0.35, 1.0, away);
        if (cover > 0.0) {
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
        }
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

// A mesh's triangles, placed like the solid it is: into a buffer the pass
// that shades everything reads -- the normal facing the eye, which solid,
// and how far along the ray.
const char* kMeshVertex = R"(#version 330 core
layout(location = 0) in vec3 a_position;  // the mesh's own space
layout(location = 1) in vec3 a_normal;
uniform mat4 u_viewProj;
uniform vec3 u_center, u_axisX, u_axisY, u_axisZ;
uniform vec3 u_scale;       // world units per mesh unit along each own axis
uniform vec3 u_meshCenter;
out vec3 v_world, v_normal;
void main() {
    vec3 local = (a_position - u_meshCenter) * u_scale;
    v_world = u_center + u_axisX * local.x + u_axisY * local.y + u_axisZ * local.z;
    vec3 n = a_normal / u_scale;  // the inverse transpose of the stretch
    v_normal = u_axisX * n.x + u_axisY * n.y + u_axisZ * n.z;
    gl_Position = u_viewProj * vec4(v_world, 1.0);
}
)";

const char* kMeshFragment = R"(#version 330 core
in vec3 v_world, v_normal;
out vec4 o_g;
uniform vec3 u_eye;
uniform float u_index;
vec2 octWrap(vec2 v) { return (1.0 - abs(v.yx)) * vec2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0); }
void main() {
    vec3 view = v_world - u_eye;
    vec3 n = normalize(v_normal);
    if (dot(n, view) > 0.0) n = -n;  // both sides: an open mesh shows its inside
    n /= abs(n.x) + abs(n.y) + abs(n.z);
    o_g = vec4(n.z >= 0.0 ? n.xy : octWrap(n.xy), u_index, length(view));
}
)";

// The displayed geometry's triangles, into the same buffer as the meshes:
// its colour in place of a solid's index, as a number below 0.
const char* kGeoVertex = R"(#version 330 core
layout(location = 0) in vec3 a_position;
layout(location = 1) in vec3 a_normal;
layout(location = 2) in vec3 a_color;
uniform mat4 u_viewProj;
out vec3 v_world, v_normal, v_color;
void main() {
    v_world = a_position;
    v_normal = a_normal;
    v_color = a_color;
    gl_Position = u_viewProj * vec4(a_position, 1.0);
}
)";

const char* kGeoFragment = R"(#version 330 core
in vec3 v_world, v_normal, v_color;
out vec4 o_g;
uniform vec3 u_eye;
vec2 octWrap(vec2 v) { return (1.0 - abs(v.yx)) * vec2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0); }
void main() {
    vec3 view = v_world - u_eye;
    vec3 n = dot(v_normal, v_normal) > 1e-20 ? normalize(v_normal) : -normalize(view);
    if (dot(n, view) > 0.0) n = -n;  // both sides
    n /= abs(n.x) + abs(n.y) + abs(n.z);
    vec3 c = floor(clamp(v_color, 0.0, 1.0) * 255.0 + 0.5);
    o_g = vec4(n.z >= 0.0 ? n.xy : octWrap(n.xy), -1.0 - (c.r + c.g * 256.0 + c.b * 65536.0), length(view));
}
)";

// The displayed geometry's loose points: round dots, shaded as little balls,
// as wide as their pscale where they have one -- else a few pixels.
const char* kDotVertex = R"(#version 330 core
layout(location = 0) in vec3 a_position;
layout(location = 1) in vec3 a_color;
layout(location = 2) in float a_radius;
uniform mat4 u_viewProj;
uniform float u_pixelsPerUnit;  // pixels a world unit spans 1 unit in front of the eye
uniform float u_dot;            // pixels across a dot with no size
out vec3 v_color;
void main() {
    gl_Position = u_viewProj * vec4(a_position, 1.0);
    float px = a_radius > 0.0 ? 2.0 * a_radius * u_pixelsPerUnit / max(gl_Position.w, 1e-4) : u_dot;
    gl_PointSize = clamp(px, 1.5, 64.0);
    v_color = a_color;
}
)";

const char* kDotFragment = R"(#version 330 core
in vec3 v_color;
out vec4 o_color;
uniform vec3 u_lightView;  // towards the sun, in the eye's frame: x right, y up, z back at the eye
uniform vec3 u_light, u_sky;
uniform float u_exposure;
vec3 toneMap(vec3 x) { return clamp(x * (2.51 * x + 0.03) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0); }
void main() {
    vec2 q = gl_PointCoord * 2.0 - 1.0;
    q.y = -q.y;
    float r2 = dot(q, q);
    if (r2 > 1.0) discard;
    vec3 n = vec3(q, sqrt(1.0 - r2));
    vec3 lit = v_color * (u_light * max(dot(n, u_lightView), 0.0) + u_sky * (0.7 + 0.5 * n.y));
    o_color = vec4(pow(toneMap(u_exposure * lit), vec3(1.0 / 2.2)), 1.0);
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

sim::Domain sceneDomain(const sim::World& world) {
    const sim::World safe = world.sanitized();
    Vec3 size;
    bool any = false;
    auto take = [&](const sim::Domain& d) {
        const Vec3 e = d.size();
        size = any ? Vec3(std::max(size.x, e.x), std::max(size.y, e.y), std::max(size.z, e.z)) : e;
        any = true;
    };
    if (safe.hasGas) take(safe.gas.solver.domain());
    if (safe.hasWater) take(safe.water.solver.domain());
    // Rain alone: some ground to fall on, not the sky it falls from -- and
    // the same however the cloud is sized, or the camera would follow it.
    if (!any && safe.hasRain) take(sim::Domain::ofBox(Vec3(3.0f, 1.5f, 3.0f), 64));
    if (!any) return sim::Scene().solver.domain();
    return sim::Domain::ofBox(size, 64);
}

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
    const sim::Domain whole = world && world->any() ? sceneDomain(*world) : domain;
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
                     dotProgram_}) {
        if (p) gl_.DeleteProgram(p);
    }
    for (GLuint a : {geoVao_, dotVao_, curveVao_}) {
        if (a) gl_.DeleteVertexArrays(1, &a);
    }
    for (GLuint b : {geoBuffer_, dotBuffer_, curveBuffer_}) {
        if (b) gl_.DeleteBuffers(1, &b);
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
    if (!dots) {
        for (GLuint p : {view, shadow, glow, lines, meshes, rain, geo}) {
            if (p) gl_.DeleteProgram(p);
        }
        return false;
    }
    for (GLuint p : {program_, shadowProgram_, glowProgram_, lineProgram_, meshProgram_, rainProgram_, geoProgram_,
                     dotProgram_}) {
        if (p) gl_.DeleteProgram(p);
    }
    program_ = view;
    shadowProgram_ = shadow;
    glowProgram_ = glow;
    lineProgram_ = lines;
    meshProgram_ = meshes;
    rainProgram_ = rain;
    geoProgram_ = geo;
    dotProgram_ = dots;
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
    setWater(frame.water);
    setRain(frame.rain);
    const int nx = frame.domain.cells[0], ny = frame.domain.cells[1], nz = frame.domain.cells[2];
    if (frame.fields.size() != 3 * frame.domain.cellCount() || nx <= 0) {
        hasFrame_ = false;  // no gas in this frame
        return;
    }
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

void VolumeRenderer::setWater(const sim::WaterFrame& water) {
    const int nx = water.domain.cells[0], ny = water.domain.cells[1], nz = water.domain.cells[2];
    if (water.cells.size() != 2 * water.domain.cellCount() || nx <= 0) {
        hasWater_ = false;
        return;
    }
    if (!water_) gl_.GenTextures(1, &water_);
    gl_.ActiveTexture(TEXTURE0);
    gl_.BindTexture(TEXTURE_3D, water_);
    gl_.PixelStorei(UNPACK_ALIGNMENT, 2);
    if (waterSize_[0] != nx || waterSize_[1] != ny || waterSize_[2] != nz) {
        gl_.TexImage3D(TEXTURE_3D, 0, static_cast<GLint>(RG8), nx, ny, nz, 0, RG, UNSIGNED_BYTE, water.cells.data());
        gl_.TexParameteri(TEXTURE_3D, TEXTURE_MIN_FILTER, LINEAR);
        gl_.TexParameteri(TEXTURE_3D, TEXTURE_MAG_FILTER, LINEAR);
        for (GLenum wrap : {TEXTURE_WRAP_S, TEXTURE_WRAP_T, TEXTURE_WRAP_R}) gl_.TexParameteri(TEXTURE_3D, wrap, CLAMP_TO_EDGE);
        waterSize_[0] = nx;
        waterSize_[1] = ny;
        waterSize_[2] = nz;
    } else {
        gl_.TexSubImage3D(TEXTURE_3D, 0, 0, 0, 0, nx, ny, nz, RG, UNSIGNED_BYTE, water.cells.data());
    }
    gl_.BindTexture(TEXTURE_3D, 0);
    gl_.PixelStorei(UNPACK_ALIGNMENT, 4);
    waterDomain_ = water.domain;
    waterBand_ = water.band;
    hasWater_ = true;
}

void VolumeRenderer::setRain(const sim::RainFrame& rain) {
    hasRain_ = !rain.drops.empty() || !rain.droplets.empty();
    rainTimeStep_ = rain.timeStep;
    rainVertices_ = 0;
    if (hasRain_) {
        // The floor is wet where the drops are.
        wetMin_[0] = wetMin_[1] = 1e30f;
        wetMax_[0] = wetMax_[1] = -1e30f;
        for (size_t i = 0; i + 5 < rain.drops.size(); i += 6) {
            wetMin_[0] = std::min(wetMin_[0], rain.drops[i]);
            wetMax_[0] = std::max(wetMax_[0], rain.drops[i]);
            wetMin_[1] = std::min(wetMin_[1], rain.drops[i + 2]);
            wetMax_[1] = std::max(wetMax_[1], rain.drops[i + 2]);
        }
        // Six corners a streak: two triangles from its tail to its head.
        static const float corners[6][2] = {{0, -1}, {1, -1}, {1, 1}, {0, -1}, {1, 1}, {0, 1}};
        std::vector<float> v;
        v.reserve((rain.drops.size() + rain.droplets.size()) * 9);
        for (int kind = 0; kind < 2; ++kind) {
            const std::vector<float>& from = kind == 0 ? rain.drops : rain.droplets;
            for (size_t i = 0; i + 5 < from.size(); i += 6) {
                for (const auto& c : corners) {
                    v.insert(v.end(), {from[i], from[i + 1], from[i + 2], from[i + 3], from[i + 4], from[i + 5], c[0],
                                       c[1], static_cast<float>(kind)});
                }
            }
        }
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
    const Vec3 c = toScreen(lit);
    gl_.Enable(DEPTH_TEST);
    gl_.DepthFunc(LEQUAL);
    gl_.DepthMask(0);
    gl_.Enable(BLEND);
    gl_.BlendFunc(SRC_ALPHA, ONE_MINUS_SRC_ALPHA);
    gl_.UseProgram(rainProgram_);
    gl_.UniformMatrix4fv(location(rainProgram_, "u_viewProj"), 1, 0, viewProjection_.data());
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
    gl_.BindFramebuffer(FRAMEBUFFER, gFbo_);
    gl_.Viewport(0, 0, width, height);
    gl_.ClearColor(0.0f, 0.0f, 0.0f, -1.0f);  // w < 0: no mesh here
    gl_.Clear(COLOR_BUFFER_BIT | DEPTH_BUFFER_BIT);
    gl_.Enable(DEPTH_TEST);
    gl_.DepthFunc(LESS);
    gl_.DepthMask(1);
    gl_.Disable(BLEND);
    gl_.Disable(CULL_FACE);
    gl_.UseProgram(meshProgram_);
    gl_.UniformMatrix4fv(location(meshProgram_, "u_viewProj"), 1, 0, viewProjection_.data());
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
    if (geoVertices_ > 0 && geoProgram_) {
        gl_.UseProgram(geoProgram_);
        gl_.UniformMatrix4fv(location(geoProgram_, "u_viewProj"), 1, 0, viewProjection_.data());
        gl_.Uniform3f(location(geoProgram_, "u_eye"), eye.x, eye.y, eye.z);
        gl_.BindVertexArray(geoVao_);
        gl_.DrawArrays(TRIANGLES, 0, geoVertices_);
    }
    gl_.BindVertexArray(0);
    gl_.UseProgram(0);
    gl_.BindFramebuffer(FRAMEBUFFER, 0);
}

void VolumeRenderer::setGeometry(const GeometryPtr& geometry) {
    if (geometry == geometry_) return;
    geometry_ = geometry;
    shownDisplay_ = geometry ? sim::displayOf(*geometry) : sim::DisplayGeometry();
    uploadGeometry();
}

void VolumeRenderer::setPieces(const GeometryPtr& pieces) {
    if (pieces == pieces_) return;
    pieces_ = pieces;
    piecesDisplay_ = pieces ? sim::displayOf(*pieces) : sim::DisplayGeometry();
    uploadGeometry();
}

void VolumeRenderer::uploadGeometry() {
    // The two one after the other.
    sim::DisplayGeometry d = shownDisplay_;
    const sim::DisplayGeometry& p = piecesDisplay_;
    d.triangles.insert(d.triangles.end(), p.triangles.begin(), p.triangles.end());
    d.dots.insert(d.dots.end(), p.dots.begin(), p.dots.end());
    d.lines.insert(d.lines.end(), p.lines.begin(), p.lines.end());
    for (int a = 0; a < 3; ++a) {
        d.lo[a] = std::min(d.lo[a], p.lo[a]);
        d.hi[a] = std::max(d.hi[a], p.hi[a]);
    }
    hasGeoBounds_ = d.lo.x <= d.hi.x;
    geoLo_ = d.lo;
    geoHi_ = d.hi;
    // Each array into its buffer, with the layout of its attributes: {location, floats}.
    auto upload = [&](GLuint& vao, GLuint& buffer, const std::vector<float>& data, std::initializer_list<std::pair<int, int>> layout) {
        if (!vao) {
            gl_.GenVertexArrays(1, &vao);
            gl_.GenBuffers(1, &buffer);
        }
        int stride = 0;
        for (const auto& [where, floats] : layout) stride += floats;
        gl_.BindVertexArray(vao);
        gl_.BindBuffer(ARRAY_BUFFER, buffer);
        gl_.BufferData(ARRAY_BUFFER, static_cast<GLsizeiptr>(data.size() * sizeof(float)), data.data(), STATIC_DRAW);
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
    upload(geoVao_, geoBuffer_, d.triangles, {{0, 3}, {1, 3}, {2, 3}});
    upload(dotVao_, dotBuffer_, d.dots, {{0, 3}, {1, 3}, {2, 1}});
    upload(curveVao_, curveBuffer_, d.lines, {{0, 3}, {1, 4}});
    geoVertices_ = static_cast<GLsizei>(d.triangles.size() / 9);
    dots_ = static_cast<GLsizei>(d.dotCount());
    curveVertices_ = static_cast<GLsizei>(d.lines.size() / 7);
}

bool VolumeRenderer::geometryBounds(Vec3& lo, Vec3& hi) const {
    if ((!geometry_ && !pieces_) || !hasGeoBounds_) return false;
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
        float towards[3], across[3], upwards[3];
        orbit.axes(towards, across, upwards);
        const Vec3 light = normalize(s.lightDirection());
        const Vec3 f(towards[0], towards[1], towards[2]), r(across[0], across[1], across[2]), u(upwards[0], upwards[1], upwards[2]);
        gl_.DepthMask(1);
        gl_.Enable(PROGRAM_POINT_SIZE);
        gl_.UseProgram(dotProgram_);
        gl_.UniformMatrix4fv(location(dotProgram_, "u_viewProj"), 1, 0, viewProjection_.data());
        const float tanHalf = std::tan(orbit.fovY * kPi / 360.0f);
        gl_.Uniform1f(location(dotProgram_, "u_pixelsPerUnit"), 0.5f * static_cast<float>(height) / tanHalf);
        gl_.Uniform1f(location(dotProgram_, "u_dot"), std::max(3.0f, 3.0f * static_cast<float>(height) / 700.0f));
        gl_.Uniform3f(location(dotProgram_, "u_lightView"), dot(light, r), dot(light, u), -dot(light, f));
        gl_.Uniform3f(location(dotProgram_, "u_light"), s.lightColor.x * s.lightIntensity, s.lightColor.y * s.lightIntensity,
                      s.lightColor.z * s.lightIntensity);
        gl_.Uniform3f(location(dotProgram_, "u_sky"), s.skyColor.x * s.skyIntensity, s.skyColor.y * s.skyIntensity,
                      s.skyColor.z * s.skyIntensity);
        gl_.Uniform1f(location(dotProgram_, "u_exposure"), s.exposure);
        gl_.BindVertexArray(dotVao_);
        gl_.DrawArrays(POINTS, 0, dots_);
        gl_.Disable(PROGRAM_POINT_SIZE);
    }
    if (curveVertices_ > 0 && lineProgram_) {
        gl_.DepthMask(0);
        gl_.Enable(BLEND);
        gl_.BlendFunc(SRC_ALPHA, ONE_MINUS_SRC_ALPHA);
        gl_.UseProgram(lineProgram_);
        gl_.UniformMatrix4fv(location(lineProgram_, "u_viewProj"), 1, 0, viewProjection_.data());
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
    o.target[0] = target.x;
    o.target[1] = target.y;
    o.target[2] = target.z;
    o.fovY = camera.fovY();
    // The roll: how far the camera's right is turned from the level one.
    float level[3], right[3], up[3];
    o.axes(level, right, up);
    const Vec3 r = camera.right();
    o.roll = std::atan2(dot(r, Vec3(up[0], up[1], up[2])), dot(r, Vec3(right[0], right[1], right[2]))) * kDegrees;
    return o;
}

sim::Camera cameraFrom(const Orbit& orbit, sim::Camera camera) {
    float eye[3], forward[3], right[3], up[3];
    orbit.eye(eye);
    orbit.axes(forward, right, up);
    camera.position = Vec3(eye[0], eye[1], eye[2]);
    camera.rotation = sim::Camera::rotationFor(Vec3(forward[0], forward[1], forward[2]), Vec3(up[0], up[1], up[2]),
                                               camera.rotation);
    return camera;
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
    // The camera: where it is, and the directions of the screen's axes.
    float eye[3], towards[3], across[3], upwards[3];
    orbit.eye(eye);
    orbit.axes(towards, across, upwards);
    const float aspect = static_cast<float>(width) / static_cast<float>(height);
    viewProjection_ = multiply(perspective(orbit.fovY, aspect, kNear, kFar), lookAlong(eye, towards, upwards));
    // The meshes first, into their own buffer, seen by the same camera.
    const bool meshes = (anyMesh_ || geoVertices_ > 0) && meshProgram_;
    if (meshes) renderMeshes(width, height, Vec3(eye[0], eye[1], eye[2]));
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

    const Vec3 e(eye[0], eye[1], eye[2]), forward(towards[0], towards[1], towards[2]);
    const Vec3 right(across[0], across[1], across[2]), up(upwards[0], upwards[1], upwards[2]);
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

    gl_.BindVertexArray(vao_);
    gl_.DrawArrays(TRIANGLES, 0, 3);
    drawGeometry(width, height);
    drawRain(width, height, e);

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

    for (int unit = 4; unit < 4 + kMaxMeshShadows; ++unit) {
        gl_.ActiveTexture(TEXTURE0 + static_cast<GLenum>(unit));
        gl_.BindTexture(TEXTURE_3D, 0);
    }
    gl_.ActiveTexture(TEXTURE8);
    gl_.BindTexture(TEXTURE_3D, 0);
    gl_.ActiveTexture(TEXTURE9);
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
}

std::vector<uint8_t> VolumeRenderer::readPixels(int factor) const {
    return readRgb(gl_, fbo_, width_, height_, factor);
}

}  // namespace pg::gl
