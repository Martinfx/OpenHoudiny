// Nodes that read, sample and build on surfaces: a file, points scattered
// over a surface, normals, copies onto points, a colour.
#include "pg/nodes/Nodes.h"

#include "pg/core/Geometry.h"
#include "pg/core/Parallel.h"
#include "pg/io/Obj.h"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <vector>

namespace pg {
namespace {

/// A number in [0, 1) that depends on i, the seed and the channel alone: the
/// same point comes out the same whatever splits the work.
float unitHash(uint64_t i, uint32_t seed, uint32_t channel) {
    uint64_t x = i * 0x9e3779b97f4a7c15ull ^ (static_cast<uint64_t>(seed) << 32) ^ (channel * 0xc2b2ae3d27d4eb4full);
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ull;
    x ^= x >> 33;
    return static_cast<float>(x >> 40) * (1.0f / 16777216.0f);
}

/// An OBJ file as geometry (pg/io/Obj.h). The file's name is `file`; `stamp`
/// is whatever says the file changed (its size and time) -- a new stamp reads
/// it again.
class FileNode : public Node {
public:
    explicit FileNode(std::string name) : Node("file", std::move(name)) {
        setInputCount(0);
        params_.setString("file", "");
        params_.setString("stamp", "");
    }

    GeometryPtr cookNode(const CookContext&, std::span<const GeometryPtr>) override {
        const std::string path = params_.getString("file", "");
        auto geo = std::make_shared<Geometry>();
        std::string error;
        if (!path.empty() && !io::readObj(path, *geo, error)) geo = std::make_shared<Geometry>();
        std::lock_guard<std::mutex> lk(mu_);
        error_ = error;
        return geo;
    }

    std::string cookError() const override {
        std::lock_guard<std::mutex> lk(mu_);
        return error_;
    }

private:
    mutable std::mutex mu_;
    std::string error_;
};

/// A triangle of a polygon's fan: its corners (points) and the primitive.
struct Triangle {
    uint32_t a, b, c;
    uint32_t prim;
};

std::vector<Triangle> fans(const Geometry& geo) {
    std::vector<Triangle> out;
    for (size_t prim = 0; prim < geo.primitiveCount(); ++prim) {
        if (!geo.primitiveClosed(prim)) continue;
        const auto face = geo.primitivePoints(prim);
        for (size_t i = 1; i + 1 < face.size(); ++i) {
            out.push_back({face[0], face[i], face[i + 1], static_cast<uint32_t>(prim)});
        }
    }
    return out;
}

/// `count` points over the surface of the polygons, as many to a square
/// metre everywhere; each with the normal of the face it is on and the
/// point attributes of its corners, blended (whole numbers and strings:
/// the nearest corner's).
class ScatterNode : public Node {
public:
    explicit ScatterNode(std::string name) : Node("scatter", std::move(name)) {
        setInputCount(1);
        params_.setInt("count", 1000);
        params_.setInt("seed", 0);
    }

    GeometryPtr cookNode(const CookContext& ctx, std::span<const GeometryPtr> in) override {
        auto out = std::make_shared<Geometry>();
        const GeometryPtr src = in.empty() ? nullptr : in[0];
        if (!src) return out;
        const std::vector<Triangle> tris = fans(*src);
        const auto P = src->positions();
        // Where each triangle's share of the area ends: a point picks its
        // triangle by where a number in [0, total) falls.
        std::vector<double> ends(tris.size());
        double total = 0.0;
        for (size_t t = 0; t < tris.size(); ++t) {
            total += 0.5 * static_cast<double>(length(cross(P[tris[t].b] - P[tris[t].a], P[tris[t].c] - P[tris[t].a])));
            ends[t] = total;
        }
        const size_t n = tris.empty() || total <= 0.0 ? 0 : static_cast<size_t>(std::clamp(params_.evalInt("count", ctx, 1000), 0, 10000000));
        if (n == 0) return out;
        const uint32_t seed = static_cast<uint32_t>(params_.evalInt("seed", ctx, 0));

        // Each point: its triangle and its weights on the three corners.
        std::vector<uint32_t> which(n);
        std::vector<Vec3> weights(n);
        parallelFor(n, 4096, [&](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i) {
                const double target = static_cast<double>(unitHash(i, seed, 0)) * total;
                const size_t t = std::min<size_t>(static_cast<size_t>(std::upper_bound(ends.begin(), ends.end(), target) - ends.begin()),
                                                  tris.size() - 1);
                const float r1 = std::sqrt(unitHash(i, seed, 1)), r2 = unitHash(i, seed, 2);
                which[i] = static_cast<uint32_t>(t);
                weights[i] = Vec3(1.0f - r1, r1 * (1.0f - r2), r1 * r2);
            }
        });

        out->addPoints(n);
        // The point attributes of the corners, blended.
        for (const std::string& name : src->points().names()) {
            const AttributeArray& from = *src->points().find(name);
            AttributeArray& to = out->points().create(name, from.type());
            switch (from.type()) {
                case AttrType::Float: blend<float>(from, to, tris, which, weights); break;
                case AttrType::Vec2: blend<Vec2>(from, to, tris, which, weights); break;
                case AttrType::Vec3: blend<Vec3>(from, to, tris, which, weights); break;
                case AttrType::Vec4: blend<Vec4>(from, to, tris, which, weights); break;
                case AttrType::Int:
                case AttrType::String: {
                    // The nearest corner's: gathered, strings and all.
                    std::vector<uint32_t> nearest(n);
                    for (size_t i = 0; i < n; ++i) {
                        const Triangle& t = tris[which[i]];
                        const Vec3& w = weights[i];
                        nearest[i] = w.x >= w.y && w.x >= w.z ? t.a : w.y >= w.z ? t.b : t.c;
                    }
                    out->points().assign(name, from.gather(nearest));
                    break;
                }
            }
        }
        // The normal of the face, unless the corners had normals of their own.
        if (!src->points().contains("N")) {
            auto N = out->points().create("N", AttrType::Vec3).write<Vec3>();
            for (size_t i = 0; i < n; ++i) {
                const Triangle& t = tris[which[i]];
                N[i] = normalize(cross(P[t.b] - P[t.a], P[t.c] - P[t.a]));
            }
        }
        return out;
    }

private:
    template <class T>
    static T mix(const T& a, const T& b, const T& c, const Vec3& w) {
        if constexpr (std::is_same_v<T, float>) {
            return a * w.x + b * w.y + c * w.z;
        } else if constexpr (std::is_same_v<T, Vec2>) {
            return Vec2(a.x * w.x + b.x * w.y + c.x * w.z, a.y * w.x + b.y * w.y + c.y * w.z);
        } else if constexpr (std::is_same_v<T, Vec3>) {
            return a * w.x + b * w.y + c * w.z;
        } else {
            return Vec4(a.x * w.x + b.x * w.y + c.x * w.z, a.y * w.x + b.y * w.y + c.y * w.z,
                        a.z * w.x + b.z * w.y + c.z * w.z, a.w * w.x + b.w * w.y + c.w * w.z);
        }
    }

    template <class T>
    static void blend(const AttributeArray& from, AttributeArray& to, const std::vector<Triangle>& tris,
                      const std::vector<uint32_t>& which, const std::vector<Vec3>& weights) {
        const auto src = from.read<T>();
        auto dst = to.write<T>();
        parallelFor(dst.size(), 8192, [&](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i) {
                const Triangle& t = tris[which[i]];
                dst[i] = mix(src[t.a], src[t.b], src[t.c], weights[i]);
            }
        });
    }
};

/// Point normals: the faces round each point, weighted by their area.
class NormalNode : public Node {
public:
    explicit NormalNode(std::string name) : Node("normal", std::move(name)) { setInputCount(1); }

    GeometryPtr cookNode(const CookContext&, std::span<const GeometryPtr> in) override {
        auto geo = editableCopy(in.empty() ? nullptr : in[0]);
        std::vector<Vec3> sum(geo->pointCount());
        for (size_t prim = 0; prim < geo->primitiveCount(); ++prim) {
            if (!geo->primitiveClosed(prim)) continue;
            const auto face = geo->primitivePoints(prim);
            const Vec3 n = polygonNormal(*geo, face);  // as long as twice the area: bigger faces count more
            for (const uint32_t p : face) sum[p] += n;
        }
        auto N = geo->points().create("N", AttrType::Vec3).write<Vec3>();
        for (size_t i = 0; i < N.size(); ++i) {
            const float len = length(sum[i]);
            N[i] = len > 0.0f ? sum[i] * (1.0f / len) : Vec3(0.0f, 1.0f, 0.0f);
        }
        return geo;
    }
};

/// The rotation that turns +y onto the unit vector n, applied to v.
Vec3 turnUpTo(const Vec3& n, const Vec3& v) {
    const Vec3 up(0.0f, 1.0f, 0.0f);
    const float c = dot(up, n);
    if (c > 0.999999f) return v;
    if (c < -0.999999f) return Vec3(v.x, -v.y, -v.z);  // half a turn about x
    const Vec3 axis = normalize(cross(up, n));
    const float s = std::sqrt(std::max(0.0f, 1.0f - c * c));
    // Rodrigues.
    return v * c + cross(axis, v) * s + axis * (dot(axis, v) * (1.0f - c));
}

/// A copy of the first input on every point of the second: moved there,
/// sized by the point's `pscale` (times `scale`), its +y turned to the
/// point's normal when `align` is on. The points' other attributes go onto
/// their copy's points.
class CopyToPointsNode : public Node {
public:
    explicit CopyToPointsNode(std::string name) : Node("copytopoints", std::move(name)) {
        setInputCount(2);
        params_.setFloat("scale", 1.0f);
        params_.setBool("align", true);
    }

    GeometryPtr cookNode(const CookContext& ctx, std::span<const GeometryPtr> in) override {
        auto out = std::make_shared<Geometry>();
        const GeometryPtr tpl = in.size() > 0 ? in[0] : nullptr;
        const GeometryPtr pts = in.size() > 1 ? in[1] : nullptr;
        if (!tpl || !pts) return out;
        const size_t copies = pts->pointCount(), tp = tpl->pointCount(), tv = tpl->vertexCount(), tprims = tpl->primitiveCount();
        if (copies == 0 || tp == 0) return out;

        // The corners first, the attributes after: each copy's primitives
        // over its own points.
        out->addPoints(copies * tp);
        for (size_t c = 0; c < copies; ++c) {
            const uint32_t offset = static_cast<uint32_t>(c * tp);
            for (size_t prim = 0; prim < tprims; ++prim) {
                std::vector<uint32_t> corners(tpl->primitivePoints(prim).begin(), tpl->primitivePoints(prim).end());
                for (uint32_t& p : corners) p += offset;
                out->addPrimitive(corners, tpl->primitiveClosed(prim));
            }
        }
        // Copy k of element i is element k * count + i.
        auto repeat = [&](size_t count) {
            std::vector<uint32_t> idx(copies * count);
            for (size_t c = 0; c < copies; ++c) {
                for (size_t i = 0; i < count; ++i) idx[c * count + i] = static_cast<uint32_t>(i);
            }
            return idx;
        };
        const std::vector<uint32_t> byPoint = repeat(tp), byVertex = repeat(tv), byPrim = repeat(tprims);
        for (const std::string& name : tpl->points().names()) out->points().assign(name, tpl->points().find(name)->gather(byPoint));
        for (const std::string& name : tpl->vertices().names()) out->vertices().assign(name, tpl->vertices().find(name)->gather(byVertex));
        for (const std::string& name : tpl->primitives().names()) {
            out->primitives().assign(name, tpl->primitives().find(name)->gather(byPrim));
        }
        for (const std::string& name : tpl->detail().names()) out->detail().assign(name, *tpl->detail().find(name));

        // The points' attributes onto their copies.
        std::vector<uint32_t> owner(copies * tp);
        for (size_t c = 0; c < copies; ++c) std::fill(owner.begin() + static_cast<long>(c * tp), owner.begin() + static_cast<long>((c + 1) * tp), static_cast<uint32_t>(c));
        for (const std::string& name : pts->points().names()) {
            if (name == "P" || name == "N" || name == "pscale") continue;
            out->points().assign(name, pts->points().find(name)->gather(owner));
        }

        // Moved, sized and turned.
        const float scale = params_.evalFloat("scale", ctx, 1.0f);
        const bool align = params_.evalBool("align", ctx, true);
        const auto at = pts->positions();
        const AttributeArray* pscaleAttr = pts->points().find("pscale");
        const AttributeArray* normalAttr = pts->points().find("N");
        const bool hasScale = pscaleAttr && pscaleAttr->type() == AttrType::Float;
        const bool hasNormal = align && normalAttr && normalAttr->type() == AttrType::Vec3;
        AttributeArray* nOut = out->points().find("N");
        const bool turnNormals = nOut && nOut->type() == AttrType::Vec3;
        auto P = out->positionsForWrite();
        std::span<Vec3> N;
        if (turnNormals) N = nOut->write<Vec3>();
        parallelFor(copies, 256, [&](size_t begin, size_t end) {
            for (size_t c = begin; c < end; ++c) {
                const float s = scale * (hasScale ? pscaleAttr->read<float>()[c] : 1.0f);
                const Vec3 up = hasNormal ? normalize(normalAttr->read<Vec3>()[c]) : Vec3(0.0f, 1.0f, 0.0f);
                const bool turn = hasNormal && length(up) > 0.5f;
                for (size_t i = c * tp; i < (c + 1) * tp; ++i) {
                    const Vec3 local = P[i] * s;
                    P[i] = at[c] + (turn ? turnUpTo(up, local) : local);
                    if (turnNormals && turn) N[i] = turnUpTo(up, N[i]);
                }
            }
        });
        return out;
    }
};

/// A colour: `Cd` on every point, or every primitive.
class ColorNode : public Node {
public:
    explicit ColorNode(std::string name) : Node("color", std::move(name)) {
        setInputCount(1);
        params_.setVec3("color", Vec3(1.0f, 1.0f, 1.0f));
        params_.setInt("class", 0);  // 0 points, 1 primitives
    }

    GeometryPtr cookNode(const CookContext& ctx, std::span<const GeometryPtr> in) override {
        auto geo = editableCopy(in.empty() ? nullptr : in[0]);
        const Vec3 color = params_.evalVec3("color", ctx, Vec3(1.0f, 1.0f, 1.0f));
        AttributeSet& set = params_.evalInt("class", ctx, 0) == 1 ? geo->primitives() : geo->points();
        auto Cd = set.create("Cd", AttrType::Vec3).write<Vec3>();
        std::fill(Cd.begin(), Cd.end(), color);
        return geo;
    }
};

}  // namespace

void registerSurfaceNodes() {
    auto& r = NodeRegistry::instance();
    r.add("file", [](const std::string& n) { return std::make_unique<FileNode>(n); });
    r.add("scatter", [](const std::string& n) { return std::make_unique<ScatterNode>(n); });
    r.add("normal", [](const std::string& n) { return std::make_unique<NormalNode>(n); });
    r.add("copytopoints", [](const std::string& n) { return std::make_unique<CopyToPointsNode>(n); });
    r.add("color", [](const std::string& n) { return std::make_unique<ColorNode>(n); });
}

}  // namespace pg
