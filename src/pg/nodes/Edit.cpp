// What the viewport's selection and brush make: a group of the elements
// picked, those points moved, an attribute painted on them. The selection
// comes as a pattern (Selection.h); the brush as its dabs.
#include "pg/nodes/Nodes.h"

#include "pg/core/Geometry.h"
#include "pg/core/Mirror.h"
#include "pg/core/Parallel.h"
#include "pg/core/Sculpt.h"
#include "pg/core/Selection.h"
#include "pg/core/Soft.h"
#include "pg/core/Spatial.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <mutex>

namespace pg {
namespace {

AttrClass classOf(int choice) { return choice == 1 ? AttrClass::Primitive : AttrClass::Point; }

/// A group of the elements a pattern names -- "0-9 12", other groups, "*":
/// Group Create in Houdini. What is in a group of that name already is not
/// kept.
class GroupCreateNode : public Node {
public:
    explicit GroupCreateNode(std::string name) : Node("groupcreate", std::move(name)) {
        setInputCount(1);
        params_.setString("name", "group1");
        params_.setInt("class", 0);  // 0 points, 1 primitives
        params_.setString("pattern", "");
    }

    GeometryPtr cookNode(const CookContext& ctx, std::span<const GeometryPtr> in) override {
        auto geo = editableCopy(in.empty() ? nullptr : in[0]);
        const std::string name = params_.getString("name", "group1");
        if (name.empty()) return geo;
        const AttrClass cls = classOf(params_.evalInt("class", ctx, 0));
        const std::vector<uint8_t> mask = selectElements(*geo, cls, params_.getString("pattern"));
        Group& g = geo->createGroup(name, cls);
        g.resize(mask.size());
        for (size_t i = 0; i < mask.size(); ++i) g.set(i, mask[i] != 0);
        return geo;
    }
};

/// The points a pattern names -- or the points of the primitives it names --
/// scaled, then turned about the pivot, then moved: what the viewport's
/// handle does to a selection. With a soft radius the points round them
/// follow too, less the further they are: all the way at the selection,
/// not at all the radius away (Soft.h) -- the distance straight, or along
/// the surface; the falloff's shape a hill, a cone, a spike, a dome, flat.
/// With Symmetry the mirror images of the points move too (Mirror.h): the
/// side of the plane the pivot is on as the Edit says, the other side as
/// its mirror image, points on the plane halfway between -- they stay on it.
class EditNode : public Node {
public:
    explicit EditNode(std::string name) : Node("edit", std::move(name)) {
        setInputCount(1);
        params_.setString("group", "");
        params_.setInt("class", 0);
        params_.setVec3("t", Vec3(0, 0, 0));
        params_.setVec3("r", Vec3(0, 0, 0));
        params_.setVec3("s", Vec3(1, 1, 1));
        params_.setVec3("p", Vec3(0, 0, 0));
        params_.setFloat("soft", 0.0f);
        params_.setInt("metric", 0);   // 0 space, 1 along the surface
        params_.setInt("falloff", 0);  // Falloff: smooth, linear, sharp, sphere, constant
        params_.setInt("symmetry", 0);  // Mirror: none, x, y, z
    }

    GeometryPtr cookNode(const CookContext& ctx, std::span<const GeometryPtr> in) override {
        auto geo = editableCopy(in.empty() ? nullptr : in[0]);
        const AttrClass cls = classOf(params_.evalInt("class", ctx, 0));
        std::vector<uint8_t> chosen = selectElements(*geo, cls, params_.getString("group"));
        if (cls == AttrClass::Primitive) chosen = pointsOfPrimitives(*geo, chosen);
        const size_t n = geo->pointCount();
        if (std::none_of(chosen.begin(), chosen.end(), [](uint8_t c) { return c != 0; })) return geo;
        const Mirror mirror = static_cast<Mirror>(std::clamp(params_.evalInt("symmetry", ctx, 0), 0, 3));
        if (mirror != Mirror::None) chosen = withMirror(*geo, chosen, mirror);
        const float soft = std::max(params_.evalFloat("soft", ctx, 0.0f), 0.0f);
        const SoftDistance metric = params_.evalInt("metric", ctx, 0) == 1 ? SoftDistance::Surface : SoftDistance::Space;
        const Falloff shape = static_cast<Falloff>(std::clamp(params_.evalInt("falloff", ctx, 0), 0, 4));
        const std::vector<float> weight = softWeights(*geo, chosen, soft, metric, shape);

        const Vec3 t = params_.evalVec3("t", ctx, Vec3(0, 0, 0));
        const Vec3 r = params_.evalVec3("r", ctx, Vec3(0, 0, 0));
        const Vec3 s = params_.evalVec3("s", ctx, Vec3(1, 1, 1));
        const Vec3 pivot = params_.evalVec3("p", ctx, Vec3(0, 0, 0));
        // Round the pivot: sized, turned, then moved.
        const Mat4 m = translation(t) * (translation(pivot) * (rotationXYZ(r) * (scaling(s) * translation(pivot * -1.0f))));
        // Which side of the plane a point is on: the pivot's, the other, or
        // the plane itself.
        const int axis = mirrorAxis(mirror);
        const float tolerance = axis >= 0 ? mirrorTolerance(*geo) : 0.0f;
        const float side = axis >= 0 && pivot[axis] < 0.0f ? -1.0f : 1.0f;
        const auto sideOf = [&](const Vec3& p) {
            if (axis < 0) return 1;
            const float c = p[axis] * side;
            return c > tolerance ? 1 : c < -tolerance ? -1 : 0;
        };
        // Where the Edit takes a place, all the way -- or a way.
        const auto moveTo = [&](const Vec3& p, int on) {
            const Vec3 direct = transformPoint(m, p);
            if (on > 0) return direct;
            const Vec3 image = mirrored(transformPoint(m, mirrored(p, mirror)), mirror);
            return on < 0 ? image : (direct + image) * 0.5f;
        };
        const auto turnTo = [&](const Vec3& v, int on) {
            const Vec3 direct = transformDirection(m, v);
            if (on > 0) return direct;
            const Vec3 image = mirrored(transformDirection(m, mirrored(v, mirror)), mirror);
            return on < 0 ? image : (direct + image) * 0.5f;
        };
        const std::vector<Vec3> before(geo->positions().begin(), geo->positions().end());
        auto P = geo->positionsForWrite();
        parallelFor(n, 16384, [&](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i) {
                if (weight[i] <= 0.0f) continue;
                P[i] = P[i] + (moveTo(P[i], sideOf(P[i])) - P[i]) * weight[i];
            }
        });
        if (AttributeArray* nAttr = geo->points().find("N"); nAttr && nAttr->type() == AttrType::Vec3) {
            auto N = nAttr->write<Vec3>();
            parallelFor(N.size(), 16384, [&](size_t begin, size_t end) {
                for (size_t i = begin; i < end; ++i) {
                    if (weight[i] <= 0.0f) continue;
                    const Vec3 turned = N[i] + (turnTo(N[i], sideOf(before[i])) - N[i]) * weight[i];
                    const float l = length(turned);
                    if (l > 1e-12f) N[i] = turned * (1.0f / l);
                }
            });
        }
        return geo;
    }
};

/// The surface pushed, smoothed, grabbed and flattened by the viewport's
/// sculpting brush (pg/core/Sculpt.h): its dabs, in the order they were
/// made, each where the surface is after the ones before. Tool, Radius and
/// Strength are what the brush makes its next dabs with; Falloff shapes
/// them all, and so does Dyntopo (pg/core/Dyntopo.h): the mesh made finer
/// under each dab, coarser where it is finer than it needs.
class SculptNode : public Node {
public:
    explicit SculptNode(std::string name) : Node("sculpt", std::move(name)) {
        setInputCount(1);
        params_.setInt("tool", 0);  // push, smooth, grab, flatten
        params_.setFloat("radius", 0.2f);
        params_.setFloat("strength", 0.5f);
        params_.setInt("falloff", 0);
        params_.setBool("dyntopo", false);
        params_.setInt("refine", 2);      // subdivide, collapse, both
        params_.setInt("detailmode", 0);  // brush, constant
        params_.setFloat("detail", 0.25f);
        params_.setFloat("detailsize", 0.05f);
        params_.setString("strokes", "");
    }

    GeometryPtr cookNode(const CookContext& ctx, std::span<const GeometryPtr> in) override {
        const std::vector<SculptDab> dabs = parseSculpt(params_.getString("strokes"));
        const Falloff shape = static_cast<Falloff>(std::clamp(params_.evalInt("falloff", ctx, 0), 0, 4));
        Dyntopo dyntopo;
        if (params_.evalBool("dyntopo", ctx, false)) {
            const int refine = std::clamp(params_.evalInt("refine", ctx, 2), 0, 2);
            dyntopo.subdivide = refine != 1;
            dyntopo.collapse = refine != 0;
            dyntopo.constant = params_.evalInt("detailmode", ctx, 0) == 1;
            dyntopo.detail = dyntopo.constant ? std::max(params_.evalFloat("detailsize", ctx, 0.05f), 1e-4f)
                                              : std::clamp(params_.evalFloat("detail", ctx, 0.25f), 0.02f, 4.0f);
        }
        // While a stroke goes on, only the dabs it added since.
        std::lock_guard<std::mutex> lk(mu_);
        return sculptor_.cook(in.empty() ? nullptr : in[0], dabs, shape, dyntopo);
    }

private:
    std::mutex mu_;
    Sculptor sculptor_;
};

/// A number painted onto the points with a brush -- the viewport's paint
/// tool: each dab a ball, where its value is laid on as far as its
/// strength says at its middle, less towards its edge, none past it. The
/// dabs go in the order they were painted. What the attribute was before
/// is where they start from; a new one starts from Default. Resolution
/// free: the dabs are places, not point numbers -- make the grid finer and
/// the paint stays where it was.
class AttribPaintNode : public Node {
public:
    explicit AttribPaintNode(std::string name) : Node("attribpaint", std::move(name)) {
        setInputCount(1);
        params_.setString("name", "pin");
        params_.setFloat("default", 0.0f);
        params_.setString("strokes", "");
        // The brush's: what the viewport paints the next dabs with.
        params_.setFloat("value", 1.0f);
        params_.setFloat("erase", 0.0f);
        params_.setFloat("radius", 0.15f);
        params_.setFloat("strength", 0.5f);
    }

    struct Dab {
        Vec3 at;
        float radius = 0.0f, value = 0.0f, strength = 0.0f;
    };

    /// "x y z radius value strength; ..."; what does not read so is left out.
    static std::vector<Dab> dabs(const std::string& text) {
        std::vector<Dab> out;
        const char* s = text.c_str();
        while (*s) {
            float v[6];
            int k = 0;
            char* end = nullptr;
            for (; k < 6; ++k) {
                v[k] = std::strtof(s, &end);
                if (end == s) break;
                s = end;
            }
            if (k == 6 && v[3] > 0.0f && std::isfinite(v[0] + v[1] + v[2] + v[3] + v[4] + v[5])) {
                out.push_back({Vec3(v[0], v[1], v[2]), v[3], v[4], std::clamp(v[5], 0.0f, 1.0f)});
            }
            // On to the next dab.
            while (*s && *s != ';') ++s;
            if (*s == ';') ++s;
        }
        return out;
    }

    GeometryPtr cookNode(const CookContext& ctx, std::span<const GeometryPtr> in) override {
        auto geo = editableCopy(in.empty() ? nullptr : in[0]);
        const std::string name = params_.getString("name", "pin");
        if (name.empty() || name == "P") return geo;
        const size_t n = geo->pointCount();
        AttributeArray* a = geo->points().find(name);
        if (a && a->type() != AttrType::Float && a->type() != AttrType::Int) return geo;  // a number only
        std::vector<float> value(n, params_.evalFloat("default", ctx, 0.0f));
        if (a && a->type() == AttrType::Float) {
            const auto v = a->read<float>();
            std::copy(v.begin(), v.end(), value.begin());
        } else if (a) {
            const auto v = a->read<int32_t>();
            for (size_t i = 0; i < n; ++i) value[i] = static_cast<float>(v[i]);
        }
        const std::vector<Dab> all = dabs(params_.getString("strokes"));
        const auto P = geo->positions();
        if (!all.empty()) {
            // Dab after dab, each on the points within its reach alone: a
            // stroke of thousands on a fine mesh stays quick.
            const PointTree tree(P);
            std::vector<int32_t> near;
            for (const Dab& d : all) {
                tree.near(d.at, d.radius, 0, near);
                const float r2 = d.radius * d.radius;
                for (const int32_t i : near) {
                    const Vec3 off = P[static_cast<size_t>(i)] - d.at;
                    const float d2 = dot(off, off);
                    if (d2 >= r2) continue;
                    const float f = 1.0f - d2 / r2;
                    value[static_cast<size_t>(i)] += (d.value - value[static_cast<size_t>(i)]) * d.strength * f * f;
                }
            }
        }
        if (a && a->type() == AttrType::Int) {
            auto v = a->write<int32_t>();
            for (size_t i = 0; i < n; ++i) v[i] = static_cast<int32_t>(std::lround(value[i]));
        } else {
            if (!a) a = &geo->points().create(name, AttrType::Float);
            auto v = a->write<float>();
            std::copy(value.begin(), value.end(), v.begin());
        }
        return geo;
    }
};

}  // namespace

void registerEditNodes() {
    auto& r = NodeRegistry::instance();
    r.add("groupcreate", [](const std::string& n) { return std::make_unique<GroupCreateNode>(n); });
    r.add("edit", [](const std::string& n) { return std::make_unique<EditNode>(n); });
    r.add("attribpaint", [](const std::string& n) { return std::make_unique<AttribPaintNode>(n); });
    r.add("sculpt", [](const std::string& n) { return std::make_unique<SculptNode>(n); });
}

}  // namespace pg
