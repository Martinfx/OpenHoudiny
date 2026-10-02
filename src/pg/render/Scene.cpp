#include "pg/render/Scene.h"

#include "pg/core/Instances.h"
#include "pg/core/Parallel.h"
#include "pg/render/Particles.h"
#include "pg/render/Textures.h"
#include "pg/sim/Display.h"
#include "pg/sim/Frame.h"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <tuple>

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

float smoothstep(float a, float b, float x) {
    const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

/// What a ray met of a mesh: which triangle, and where on it.
struct TriangleHit {
    uint32_t triangle = 0;
    float u = 0.0f, v = 0.0f;
};

/// The nearest triangle of `m` a ray (in the mesh's space) meets before `best`; `best` shortened to it.
bool nearestTriangle(const Mesh& m, const Vec3& origin, const Vec3& dir, float& best, TriangleHit& out) {
    bool found = false;
    traverse(m.bvh, origin, dir, 0.0f, best, [&](uint32_t first, uint32_t count) {
        for (uint32_t t = first; t < first + count; ++t) {
            const Vec3 p = cross(dir, m.e2[t]);
            const float det = dot(m.e1[t], p);
            if (det == 0.0f) continue;
            const float inv = 1.0f / det;
            const Vec3 s = origin - m.v0[t];
            const float u = dot(s, p) * inv;
            if (u < 0.0f || u > 1.0f) continue;
            const Vec3 q = cross(s, m.e1[t]);
            const float v = dot(dir, q) * inv;
            if (v < 0.0f || u + v > 1.0f) continue;
            const float d = dot(m.e2[t], q) * inv;
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
Box placedBox(const Placed& p, const Mesh& m) {
    Box b;
    if (m.box.empty()) return b;
    for (int c = 0; c < 8; ++c) {
        b.grow(p.toWorld(Vec3(c & 1 ? m.box.hi.x : m.box.lo.x, c & 2 ? m.box.hi.y : m.box.lo.y,
                              c & 4 ? m.box.hi.z : m.box.lo.z)));
    }
    return b;
}

}  // namespace

PresetSurface presetSurface(MaterialPreset preset) {
    switch (preset) {
        case MaterialPreset::None: return {0.5f, 0.0f, Vec3(0.72f, 0.72f, 0.74f)};
        case MaterialPreset::Concrete: return {0.85f, 0.0f, Vec3(0.31f, 0.3f, 0.28f)};
        case MaterialPreset::BrokenConcrete: return {0.95f, 0.0f, Vec3(0.36f, 0.34f, 0.31f)};
        case MaterialPreset::Brick: return {0.85f, 0.0f, Vec3(0.3f, 0.1f, 0.06f)};
        case MaterialPreset::BrickWall: return {0.85f, 0.0f, Vec3(0.25f, 0.08f, 0.05f)};
        case MaterialPreset::Mortar: return {0.95f, 0.0f, Vec3(0.42f, 0.4f, 0.36f)};
        case MaterialPreset::Plaster: return {0.8f, 0.0f, Vec3(0.6f, 0.58f, 0.54f)};
        case MaterialPreset::Window: return {0.04f, 0.0f, Vec3(0.04f, 0.05f, 0.06f)};
        case MaterialPreset::Glass: return {0.0f, 0.0f, Vec3(0.9f, 0.95f, 0.95f)};
        case MaterialPreset::Steel: return {0.45f, 0.8f, Vec3(0.4f, 0.4f, 0.42f)};
        case MaterialPreset::Metal: return {0.3f, 1.0f, Vec3(0.55f, 0.56f, 0.57f)};
        case MaterialPreset::Asphalt: return {0.9f, 0.0f, Vec3(0.06f, 0.06f, 0.065f)};
        case MaterialPreset::Wood: return {0.65f, 0.0f, Vec3(0.23f, 0.14f, 0.08f)};
        case MaterialPreset::Stone: return {0.75f, 0.0f, Vec3(0.3f, 0.29f, 0.27f)};
        case MaterialPreset::Roof: return {0.8f, 0.0f, Vec3(0.2f, 0.19f, 0.18f)};
        case MaterialPreset::Bark: return {0.9f, 0.0f, Vec3(0.15f, 0.1f, 0.06f)};
        case MaterialPreset::Leaf: return {0.5f, 0.0f, Vec3(0.08f, 0.17f, 0.03f)};
        case MaterialPreset::Grass: return {0.6f, 0.0f, Vec3(0.1f, 0.22f, 0.04f)};
        case MaterialPreset::Soil: return {0.95f, 0.0f, Vec3(0.09f, 0.065f, 0.045f)};
        case MaterialPreset::Paving: return {0.8f, 0.0f, Vec3(0.16f, 0.15f, 0.14f)};
        case MaterialPreset::RoofTiles: return {0.7f, 0.0f, Vec3(0.06f, 0.065f, 0.07f)};
        case MaterialPreset::Lawn: return {0.65f, 0.0f, Vec3(0.14f, 0.24f, 0.05f)};
        case MaterialPreset::Sand: return {0.95f, 0.0f, Vec3(0.45f, 0.36f, 0.22f)};
    }
    return {};
}

std::shared_ptr<const Mesh> meshOf(const Geometry& geo, bool water, RayEngine engine) {
    auto mesh = std::make_shared<Mesh>();
    const sim::ShadedTriangles tris = sim::shadedTriangles(geo);
    const size_t n = tris.count();
    if (n == 0) return mesh;

    // What each triangle is made of: materials told apart by their numbers,
    // in steps of a 256th.
    const MaterialNumber roughness(geo, "roughness", 0.5f), metallic(geo, "metallic", 0.0f),
        translucency(geo, "translucency", 0.0f);
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
    auto textureOfPrim = [&](uint32_t prim) -> const std::string& {
        static const std::string none;
        if (!textures || prim >= textures->size()) return none;
        return textures->stringValue(textures->read<int32_t>()[prim]);
    };
    // Geometry with no colour of its own: each surface its material's.
    const bool colored = geo.vertices().find("Cd") || geo.points().find("Cd") || geo.primitives().find("Cd") ||
                         geo.detail().find("Cd");
    std::map<std::tuple<std::array<int, 5>, std::string, int>, uint16_t> known;
    std::vector<uint16_t> which(n);
    auto quantize = [](float x) { return static_cast<int>(std::lround(std::clamp(x, 0.0f, 1.0f) * 255.0f)); };
    for (size_t t = 0; t < n; ++t) {
        Material m;
        const uint32_t prim = tris.prims[t];
        const MaterialPreset preset = presetOf(prim);
        const std::string& texture = textureOfPrim(prim);
        if (water) {
            m.kind = Material::Kind::Water;
            m.roughness = 0.0f;
            m.ior = 1.33f;
        } else if (tris.glass[t] == 1 || (tris.glass[t] == 0 && preset == MaterialPreset::Glass)) {
            m.kind = Material::Kind::Glass;
            m.roughness = 0.0f;
            m.ior = 1.5f;
        } else {
            const PresetSurface base = presetSurface(preset);
            m.preset = preset;
            m.roughness = static_cast<float>(quantize(roughness.given ? roughness.at(geo, prim) : base.roughness)) / 255.0f;
            m.metallic = static_cast<float>(quantize(metallic.given ? metallic.at(geo, prim) : base.metallic)) / 255.0f;
            m.translucency = static_cast<float>(quantize(translucency.at(geo, prim))) / 255.0f;
            if (tris.glass[t] == 2) m.roughness = 0.35f;  // a crack: a rough, white break in the glass
            if (!texture.empty()) {
                m.texture = texture;
                m.textureSize = std::max(textureSize.at(geo, prim), 0.0f);
                m.textureTint = static_cast<int8_t>(tints && prim < tints->size() && tints->read<int32_t>()[prim] != 0);
            }
        }
        const auto key = std::make_tuple(std::array<int, 5>{static_cast<int>(m.kind), quantize(m.roughness), quantize(m.metallic),
                                                            quantize(m.translucency), static_cast<int>(m.preset)},
                                         m.texture, static_cast<int>(std::lround(m.textureSize * 1000.0f)) * 3 + m.textureTint + 1);
        auto it = known.find(key);
        if (it == known.end()) {
            if (mesh->materials.size() >= 65535) {
                which[t] = 0;
                continue;
            }
            it = known.emplace(key, static_cast<uint16_t>(mesh->materials.size())).first;
            mesh->materials.push_back(m);
        }
        which[t] = it->second;
    }
    for (const Material& m : mesh->materials) mesh->clear = mesh->clear || m.kind != Material::Kind::Surface;
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
            if (!embree) boxes[t].grow(tris.positions[3 * t + c]);
            mesh->box.grow(tris.positions[3 * t + c]);
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
        mesh->embree = EmbreeMesh::build(tris.positions);
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

bool Scene::intersect(const Vec3& origin, const Vec3& dir, float tMax, float fade, Hit& hit) const {
    float best = tMax;
    int placedHit = -1, solidHit = -1;
    TriangleHit triangle;
    Vec3 solidNormal;
    auto solid = [&](size_t s) {
        float t = 0.0f;
        Vec3 n;
        if (shapes[s].intersect(origin, dir, 0.0f, t, n) && t > 0.0f && t < best) {
            best = t;
            solidHit = static_cast<int>(s);
            placedHit = -1;
            solidNormal = n;
        }
    };
    if (embree) {
        // The meshes through Embree, the solids nearer than what it met through ours.
        EmbreeHit e;
        if (embree->nearest(origin, dir, 0.0f, best, e)) {
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
                    const Placed& p = placed[item];
                    TriangleHit th;
                    if (nearestTriangle(*meshes[p.mesh], p.toLocal(origin), p.dirToLocal(dir), best, th)) {
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
        hit.face = normalize(p.turn(cross(m.e1[t], m.e2[t])));
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
        hit.material = &m.materials[m.material[t]];
        hit.floor = false;
        return true;
    }
    if (solidHit >= 0) {
        hit.t = best;
        hit.position = origin + dir * best;
        hit.normal = hit.face = normalize(solidNormal);
        hit.color = solids[static_cast<size_t>(solidHit)].color;
        hit.material = &solidMaterial;
        hit.floor = false;
        return true;
    }
    return false;
}

namespace {

/// Light through a clear triangle -- glass or water: what it does not
/// reflect, tinted a little at each face; water a little less. False -- no
/// light at all -- for an opaque one.
bool passThrough(const Mesh& m, uint32_t t, const Placed& p, Vec3& through) {
    const Material& mat = m.materials[m.material[t]];
    if (mat.kind == Material::Kind::Surface) return false;
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

Vec3 Scene::transmittance(const Vec3& origin, const Vec3& dir, float tMax) const {
    Vec3 through(1.0f, 1.0f, 1.0f);
    bool blocked = false;
    if (embree) {
        // A solid in the way, then an opaque mesh: no light.
        traverse(top, origin, dir, 0.0f, tMax, [&](uint32_t first, uint32_t count) {
            for (uint32_t k = first; k < first + count && !blocked; ++k) {
                float t = 0.0f;
                Vec3 n;
                if (shapes[top.items[k]].intersect(origin, dir, 0.0f, t, n) && t > 0.0f && t < tMax) blocked = true;
            }
            return !blocked;
        });
        if (blocked || embree->blocked(origin, dir, tMax)) return {};
        // Through the glass and the water, face after face, from the nearest.
        float from = 0.0f;
        EmbreeHit e;
        for (int faces = 0; faces < 256 && embree->nearestClear(origin, dir, from, tMax, e); ++faces) {
            const Placed& p = placed[e.placed];
            if (!passThrough(*meshes[p.mesh], e.triangle, p, through)) return {};
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
                if (shapes[item - placed.size()].intersect(origin, dir, 0.0f, t, n) && t > 0.0f && t < tMax) blocked = true;
                continue;
            }
            const Placed& p = placed[item];
            const Mesh& m = *meshes[p.mesh];
            if (!m.shadows) continue;
            const Vec3 o = p.toLocal(origin), d = p.dirToLocal(dir);
            traverse(m.bvh, o, d, 0.0f, tMax, [&](uint32_t f, uint32_t c) {
                for (uint32_t t = f; t < f + c; ++t) {
                    const Vec3 pv = cross(d, m.e2[t]);
                    const float det = dot(m.e1[t], pv);
                    if (det == 0.0f) continue;
                    const float inv = 1.0f / det;
                    const Vec3 s = o - m.v0[t];
                    const float u = dot(s, pv) * inv;
                    if (u < 0.0f || u > 1.0f) continue;
                    const Vec3 q = cross(s, m.e1[t]);
                    const float v = dot(d, q) * inv;
                    if (v < 0.0f || u + v > 1.0f) continue;
                    const float h = dot(m.e2[t], q) * inv;
                    if (h <= 0.0f || h >= tMax) continue;
                    if (!passThrough(m, t, p, through)) {
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
            add(meshOf(*in.geometry, false, engine_));
        }
    }
    if (in.bodies) {
        add(meshOf(*in.bodies, false, engine_));
        // Their grit: a chip on each of its points.
        placeChips(*in.bodies, s);
    }
    if (in.water) add(meshOf(*in.water, true, engine_));
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
        if (gasFrame_.lock() != in.frame) {
            gas_ = Gas::build(*in.frame);
            gasFrame_ = in.frame;
        }
        s.gas = gas_;
    }

    // The hierarchy over them all: ours; or Embree's over the placed meshes
    // and ours over the solids.
    std::vector<Box> boxes(s.placed.size() + s.shapes.size());
    parallelFor(s.placed.size(), 4096, [&](size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i) boxes[i] = placedBox(s.placed[i], *s.meshes[s.placed[i].mesh]);
    });
    for (size_t i = 0; i < s.shapes.size(); ++i) s.shapes[i].bounds(boxes[s.placed.size() + i].lo, boxes[s.placed.size() + i].hi);
    for (const Box& b : boxes) s.bounds.grow(b);
    if (s.gas) s.bounds.grow(s.gas->bounds());
    if (engine_ == RayEngine::Embree) {
        s.embree = EmbreeScene::build(s.meshes, s.placed);
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
