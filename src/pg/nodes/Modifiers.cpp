#include "pg/nodes/Nodes.h"

#include "pg/core/Geometry.h"
#include "pg/core/Parallel.h"

#include <algorithm>

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
    }

    GeometryPtr cookNode(const CookContext& ctx, std::span<const GeometryPtr> in) override {
        auto geo = editableCopy(in.empty() ? nullptr : in[0]);

        const Vec3 t = params_.evalVec3("t", ctx, Vec3(0, 0, 0));
        const Vec3 r = params_.evalVec3("r", ctx, Vec3(0, 0, 0));
        const Vec3 s = params_.evalVec3("s", ctx, Vec3(1, 1, 1));
        const Mat4 m = Mat4::scale(s) * Mat4::rotate(r) * Mat4::translate(t);

        auto P = geo->positionsForWrite();
        parallelFor(P.size(), 16384, [&](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i) P[i] = m.transformPoint(P[i]);
        });

        if (AttributeArray* nAttr = geo->points().find("N");
            nAttr && nAttr->type() == AttrType::Vec3) {
            auto N = nAttr->write<Vec3>();
            parallelFor(N.size(), 16384, [&](size_t begin, size_t end) {
                for (size_t i = begin; i < end; ++i) N[i] = m.transformDirection(N[i]);
            });
        }
        return geo;
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
                                                     static_cast<float>(params_.getInt("index", 0))));
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
            std::clamp(params_.getInt("class", 1), 0, 3));
        AttributeSet& set = geo->attributes(cls);

        if (params_.getBool("vector", false)) {
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

/// Deletes the points of a group (or keeps only them when `invert` is set).
/// Primitives losing any point are deleted with it.
class BlastNode : public Node {
public:
    explicit BlastNode(std::string name) : Node("blast", std::move(name)) {
        setInputCount(1);
        params_.setString("group", "selected");
        params_.setBool("invert", false);
    }

    GeometryPtr cookNode(const CookContext&, std::span<const GeometryPtr> in) override {
        auto geo = editableCopy(in.empty() ? nullptr : in[0]);
        const Group* g = geo->findGroup(params_.getString("group", "selected"));
        if (!g) return geo;

        const bool invert = params_.getBool("invert", false);
        std::vector<uint8_t> keep(geo->pointCount(), invert ? 0 : 1);
        for (size_t i = 0; i < keep.size(); ++i) {
            const bool member = g->contains(i);
            keep[i] = static_cast<uint8_t>(invert ? member : !member);
        }
        geo->deletePoints(keep);
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
        return true;
    }();
    (void)once;
}

}  // namespace pg
