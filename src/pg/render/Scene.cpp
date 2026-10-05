#include "pg/render/Scene.h"

#include "pg/core/Instances.h"
#include "pg/core/Parallel.h"
#include "pg/render/Particles.h"
#include "pg/render/Textures.h"
#include "pg/sim/Display.h"
#include "pg/sim/Frame.h"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <numeric>
#include <tuple>
#include <unordered_map>

namespace pg::render {
namespace {

constexpr float kPi = 3.14159265358979f;

/// A number attribute of `set`, when it is one.
const AttributeArray* numberAttribute(const AttributeSet& set, const char* name) {
    const AttributeArray* a = set.find(name);
    return a && a->type() == AttrType::Float ? a : nullptr;
}

/// A material's number: of the primitive, else of its first point, else of
/// the detail, else `fallback`.
struct MaterialNumber {
    const AttributeArray* prim;
    const AttributeArray* point;
    float detail;
    bool given;  ///< the geometry has it, of one class or another

    MaterialNumber(const Geometry& geo, const char* name, float fallback)
        : prim(numberAttribute(geo.primitives(), name)), point(numberAttribute(geo.points(), name)), detail(fallback) {
        const AttributeArray* d = numberAttribute(geo.detail(), name);
        if (d && d->size() > 0) detail = d->read<float>()[0];
        given = prim || point || (d && d->size() > 0);
    }
    float at(const Geometry& geo, uint32_t primitive) const {
        if (prim && primitive < prim->size()) return prim->read<float>()[primitive];
        if (point) {
            const auto pts = geo.primitivePoints(primitive);
            if (!pts.empty() && pts[0] < point->size()) return point->read<float>()[pts[0]];
        }
        return detail;
    }
};

/// The tangent of each corner of `tris` (Mesh::tangents): of each
/// triangle, the way u goes along it -- and v, for which way round -- summed
/// over the corners that are one (the same place, normal and uv, the uv not
/// mirrored one way and the other), each as wide an angle as it has there;
/// made a unit long across the corner's normal.
std::vector<Vec4> cornerTangents(const sim::ShadedTriangles& tris) {
    const size_t n = tris.count();
    std::vector<Vec4> out(3 * n, Vec4(1.0f, 0.0f, 0.0f, 1.0f));
    if (tris.uvs.size() != 3 * n) return out;
    std::vector<Vec3> du(3 * n, Vec3(0.0f)), dv(3 * n, Vec3(0.0f));
    std::vector<int8_t> side(n, 1);
    for (size_t t = 0; t < n; ++t) {
        const Vec3* p = &tris.positions[3 * t];
        const Vec2* q = &tris.uvs[3 * t];
        const Vec3 e1 = p[1] - p[0], e2 = p[2] - p[0];
        const Vec2 d1 = q[1] - q[0], d2 = q[2] - q[0];
        const float r = d1.x * d2.y - d2.x * d1.y;
        if (!(std::fabs(r) > 1e-20f) || !std::isfinite(r)) continue;
        side[t] = r > 0.0f ? 1 : -1;
        const Vec3 tu = (e1 * d2.y - e2 * d1.y) / r, tv = (e2 * d1.x - e1 * d2.x) / r;
        for (int c = 0; c < 3; ++c) {
            // The angle at the corner.
            const Vec3 a = p[(c + 1) % 3] - p[c], b = p[(c + 2) % 3] - p[c];
            const float la = length(a), lb = length(b);
            const float angle = la > 0.0f && lb > 0.0f ? std::acos(std::clamp(dot(a, b) / (la * lb), -1.0f, 1.0f)) : 0.0f;
            du[3 * t + static_cast<size_t>(c)] = tu * angle;
            dv[3 * t + static_cast<size_t>(c)] = tv * angle;
        }
    }
    // The corners that are one: the bits of their place, normal and uv,
    // and which way round their uv goes.
    struct Key {
        std::array<uint32_t, 8> bits;
        int8_t side;
        bool operator==(const Key&) const = default;
    };
    struct Hash {
        size_t operator()(const Key& k) const {
            uint64_t h = 0x9e3779b97f4a7c15ull ^ static_cast<uint64_t>(k.side + 2);
            for (const uint32_t b : k.bits) {
                h ^= b + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
                h *= 0xbf58476d1ce4e5b9ull;
            }
            return static_cast<size_t>(h ^ (h >> 31));
        }
    };
    auto bitsOf = [](float x) {
        uint32_t b;
        x = x == 0.0f ? 0.0f : x;  // -0 is 0
        std::memcpy(&b, &x, sizeof b);
        return b;
    };
    std::unordered_map<Key, std::pair<Vec3, Vec3>, Hash> sums;
    sums.reserve(3 * n);
    std::vector<Key> keys(3 * n);
    for (size_t i = 0; i < 3 * n; ++i) {
        const Vec3& p = tris.positions[i];
        const Vec3& nn = tris.normals[i];
        const Vec2& q = tris.uvs[i];
        keys[i] = {{bitsOf(p.x), bitsOf(p.y), bitsOf(p.z), bitsOf(nn.x), bitsOf(nn.y), bitsOf(nn.z), bitsOf(q.x), bitsOf(q.y)},
                   side[i / 3]};
        auto& sum = sums[keys[i]];
        sum.first = sum.first + du[i];
        sum.second = sum.second + dv[i];
    }
    for (size_t i = 0; i < 3 * n; ++i) {
        const auto& [su, sv] = sums[keys[i]];
        const Vec3 nn = tris.normals[i];
        Vec3 t = su - nn * dot(nn, su);
        if (!(dot(t, t) > 1e-30f)) {
            // No way u goes: any way across the normal.
            t = std::fabs(nn.x) < 0.9f ? cross(nn, Vec3(1.0f, 0.0f, 0.0f)) : cross(nn, Vec3(0.0f, 1.0f, 0.0f));
            t = cross(t, nn);
        }
        t = normalize(t);
        out[i] = Vec4(t, dot(cross(nn, t), sv) < 0.0f ? -1.0f : 1.0f);
    }
    return out;
}

float smoothstep(float a, float b, float x) {
    const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

/// What a ray met of a mesh: which triangle, and where on it.
struct TriangleHit {
    uint32_t triangle = 0;
    float u = 0.0f, v = 0.0f;
};

/// Triangle `t` of `m` -- a corner and the edges from it -- where it is
/// `time` seconds from now, its corners moving along their velocity.
struct Corners {
    Vec3 v0, e1, e2;
};
Corners cornersAt(const Mesh& m, uint32_t t, float time) {
    if (time == 0.0f || m.velocity.empty()) return {m.v0[t], m.e1[t], m.e2[t]};
    const Vec3 &a = m.velocity[3 * t], &b = m.velocity[3 * t + 1], &c = m.velocity[3 * t + 2];
    return {m.v0[t] + a * time, m.e1[t] + (b - a) * time, m.e2[t] + (c - a) * time};
}

/// The nearest triangle of `m` a ray (in the mesh's space) meets before
/// `best`, `time` seconds from now; `best` shortened to it.
bool nearestTriangle(const Mesh& m, const Vec3& origin, const Vec3& dir, float& best, TriangleHit& out, float time) {
    bool found = false;
    traverse(m.bvh, origin, dir, 0.0f, best, [&](uint32_t first, uint32_t count) {
        for (uint32_t t = first; t < first + count; ++t) {
            const Corners c = cornersAt(m, t, time);
            const Vec3 p = cross(dir, c.e2);
            const float det = dot(c.e1, p);
            if (det == 0.0f) continue;
            const float inv = 1.0f / det;
            const Vec3 s = origin - c.v0;
            const float u = dot(s, p) * inv;
            if (u < 0.0f || u > 1.0f) continue;
            const Vec3 q = cross(s, c.e1);
            const float v = dot(dir, q) * inv;
            if (v < 0.0f || u + v > 1.0f) continue;
            const float d = dot(c.e2, q) * inv;
            if (d > 0.0f && d < best) {
                best = d;
                out = {t, u, v};
                found = true;
            }
        }
        return true;
    });
    return found;
}

/// A turn as the images of the three axes.
void axesOf(const Vec4& q, Placed& p) {
    p.axes = glm::mat3_cast(quatOf(q));
}

/// The box round a placed mesh: its box's corners, placed.
/// Where a placed mesh is -- `sweep` seconds before now and after too, if
/// it flies.
Box placedBox(const Placed& p, const Mesh& m, float sweep) {
    Box b;
    if (m.box.empty()) return b;
    const bool flies = sweep > 0.0f && p.velocity != Vec3(0.0f);
    for (const float t : {0.0f, -sweep, sweep}) {
        if (t != 0.0f && !flies) continue;
        const Placed q = p.movedBy(t);
        for (int c = 0; c < 8; ++c) {
            b.grow(q.toWorld(Vec3(c & 1 ? m.box.hi.x : m.box.lo.x, c & 2 ? m.box.hi.y : m.box.lo.y,
                                  c & 4 ? m.box.hi.z : m.box.lo.z)));
        }
    }
    return b;
}

/// Where a solid is while it moves `sweep` seconds either way: turning, as
/// far round its centre as it reaches; carried, along its way.
Box solidBox(const sim::Solid& solid, const sim::ShapeInstance& shape, float sweep) {
    Box b;
    shape.bounds(b.lo, b.hi);
    const sim::Collider& body = solid.body;
    if (!(sweep > 0.0f) || !body.moves()) return b;
    if (body.spin != Vec3(0.0f)) {
        float reach = 0.0f;
        for (int c = 0; c < 8; ++c) {
            const Vec3 corner(c & 1 ? b.hi.x : b.lo.x, c & 2 ? b.hi.y : b.lo.y, c & 4 ? b.hi.z : b.lo.z);
            reach = std::max(reach, length(corner - body.center));
        }
        b.lo = body.center - Vec3(reach);
        b.hi = body.center + Vec3(reach);
    }
    const Vec3 go = body.velocity * sweep;
    Box swept;
    swept.grow(b.lo - glm::abs(go));
    swept.grow(b.hi + glm::abs(go));
    return swept;
}

}  // namespace

void primitiveMaterials(const Geometry& geo, bool water, bool hasUv, std::vector<Material>& out, std::vector<uint16_t>& ofPrim) {
    out.clear();
    // What each primitive is made of: materials told apart by their numbers,
    // in steps of a 256th.
    const MaterialNumber roughness(geo, "roughness", 0.5f), metallic(geo, "metallic", 0.0f),
        translucency(geo, "translucency", 0.0f), detail(geo, "surface_detail", 1.0f);
    // What each primitive is made of (s@material): the preset of each name
    // of its table.
    const AttributeArray* named = geo.primitives().find("material");
    if (named && named->type() != AttrType::String) named = nullptr;
    std::vector<MaterialPreset> presetOfName;
    if (named) {
        for (const std::string& s : named->strings()) presetOfName.push_back(materialPreset(s));
    }
    auto presetOf = [&](uint32_t prim) {
        if (!named || prim >= named->size()) return MaterialPreset::None;
        const int32_t i = named->read<int32_t>()[prim];
        return i >= 0 && static_cast<size_t>(i) < presetOfName.size() ? presetOfName[static_cast<size_t>(i)]
                                                                      : MaterialPreset::None;
    };
    // Textures of their own (the Material node's): which, how big, and
    // whether the colour Cd tints them.
    const AttributeArray* textures = geo.primitives().find("texture");
    if (textures && textures->type() != AttrType::String) textures = nullptr;
    const MaterialNumber textureSize(geo, "texture_size", 0.0f);
    const AttributeArray* tints = geo.primitives().find("texture_tint");
    if (tints && tints->type() != AttrType::Int) tints = nullptr;
    // How the pictures are laid on (0 auto, 1 by uv, 2 from three sides) and
    // how strongly a normal map bends the light.
    const AttributeArray* projections = geo.primitives().find("texture_projection");
    if (projections && projections->type() != AttrType::Int) projections = nullptr;
    const MaterialNumber normalStrength(geo, "texture_normal", 1.0f);
    auto textureOfPrim = [&](uint32_t prim) -> const std::string& {
        static const std::string none;
        if (!textures || prim >= textures->size()) return none;
        return textures->stringValue(textures->read<int32_t>()[prim]);
    };
    std::map<std::tuple<std::array<int, 8>, std::string, int>, uint16_t> known;
    const AttributeArray* glassAttr = geo.primitives().find("glass");
    auto glassOf = [&](size_t prim) -> int {
        if (!glassAttr || prim >= glassAttr->size()) return 0;
        const float g = glassAttr->type() == AttrType::Int ? static_cast<float>(glassAttr->read<int32_t>()[prim])
                        : glassAttr->type() == AttrType::Float ? glassAttr->read<float>()[prim]
                                                               : 0.0f;
        return g >= 1.5f ? 2 : g >= 0.5f ? 1 : 0;
    };
    ofPrim.assign(geo.primitiveCount(), 0);
    auto quantize = [](float x) { return static_cast<int>(std::lround(std::clamp(x, 0.0f, 1.0f) * 255.0f)); };
    for (uint32_t prim = 0; prim < geo.primitiveCount(); ++prim) {
        // Only what is drawn: the closed polygons.
        if (!geo.primitiveClosed(prim) || geo.primitiveVertexCount(prim) < 3) continue;
        Material m;
        const int glass = glassOf(prim);
        const MaterialPreset preset = presetOf(prim);
        const std::string& texture = textureOfPrim(prim);
        if (water) {
            m.kind = Material::Kind::Water;
            m.roughness = 0.0f;
            m.ior = 1.33f;
        } else if (glass == 1 || (glass == 0 && preset == MaterialPreset::Glass)) {
            m.kind = Material::Kind::Glass;
            m.roughness = 0.0f;
            m.ior = 1.5f;
        } else {
            const PresetSurface base = presetSurface(preset);
            m.preset = preset;
            m.roughness = static_cast<float>(quantize(roughness.given ? roughness.at(geo, prim) : base.roughness)) / 255.0f;
            m.metallic = static_cast<float>(quantize(metallic.given ? metallic.at(geo, prim) : base.metallic)) / 255.0f;
            m.translucency = static_cast<float>(quantize(translucency.at(geo, prim))) / 255.0f;
            m.detail = static_cast<float>(quantize(detail.at(geo, prim))) / 255.0f;
            if (glass == 2) m.roughness = 0.35f;  // a crack: a rough, white break in the glass
            if (!texture.empty()) {
                m.texture = texture;
                m.textureSize = std::max(textureSize.at(geo, prim), 0.0f);
                m.textureTint = static_cast<int8_t>(tints && prim < tints->size() && tints->read<int32_t>()[prim] != 0);
            }
            // By uv where the triangles have it: asked for, or -- Auto -- a
            // texture of one's own, or a material whose pictures are made
            // for uv (laidByUv: bark, leaves, grass); the other materials'
            // photographs are made to be laid on from three sides, so many
            // metres a picture.
            const int32_t how = projections && prim < projections->size() ? projections->read<int32_t>()[prim] : 0;
            m.byUv = hasUv && (how == 1 || (how == 0 && (!texture.empty() || laidByUv(preset))));
            m.normalStrength = std::round(std::clamp(normalStrength.at(geo, prim), 0.0f, 10.0f) * 100.0f) / 100.0f;
        }
        const auto key = std::make_tuple(std::array<int, 8>{static_cast<int>(m.kind), quantize(m.roughness), quantize(m.metallic),
                                                            quantize(m.translucency), static_cast<int>(m.preset),
                                                            static_cast<int>(m.byUv),
                                                            static_cast<int>(std::lround(m.normalStrength * 100.0f)),
                                                            quantize(m.detail)},
                                         m.texture, static_cast<int>(std::lround(m.textureSize * 1000.0f)) * 3 + m.textureTint + 1);
        auto it = known.find(key);
        if (it == known.end()) {
            if (out.size() >= 65535) {
                ofPrim[prim] = 0;
                continue;
            }
            // Cut out where its pictures' alpha has none: its own set's, or
            // its material's in the library.
            if (m.byUv) {
                const TextureSet set = !m.texture.empty() ? textureSet(m.texture) : presetTextureSet(textureLibrary(), m.preset);
                m.cutout = !set.alpha.empty();
            }
            it = known.emplace(key, static_cast<uint16_t>(out.size())).first;
            out.push_back(m);
        }
        ofPrim[prim] = it->second;
    }
}

std::shared_ptr<const Mesh> meshOf(const Geometry& geo, bool water, RayEngine engine, float sweep) {
    auto mesh = std::make_shared<Mesh>();
    const sim::ShadedTriangles tris = sim::shadedTriangles(geo);
    const size_t n = tris.count();
    if (n == 0) return mesh;
    // Moving: its corners met where they are within `sweep` of now.
    const bool moving = sweep > 0.0f && tris.velocities.size() == tris.positions.size() &&
                        std::any_of(tris.velocities.begin(), tris.velocities.end(), [](const Vec3& v) { return v != Vec3(0.0f); });
    if (moving) mesh->sweep = sweep;

    // What each triangle is made of: its primitive's material.
    const bool hasUv = tris.uvs.size() == 3 * n;
    std::vector<uint16_t> ofPrim, which(n);
    primitiveMaterials(geo, water, hasUv, mesh->materials, ofPrim);
    for (size_t t = 0; t < n; ++t) which[t] = ofPrim[tris.prims[t]];
    // Geometry with no colour of its own: each surface its material's.
    const bool colored = geo.vertices().find("Cd") || geo.points().find("Cd") || geo.primitives().find("Cd") ||
                         geo.detail().find("Cd");
    for (const Material& m : mesh->materials) {
        mesh->clear = mesh->clear || m.kind != Material::Kind::Surface;
        mesh->cutout = mesh->cutout || m.cutout;
    }
    // The colour of each material, for geometry with none of its own: the
    // viewport's grey where it is of none, or is glass or water.
    std::vector<Vec3> own;
    if (!colored) {
        for (const Material& m : mesh->materials) {
            own.push_back(m.kind == Material::Kind::Surface && m.preset != MaterialPreset::None
                              ? presetSurface(m.preset).color
                              : presetSurface(MaterialPreset::None).color);
        }
    }
    // The windows' faces each a number of its own: of where its first
    // triangle was before it moved, so that it stays the same while pieces
    // come and go.
    std::vector<float> random;
    const bool windows = std::any_of(mesh->materials.begin(), mesh->materials.end(),
                                     [](const Material& m) { return m.preset == MaterialPreset::Window; });
    if (windows) {
        random.resize(n);
        const std::vector<Vec3>& still = tris.rest.empty() ? tris.positions : tris.rest;
        size_t first = 0;
        for (size_t t = 0; t < n; ++t) {
            if (t == 0 || tris.prims[t] != tris.prims[t - 1]) first = t;
            const Vec3 c = (still[3 * first] + still[3 * first + 1] + still[3 * first + 2]) / 3.0f;
            uint64_t h = 0x9e3779b97f4a7c15ull;
            for (int a = 0; a < 3; ++a) {
                h ^= static_cast<uint64_t>(static_cast<int64_t>(std::lround(c[a] * 1000.0f)));
                h *= 0xbf58476d1ce4e5b9ull;
                h ^= h >> 31;
            }
            random[t] = static_cast<float>(h >> 40) / static_cast<float>(1u << 24);
        }
    }

    // Our own hierarchy, and the triangles in the order its leaves take
    // them; Embree's keeps them as they come.
    const bool embree = engine == RayEngine::Embree && embreeAvailable();
    std::vector<Box> boxes(embree ? 0 : n);
    for (size_t t = 0; t < n; ++t) {
        for (size_t c = 0; c < 3; ++c) {
            const Vec3& p = tris.positions[3 * t + c];
            const Vec3 go = moving ? tris.velocities[3 * t + c] * sweep : Vec3(0.0f);
            for (const Vec3& q : {p, p - go, p + go}) {
                if (!embree) boxes[t].grow(q);
                mesh->box.grow(q);
            }
        }
    }
    if (!embree) {
        mesh->bvh = buildBvh(boxes, 4);
    } else {
        mesh->bvh.items.resize(n);
        std::iota(mesh->bvh.items.begin(), mesh->bvh.items.end(), 0u);
    }
    mesh->v0.resize(n);
    mesh->e1.resize(n);
    mesh->e2.resize(n);
    mesh->normals.resize(3 * n);
    mesh->colors.resize(3 * n);
    if (!tris.rest.empty()) mesh->rest.resize(3 * n);
    if (!tris.velocities.empty()) mesh->velocity.resize(3 * n);
    if (windows) mesh->random.resize(n);
    mesh->material.resize(n);
    // The uv and its tangents, where a material lays its pictures on by it.
    std::vector<Vec4> tangents;
    if (hasUv && std::any_of(mesh->materials.begin(), mesh->materials.end(), [](const Material& m) { return m.byUv; })) {
        tangents = cornerTangents(tris);
        mesh->uv.resize(3 * n);
        mesh->tangents.resize(3 * n);
    }
    parallelFor(n, 8192, [&](size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i) {
            const size_t t = mesh->bvh.items[i];
            const Vec3& a = tris.positions[3 * t];
            mesh->v0[i] = a;
            mesh->e1[i] = tris.positions[3 * t + 1] - a;
            mesh->e2[i] = tris.positions[3 * t + 2] - a;
            for (size_t c = 0; c < 3; ++c) {
                mesh->normals[3 * i + c] = tris.normals[3 * t + c];
                if (!mesh->rest.empty()) mesh->rest[3 * i + c] = tris.rest[3 * t + c];
                if (!mesh->velocity.empty()) mesh->velocity[3 * i + c] = tris.velocities[3 * t + c];
                if (!mesh->uv.empty()) {
                    mesh->uv[3 * i + c] = tris.uvs[3 * t + c];
                    mesh->tangents[3 * i + c] = tangents[3 * t + c];
                }
                const Vec3 col = colored ? tris.colors[3 * t + c] : own[which[t]];
                mesh->colors[3 * i + c] = Vec3(std::clamp(col.x, 0.0f, 1.0f), std::clamp(col.y, 0.0f, 1.0f),
                                               std::clamp(col.z, 0.0f, 1.0f));
            }
            mesh->material[i] = which[t];
            if (windows) mesh->random[i] = random[t];
        }
    });
    if (embree) {
        mesh->bvh = Bvh();
        mesh->embree = moving ? EmbreeMesh::build(tris.positions, tris.velocities, sweep) : EmbreeMesh::build(tris.positions);
    } else {
        std::iota(mesh->bvh.items.begin(), mesh->bvh.items.end(), 0u);
    }
    return mesh;
}

std::shared_ptr<const Mesh> meshOfTriangles(const std::vector<Vec3>& corners, const std::vector<Vec3>& normals,
                                            const Material& material, const Vec3& color, RayEngine engine) {
    auto mesh = std::make_shared<Mesh>();
    const size_t n = corners.size() / 3;
    if (n == 0) return mesh;
    mesh->materials.push_back(material);
    mesh->clear = material.kind == Material::Kind::Glass || material.kind == Material::Kind::Water;
    mesh->shadows = material.kind != Material::Kind::Rain;
    // Our own hierarchy, and the triangles in the order its leaves take
    // them; Embree's keeps them as they come (as meshOf).
    const bool embree = engine == RayEngine::Embree && embreeAvailable();
    std::vector<Box> boxes(embree ? 0 : n);
    for (size_t t = 0; t < n; ++t) {
        for (size_t c = 0; c < 3; ++c) {
            if (!embree) boxes[t].grow(corners[3 * t + c]);
            mesh->box.grow(corners[3 * t + c]);
        }
    }
    if (!embree) {
        mesh->bvh = buildBvh(boxes, 4);
    } else {
        mesh->bvh.items.resize(n);
        std::iota(mesh->bvh.items.begin(), mesh->bvh.items.end(), 0u);
    }
    mesh->v0.resize(n);
    mesh->e1.resize(n);
    mesh->e2.resize(n);
    mesh->normals.resize(3 * n);
    mesh->colors.assign(3 * n, color);
    mesh->material.assign(n, 0);
    const bool shaded = normals.size() == corners.size();
    parallelFor(n, 8192, [&](size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i) {
            const size_t t = mesh->bvh.items[i];
            const Vec3& a = corners[3 * t];
            mesh->v0[i] = a;
            mesh->e1[i] = corners[3 * t + 1] - a;
            mesh->e2[i] = corners[3 * t + 2] - a;
            const Vec3 face = cross(mesh->e1[i], mesh->e2[i]);
            const float l = length(face);
            for (size_t c = 0; c < 3; ++c) {
                mesh->normals[3 * i + c] = shaded ? normals[3 * t + c] : l > 0.0f ? face / l : Vec3(0.0f, 1.0f, 0.0f);
            }
        }
    });
    if (embree) {
        mesh->bvh = Bvh();
        mesh->embree = EmbreeMesh::build(corners);
    } else {
        std::iota(mesh->bvh.items.begin(), mesh->bvh.items.end(), 0u);
    }
    return mesh;
}

bool Scene::meetSolid(size_t s, const Vec3& origin, const Vec3& dir, float time, float& t, Vec3& normal) const {
    const sim::Collider& body = solids[s].body;
    if (time == 0.0f || !body.moves()) return shapes[s].intersect(origin, dir, 0.0f, t, normal);
    // The ray where the solid stands now, turned and carried back as it
    // moves; the normal turned with it again.
    const Mat3 turn = body.turnAt(time), back = glm::transpose(turn);
    const Vec3 o = back * (origin - body.center - body.velocity * time) + body.center;
    if (!shapes[s].intersect(o, back * dir, 0.0f, t, normal)) return false;
    normal = turn * normal;
    return true;
}

sim::Camera Scene::cameraAt(float time) const {
    if (time == 0.0f || !cameraMoves || !(frameTime > 0.0f)) return camera;
    return time < 0.0f ? camera.toward(cameraBefore, -time / frameTime) : camera.toward(cameraAfter, time / frameTime);
}

bool Scene::intersect(const Vec3& origin, const Vec3& dir, float tMax, float fade, Hit& hit, float time,
                      const Coverage* coverage) const {
    // Past what is cut out, face after face: each there as often as its
    // coverage says, by a number of its own made of `fade`.
    Vec3 from = origin;
    float gone = 0.0f;
    uint32_t bits = 0;
    std::memcpy(&bits, &fade, sizeof bits);
    for (uint32_t layer = 0;; ++layer) {
        if (!intersectWhole(from, dir, tMax - gone, fade, hit, time)) return false;
        if (!coverage || !hit.material || !hit.material->cutout || layer >= 64) break;
        uint32_t h = bits ^ (layer + 1) * 0x9E3779B9u;
        h = (h ^ (h >> 16)) * 0x7FEB352Du;
        h = (h ^ (h >> 15)) * 0x846CA68Bu;
        h ^= h >> 16;
        if (static_cast<float>(h >> 8) * (1.0f / 16777216.0f) < coverage->at(*hit.material, hit.uv)) break;
        const float step = hit.t + std::max(hit.t * 1e-5f, 1e-5f);
        from = from + dir * step;
        gone += step;
        if (gone >= tMax) return false;
    }
    hit.t += gone;
    return true;
}

bool Scene::intersectWhole(const Vec3& origin, const Vec3& dir, float tMax, float fade, Hit& hit, float time) const {
    float best = tMax;
    int placedHit = -1, solidHit = -1;
    TriangleHit triangle;
    Vec3 solidNormal;
    auto solid = [&](size_t s) {
        float t = 0.0f;
        Vec3 n;
        if (meetSolid(s, origin, dir, time, t, n) && t > 0.0f && t < best) {
            best = t;
            solidHit = static_cast<int>(s);
            placedHit = -1;
            solidNormal = n;
        }
    };
    if (embree) {
        // The meshes through Embree, the solids nearer than what it met through ours.
        EmbreeHit e;
        if (embree->nearest(origin, dir, 0.0f, best, e, time)) {
            best = e.t;
            placedHit = static_cast<int>(e.placed);
            triangle = {e.triangle, e.u, e.v};
        }
        traverse(top, origin, dir, 0.0f, best, [&](uint32_t first, uint32_t count) {
            for (uint32_t k = first; k < first + count; ++k) solid(top.items[k]);
            return true;
        });
    } else {
        traverse(top, origin, dir, 0.0f, best, [&](uint32_t first, uint32_t count) {
            for (uint32_t k = first; k < first + count; ++k) {
                const uint32_t item = top.items[k];
                if (item < placed.size()) {
                    const Placed p = placed[item].movedBy(time);
                    TriangleHit th;
                    if (nearestTriangle(*meshes[p.mesh], p.toLocal(origin), p.dirToLocal(dir), best, th, time)) {
                        placedHit = static_cast<int>(item);
                        solidHit = -1;
                        triangle = th;
                    }
                } else {
                    solid(item - placed.size());
                }
            }
            return true;
        });
    }

    // The floor: under what lies on it, fading out far away.
    if (look.floor && dir.y < -1e-9f && origin.y > 0.0f) {
        const float t = -origin.y / dir.y;
        const bool behind = placedHit < 0 && solidHit < 0 ? t < best : t < best * (1.0f - 2e-4f);
        if (behind) {
            const Vec3 at = origin + dir * t;
            const float away = std::sqrt(at.x * at.x + at.z * at.z) / floorRadius;
            if (fade < 1.0f - smoothstep(0.35f, 1.0f, away)) {
                hit.t = t;
                hit.position = at;
                hit.normal = hit.face = Vec3(0.0f, 1.0f, 0.0f);
                hit.color = look.groundColor;
                hit.material = &floorMaterial;
                hit.floor = true;
                hit.solid = -1;
                return true;
            }
        }
    }
    if (placedHit >= 0) {
        const Placed& p = placed[static_cast<size_t>(placedHit)];
        const Mesh& m = *meshes[p.mesh];
        const uint32_t t = triangle.triangle;
        const float w = 1.0f - triangle.u - triangle.v;
        hit.t = best;
        hit.position = origin + dir * best;
        // Its face as it is then; its pattern stays where it was (rest).
        const Corners moved = cornersAt(m, t, time);
        hit.face = normalize(p.turn(cross(moved.e1, moved.e2)));
        const Vec3 n = m.normals[3 * t] * w + m.normals[3 * t + 1] * triangle.u + m.normals[3 * t + 2] * triangle.v;
        hit.normal = dot(n, n) > 1e-20f ? normalize(p.turn(n)) : hit.face;
        hit.color = (m.colors[3 * t] * w + m.colors[3 * t + 1] * triangle.u + m.colors[3 * t + 2] * triangle.v) * p.tint;
        hit.tint = p.tint;
        if (m.rest.empty()) {
            hit.rest = m.v0[t] + m.e1[t] * triangle.u + m.e2[t] * triangle.v;
            hit.restFace = cross(m.e1[t], m.e2[t]);
        } else {
            const Vec3 &r0 = m.rest[3 * t], &r1 = m.rest[3 * t + 1], &r2 = m.rest[3 * t + 2];
            hit.rest = r0 * w + r1 * triangle.u + r2 * triangle.v;
            hit.restFace = cross(r1 - r0, r2 - r0);
        }
        if (m.uv.empty()) {
            hit.uv = Vec2(0.0f);
            hit.tangent = Vec3(0.0f);
        } else {
            hit.uv = m.uv[3 * t] * w + m.uv[3 * t + 1] * triangle.u + m.uv[3 * t + 2] * triangle.v;
            const Vec4 k = m.tangents[3 * t] * w + m.tangents[3 * t + 1] * triangle.u + m.tangents[3 * t + 2] * triangle.v;
            hit.tangent = p.turn(Vec3(k));
            hit.handed = m.tangents[3 * t].w < 0.0f ? -1.0f : 1.0f;
        }
        hit.material = &m.materials[m.material[t]];
        hit.floor = false;
        hit.solid = -1;
        return true;
    }
    if (solidHit >= 0) {
        hit.t = best;
        hit.position = origin + dir * best;
        hit.normal = hit.face = normalize(solidNormal);
        hit.color = solids[static_cast<size_t>(solidHit)].color;
        hit.material = &solidMaterial;
        hit.floor = false;
        hit.solid = solidHit;
        return true;
    }
    return false;
}

sim::Matte Scene::matteOf(const Hit& hit) const {
    if (!plate) return sim::Matte::None;
    if (hit.floor) return look.floorMatte;
    return hit.solid >= 0 && static_cast<size_t>(hit.solid) < solids.size() ? solids[static_cast<size_t>(hit.solid)].matte
                                                                              : sim::Matte::None;
}

bool Scene::realBlocks(const Vec3& origin, const Vec3& dir, float tMax, float time) const {
    for (size_t i = 0; i < solids.size() && i < shapes.size(); ++i) {
        if (solids[i].matte == sim::Matte::None) continue;
        float t = 0.0f;
        Vec3 n;
        if (meetSolid(i, origin, dir, time, t, n) && t > 0.0f && t < tMax) return true;
    }
    // The floor, where it is a real thing too.
    if (look.floor && look.floorMatte != sim::Matte::None && dir.y < -1e-9f && origin.y > 0.0f) {
        const float t = -origin.y / dir.y;
        if (t < tMax) {
            const Vec3 at = origin + dir * t;
            if (std::sqrt(at.x * at.x + at.z * at.z) < floorRadius) return true;
        }
    }
    return false;
}

namespace {

/// Light through a clear triangle -- glass or water: what it does not
/// reflect, tinted a little at each face; water a little less -- or past
/// one cut out, at (u, v) on it, as much as is not there. False -- no
/// light at all -- for an opaque one.
bool passThrough(const Mesh& m, uint32_t t, float u, float v, const Placed& p, const Coverage* coverage, Vec3& through) {
    const Material& mat = m.materials[m.material[t]];
    if (mat.kind == Material::Kind::Surface) {
        if (!mat.cutout || !coverage || m.uv.empty()) return false;
        const Vec2 uv = m.uv[3 * t] * (1.0f - u - v) + m.uv[3 * t + 1] * u + m.uv[3 * t + 2] * v;
        through = through * (1.0f - std::clamp(coverage->at(mat, uv), 0.0f, 1.0f));
        return std::max({through.x, through.y, through.z}) > 1e-4f;
    }
    if (mat.kind == Material::Kind::Rain) return true;  // casts no shadow
    if (mat.kind == Material::Kind::Glass) {
        const Vec3 c = (m.colors[3 * t] + m.colors[3 * t + 1] + m.colors[3 * t + 2]) * (1.0f / 3.0f) * p.tint;
        through = through * (Vec3(1.0f, 1.0f, 1.0f) * 0.65f + c * 0.35f) * 0.92f;
    } else {
        through = through * 0.9f;
    }
    return true;
}

}  // namespace

Vec3 Scene::transmittance(const Vec3& origin, const Vec3& dir, float tMax, float time, const Coverage* coverage) const {
    Vec3 through(1.0f, 1.0f, 1.0f);
    bool blocked = false;
    if (embree) {
        // A solid in the way, then an opaque mesh: no light.
        traverse(top, origin, dir, 0.0f, tMax, [&](uint32_t first, uint32_t count) {
            for (uint32_t k = first; k < first + count && !blocked; ++k) {
                float t = 0.0f;
                Vec3 n;
                if (meetSolid(top.items[k], origin, dir, time, t, n) && t > 0.0f && t < tMax) blocked = true;
            }
            return !blocked;
        });
        if (blocked || embree->blocked(origin, dir, tMax, time)) return {};
        // Through the glass and the water and past what is cut out, face
        // after face, from the nearest -- no light past 256 of them.
        float from = 0.0f;
        EmbreeHit e;
        for (int faces = 0; embree->nearestClear(origin, dir, from, tMax, e, time); ++faces) {
            const Placed& p = placed[e.placed];
            if (faces >= 256 || !passThrough(*meshes[p.mesh], e.triangle, e.u, e.v, p, coverage, through)) return {};
            from = e.t + std::max(e.t * 1e-6f, 1e-6f);
        }
        return through;
    }
    traverse(top, origin, dir, 0.0f, tMax, [&](uint32_t first, uint32_t count) {
        for (uint32_t k = first; k < first + count && !blocked; ++k) {
            const uint32_t item = top.items[k];
            if (item >= placed.size()) {
                float t = 0.0f;
                Vec3 n;
                if (meetSolid(item - placed.size(), origin, dir, time, t, n) && t > 0.0f && t < tMax) blocked = true;
                continue;
            }
            const Placed p = placed[item].movedBy(time);
            const Mesh& m = *meshes[p.mesh];
            if (!m.shadows) continue;
            const Vec3 o = p.toLocal(origin), d = p.dirToLocal(dir);
            traverse(m.bvh, o, d, 0.0f, tMax, [&](uint32_t f, uint32_t c) {
                for (uint32_t t = f; t < f + c; ++t) {
                    const Corners k = cornersAt(m, t, time);
                    const Vec3 pv = cross(d, k.e2);
                    const float det = dot(k.e1, pv);
                    if (det == 0.0f) continue;
                    const float inv = 1.0f / det;
                    const Vec3 s = o - k.v0;
                    const float u = dot(s, pv) * inv;
                    if (u < 0.0f || u > 1.0f) continue;
                    const Vec3 q = cross(s, k.e1);
                    const float v = dot(d, q) * inv;
                    if (v < 0.0f || u + v > 1.0f) continue;
                    const float h = dot(k.e2, q) * inv;
                    if (h <= 0.0f || h >= tMax) continue;
                    if (!passThrough(m, t, u, v, p, coverage, through)) {
                        blocked = true;
                        return false;
                    }
                }
                return true;
            });
        }
        return !blocked;
    });
    return blocked ? Vec3() : through;
}

Vec3 Scene::sky(const Vec3& dir) const {
    if (!look.skyBehind) return skyLight;
    const float up = std::max(dir.y, 0.0f);
    const float toSun = std::max(dot(dir, sunDirection), 0.0f);
    return skyLight * (2.2f - 1.2f * std::sqrt(up)) + sunLight * 0.06f +
           sunLight * (0.5f * std::pow(toSun, 48.0f) + 0.12f * std::pow(toSun, 6.0f));
}

Vec3 Scene::background(const Vec3& dir, float up) const {
    if (look.skyBehind) return sky(dir);
    // The studio's backdrop, as the viewport draws it: not exposed.
    const Vec3 top(0.075f, 0.082f, 0.095f), bottom(0.022f, 0.023f, 0.027f);
    const float u = std::clamp(up, 0.0f, 1.0f);
    return (bottom + (top - bottom) * u) * (1.0f / std::max(look.exposure, 1e-6f));
}

float Scene::wetAt(const Vec3& p, const Vec3& n) const {
    if (wetness <= 0.0f) return 0.0f;
    const float dx = std::max({wetLo.x - p.x, p.x - wetHi.x, 0.0f});
    const float dz = std::max({wetLo.y - p.z, p.z - wetHi.y, 0.0f});
    return wetness * (1.0f - smoothstep(0.0f, 0.35f, std::sqrt(dx * dx + dz * dz))) * smoothstep(0.1f, 0.7f, n.y);
}

Vec3 Scene::sunRadiance() const {
    // The viewport's sun lights a surface facing it as albedo times the
    // sun's light: an irradiance of pi times it, spread over the disc.
    const float solidAngle = 2.0f * kPi * std::max(1.0f - sunCosine, 1e-9f);
    return sunLight * (kPi / solidAngle);
}

SceneBuilder::SceneBuilder(RayEngine engine)
    : engine_(engine == RayEngine::Embree && !embreeAvailable() ? RayEngine::Own : engine) {}

std::shared_ptr<const Mesh> SceneBuilder::prototype(const GeometryPtr& geo) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = kept_.begin(); it != kept_.end();) {
        if (it->second.geometry.expired()) it = kept_.erase(it);
        else ++it;
    }
    const auto it = kept_.find(geo.get());
    if (it != kept_.end() && it->second.geometry.lock() == geo) return it->second.mesh;
    // A prototype's own instances made copies of: a mesh of it all.
    std::shared_ptr<const Mesh> mesh =
        geo->prototypeCount() > 0 ? meshOf(*unpackInstances(*geo), false, engine_) : meshOf(*geo, false, engine_);
    kept_[geo.get()] = {geo, mesh};
    return mesh;
}

std::shared_ptr<const Scene> SceneBuilder::build(const SceneInput& in) {
    auto scene = std::make_shared<Scene>();
    Scene& s = *scene;
    s.engine = engine_;
    s.look = in.look;
    s.camera = in.camera;
    s.cameraMoves = in.cameraMotion;
    s.cameraBefore = in.cameraMotion ? in.cameraBefore : in.camera;
    s.cameraAfter = in.cameraMotion ? in.cameraAfter : in.camera;
    s.frameTime = in.frameTime;
    s.time = in.time;
    // What moves is met where it is up to half a frame either way: the
    // longest a shutter is open.
    s.sweep = 0.5f * std::max(in.frameTime, 0.0f);
    s.plate = in.plate;
    s.sunDirection = normalize(in.look.lightDirection());
    s.sunLight = in.look.lightColor * in.look.lightIntensity;
    s.skyLight = in.look.skyColor * in.look.skyIntensity;
    s.sunCosine = std::cos(std::clamp(in.sunAngle, 0.01f, 30.0f) * 0.5f * kPi / 180.0f);

    auto add = [&](std::shared_ptr<const Mesh> mesh) {
        if (!mesh || mesh->count() == 0) return;
        Placed p;
        p.mesh = static_cast<uint32_t>(s.meshes.size());
        s.meshes.push_back(std::move(mesh));
        s.placed.push_back(p);
    };
    if (in.geometry) {
        if (in.geometry->prototypeCount() > 0) {
            add(meshOf(*withoutInstances(*in.geometry), false, engine_));
            // What stands on the points: each prototype once, placed on its points.
            const auto byPrototype = instancesByPrototype(*in.geometry);
            const std::vector<Placement> places = placementsOf(*in.geometry);
            const AttributeArray* tint = in.geometry->points().find("tint");
            if (tint && tint->type() != AttrType::Vec3) tint = nullptr;
            const AttributeArray* velocity = in.geometry->points().find("v");
            if (velocity && velocity->type() != AttrType::Vec3) velocity = nullptr;
            for (size_t k = 0; k < byPrototype.size(); ++k) {
                const GeometryPtr& proto = in.geometry->prototypes()[k];
                if (byPrototype[k].empty() || !proto) continue;
                std::shared_ptr<const Mesh> mesh = prototype(proto);
                if (!mesh || mesh->count() == 0) continue;
                const uint32_t index = static_cast<uint32_t>(s.meshes.size());
                s.meshes.push_back(mesh);
                for (const uint32_t pt : byPrototype[k]) {
                    const Placement& pl = places[pt];
                    if (!(pl.scale > 0.0f)) continue;
                    Placed p;
                    p.mesh = index;
                    p.at = pl.at;
                    axesOf(pl.orient, p);
                    p.scale = pl.scale;
                    if (tint) p.tint = tint->read<Vec3>()[pt];
                    if (velocity) p.velocity = velocity->read<Vec3>()[pt];
                    s.placed.push_back(p);
                }
            }
        } else {
            add(meshOf(*in.geometry, false, engine_, s.sweep));
        }
    }
    if (in.bodies) {
        add(meshOf(*in.bodies, false, engine_, s.sweep));
        // Their grit: a chip on each of its points.
        placeChips(*in.bodies, s);
    }
    if (in.water) add(meshOf(*in.water, true, engine_, s.sweep));
    // The rain: each drop the streak it falls in a share of a frame; wet
    // where it falls, as far as the drops reach in x and z.
    if (in.frame && in.frame->rain.dropCount() + in.frame->rain.dropletCount() > 0) {
        const sim::RainFrame& rain = in.frame->rain;
        add(rainMesh(rain, in.look, engine_, &in.camera.position));
        // Wet as far as the drops reach -- the splashes' droplets aside.
        Vec2 lo(1e30f), hi(-1e30f);
        for (size_t i = 0; i + 5 < rain.drops.size(); i += 6) {
            lo = glm::min(lo, Vec2(rain.drops[i], rain.drops[i + 2]));
            hi = glm::max(hi, Vec2(rain.drops[i], rain.drops[i + 2]));
        }
        if (lo.x <= hi.x) {
            s.wetness = std::clamp(in.look.wetness, 0.0f, 1.0f);
            s.wetLo = lo;
            s.wetHi = hi;
        }
    }
    s.solids = in.solids;
    s.shapes.reserve(s.solids.size());
    for (const sim::Solid& solid : s.solids) s.shapes.push_back(solid.body.instance());
    // The smoke and the fire: made once a frame, however often it is rendered.
    s.gasLook = GasLook::of(in.look);
    if (in.frame) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (gasFrame_.lock() != in.frame || gasFrameTime_ != in.frameTime) {
            gas_ = Gas::build(*in.frame, in.frameTime);
            gasFrame_ = in.frame;
            gasFrameTime_ = in.frameTime;
        }
        s.gas = gas_;
    }

    // The hierarchy over them all: ours; or Embree's over the placed meshes
    // and ours over the solids.
    std::vector<Box> boxes(s.placed.size() + s.shapes.size());
    parallelFor(s.placed.size(), 4096, [&](size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i) boxes[i] = placedBox(s.placed[i], *s.meshes[s.placed[i].mesh], s.sweep);
    });
    for (size_t i = 0; i < s.shapes.size(); ++i) boxes[s.placed.size() + i] = solidBox(s.solids[i], s.shapes[i], s.sweep);
    for (const Box& b : boxes) s.bounds.grow(b);
    if (s.gas) s.bounds.grow(s.gas->bounds());
    // Whether anything moves while a shutter is open.
    s.moving = s.cameraMoves || (s.gas && s.gas->moves()) ||
               std::any_of(s.meshes.begin(), s.meshes.end(), [](const auto& m) { return m && m->sweep > 0.0f; }) ||
               std::any_of(s.placed.begin(), s.placed.end(), [](const Placed& p) { return p.velocity != Vec3(0.0f); }) ||
               std::any_of(s.solids.begin(), s.solids.end(), [](const sim::Solid& o) { return o.body.moves(); });
    if (engine_ == RayEngine::Embree) {
        s.embree = EmbreeScene::build(s.meshes, s.placed, s.sweep);
        s.top = buildBvh(std::span<const Box>(boxes).subspan(s.placed.size()), 2);
    } else {
        s.top = buildBvh(boxes, 2);
    }

    // How far the floor goes, as the viewport has it: past the simulations,
    // past what is drawn, as far as the eye is off.
    float reach = 4.0f;
    if (!in.domain.empty()) reach = std::max(reach, 2.5f * std::max(in.domain.hi.x - in.domain.lo.x, in.domain.hi.z - in.domain.lo.z));
    if (!s.bounds.empty()) reach = std::max(reach, 2.5f * std::max(s.bounds.hi.x - s.bounds.lo.x, s.bounds.hi.z - s.bounds.lo.z));
    reach = std::max(reach, 3.0f * length(in.camera.position));
    s.floorRadius = reach;
    return scene;
}

}  // namespace pg::render
