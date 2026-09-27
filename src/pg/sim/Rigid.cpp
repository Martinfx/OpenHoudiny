#include "pg/sim/Rigid.h"

#include "pg/core/Spatial.h"
#include "pg/sim/Mesh.h"
#include "pg/sim/Shape.h"
#include "pg/sim/Shared.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <numeric>

#ifdef PG_HAVE_JOLT
#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayerInterfaceTable.h>
#include <Jolt/Physics/Collision/BroadPhase/ObjectVsBroadPhaseLayerFilterTable.h>
#include <Jolt/Physics/Collision/ObjectLayerPairFilterTable.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/CylinderShape.h>
#include <Jolt/Physics/Collision/Shape/ScaledShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>
#endif

namespace pg::sim {

RigidScene RigidScene::sanitized() const {
    RigidScene r = *this;
    RigidSettings& s = r.solver;
    const RigidSettings d;
    auto finite = [](float v, float fallback) { return std::isfinite(v) ? v : fallback; };
    s.density = std::clamp(finite(s.density, d.density), 1.0f, 1e6f);
    s.friction = std::clamp(finite(s.friction, d.friction), 0.0f, 10.0f);
    s.bounce = std::clamp(finite(s.bounce, d.bounce), 0.0f, 1.0f);
    s.glue = std::clamp(finite(s.glue, d.glue), 0.0f, 1e12f);
    for (int a = 0; a < 3; ++a) s.gravity[a] = std::clamp(finite(s.gravity[a], 0.0f), -1000.0f, 1000.0f);
    s.substeps = std::clamp(s.substeps, 1, 16);
    s.dust = std::clamp(finite(s.dust, d.dust), 0.0f, 1000.0f);
    s.dustSize = std::clamp(finite(s.dustSize, d.dustSize), 0.01f, 100.0f);
    s.timeStep = std::clamp(finite(s.timeStep, d.timeStep), 1e-4f, 1.0f);
    detail::sanitize(r.colliders);
    return r;
}

Vec3 RigidPose::apply(const Vec3& p) const {
    // q p q*, for a unit quaternion: p + 2 w (u x p) + 2 u x (u x p).
    const Vec3 u(rotation.x, rotation.y, rotation.z);
    const Vec3 t = cross(u, p) * 2.0f;
    return position + p + t * rotation.w + cross(u, t);
}

std::vector<int32_t> pieceOfPrimitives(const Geometry& pieces, const std::string& attribute, int& count) {
    const size_t nprims = pieces.primitiveCount();
    std::vector<int32_t> out(nprims, 0);
    count = 0;
    // The values of the attribute, numbered in their order.
    auto number = [&](const std::vector<int32_t>& values) {
        std::vector<int32_t> sorted = values;
        std::sort(sorted.begin(), sorted.end());
        sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
        for (size_t p = 0; p < nprims; ++p) {
            out[p] = static_cast<int32_t>(std::lower_bound(sorted.begin(), sorted.end(), values[p]) - sorted.begin());
        }
        count = static_cast<int>(sorted.size());
    };
    if (const AttributeArray* a = pieces.primitives().find(attribute); a && a->type() == AttrType::Int) {
        const auto v = a->read<int32_t>();
        number(std::vector<int32_t>(v.begin(), v.end()));
        return out;
    }
    if (const AttributeArray* a = pieces.points().find(attribute); a && a->type() == AttrType::Int) {
        const auto v = a->read<int32_t>();
        std::vector<int32_t> values(nprims, 0);
        for (size_t p = 0; p < nprims; ++p) {
            const auto c = pieces.primitivePoints(p);
            if (!c.empty()) values[p] = v[c[0]];
        }
        number(values);
        return out;
    }
    // None: what touches what -- primitives that share points are one piece.
    std::vector<uint32_t> parent(pieces.pointCount());
    std::iota(parent.begin(), parent.end(), 0u);
    auto find = [&](uint32_t x) {
        while (parent[x] != x) x = parent[x] = parent[parent[x]];
        return x;
    };
    for (size_t p = 0; p < nprims; ++p) {
        const auto c = pieces.primitivePoints(p);
        for (size_t i = 1; i < c.size(); ++i) {
            const uint32_t a = find(c[0]), b = find(c[i]);
            if (a != b) parent[std::max(a, b)] = std::min(a, b);
        }
    }
    std::vector<int32_t> roots(nprims, 0);
    for (size_t p = 0; p < nprims; ++p) {
        const auto c = pieces.primitivePoints(p);
        roots[p] = c.empty() ? -1 - static_cast<int32_t>(p) : static_cast<int32_t>(find(c[0]));
    }
    number(roots);
    return out;
}

std::shared_ptr<Geometry> posedPieces(const RigidFrame& f) {
    if (!f.pieces) return std::make_shared<Geometry>();
    auto geo = std::make_shared<Geometry>(*f.pieces);
    int count = 0;
    const std::vector<int32_t> piece = pieceOfPrimitives(*geo, f.attribute, count);
    // Each point with the piece of the first primitive it is a corner of.
    std::vector<int32_t> pointPiece(geo->pointCount(), -1);
    for (size_t p = 0; p < geo->primitiveCount(); ++p) {
        for (const uint32_t c : geo->primitivePoints(p)) {
            if (pointPiece[c] < 0) pointPiece[c] = piece[p];
        }
    }
    auto v = geo->points().create("v", AttrType::Vec3).write<Vec3>();
    auto P = geo->positionsForWrite();
    AttributeArray* nAttr = geo->points().find("N");
    std::span<Vec3> N;
    if (nAttr && nAttr->type() == AttrType::Vec3) N = nAttr->write<Vec3>();
    for (size_t i = 0; i < P.size(); ++i) {
        const int32_t k = pointPiece[i];
        if (k < 0 || static_cast<size_t>(k) >= f.poses.size()) {
            v[i] = Vec3();
            continue;
        }
        const RigidPose& pose = f.poses[static_cast<size_t>(k)];
        v[i] = pose.velocityAt(P[i]);
        P[i] = pose.apply(P[i]);
        if (!N.empty()) N[i] = pose.apply(N[i]) - pose.position;
    }
    return geo;
}

namespace {

/// Element i of a colour attribute as a colour: a grey for a float.
Vec3 colorAt(const AttributeArray& a, size_t i) {
    switch (a.type()) {
        case AttrType::Vec3: return a.read<Vec3>()[i];
        case AttrType::Vec4: {
            const Vec4 c = a.read<Vec4>()[i];
            return Vec3(c.x, c.y, c.z);
        }
        case AttrType::Float: return Vec3(a.read<float>()[i]);
        default: return Vec3(1.0f);
    }
}

bool isColor(const AttributeArray* a) {
    return a && (a->type() == AttrType::Vec3 || a->type() == AttrType::Vec4 || a->type() == AttrType::Float);
}

}  // namespace

std::shared_ptr<Geometry> drawnPieces(const RigidFrame& f, const Vec3& color, const Vec3& inside,
                                      const std::string& insideGroup) {
    std::shared_ptr<Geometry> geo = posedPieces(f);
    const size_t nprims = geo->primitiveCount();
    const Group* cut = insideGroup.empty() ? nullptr : geo->findGroup(insideGroup);
    if (cut && cut->classOf() != AttrClass::Primitive) cut = nullptr;
    auto isCut = [&](size_t prim) { return cut && cut->contains(prim); };
    // The colour where it is -- on the corners, the points, the faces, the
    // whole -- becomes one the cut faces can differ in: the corners' (a
    // point's colour is shared by the faces round it), else the faces'.
    const AttributeArray* vertexCd = geo->vertices().find("Cd");
    const AttributeArray* pointCd = geo->points().find("Cd");
    if (isColor(vertexCd) || isColor(pointCd)) {
        std::vector<Vec3> corners(geo->vertexCount());
        for (size_t p = 0; p < nprims; ++p) {
            const auto c = geo->primitivePoints(p);
            const size_t start = geo->primitiveVertexStart(p);
            for (size_t i = 0; i < c.size(); ++i) {
                const size_t at = start + i;
                corners[at] = isCut(p) ? inside : isColor(vertexCd) ? colorAt(*vertexCd, at) : colorAt(*pointCd, c[i]);
            }
        }
        geo->points().erase("Cd");
        geo->vertices().erase("Cd");
        auto cd = geo->vertices().create("Cd", AttrType::Vec3).write<Vec3>();
        std::copy(corners.begin(), corners.end(), cd.begin());
        return geo;
    }
    std::vector<Vec3> faces(nprims, color);
    if (const AttributeArray* a = geo->primitives().find("Cd"); isColor(a)) {
        for (size_t p = 0; p < nprims; ++p) faces[p] = colorAt(*a, p);
    } else if (const AttributeArray* d = geo->detail().find("Cd"); isColor(d) && d->size() > 0) {
        std::fill(faces.begin(), faces.end(), colorAt(*d, 0));
    }
    for (size_t p = 0; p < nprims; ++p) {
        if (isCut(p)) faces[p] = inside;
    }
    geo->primitives().erase("Cd");
    geo->detail().erase("Cd");
    auto cd = geo->primitives().create("Cd", AttrType::Vec3).write<Vec3>();
    std::copy(faces.begin(), faces.end(), cd.begin());
    return geo;
}

#ifdef PG_HAVE_JOLT

bool rigidAvailable() { return true; }

namespace {

namespace Layers {
constexpr JPH::ObjectLayer still = 0;
constexpr JPH::ObjectLayer moving = 1;
constexpr JPH::uint count = 2;
}  // namespace Layers

/// How fast a piece may go: metres a second, and radians a second.
constexpr float kMaxSpeed = 40.0f;
constexpr float kMaxSpin = 30.0f;

/// `v` no longer than `limit`.
Vec3 clampLength(const Vec3& v, float limit) {
    const float l = length(v);
    return l > limit ? v * (limit / l) : v;
}

void trace(const char*, ...) {}

/// Jolt set up, once for the program.
void startJolt() {
    static std::once_flag once;
    std::call_once(once, [] {
        JPH::RegisterDefaultAllocator();
        JPH::Trace = trace;
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
    });
}

JPH::Vec3 jolt(const Vec3& v) { return JPH::Vec3(v.x, v.y, v.z); }
Vec3 ours(JPH::Vec3Arg v) { return Vec3(v.GetX(), v.GetY(), v.GetZ()); }

/// A rotation of ours as Jolt's quaternion.
JPH::Quat quaternionOf(const Rotation& r) {
    const JPH::Mat44 m(JPH::Vec4(r.x.x, r.x.y, r.x.z, 0.0f), JPH::Vec4(r.y.x, r.y.y, r.y.z, 0.0f),
                       JPH::Vec4(r.z.x, r.z.y, r.z.z, 0.0f), JPH::Vec4(0.0f, 0.0f, 0.0f, 1.0f));
    return m.GetQuaternion().Normalized();
}

Rotation rotationOf(JPH::QuatArg q) {
    Rotation r;
    r.x = ours(q * JPH::Vec3::sAxisX());
    r.y = ours(q * JPH::Vec3::sAxisY());
    r.z = ours(q * JPH::Vec3::sAxisZ());
    return r;
}

/// The shape of a collider, for Jolt: a ball, a box, a column; anything
/// else the hull of its triangles or of its box.
JPH::ShapeRefC shapeOf(const Collider& c) {
    const Vec3 half = c.size * 0.5f;
    switch (c.shape) {
        case Shape::Sphere: return new JPH::SphereShape(std::max({half.x, half.y, half.z, 1e-3f}));
        case Shape::Box: return new JPH::BoxShape(jolt(Vec3(std::max(half.x, 1e-3f), std::max(half.y, 1e-3f), std::max(half.z, 1e-3f))));
        case Shape::Cylinder: return new JPH::CylinderShape(std::max(half.y, 1e-3f), std::max(std::max(half.x, half.z), 1e-3f));
        default: break;
    }
    JPH::Array<JPH::Vec3> points;
    if (c.shape == Shape::Mesh && c.mesh) {
        // The mesh's box onto the collider's size, round its middle.
        const Vec3 center = c.mesh->center(), h = c.mesh->half();
        for (const Vec3& p : c.mesh->mesh().positions) {
            const Vec3 local = p - center;
            points.push_back(jolt(Vec3(local.x / std::max(h.x, 1e-6f) * half.x, local.y / std::max(h.y, 1e-6f) * half.y,
                                       local.z / std::max(h.z, 1e-6f) * half.z)));
        }
    } else if (c.shape == Shape::Cone) {
        for (int i = 0; i < 24; ++i) {
            const float a = static_cast<float>(i) * 6.2831853f / 24.0f;
            points.push_back(jolt(Vec3(std::cos(a) * half.x, -half.y, std::sin(a) * half.z)));
        }
        points.push_back(jolt(Vec3(0.0f, half.y, 0.0f)));
    }
    if (points.size() < 4) {
        return new JPH::BoxShape(jolt(Vec3(std::max(half.x, 1e-3f), std::max(half.y, 1e-3f), std::max(half.z, 1e-3f))));
    }
    JPH::ConvexHullShapeSettings settings(points, 0.0f);
    const JPH::ShapeSettings::ShapeResult r = settings.Create();
    if (r.HasError()) return new JPH::BoxShape(jolt(Vec3(std::max(half.x, 1e-3f), std::max(half.y, 1e-3f), std::max(half.z, 1e-3f))));
    return r.Get();
}

}  // namespace

struct RigidSolver::Impl {
    JPH::TempAllocatorImpl temp{32 * 1024 * 1024};
    JPH::JobSystemSingleThreaded jobs{JPH::cMaxPhysicsJobs};
    JPH::BroadPhaseLayerInterfaceTable broadPhase{Layers::count, 2};
    JPH::ObjectLayerPairFilterTable pairs{Layers::count};
    std::unique_ptr<JPH::ObjectVsBroadPhaseLayerFilterTable> objectVsBroadPhase;
    JPH::PhysicsSystem physics;

    struct Piece {
        JPH::BodyID body;
        std::shared_ptr<const MeshShape> mesh;  ///< at rest, for the water and the gas; null if not wanted
        Vec3 euler;                             ///< the last angles given out: the next near them
    };
    std::vector<Piece> pieces;
    struct Joint {
        JPH::Ref<JPH::FixedConstraint> constraint;
        size_t a = 0;
        Vec3 at;  ///< where the pieces touch, at rest, in the space of piece a
        bool broken = false;
    };
    std::vector<Joint> joints;
    size_t broken = 0;
    struct Object {
        JPH::BodyID body;
        int node = 0;
    };
    std::vector<Object> objects;
    struct Puff {
        Vec3 at;
        int age = 0;
    };
    std::vector<Puff> puffs;

    Impl() {
        broadPhase.MapObjectToBroadPhaseLayer(Layers::still, JPH::BroadPhaseLayer(0));
        broadPhase.MapObjectToBroadPhaseLayer(Layers::moving, JPH::BroadPhaseLayer(1));
        pairs.EnableCollision(Layers::moving, Layers::moving);
        pairs.EnableCollision(Layers::moving, Layers::still);
        objectVsBroadPhase = std::make_unique<JPH::ObjectVsBroadPhaseLayerFilterTable>(broadPhase, 2, pairs, Layers::count);
    }
};

RigidSolver::RigidSolver(const RigidScene& scene) : scene_(scene) {
    startJolt();
    impl_ = std::make_unique<Impl>();
    Impl& m = *impl_;
    const RigidSettings& s = scene_.solver;
    const Geometry* geo = scene_.pieces.get();
    int count = 0;
    const std::vector<int32_t> pieceOf = geo ? pieceOfPrimitives(*geo, scene_.attribute, count) : std::vector<int32_t>();
    const size_t bodies = static_cast<size_t>(count) + scene_.colliders.size() + 1;
    m.physics.Init(static_cast<JPH::uint>(bodies + 16), 0, static_cast<JPH::uint>(std::max<size_t>(1024, bodies * 16)),
                   static_cast<JPH::uint>(std::max<size_t>(1024, bodies * 32)), m.broadPhase, *m.objectVsBroadPhase, m.pairs);
    m.physics.SetGravity(jolt(s.gravity));
    {
        // Glued pieces are chains of fixed joints: the solver goes round
        // them more times than a game would, so that a glued beam is stiff
        // and a stack stands still.
        JPH::PhysicsSettings ps = m.physics.GetPhysicsSettings();
        ps.mNumVelocitySteps = 24;
        ps.mNumPositionSteps = 6;
        ps.mPenetrationSlop = 0.005f;
        ps.mSpeculativeContactDistance = 0.01f;
        m.physics.SetPhysicsSettings(ps);
    }
    JPH::BodyInterface& bi = m.physics.GetBodyInterface();
    if (!geo || count == 0) {
        error_ = "no pieces: link geometry into Pieces -- a Voronoi Fracture's";
        return;
    }

    // The pieces: the hull of each one's points, where they are.
    std::vector<std::vector<uint32_t>> pointsOf(static_cast<size_t>(count));
    std::vector<std::vector<uint32_t>> primsOf(static_cast<size_t>(count));
    for (size_t p = 0; p < geo->primitiveCount(); ++p) {
        primsOf[static_cast<size_t>(pieceOf[p])].push_back(static_cast<uint32_t>(p));
        for (const uint32_t c : geo->primitivePoints(p)) pointsOf[static_cast<size_t>(pieceOf[p])].push_back(c);
    }
    const auto P = geo->positions();
    const bool meshes = scene_.intoGas || scene_.intoWater;
    std::vector<JPH::Body*> made;
    for (int k = 0; k < count; ++k) {
        std::vector<uint32_t>& pts = pointsOf[static_cast<size_t>(k)];
        std::sort(pts.begin(), pts.end());
        pts.erase(std::unique(pts.begin(), pts.end()), pts.end());
        JPH::Array<JPH::Vec3> hull;
        Vec3 lo(1e30f, 1e30f, 1e30f), hi(-1e30f, -1e30f, -1e30f);
        for (const uint32_t i : pts) {
            hull.push_back(jolt(P[i]));
            for (int a = 0; a < 3; ++a) {
                lo[a] = std::min(lo[a], P[i][a]);
                hi[a] = std::max(hi[a], P[i][a]);
            }
        }
        const float least = std::min({hi.x - lo.x, hi.y - lo.y, hi.z - lo.z});
        JPH::ConvexHullShapeSettings hs(hull, std::clamp(least * 0.05f, 0.0f, 0.02f));
        hs.SetDensity(std::max(s.density, 1.0f));
        const JPH::ShapeSettings::ShapeResult r = hs.Create();
        Impl::Piece piece;
        if (r.HasError()) {
            // Too flat to be a body: a still one, out of the way.
            m.pieces.push_back(piece);
            made.push_back(nullptr);
            continue;
        }
        JPH::BodyCreationSettings bcs(r.Get(), JPH::RVec3::sZero(), JPH::Quat::sIdentity(), JPH::EMotionType::Dynamic,
                                      Layers::moving);
        bcs.mFriction = s.friction;
        bcs.mRestitution = s.bounce;
        // No faster than debris flies: a keyed object -- infinitely heavy --
        // that lands deep in the pieces would otherwise fire them off at the
        // speed of a bullet, and the water and the gas they go into with them.
        bcs.mMaxLinearVelocity = kMaxSpeed;
        bcs.mMaxAngularVelocity = kMaxSpin;
        JPH::Body* body = bi.CreateBody(bcs);
        if (!body) {
            m.pieces.push_back(piece);
            made.push_back(nullptr);
            continue;
        }
        bi.AddBody(body->GetID(), JPH::EActivation::Activate);
        piece.body = body->GetID();
        if (meshes) {
            // Its triangles at rest, for the water and the gas.
            Geometry one;
            one.addPoints(pts.size());
            auto op = one.positionsForWrite();
            std::map<uint32_t, uint32_t> local;
            for (size_t i = 0; i < pts.size(); ++i) {
                op[i] = P[pts[i]];
                local[pts[i]] = static_cast<uint32_t>(i);
            }
            for (const uint32_t prim : primsOf[static_cast<size_t>(k)]) {
                std::vector<uint32_t> c;
                for (const uint32_t q : geo->primitivePoints(prim)) c.push_back(local[q]);
                one.addPrimitive(c, geo->primitiveClosed(prim));
            }
            piece.mesh = std::make_shared<MeshShape>(triangulate(one), 16);
        }
        m.pieces.push_back(piece);
        made.push_back(body);
    }

    // The floor.
    if (s.floor) {
        JPH::BodyCreationSettings floor(new JPH::BoxShape(JPH::Vec3(500.0f, 1.0f, 500.0f)), JPH::RVec3(0.0f, -1.0f, 0.0f),
                                        JPH::Quat::sIdentity(), JPH::EMotionType::Static, Layers::still);
        floor.mFriction = s.friction;
        floor.mRestitution = s.bounce;
        bi.CreateAndAddBody(floor, JPH::EActivation::DontActivate);
    }
    // The objects: they move as they are keyed, and push what is in the way.
    for (const Collider& c : scene_.colliders) {
        JPH::BodyCreationSettings bcs(shapeOf(c), JPH::RVec3(c.center.x, c.center.y, c.center.z),
                                      quaternionOf(Rotation::fromEuler(c.rotation)), JPH::EMotionType::Kinematic,
                                      Layers::moving);
        bcs.mFriction = s.friction;
        bcs.mRestitution = s.bounce;
        const JPH::BodyID id = bi.CreateAndAddBody(bcs, JPH::EActivation::Activate);
        m.objects.push_back({id, c.node});
    }

    // The glue: pieces that share points where they were cut, joined there.
    if (s.glue > 0.0f) {
        std::vector<int32_t> pointPiece(P.size(), -1);
        for (size_t p = 0; p < geo->primitiveCount(); ++p) {
            for (const uint32_t c : geo->primitivePoints(p)) pointPiece[c] = pieceOf[p];
        }
        Vec3 lo = P.empty() ? Vec3() : P[0], hi = lo;
        for (const Vec3& p : P) {
            for (int a = 0; a < 3; ++a) {
                lo[a] = std::min(lo[a], p[a]);
                hi[a] = std::max(hi[a], p[a]);
            }
        }
        const float eps = 1e-4f * std::max(length(hi - lo), 1e-3f);
        PointTree tree(P);
        std::map<std::pair<int32_t, int32_t>, std::pair<int, Vec3>> touching;  // how many points, their sum
        std::vector<int32_t> found;
        for (size_t i = 0; i < P.size(); ++i) {
            const int32_t a = pointPiece[i];
            if (a < 0) continue;
            tree.near(P[i], eps, 0, found);
            for (const int32_t j : found) {
                const int32_t b = pointPiece[static_cast<size_t>(j)];
                if (b <= a) continue;
                auto& t = touching[{a, b}];
                ++t.first;
                t.second += P[i];
            }
        }
        for (const auto& [pair, t] : touching) {
            if (t.first < 3) continue;  // an edge or a corner: not a face they share
            JPH::Body* a = made[static_cast<size_t>(pair.first)];
            JPH::Body* b = made[static_cast<size_t>(pair.second)];
            if (!a || !b) continue;
            JPH::FixedConstraintSettings fs;
            fs.mAutoDetectPoint = true;
            Impl::Joint joint;
            joint.constraint = static_cast<JPH::FixedConstraint*>(fs.Create(*a, *b));
            joint.a = static_cast<size_t>(pair.first);
            joint.at = t.second * (1.0f / static_cast<float>(t.first));
            m.physics.AddConstraint(joint.constraint);
            m.joints.push_back(std::move(joint));
        }
    }
    m.physics.OptimizeBroadPhase();
}

RigidSolver::~RigidSolver() = default;

size_t RigidSolver::pieceCount() const { return impl_ ? impl_->pieces.size() : 0; }

void RigidSolver::setColliders(const std::vector<Collider>& colliders) {
    Impl& m = *impl_;
    JPH::BodyInterface& bi = m.physics.GetBodyInterface();
    const float dt = std::max(scene_.solver.timeStep, 1e-5f);
    for (const Collider& c : colliders) {
        for (const Impl::Object& o : m.objects) {
            if (o.node != c.node) continue;
            bi.MoveKinematic(o.body, JPH::RVec3(c.center.x, c.center.y, c.center.z),
                             quaternionOf(Rotation::fromEuler(c.rotation)), dt);
        }
    }
}

void RigidSolver::step() {
    Impl& m = *impl_;
    if (m.pieces.empty()) return;
    const RigidSettings& s = scene_.solver;
    const int substeps = std::clamp(s.substeps, 1, 16);
    const float dt = std::max(s.timeStep, 1e-5f);
    m.physics.Update(dt, substeps, &m.temp, &m.jobs);
    // What was puffed ages; the glue that pulled too hard breaks.
    for (Impl::Puff& p : m.puffs) ++p.age;
    m.puffs.erase(std::remove_if(m.puffs.begin(), m.puffs.end(), [](const Impl::Puff& p) { return p.age > 8; }), m.puffs.end());
    const float subStep = dt / static_cast<float>(substeps);
    JPH::BodyInterface& bi = m.physics.GetBodyInterface();
    for (Impl::Joint& j : m.joints) {
        if (j.broken) continue;
        const float pull = j.constraint->GetTotalLambdaPosition().Length() / subStep;
        if (pull <= s.glue) continue;
        j.broken = true;
        j.constraint->SetEnabled(false);
        m.physics.RemoveConstraint(j.constraint);
        ++m.broken;
        const JPH::BodyID body = m.pieces[j.a].body;
        const JPH::RVec3 pos = bi.GetPosition(body);
        const JPH::Quat rot = bi.GetRotation(body);
        m.puffs.push_back({ours(JPH::Vec3(pos) + rot * jolt(j.at)), 0});
    }
}

RigidFrame RigidSolver::capture() const {
    RigidFrame f;
    f.pieces = scene_.pieces;
    f.attribute = scene_.attribute;
    if (!impl_) return f;
    const JPH::BodyInterface& bi = impl_->physics.GetBodyInterface();
    f.poses.reserve(impl_->pieces.size());
    for (const Impl::Piece& p : impl_->pieces) {
        RigidPose pose;
        if (!p.body.IsInvalid()) {
            const JPH::RVec3 pos = bi.GetPosition(p.body);
            const JPH::Quat q = bi.GetRotation(p.body);
            const JPH::RVec3 com = bi.GetCenterOfMassPosition(p.body);
            pose.position = Vec3(static_cast<float>(pos.GetX()), static_cast<float>(pos.GetY()), static_cast<float>(pos.GetZ()));
            pose.rotation = Vec4(q.GetX(), q.GetY(), q.GetZ(), q.GetW());
            // The velocity of the centre of mass, carried to the point at `position`.
            pose.spin = ours(bi.GetAngularVelocity(p.body));
            const Vec3 comAt(static_cast<float>(com.GetX()), static_cast<float>(com.GetY()), static_cast<float>(com.GetZ()));
            pose.velocity = ours(bi.GetLinearVelocity(p.body)) + cross(pose.spin, pose.position - comAt);
        }
        f.poses.push_back(pose);
    }
    f.joints = impl_->joints.size();
    f.broken = impl_->broken;
    return f;
}

std::vector<Collider> RigidSolver::colliders() const {
    std::vector<Collider> out;
    if (!impl_) return out;
    const JPH::BodyInterface& bi = impl_->physics.GetBodyInterface();
    for (Impl::Piece& p : impl_->pieces) {
        if (p.body.IsInvalid() || !p.mesh) continue;
        const JPH::RVec3 pos = bi.GetPosition(p.body);
        const JPH::Quat q = bi.GetRotation(p.body);
        const Rotation r = rotationOf(q);
        const Vec3 position(static_cast<float>(pos.GetX()), static_cast<float>(pos.GetY()), static_cast<float>(pos.GetZ()));
        Collider c;
        c.shape = Shape::Mesh;
        c.mesh = p.mesh;
        c.size = p.mesh->half() * 2.0f;
        // Where the middle of the mesh's box has gone, and the turn about it.
        c.center = position + r.apply(p.mesh->center());
        p.euler = r.toEuler(p.euler);
        c.rotation = p.euler;
        const JPH::RVec3 com = bi.GetCenterOfMassPosition(p.body);
        const Vec3 v = ours(bi.GetLinearVelocity(p.body)), w = ours(bi.GetAngularVelocity(p.body));
        const Vec3 comAt(static_cast<float>(com.GetX()), static_cast<float>(com.GetY()), static_cast<float>(com.GetZ()));
        c.velocity = clampLength(v + cross(w, c.center - comAt), kMaxSpeed);
        c.spin = clampLength(w, kMaxSpin);
        c.node = scene_.node;
        out.push_back(std::move(c));
    }
    return out;
}

std::vector<std::pair<Vec3, float>> RigidSolver::dust() const {
    std::vector<std::pair<Vec3, float>> out;
    if (!impl_) return out;
    for (const Impl::Puff& p : impl_->puffs) out.push_back({p.at, 1.0f - static_cast<float>(p.age) / 9.0f});
    return out;
}

#else  // no Jolt

bool rigidAvailable() { return false; }

struct RigidSolver::Impl {};

RigidSolver::RigidSolver(const RigidScene& scene) : scene_(scene) {
    error_ = "this build has no rigid bodies: it was built without Jolt (PG_WITH_JOLT=OFF)";
}
RigidSolver::~RigidSolver() = default;
size_t RigidSolver::pieceCount() const { return 0; }
void RigidSolver::setColliders(const std::vector<Collider>&) {}
void RigidSolver::step() {}
RigidFrame RigidSolver::capture() const {
    RigidFrame f;
    f.pieces = scene_.pieces;
    f.attribute = scene_.attribute;
    return f;
}
std::vector<Collider> RigidSolver::colliders() const { return {}; }
std::vector<std::pair<Vec3, float>> RigidSolver::dust() const { return {}; }

#endif

}  // namespace pg::sim
