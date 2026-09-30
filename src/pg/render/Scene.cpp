#include "pg/render/Scene.h"

#include "pg/core/Instances.h"
#include "pg/core/Parallel.h"
#include "pg/sim/Display.h"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <numeric>

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

    MaterialNumber(const Geometry& geo, const char* name, float fallback)
        : prim(numberAttribute(geo.primitives(), name)), point(numberAttribute(geo.points(), name)), detail(fallback) {
        if (const AttributeArray* d = numberAttribute(geo.detail(), name); d && d->size() > 0) detail = d->read<float>()[0];
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

std::shared_ptr<const Mesh> meshOf(const Geometry& geo, bool water) {
    auto mesh = std::make_shared<Mesh>();
    const sim::ShadedTriangles tris = sim::shadedTriangles(geo);
    const size_t n = tris.count();
    if (n == 0) return mesh;

    // What each triangle is made of: materials told apart by their numbers,
    // in steps of a 256th.
    const MaterialNumber roughness(geo, "roughness", 0.5f), metallic(geo, "metallic", 0.0f),
        translucency(geo, "translucency", 0.0f);
    std::map<std::array<int, 4>, uint16_t> known;
    std::vector<uint16_t> which(n);
    auto quantize = [](float x) { return static_cast<int>(std::lround(std::clamp(x, 0.0f, 1.0f) * 255.0f)); };
    for (size_t t = 0; t < n; ++t) {
        Material m;
        const uint32_t prim = tris.prims[t];
        if (water) {
            m.kind = Material::Kind::Water;
            m.roughness = 0.0f;
            m.ior = 1.33f;
        } else if (tris.glass[t] == 1) {
            m.kind = Material::Kind::Glass;
            m.roughness = 0.0f;
            m.ior = 1.5f;
        } else {
            m.roughness = static_cast<float>(quantize(roughness.at(geo, prim))) / 255.0f;
            m.metallic = static_cast<float>(quantize(metallic.at(geo, prim))) / 255.0f;
            m.translucency = static_cast<float>(quantize(translucency.at(geo, prim))) / 255.0f;
            if (tris.glass[t] == 2) m.roughness = 0.35f;  // a crack: a rough, white break in the glass
        }
        const std::array<int, 4> key{static_cast<int>(m.kind), quantize(m.roughness), quantize(m.metallic),
                                     quantize(m.translucency)};
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

    // The hierarchy, then the triangles in the order its leaves take them.
    std::vector<Box> boxes(n);
    for (size_t t = 0; t < n; ++t) {
        for (size_t c = 0; c < 3; ++c) {
            boxes[t].grow(tris.positions[3 * t + c]);
            mesh->box.grow(tris.positions[3 * t + c]);
        }
    }
    mesh->bvh = buildBvh(boxes, 4);
    mesh->v0.resize(n);
    mesh->e1.resize(n);
    mesh->e2.resize(n);
    mesh->normals.resize(3 * n);
    mesh->colors.resize(3 * n);
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
                const Vec3 col = tris.colors[3 * t + c];
                mesh->colors[3 * i + c] = Vec3(std::clamp(col.x, 0.0f, 1.0f), std::clamp(col.y, 0.0f, 1.0f),
                                               std::clamp(col.z, 0.0f, 1.0f));
            }
            mesh->material[i] = which[t];
        }
    });
    std::iota(mesh->bvh.items.begin(), mesh->bvh.items.end(), 0u);
    return mesh;
}

bool Scene::intersect(const Vec3& origin, const Vec3& dir, float tMax, float fade, Hit& hit) const {
    float best = tMax;
    int placedHit = -1, solidHit = -1;
    TriangleHit triangle;
    Vec3 solidNormal;
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
                const size_t s = item - placed.size();
                float t = 0.0f;
                Vec3 n;
                if (shapes[s].intersect(origin, dir, 0.0f, t, n) && t > 0.0f && t < best) {
                    best = t;
                    solidHit = static_cast<int>(s);
                    placedHit = -1;
                    solidNormal = n;
                }
            }
        }
        return true;
    });

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

Vec3 Scene::transmittance(const Vec3& origin, const Vec3& dir, float tMax) const {
    Vec3 through(1.0f, 1.0f, 1.0f);
    bool blocked = false;
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
                    const Material& mat = m.materials[m.material[t]];
                    if (mat.kind == Material::Kind::Surface) {
                        blocked = true;
                        return false;
                    }
                    // Glass lets through what it does not reflect, tinted a
                    // little at each face; water a little less.
                    if (mat.kind == Material::Kind::Glass) {
                        const Vec3 c = (m.colors[3 * t] + m.colors[3 * t + 1] + m.colors[3 * t + 2]) * (1.0f / 3.0f) * p.tint;
                        through = through * (Vec3(1.0f, 1.0f, 1.0f) * 0.65f + c * 0.35f) * 0.92f;
                    } else {
                        through = through * 0.9f;
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

Vec3 Scene::sunRadiance() const {
    // The viewport's sun lights a surface facing it as albedo times the
    // sun's light: an irradiance of pi times it, spread over the disc.
    const float solidAngle = 2.0f * kPi * std::max(1.0f - sunCosine, 1e-9f);
    return sunLight * (kPi / solidAngle);
}

std::shared_ptr<const Mesh> SceneBuilder::prototype(const GeometryPtr& geo) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = kept_.begin(); it != kept_.end();) {
        if (it->second.geometry.expired()) it = kept_.erase(it);
        else ++it;
    }
    const auto it = kept_.find(geo.get());
    if (it != kept_.end() && it->second.geometry.lock() == geo) return it->second.mesh;
    // A prototype's own instances made copies of: a mesh of it all.
    std::shared_ptr<const Mesh> mesh = geo->prototypeCount() > 0 ? meshOf(*unpackInstances(*geo)) : meshOf(*geo);
    kept_[geo.get()] = {geo, mesh};
    return mesh;
}

std::shared_ptr<const Scene> SceneBuilder::build(const SceneInput& in) {
    auto scene = std::make_shared<Scene>();
    Scene& s = *scene;
    s.look = in.look;
    s.camera = in.camera;
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
            add(meshOf(*withoutInstances(*in.geometry)));
            // What stands on the points: each prototype once, placed on its points.
            const auto byPrototype = instancesByPrototype(*in.geometry);
            const std::vector<Placement> places = placementsOf(*in.geometry);
            const AttributeArray* tint = in.geometry->points().find("tint");
            if (tint && tint->type() != AttrType::Vec3) tint = nullptr;
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
                    s.placed.push_back(p);
                }
            }
        } else {
            add(meshOf(*in.geometry));
        }
    }
    if (in.bodies) add(meshOf(*in.bodies));
    if (in.water) add(meshOf(*in.water, true));
    s.solids = in.solids;
    s.shapes.reserve(s.solids.size());
    for (const sim::Solid& solid : s.solids) s.shapes.push_back(solid.body.instance());

    // The hierarchy over them all.
    std::vector<Box> boxes(s.placed.size() + s.shapes.size());
    parallelFor(s.placed.size(), 4096, [&](size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i) boxes[i] = placedBox(s.placed[i], *s.meshes[s.placed[i].mesh]);
    });
    for (size_t i = 0; i < s.shapes.size(); ++i) s.shapes[i].bounds(boxes[s.placed.size() + i].lo, boxes[s.placed.size() + i].hi);
    for (const Box& b : boxes) s.bounds.grow(b);
    s.top = buildBvh(boxes, 2);

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
