#include "pg/render/Embree.h"

#include "pg/core/Parallel.h"
#include "pg/render/Scene.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string_view>

#ifdef PG_HAVE_EMBREE
#include <embree4/rtcore.h>
#endif

namespace pg::render {

#ifdef PG_HAVE_EMBREE
namespace {

/// The one device, made at first need and kept while the program runs: the
/// meshes kept in the scene builders' caches may outlive whatever would
/// release it.
RTCDevice device() {
    static const RTCDevice made = [] {
        RTCDevice d = rtcNewDevice(nullptr);
        if (!d) {
            std::fprintf(stderr, "Embree: no device (error %d): the path tracer uses its own BVH\n",
                         static_cast<int>(rtcGetDeviceError(nullptr)));
            return d;
        }
        rtcSetDeviceErrorFunction(
            d,
            [](void*, RTCError code, const char* what) {
                std::fprintf(stderr, "Embree: error %d: %s\n", static_cast<int>(code), what ? what : "");
            },
            nullptr);
        return d;
    }();
    return made;
}

RTCScene sceneOf(void* scene) { return static_cast<RTCScene>(scene); }

/// A scene built as all of ours are: by the surface area heuristic without
/// spatial splits, the same whatever the threads; robust at shared edges.
RTCScene newScene() {
    RTCScene s = rtcNewScene(device());
    rtcSetSceneBuildQuality(s, RTC_BUILD_QUALITY_MEDIUM);
    rtcSetSceneFlags(s, RTC_SCENE_FLAG_ROBUST);
    return s;
}

bool identity(const Placed& p) {
    return p.at == Vec3(0.0f, 0.0f, 0.0f) && p.scale == 1.0f && p.axes == Mat3(1.0f) && p.velocity == Vec3(0.0f);
}

/// A placement's transform: the columns, the mesh's axes as placed times
/// its size; then where it stands.
void placementOf(const Placed& p, float m[12]) {
    for (int c = 0; c < 3; ++c) {
        const Vec3 axis = p.axes[c] * p.scale;
        m[3 * c] = axis.x;
        m[3 * c + 1] = axis.y;
        m[3 * c + 2] = axis.z;
    }
    m[9] = p.at.x;
    m[10] = p.at.y;
    m[11] = p.at.z;
}

/// The placements `which` -- numbers among `placed`, each with an Embree
/// mesh -- in one scene: each an instance of its mesh's scene, turned,
/// sized and moved; a mesh placed as it is, its triangles themselves.
EmbreeScene::Top topOf(const std::vector<std::shared_ptr<const Mesh>>& meshes, const std::vector<Placed>& placed,
                       const std::vector<uint32_t>& which, float sweep) {
    EmbreeScene::Top top;
    RTCScene scene = newScene();
    top.scene = scene;
    top.placed = which;
    // Made side by side, put in the scene in their order: the same scene whatever the threads.
    std::vector<RTCGeometry> geometries(which.size());
    std::vector<uint8_t> direct(meshes.size(), 0);
    for (size_t k = 0; k < which.size(); ++k) {
        const Placed& p = placed[which[k]];
        if (identity(p) && !direct[p.mesh]) {
            direct[p.mesh] = 1;
            geometries[k] = static_cast<RTCGeometry>(meshes[p.mesh]->embree->geometry());
            rtcRetainGeometry(geometries[k]);
        }
    }
    parallelFor(which.size(), 2048, [&](size_t begin, size_t end) {
        for (size_t k = begin; k < end; ++k) {
            if (geometries[k]) continue;
            const Placed& p = placed[which[k]];
            RTCGeometry g = rtcNewGeometry(device(), RTC_GEOMETRY_TYPE_INSTANCE);
            rtcSetGeometryInstancedScene(g, sceneOf(meshes[p.mesh]->embree->scene()));
            float m[12];
            if (sweep > 0.0f && p.velocity != Vec3(0.0f)) {
                // Flying: where it is `sweep` before now and after, Embree's
                // time 0 and 1, between them along a line.
                rtcSetGeometryTimeStepCount(g, 2);
                placementOf(p.movedBy(-sweep), m);
                rtcSetGeometryTransform(g, 0, RTC_FORMAT_FLOAT3X4_COLUMN_MAJOR, m);
                placementOf(p.movedBy(sweep), m);
                rtcSetGeometryTransform(g, 1, RTC_FORMAT_FLOAT3X4_COLUMN_MAJOR, m);
            } else {
                placementOf(p, m);
                rtcSetGeometryTransform(g, 0, RTC_FORMAT_FLOAT3X4_COLUMN_MAJOR, m);
            }
            rtcCommitGeometry(g);
            geometries[k] = g;
        }
    });
    for (size_t k = 0; k < which.size(); ++k) {
        rtcAttachGeometryByID(scene, geometries[k], static_cast<unsigned>(k));
        rtcReleaseGeometry(geometries[k]);
    }
    rtcCommitScene(scene);
    return top;
}

/// `time` the share of the way from Embree's time 0 to 1: a half, now.
void setRay(RTCRay& r, const Vec3& origin, const Vec3& dir, float tMin, float tMax, float time) {
    r.org_x = origin.x;
    r.org_y = origin.y;
    r.org_z = origin.z;
    r.tnear = tMin;
    r.dir_x = dir.x;
    r.dir_y = dir.y;
    r.dir_z = dir.z;
    r.time = time;
    r.tfar = tMax;
    r.mask = 0xFFFFFFFFu;
    r.id = 0;
    r.flags = 0;
}

/// The nearest triangle of `top` a ray meets.
bool nearestIn(const EmbreeScene::Top& top, const Vec3& origin, const Vec3& dir, float tMin, float tMax,
               EmbreeHit& hit, float time) {
    if (!top.scene) return false;
    RTCRayHit rh;
    setRay(rh.ray, origin, dir, tMin, tMax, time);
    rh.hit.geomID = RTC_INVALID_GEOMETRY_ID;
    rh.hit.instID[0] = RTC_INVALID_GEOMETRY_ID;
    rtcIntersect1(sceneOf(top.scene), &rh);
    if (rh.hit.geomID == RTC_INVALID_GEOMETRY_ID) return false;
    // In an instance: its number in the scene; else the mesh's own.
    const unsigned id = rh.hit.instID[0] != RTC_INVALID_GEOMETRY_ID ? rh.hit.instID[0] : rh.hit.geomID;
    if (id >= top.placed.size()) return false;
    hit.placed = top.placed[id];
    hit.triangle = rh.hit.primID;
    hit.t = rh.ray.tfar;
    hit.u = rh.hit.u;
    hit.v = rh.hit.v;
    return true;
}

}  // namespace
#endif

bool embreeAvailable() {
#ifdef PG_HAVE_EMBREE
    return device() != nullptr;
#else
    return false;
#endif
}

std::string embreeVersion() {
#ifdef PG_HAVE_EMBREE
    if (!device()) return {};
    char text[64];
    std::snprintf(text, sizeof text, "Embree %d.%d.%d",
                  static_cast<int>(rtcGetDeviceProperty(device(), RTC_DEVICE_PROPERTY_VERSION_MAJOR)),
                  static_cast<int>(rtcGetDeviceProperty(device(), RTC_DEVICE_PROPERTY_VERSION_MINOR)),
                  static_cast<int>(rtcGetDeviceProperty(device(), RTC_DEVICE_PROPERTY_VERSION_PATCH)));
    return text;
#else
    return {};
#endif
}

RayEngine defaultRayEngine() {
    static const RayEngine engine = [] {
        const char* asked = std::getenv("PG_RAYS");
        if (asked && std::string_view(asked) == "own") return RayEngine::Own;
        return embreeAvailable() ? RayEngine::Embree : RayEngine::Own;
    }();
    return engine;
}

std::string rayEngineName(RayEngine engine) {
    return engine == RayEngine::Embree && embreeAvailable() ? embreeVersion() : std::string("own BVH");
}

// --- a mesh -------------------------------------------------------------------------

std::shared_ptr<const EmbreeMesh> EmbreeMesh::build(std::span<const Vec3> corners, std::span<const Vec3> velocities,
                                                    float sweep) {
#ifdef PG_HAVE_EMBREE
    const size_t n = corners.size() / 3;
    if (!device() || n == 0) return nullptr;
    RTCScene scene = newScene();
    RTCGeometry g = rtcNewGeometry(device(), RTC_GEOMETRY_TYPE_TRIANGLE);
    // Moving: the corners `sweep` before now and after, Embree's time 0
    // and 1, between them along a line.
    const bool moving = sweep > 0.0f && velocities.size() == corners.size();
    if (moving) rtcSetGeometryTimeStepCount(g, 2);
    const float steps[2] = {moving ? -sweep : 0.0f, sweep};
    for (unsigned k = 0; k < (moving ? 2u : 1u); ++k) {
        auto* vertices = static_cast<float*>(
            rtcSetNewGeometryBuffer(g, RTC_BUFFER_TYPE_VERTEX, k, RTC_FORMAT_FLOAT3, 3 * sizeof(float), 3 * n));
        const float t = steps[k];
        parallelFor(3 * n, 65536, [&](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i) {
                const Vec3 c = moving ? corners[i] + velocities[i] * t : corners[i];
                vertices[3 * i] = c.x;
                vertices[3 * i + 1] = c.y;
                vertices[3 * i + 2] = c.z;
            }
        });
    }
    auto* triangles = static_cast<uint32_t*>(
        rtcSetNewGeometryBuffer(g, RTC_BUFFER_TYPE_INDEX, 0, RTC_FORMAT_UINT3, 3 * sizeof(uint32_t), n));
    for (size_t i = 0; i < 3 * n; ++i) triangles[i] = static_cast<uint32_t>(i);
    rtcCommitGeometry(g);
    rtcAttachGeometry(scene, g);
    rtcCommitScene(scene);
    std::shared_ptr<EmbreeMesh> mesh(new EmbreeMesh());
    mesh->scene_ = scene;
    mesh->geometry_ = g;  // kept: to be in other scenes as it is
    return mesh;
#else
    (void)corners;
    (void)velocities;
    (void)sweep;
    return nullptr;
#endif
}

EmbreeMesh::~EmbreeMesh() {
#ifdef PG_HAVE_EMBREE
    if (scene_) rtcReleaseScene(sceneOf(scene_));
    if (geometry_) rtcReleaseGeometry(static_cast<RTCGeometry>(geometry_));
#endif
}

// --- the placed -----------------------------------------------------------------------

std::shared_ptr<const EmbreeScene> EmbreeScene::build(const std::vector<std::shared_ptr<const Mesh>>& meshes,
                                                      const std::vector<Placed>& placed, float sweep) {
#ifdef PG_HAVE_EMBREE
    if (!device()) return nullptr;
    std::vector<uint32_t> all, opaque, clear;
    for (uint32_t i = 0; i < placed.size(); ++i) {
        const uint32_t m = placed[i].mesh;
        if (m >= meshes.size() || !meshes[m] || !meshes[m]->embree) continue;  // nothing there to meet
        all.push_back(i);
        if (!meshes[m]->shadows) continue;  // rain: seen, casting no shadow
        (meshes[m]->clear ? clear : opaque).push_back(i);
    }
    std::shared_ptr<EmbreeScene> s(new EmbreeScene());
    s->sweep_ = sweep;
    s->all_ = topOf(meshes, placed, all, sweep);
    if (opaque.size() == all.size()) {
        // Nothing clear, nothing without a shadow: the shadows see what the eye sees.
        rtcRetainScene(sceneOf(s->all_.scene));
        s->opaque_ = s->all_;
    } else {
        s->opaque_ = topOf(meshes, placed, opaque, sweep);
    }
    if (!clear.empty()) s->clear_ = topOf(meshes, placed, clear, sweep);
    return s;
#else
    (void)meshes;
    (void)placed;
    (void)sweep;
    return nullptr;
#endif
}

float EmbreeScene::timeOf(float time) const {
    return sweep_ > 0.0f ? std::clamp(0.5f + 0.5f * time / sweep_, 0.0f, 1.0f) : 0.5f;
}

EmbreeScene::~EmbreeScene() {
#ifdef PG_HAVE_EMBREE
    for (const Top* top : {&all_, &opaque_, &clear_}) {
        if (top->scene) rtcReleaseScene(sceneOf(top->scene));
    }
#endif
}

bool EmbreeScene::nearest(const Vec3& origin, const Vec3& dir, float tMin, float tMax, EmbreeHit& hit,
                          float time) const {
#ifdef PG_HAVE_EMBREE
    return nearestIn(all_, origin, dir, tMin, tMax, hit, timeOf(time));
#else
    (void)origin, (void)dir, (void)tMin, (void)tMax, (void)hit, (void)time;
    return false;
#endif
}

bool EmbreeScene::blocked(const Vec3& origin, const Vec3& dir, float tMax, float time) const {
#ifdef PG_HAVE_EMBREE
    if (!opaque_.scene) return false;
    RTCRay r;
    setRay(r, origin, dir, 0.0f, tMax, timeOf(time));
    rtcOccluded1(sceneOf(opaque_.scene), &r);
    return r.tfar < 0.0f;  // -infinity: something is in the way
#else
    (void)origin, (void)dir, (void)tMax, (void)time;
    return false;
#endif
}

bool EmbreeScene::nearestClear(const Vec3& origin, const Vec3& dir, float tMin, float tMax, EmbreeHit& hit,
                               float time) const {
#ifdef PG_HAVE_EMBREE
    return nearestIn(clear_, origin, dir, tMin, tMax, hit, timeOf(time));
#else
    (void)origin, (void)dir, (void)tMin, (void)tMax, (void)hit, (void)time;
    return false;
#endif
}

}  // namespace pg::render
