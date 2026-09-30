// What the viewport's selection and brush make: a group of the elements
// picked, those points moved, an attribute painted on them. The selection
// comes as a pattern (Selection.h); the brush as its dabs.
#include "pg/nodes/Nodes.h"

#include "pg/core/Geometry.h"
#include "pg/core/Parallel.h"
#include "pg/core/Selection.h"
#include "pg/core/Spatial.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

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
/// not at all the radius away.
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
    }

    GeometryPtr cookNode(const CookContext& ctx, std::span<const GeometryPtr> in) override {
        auto geo = editableCopy(in.empty() ? nullptr : in[0]);
        const AttrClass cls = classOf(params_.evalInt("class", ctx, 0));
        std::vector<uint8_t> chosen = selectElements(*geo, cls, params_.getString("group"));
        if (cls == AttrClass::Primitive) chosen = pointsOfPrimitives(*geo, chosen);
        const size_t n = geo->pointCount();
        std::vector<float> weight(n, 0.0f);
        std::vector<Vec3> picked;
        const auto P0 = geo->positions();
        for (size_t i = 0; i < n; ++i) {
            if (!chosen[i]) continue;
            weight[i] = 1.0f;
            picked.push_back(P0[i]);
        }
        if (picked.empty()) return geo;
        const float soft = std::max(params_.evalFloat("soft", ctx, 0.0f), 0.0f);
        if (soft > 0.0f) {
            // How far each other point is from the nearest one picked.
            const PointTree tree(picked);
            parallelFor(n, 4096, [&](size_t begin, size_t end) {
                for (size_t i = begin; i < end; ++i) {
                    if (chosen[i]) continue;
                    const int32_t j = tree.nearest(P0[i], soft);
                    if (j < 0) continue;
                    const float d = length(P0[i] - picked[static_cast<size_t>(j)]) / soft;
                    const float f = std::max(0.0f, 1.0f - d * d);
                    weight[i] = f * f;
                }
            });
        }

        const Vec3 t = params_.evalVec3("t", ctx, Vec3(0, 0, 0));
        const Vec3 r = params_.evalVec3("r", ctx, Vec3(0, 0, 0));
        const Vec3 s = params_.evalVec3("s", ctx, Vec3(1, 1, 1));
        const Vec3 pivot = params_.evalVec3("p", ctx, Vec3(0, 0, 0));
        const Mat4 m = Mat4::translate(pivot * -1.0f) * Mat4::scale(s) * Mat4::rotate(r) * Mat4::translate(pivot) *
                       Mat4::translate(t);
        auto P = geo->positionsForWrite();
        parallelFor(n, 16384, [&](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i) {
                if (weight[i] <= 0.0f) continue;
                P[i] = P[i] + (m.transformPoint(P[i]) - P[i]) * weight[i];
            }
        });
        if (AttributeArray* nAttr = geo->points().find("N"); nAttr && nAttr->type() == AttrType::Vec3) {
            auto N = nAttr->write<Vec3>();
            parallelFor(N.size(), 16384, [&](size_t begin, size_t end) {
                for (size_t i = begin; i < end; ++i) {
                    if (weight[i] <= 0.0f) continue;
                    const Vec3 turned = N[i] + (m.transformDirection(N[i]) - N[i]) * weight[i];
                    const float l = length(turned);
                    if (l > 1e-12f) N[i] = turned * (1.0f / l);
                }
            });
        }
        return geo;
    }
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
        parallelFor(n, 1024, [&](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i) {
                for (const Dab& d : all) {
                    const Vec3 off = P[i] - d.at;
                    const float r2 = d.radius * d.radius, d2 = dot(off, off);
                    if (d2 >= r2) continue;
                    const float f = 1.0f - d2 / r2;
                    value[i] += (d.value - value[i]) * d.strength * f * f;
                }
            }
        });
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
}

}  // namespace pg
