#include "pg/nodes/Nodes.h"

#include "pg/core/Geometry.h"
#include "pg/core/Instances.h"
#include "pg/core/Parallel.h"
#include "pg/core/Selection.h"

#include <algorithm>
#include <cmath>

namespace pg {
namespace {

/// Pass-through. Exists to give graphs a stable handle to wire against.
class NullNode : public Node {
public:
    explicit NullNode(std::string name) : Node("null", std::move(name)) {
        setInputCount(1);
    }
    GeometryPtr cookNode(const CookContext&, std::span<const GeometryPtr> in) override {
        return in.empty() || !in[0] ? std::make_shared<Geometry>() : in[0];
    }
};

/// Rigid transform.
///
/// This node is the clearest demonstration of invariant I1: it writes `P`
/// (and `N` if present) and nothing else, so every other attribute buffer in
/// the output is the *same allocation* as in the input. Cost is O(points),
/// not O(points x attributes).
class TransformNode : public Node {
public:
    explicit TransformNode(std::string name) : Node("transform", std::move(name)) {
        setInputCount(1);
        params_.setVec3("t", Vec3(0, 0, 0));
        params_.setVec3("r", Vec3(0, 0, 0));
        params_.setVec3("s", Vec3(1, 1, 1));
        params_.setFloat("scale", 1.0f);  // uniform, on top of s
        params_.setVec3("p", Vec3(0, 0, 0));  // the pivot: what it scales and turns about
    }

    GeometryPtr cookNode(const CookContext& ctx, std::span<const GeometryPtr> in) override {
        auto geo = editableCopy(in.empty() ? nullptr : in[0]);

        const Vec3 t = params_.evalVec3("t", ctx, Vec3(0, 0, 0));
        const Vec3 r = params_.evalVec3("r", ctx, Vec3(0, 0, 0));
        const Vec3 s = params_.evalVec3("s", ctx, Vec3(1, 1, 1)) * params_.evalFloat("scale", ctx, 1.0f);
        const Vec3 pivot = params_.evalVec3("p", ctx, Vec3(0, 0, 0));
        // Round the pivot: sized, turned, then moved.
        const Mat4 m = translation(t) * (translation(pivot) * (rotationXYZ(r) * (scaling(s) * translation(pivot * -1.0f))));
        // Before N turns: a point turned to it keeps the turn it had.
        turnInstances(*geo, m);

        auto P = geo->positionsForWrite();
        parallelFor(P.size(), 16384, [&](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i) P[i] = transformPoint(m, P[i]);
        });
        // A proxy -- where the pieces of rough concrete are simulated -- is a
        // place too.
        if (AttributeArray* proxy = geo->points().find("proxy"); proxy && proxy->type() == AttrType::Vec3) {
            auto Q = proxy->write<Vec3>();
            parallelFor(Q.size(), 16384, [&](size_t begin, size_t end) {
                for (size_t i = begin; i < end; ++i) Q[i] = transformPoint(m, Q[i]);
            });
        }

        if (AttributeArray* nAttr = geo->points().find("N");
            nAttr && nAttr->type() == AttrType::Vec3) {
            auto N = nAttr->write<Vec3>();
            parallelFor(N.size(), 16384, [&](size_t begin, size_t end) {
                for (size_t i = begin; i < end; ++i) N[i] = transformDirection(m, N[i]);
            });
        }
        return geo;
    }

private:
    /// What the points stand for turns and sizes with them: their orient
    /// turned as the transform turns -- the nearest turn to it, where it
    /// stretches -- and their pscale times how much it sizes, on the whole.
    /// A point turned to its N gets the orient that turned it so, turned:
    /// turned again to its turned N, what stands on it would twist about it.
    static void turnInstances(Geometry& geo, const Mat4& m) {
        const auto byPrototype = instancesByPrototype(geo);
        std::vector<uint32_t> points;
        for (const auto& p : byPrototype) points.insert(points.end(), p.begin(), p.end());
        if (points.empty()) return;
        const Vec3 x = transformDirection(m, Vec3(1.0f, 0.0f, 0.0f)), y = transformDirection(m, Vec3(0.0f, 1.0f, 0.0f)),
                   z = transformDirection(m, Vec3(0.0f, 0.0f, 1.0f));
        const float det = dot(x, cross(y, z));
        if (std::fabs(det) < 1e-20f) return;
        const float size = std::cbrt(std::fabs(det));
        // The turn: the frame the axes go to, made square.
        const Vec3 X = normalize(x), Z = normalize(cross(X, y)), Y = cross(Z, X);
        const Vec4 turn = quatFromAxes(X, Y, Z);
        const bool turned = std::fabs(turn.w) < 0.9999999f;
        const AttributeArray* oAttr = geo.points().find("orient");
        const bool hasOrient = oAttr && oAttr->type() == AttrType::Vec4;
        const AttributeArray* nAttr = geo.points().find("N");
        const bool hasN = nAttr && nAttr->type() == AttrType::Vec3;
        if (turned) {
            if (!hasOrient) {
                // As they stand now: turned to N, else not at all.
                const std::vector<Placement> now = placementsOf(geo);
                auto o = geo.points().create("orient", AttrType::Vec4).write<Vec4>();
                std::fill(o.begin(), o.end(), Vec4(0.0f, 0.0f, 0.0f, 1.0f));
                if (hasN) {
                    for (const uint32_t p : points) o[p] = now[p].orient;
                }
            }
            auto o = geo.points().find("orient")->write<Vec4>();
            for (const uint32_t p : points) o[p] = quatMultiply(turn, o[p]);
        }
        if (std::fabs(size - 1.0f) > 1e-6f) {
            AttributeArray* ps = geo.points().find("pscale");
            const bool had = ps && ps->type() == AttrType::Float;
            // A point that stands for nothing keeps what it had -- 0, a dot
            // a few pixels wide, where there was no pscale.
            auto S = geo.points().create("pscale", AttrType::Float).write<float>();
            for (const uint32_t p : points) S[p] = (had ? S[p] : 1.0f) * size;
        }
    }
};

/// Concatenates every connected input, in input order.
class MergeNode : public Node {
public:
    explicit MergeNode(std::string name) : Node("merge", std::move(name)) {
        setInputCount(4);
    }

    GeometryPtr cookNode(const CookContext&, std::span<const GeometryPtr> in) override {
        std::shared_ptr<Geometry> out;
        for (const auto& g : in) {
            if (!g) continue;
            if (!out) {
                out = editableCopy(g);  // first input is taken wholesale, no copy
            } else {
                out->append(*g);
            }
        }
        return out ? GeometryPtr(out) : std::make_shared<Geometry>();
    }
};

/// Selects one input by index.
class SwitchNode : public Node {
public:
    explicit SwitchNode(std::string name) : Node("switch", std::move(name)) {
        setInputCount(2);
        params_.setInt("index", 0);
    }

    GeometryPtr cookNode(const CookContext& ctx, std::span<const GeometryPtr> in) override {
        int idx = static_cast<int>(params_.evalFloat("index", ctx,
                                                     static_cast<float>(params_.evalInt("index", ctx, 0))));
        if (in.empty()) return std::make_shared<Geometry>();
        idx = std::clamp(idx, 0, static_cast<int>(in.size()) - 1);
        return in[static_cast<size_t>(idx)] ? in[static_cast<size_t>(idx)]
                                            : std::make_shared<Geometry>();
    }
};

/// Creates (or overwrites) a constant attribute.
class AttribCreateNode : public Node {
public:
    explicit AttribCreateNode(std::string name) : Node("attribcreate", std::move(name)) {
        setInputCount(1);
        params_.setString("name", "mass");
        params_.setInt("class", static_cast<int>(AttrClass::Point));
        params_.setBool("vector", false);
        params_.setFloat("value", 1.0f);
        params_.setVec3("vvalue", Vec3(0, 0, 0));
    }

    GeometryPtr cookNode(const CookContext& ctx, std::span<const GeometryPtr> in) override {
        auto geo = editableCopy(in.empty() ? nullptr : in[0]);
        const std::string attrName = params_.getString("name", "mass");
        if (attrName.empty()) return geo;

        const auto cls = static_cast<AttrClass>(
            std::clamp(params_.evalInt("class", ctx, 1), 0, 3));
        AttributeSet& set = geo->attributes(cls);

        if (params_.evalBool("vector", ctx, false)) {
            const Vec3 v = params_.evalVec3("vvalue", ctx, Vec3(0, 0, 0));
            auto span = set.create(attrName, AttrType::Vec3).write<Vec3>();
            std::fill(span.begin(), span.end(), v);
        } else {
            const float v = params_.evalFloat("value", ctx, 1.0f);
            auto span = set.create(attrName, AttrType::Float).write<float>();
            std::fill(span.begin(), span.end(), v);
        }
        return geo;
    }
};

/// Builds a point group from an axis-aligned box.
class GroupBoxNode : public Node {
public:
    explicit GroupBoxNode(std::string name) : Node("groupbox", std::move(name)) {
        setInputCount(1);
        params_.setString("name", "selected");
        params_.setVec3("min", Vec3(-0.5f, -0.5f, -0.5f));
        params_.setVec3("max", Vec3(0.5f, 0.5f, 0.5f));
    }

    GeometryPtr cookNode(const CookContext& ctx, std::span<const GeometryPtr> in) override {
        auto geo = editableCopy(in.empty() ? nullptr : in[0]);
        const std::string groupName = params_.getString("name", "selected");
        if (groupName.empty()) return geo;

        const Vec3 lo = params_.evalVec3("min", ctx, Vec3(-0.5f, -0.5f, -0.5f));
        const Vec3 hi = params_.evalVec3("max", ctx, Vec3(0.5f, 0.5f, 0.5f));

        Group& g = geo->createGroup(groupName, AttrClass::Point);
        g.resize(geo->pointCount());
        auto P = geo->positions();
        for (size_t i = 0; i < P.size(); ++i) {
            const Vec3& p = P[i];
            const bool inside = p.x >= lo.x && p.x <= hi.x && p.y >= lo.y &&
                                p.y <= hi.y && p.z >= lo.z && p.z <= hi.z;
            if (inside) g.set(i, true);
        }
        return geo;
    }
};

/// Deletes the points a group or a pattern names -- "0-9 12", groups, "*"
/// (Selection.h) -- and the primitives they were part of; or, of class
/// primitive, the primitives it names, and the points only they used. With
/// `invert` it keeps only them. When it names nothing there is -- no such
/// group -- nothing goes; an empty group kept leaves nothing.
class BlastNode : public Node {
public:
    explicit BlastNode(std::string name) : Node("blast", std::move(name)) {
        setInputCount(1);
        params_.setString("group", "selected");
        params_.setInt("class", 0);  // 0 points, 1 primitives
        params_.setBool("invert", false);
    }

    GeometryPtr cookNode(const CookContext& ctx, std::span<const GeometryPtr> in) override {
        auto geo = editableCopy(in.empty() ? nullptr : in[0]);
        const bool prims = params_.evalInt("class", ctx, 0) == 1;
        bool named = false;
        const std::vector<uint8_t> chosen = selectElements(*geo, prims ? AttrClass::Primitive : AttrClass::Point,
                                                           params_.getString("group", "selected"), &named);
        if (!named) return geo;

        const bool invert = params_.evalBool("invert", ctx, false);
        std::vector<uint8_t> keep(chosen.size());
        for (size_t i = 0; i < keep.size(); ++i) keep[i] = static_cast<uint8_t>(invert ? chosen[i] : !chosen[i]);
        if (prims) {
            geo->deletePrimitives(keep, true);
        } else {
            geo->deletePoints(keep);
        }
        return geo;
    }
};

}  // namespace

void registerModifierNodes() {
    auto& r = NodeRegistry::instance();
    r.add("null", [](const std::string& n) { return std::make_unique<NullNode>(n); });
    r.add("transform", [](const std::string& n) { return std::make_unique<TransformNode>(n); });
    r.add("merge", [](const std::string& n) { return std::make_unique<MergeNode>(n); });
    r.add("switch", [](const std::string& n) { return std::make_unique<SwitchNode>(n); });
    r.add("attribcreate", [](const std::string& n) { return std::make_unique<AttribCreateNode>(n); });
    r.add("groupbox", [](const std::string& n) { return std::make_unique<GroupBoxNode>(n); });
    r.add("blast", [](const std::string& n) { return std::make_unique<BlastNode>(n); });
}

void registerBuiltinNodes() {
    static const bool once = [] {
        registerGeneratorNodes();
        registerModifierNodes();
        registerWrangleNodes();
        registerPrimitiveNodes();
        registerSurfaceNodes();
        registerTopologyNodes();
        registerFractureNodes();
        registerConcreteNodes();
        registerClusterNodes();
        registerRebarNodes();
        registerGlassNodes();
        registerBrickNodes();
        registerVolumeNodes();
        registerEditNodes();
        registerTreeNodes();
        registerGrassNodes();
        registerUsdNodes();
        return true;
    }();
    (void)once;
}

}  // namespace pg
