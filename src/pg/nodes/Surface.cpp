// Nodes that read, sample and build on surfaces: a file, points scattered
// over a surface, normals, copies onto points -- or instances of them, and
// the copies made of instances -- a colour, a material, texture
// coordinates.
#include "pg/nodes/Nodes.h"

#include "pg/core/Geometry.h"
#include "pg/core/Instances.h"
#include "pg/core/Material.h"
#include "pg/core/Parallel.h"
#include "pg/core/Selection.h"
#include "pg/io/Obj.h"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <string>
#include <unordered_map>
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

template <class T>
T mixCorners(const T& a, const T& b, const T& c, const Vec3& w) {
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
void blendCorners(const AttributeArray& from, AttributeArray& to, const std::vector<Triangle>& tris,
                  const std::vector<uint32_t>& which, const std::vector<Vec3>& weights) {
    const auto src = from.read<T>();
    auto dst = to.write<T>();
    parallelFor(dst.size(), 8192, [&](size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i) {
            const Triangle& t = tris[which[i]];
            dst[i] = mixCorners(src[t.a], src[t.b], src[t.c], weights[i]);
        }
    });
}

/// Leaves out the points the rules keep off -- where the face leans more
/// than maxSlope from level; where the density attribute (0 to 1, blended
/// from the corners) says a share of them is enough; nearer than
/// minDistance to a point kept before it -- the rest moved to the front in
/// their order.
void thinScattered(const Geometry& src, const std::vector<Triangle>& tris, const ScatterRules& r,
                   std::vector<uint32_t>& which, std::vector<Vec3>& weights) {
    const size_t n = which.size();
    const auto P = src.positions();
    const float maxSlope = std::clamp(r.maxSlope, 0.0f, 180.0f);
    const float lowest = std::cos(maxSlope * 3.14159265f / 180.0f);  // the least y of a face's normal
    const AttributeArray* density = r.densityAttribute.empty() ? nullptr : src.points().find(r.densityAttribute);
    if (density && density->type() != AttrType::Float) density = nullptr;
    const auto share = density ? density->read<float>() : std::span<const float>();
    const float apart = std::max(r.minDistance, 0.0f);
    if (maxSlope >= 180.0f && !density && apart <= 0.0f) return;
    std::vector<uint8_t> keep(n, 1);
    parallelFor(n, 4096, [&](size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i) {
            const Triangle& t = tris[which[i]];
            if (maxSlope < 180.0f) {
                const Vec3 up = normalize(cross(P[t.b] - P[t.a], P[t.c] - P[t.a]));
                if (up.y < lowest - 1e-6f) keep[i] = 0;
            }
            if (keep[i] && density) {
                const Vec3& w = weights[i];
                if (unitHash(i, r.seed, 3) >= share[t.a] * w.x + share[t.b] * w.y + share[t.c] * w.z) keep[i] = 0;
            }
        }
    });
    if (apart > 0.0f) {
        // One after the other: none nearer than `apart` to one kept before
        // it -- in cells as wide, the neighbours looked up.
        std::unordered_map<uint64_t, std::vector<Vec3>> cells;
        auto cellOf = [&](const Vec3& p, int dx, int dy, int dz) {
            const int64_t x = static_cast<int64_t>(std::floor(p.x / apart)) + dx;
            const int64_t y = static_cast<int64_t>(std::floor(p.y / apart)) + dy;
            const int64_t z = static_cast<int64_t>(std::floor(p.z / apart)) + dz;
            return static_cast<uint64_t>(x) * 73856093ull ^ static_cast<uint64_t>(y) * 19349663ull ^
                   static_cast<uint64_t>(z) * 83492791ull;
        };
        for (size_t i = 0; i < n; ++i) {
            if (!keep[i]) continue;
            const Triangle& t = tris[which[i]];
            const Vec3& w = weights[i];
            const Vec3 p = P[t.a] * w.x + P[t.b] * w.y + P[t.c] * w.z;
            bool near = false;
            for (int dx = -1; dx <= 1 && !near; ++dx) {
                for (int dy = -1; dy <= 1 && !near; ++dy) {
                    for (int dz = -1; dz <= 1 && !near; ++dz) {
                        const auto it = cells.find(cellOf(p, dx, dy, dz));
                        if (it == cells.end()) continue;
                        for (const Vec3& q : it->second) {
                            if (length(q - p) < apart) {
                                near = true;
                                break;
                            }
                        }
                    }
                }
            }
            if (near) keep[i] = 0;
            else cells[cellOf(p, 0, 0, 0)].push_back(p);
        }
    }
    size_t kept = 0;
    for (size_t i = 0; i < n; ++i) {
        if (!keep[i]) continue;
        which[kept] = which[i];
        weights[kept] = weights[i];
        ++kept;
    }
    which.resize(kept);
    weights.resize(kept);
}

/// Points over the surface of the polygons, as many to a square metre
/// everywhere, by Scatter's rules (scatterPoints).
class ScatterNode : public Node {
public:
    explicit ScatterNode(std::string name) : Node("scatter", std::move(name)) {
        setInputCount(1);
        params_.setInt("mode", 0);  // 0 count, 1 density
        params_.setInt("count", 1000);
        params_.setFloat("density", 10.0f);
        params_.setInt("seed", 0);
        params_.setString("densityattribute", "");
        params_.setFloat("maxslope", 180.0f);
        params_.setFloat("mindistance", 0.0f);
    }

    GeometryPtr cookNode(const CookContext& ctx, std::span<const GeometryPtr> in) override {
        const GeometryPtr src = in.empty() ? nullptr : in[0];
        if (!src) return std::make_shared<Geometry>();
        ScatterRules r;
        r.count = static_cast<size_t>(std::clamp(params_.evalInt("count", ctx, 1000), 0, 10000000));
        if (params_.evalInt("mode", ctx, 0) == 1) r.density = std::max(params_.evalFloat("density", ctx, 10.0f), 0.0f);
        r.seed = static_cast<uint32_t>(params_.evalInt("seed", ctx, 0));
        r.densityAttribute = params_.getString("densityattribute", "");
        r.maxSlope = params_.evalFloat("maxslope", ctx, 180.0f);
        r.minDistance = params_.evalFloat("mindistance", ctx, 0.0f);
        return scatterPoints(*src, r);
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

/// A copy of the first input on every point of the second: moved there,
/// sized by the point's `pscale` (times `scale`), turned by the point's
/// `orient` -- a unit quaternion x, y, z, w -- where it has one, else its +y
/// turned to the point's normal when `align` is on. The points' other
/// attributes go onto their copy's points (Instances.h: copiesOnPoints).
///
///   pieceattribute  names a primitive attribute of the first input: each
///                   of its values a piece -- the primitives of it and
///                   their points -- and each point gets the piece of its
///                   own value of an attribute of that name; a point
///                   without one, piece `number % pieces`. Variants: eight
///                   clumps of grass, one of them on each point.
///   instance        the points themselves, each standing for its piece
///                   (its `instance`), the pieces held once as prototypes
///                   -- what a forest of a few trees needs.
class CopyToPointsNode : public Node {
public:
    explicit CopyToPointsNode(std::string name) : Node("copytopoints", std::move(name)) {
        setInputCount(2);
        params_.setFloat("scale", 1.0f);
        params_.setBool("align", true);
        params_.setBool("instance", false);
        params_.setString("pieceattribute", "");
    }

    GeometryPtr cookNode(const CookContext& ctx, std::span<const GeometryPtr> in) override {
        const GeometryPtr tpl = in.size() > 0 ? in[0] : nullptr;
        const GeometryPtr pts = in.size() > 1 ? in[1] : nullptr;
        if (!tpl || !pts || pts->pointCount() == 0) return std::make_shared<Geometry>();
        const float scale = params_.evalFloat("scale", ctx, 1.0f);
        const bool align = params_.evalBool("align", ctx, true);
        const bool instance = params_.evalBool("instance", ctx, false);

        // The pieces, and which each point gets.
        std::vector<GeometryPtr> pieces;
        std::vector<uint32_t> pick(pts->pointCount(), 0);
        splitPieces(tpl, *pts, params_.getString("pieceattribute", ""), pieces, pick);

        if (instance) {
            // The points, each standing for its piece.
            auto out = editableCopy(pts);
            out->clearPrototypes();
            out->points().erase("instance");
            for (const GeometryPtr& piece : pieces) out->addPrototype(piece);
            auto k = out->points().create("instance", AttrType::Int).write<int32_t>();
            for (size_t p = 0; p < pick.size(); ++p) k[p] = static_cast<int32_t>(pick[p]);
            const bool hasOrient = out->points().find("orient") && out->points().find("orient")->type() == AttrType::Vec4;
            if (!align && !hasOrient) {
                // Not turned to N: as the piece stands.
                auto o = out->points().create("orient", AttrType::Vec4).write<Vec4>();
                std::fill(o.begin(), o.end(), Vec4(0.0f, 0.0f, 0.0f, 1.0f));
            }
            if (scale != 1.0f) {
                AttributeArray* ps = out->points().find("pscale");
                const bool had = ps && ps->type() == AttrType::Float;
                auto S = out->points().create("pscale", AttrType::Float).write<float>();
                for (float& v : S) v = had ? v * scale : scale;
            }
            return out;
        }
        // Copies of each piece on its points, the pieces in turn.
        std::vector<std::vector<uint32_t>> which(pieces.size());
        for (size_t p = 0; p < pick.size(); ++p) which[pick[p]].push_back(static_cast<uint32_t>(p));
        std::shared_ptr<Geometry> out;
        for (size_t k = 0; k < pieces.size(); ++k) {
            if (which[k].empty()) continue;
            auto copies = copiesOnPoints(*pieces[k], *pts, which[k], scale, align);
            if (!out) out = copies;
            else out->append(*copies);
        }
        return out ? GeometryPtr(out) : std::make_shared<Geometry>();
    }

private:
    /// `tpl` in pieces by the primitive attribute `name` -- in the order their
    /// values first come -- and for each point of `pts` its piece; the whole
    /// of `tpl` one piece without it.
    static void splitPieces(const GeometryPtr& tpl, const Geometry& pts, const std::string& name,
                            std::vector<GeometryPtr>& pieces, std::vector<uint32_t>& pick) {
        const AttributeArray* attr = name.empty() ? nullptr : tpl->primitives().find(name);
        if (!attr || (attr->type() != AttrType::Int && attr->type() != AttrType::String) || tpl->primitiveCount() == 0) {
            pieces = {tpl};
            std::fill(pick.begin(), pick.end(), 0u);
            return;
        }
        auto key = [](const AttributeArray& a, size_t i) {
            const int32_t v = a.read<int32_t>()[i];
            return a.type() == AttrType::String ? a.stringValue(v) : std::to_string(v);
        };
        std::unordered_map<std::string, uint32_t> keys;  // each value's piece, in the order they first come
        std::vector<uint32_t> of(tpl->primitiveCount());
        for (size_t prim = 0; prim < of.size(); ++prim) {
            of[prim] = keys.emplace(key(*attr, prim), static_cast<uint32_t>(keys.size())).first->second;
        }
        pieces.clear();
        for (uint32_t k = 0; k < keys.size(); ++k) {
            std::vector<uint8_t> keep(of.size());
            for (size_t prim = 0; prim < of.size(); ++prim) keep[prim] = of[prim] == k ? 1 : 0;
            auto piece = std::make_shared<Geometry>(*tpl);
            piece->deletePrimitives(keep, true);
            pieces.push_back(piece);
        }
        const AttributeArray* own = pts.points().find(name);
        if (own && own->type() != AttrType::Int && own->type() != AttrType::String) own = nullptr;
        for (size_t p = 0; p < pick.size(); ++p) {
            const auto it = own ? keys.find(key(*own, p)) : keys.end();
            pick[p] = it != keys.end() ? it->second : static_cast<uint32_t>(p % keys.size());
        }
    }
};

/// What its points stand for made copies of (Instances.h): geometry that
/// every node can change -- a clump of grass made of its blades.
class UnpackNode : public Node {
public:
    explicit UnpackNode(std::string name) : Node("unpack", std::move(name)) { setInputCount(1); }

    GeometryPtr cookNode(const CookContext&, std::span<const GeometryPtr> in) override {
        if (in.empty() || !in[0]) return std::make_shared<Geometry>();
        if (in[0]->prototypeCount() == 0) return in[0];
        return unpackInstances(*in[0]);
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

/// What the primitives of a group -- all of them without one -- are made
/// of, for a renderer: the string attribute `material`, one of the names of
/// kMaterialNames; None takes it away.
class MaterialNode : public Node {
public:
    explicit MaterialNode(std::string name) : Node("material", std::move(name)) {
        setInputCount(1);
        params_.setString("group", "");
        params_.setInt("material", static_cast<int>(MaterialPreset::Concrete));
        params_.setString("texture", "");
        params_.setFloat("texture_size", 0.0f);
        params_.setBool("texture_tint", false);
        params_.setInt("texture_projection", 0);  // auto, uv, three sides
        params_.setFloat("texture_normal", 1.0f);
    }

    GeometryPtr cookNode(const CookContext& ctx, std::span<const GeometryPtr> in) override {
        auto geo = editableCopy(in.empty() ? nullptr : in[0]);
        const std::string group = params_.getString("group");
        error_.clear();
        bool named = true;
        const std::vector<uint8_t> picked =
            group.empty() ? std::vector<uint8_t>() : selectElements(*geo, AttrClass::Primitive, group, &named);
        if (!named) {
            error_ = "no primitive group '" + group + "'";
            return geo;
        }
        const int which = std::clamp(params_.evalInt("material", ctx, 1), 0, static_cast<int>(kMaterialPresets) - 1);
        // An empty pattern picks every face; one that picks none, none.
        if (!group.empty() && std::none_of(picked.begin(), picked.end(), [](uint8_t c) { return c != 0; })) return geo;
        if (geo->primitiveCount() == 0) return geo;
        setPrimitiveString(*geo, "material", std::string(kMaterialNames[static_cast<size_t>(which)]), picked);
        // How the pictures are laid on (i@texture_projection: 0 auto, 1 by
        // uv, 2 from three sides) and how strongly a normal map bends the
        // light (f@texture_normal, 1 where none is said): where not as by
        // default, or where an earlier one said otherwise.
        const int32_t projection = std::clamp(params_.evalInt("texture_projection", ctx, 0), 0, 2);
        if (projection != 0 || geo->primitives().find("texture_projection")) {
            auto how = geo->primitives().create("texture_projection", AttrType::Int).write<int32_t>();
            for (size_t p = 0; p < how.size(); ++p) {
                if (picked.empty() || picked[p]) how[p] = projection;
            }
        }
        const float strength = std::max(params_.evalFloat("texture_normal", ctx, 1.0f), 0.0f);
        if (strength != 1.0f || geo->primitives().find("texture_normal")) {
            const bool fresh = geo->primitives().find("texture_normal") == nullptr;
            auto bends = geo->primitives().create("texture_normal", AttrType::Float).write<float>();
            for (size_t p = 0; p < bends.size(); ++p) {
                if (picked.empty() || picked[p]) bends[p] = strength;
                else if (fresh) bends[p] = 1.0f;
            }
        }
        // A texture of one's own (render/Textures.h): s@texture -- "" where
        // the material's own -- f@texture_size, i@texture_tint.
        const std::string texture = params_.getString("texture", "");
        const bool has = geo->primitives().find("texture") != nullptr;
        if (texture.empty() && !has) return geo;
        setPrimitiveString(*geo, "texture", texture, picked);
        const float size = std::max(params_.evalFloat("texture_size", ctx, 0.0f), 0.0f);
        const int32_t tint = params_.evalBool("texture_tint", ctx, false) ? 1 : 0;
        auto sizes = geo->primitives().create("texture_size", AttrType::Float).write<float>();
        auto tints = geo->primitives().create("texture_tint", AttrType::Int).write<int32_t>();
        for (size_t p = 0; p < sizes.size(); ++p) {
            if (!picked.empty() && !picked[p]) continue;
            sizes[p] = size;
            tints[p] = tint;
        }
        return geo;
    }
    std::string cookError() const override { return error_; }

private:
    std::string error_;
};

/// Texture coordinates on the corners of the faces of a group -- all of them
/// without one: the vertex attribute uv, (u, v, 0) as Houdini keeps it --
/// what a picture is laid on by where the Material node's Projection says
/// UV. Projected from `center`, `scale` metres one picture:
///   planar       along `axis`, as seen from its + side, the picture upright
///                (from above, x across and -z up);
///   box          each face along the axis it faces most, as seen from
///                outside -- a box's six sides each a picture, none mirrored;
///   cylindrical  round `axis`: u once round, from behind and to the right as
///                seen from its front (+z for y), v along it;
///   spherical    round `axis` as the cylinder, v from the lower pole (0) to
///                the upper (1).
/// Round ones keep each face whole: a face across the seam has its u past 1
/// on that side, and a corner on a pole the u of the rest of its face.
class UvProjectNode : public Node {
public:
    explicit UvProjectNode(std::string name) : Node("uvproject", std::move(name)) {
        setInputCount(1);
        params_.setString("group", "");
        params_.setInt("projection", 0);  // planar, box, cylindrical, spherical
        params_.setInt("axis", 1);        // x, y, z
        params_.setVec3("center", Vec3(0.0f));
        params_.setFloat("scale", 1.0f);
    }

    GeometryPtr cookNode(const CookContext& ctx, std::span<const GeometryPtr> in) override {
        auto geo = editableCopy(in.empty() ? nullptr : in[0]);
        error_.clear();
        const std::string group = params_.getString("group");
        bool named = true;
        const std::vector<uint8_t> picked =
            group.empty() ? std::vector<uint8_t>() : selectElements(*geo, AttrClass::Primitive, group, &named);
        if (!named) {
            error_ = "no primitive group '" + group + "'";
            return geo;
        }
        if (geo->primitiveCount() == 0) return geo;
        const int projection = std::clamp(params_.evalInt("projection", ctx, 0), 0, 3);
        const int axis = std::clamp(params_.evalInt("axis", ctx, 1), 0, 2);
        const Vec3 center = params_.evalVec3("center", ctx, Vec3(0.0f));
        const float scale = std::max(params_.evalFloat("scale", ctx, 1.0f), 1e-6f);

        // The corners' uv as they were -- a two-part one taken on -- the
        // faces outside the group keeping theirs.
        std::vector<Vec3> was;
        if (const AttributeArray* old = geo->vertices().find("uv")) {
            if (old->type() == AttrType::Vec2) {
                for (const Vec2& t : old->read<Vec2>()) was.emplace_back(t.x, t.y, 0.0f);
            } else if (old->type() == AttrType::Vec3) {
                was.assign(old->read<Vec3>().begin(), old->read<Vec3>().end());
            }
        }
        auto uv = geo->vertices().create("uv", AttrType::Vec3).write<Vec3>();
        if (was.size() == uv.size()) std::copy(was.begin(), was.end(), uv.begin());

        // The two axes across `along`, seen from its + side, upright: x
        // across and y up from +z; -z across from +x; x across and -z up
        // from above -- and from the - side, across the other way.
        auto across = [](int along, float side, const Vec3& d) {
            switch (along) {
                case 0: return Vec2(-d.z * side, d.y);
                case 1: return Vec2(d.x, -d.z * side);
                default: return Vec2(d.x * side, d.y);
            }
        };
        const auto P = geo->positions();
        // Round the axis: the share of a turn from behind, through the front
        // (a half) to the right -- the front and the right of y +z and +x, of
        // z -y and +x, of x +y and +z -- and the distance from it.
        const Vec3 rights[3] = {Vec3(0.0f, 0.0f, 1.0f), Vec3(1.0f, 0.0f, 0.0f), Vec3(1.0f, 0.0f, 0.0f)};
        const Vec3 fronts[3] = {Vec3(0.0f, 1.0f, 0.0f), Vec3(0.0f, 0.0f, 1.0f), Vec3(0.0f, -1.0f, 0.0f)};
        auto round = [&](const Vec3& d, float& turn, float& off) {
            const float right = dot(d, rights[axis]), front = dot(d, fronts[axis]);
            off = std::sqrt(front * front + right * right);
            turn = std::atan2(right, front) / (2.0f * 3.14159265f) + 0.5f;
        };
        for (size_t prim = 0; prim < geo->primitiveCount(); ++prim) {
            if (!picked.empty() && !picked[prim]) continue;
            const auto pts = geo->primitivePoints(prim);
            const size_t first = geo->primitiveVertexStart(prim);
            if (pts.empty()) continue;
            std::vector<Vec3> corner(pts.size());
            if (projection <= 1) {
                // Planar, or box: along the axis the face faces most.
                int along = axis;
                float side = 1.0f;
                if (projection == 1) {
                    const Vec3 n = polygonNormal(*geo, pts);
                    along = std::fabs(n.x) >= std::fabs(n.y) && std::fabs(n.x) >= std::fabs(n.z) ? 0
                            : std::fabs(n.y) >= std::fabs(n.z)                                  ? 1
                                                                                                : 2;
                    side = n[along] < 0.0f ? -1.0f : 1.0f;
                }
                for (size_t k = 0; k < pts.size(); ++k) {
                    const Vec2 t = across(along, side, (P[pts[k]] - center) / scale);
                    corner[k] = Vec3(t.x, t.y, 0.0f);
                }
            } else {
                // Round: once round in u, the face kept whole across the seam
                // and over a pole.
                std::vector<float> turn(pts.size()), off(pts.size());
                float most = 0.0f;
                for (size_t k = 0; k < pts.size(); ++k) {
                    round(P[pts[k]] - center, turn[k], off[k]);
                    most = std::max(most, off[k]);
                }
                const float lo = *std::min_element(turn.begin(), turn.end());
                const float hi = *std::max_element(turn.begin(), turn.end());
                if (hi - lo > 0.5f) {
                    for (float& t : turn) {
                        if (t < 0.5f) t += 1.0f;
                    }
                }
                // A corner on the axis: the u of the rest of its face.
                const auto onAxis = [&](size_t k) { return off[k] <= 1e-5f * std::max(most, 1e-6f); };
                float sum = 0.0f;
                int count = 0;
                for (size_t k = 0; k < pts.size(); ++k) {
                    if (onAxis(k)) continue;
                    sum += turn[k];
                    ++count;
                }
                for (size_t k = 0; k < pts.size(); ++k) {
                    if (onAxis(k) && count > 0) turn[k] = sum / static_cast<float>(count);
                    const Vec3 d = P[pts[k]] - center;
                    float v = d[axis] / scale;
                    if (projection == 3) {
                        const float r = length(d);
                        v = r > 0.0f ? std::asin(std::clamp(d[axis] / r, -1.0f, 1.0f)) / 3.14159265f + 0.5f : 0.5f;
                    }
                    corner[k] = Vec3(turn[k], v, 0.0f);
                }
            }
            for (size_t k = 0; k < pts.size() && first + k < uv.size(); ++k) uv[first + k] = corner[k];
        }
        return geo;
    }
    std::string cookError() const override { return error_; }

private:
    std::string error_;
};

}  // namespace

std::shared_ptr<Geometry> scatterPoints(const Geometry& src, const ScatterRules& r) {
    auto out = std::make_shared<Geometry>();
    const std::vector<Triangle> tris = fans(src);
    const auto P = src.positions();
    // Where each triangle's share of the area ends: a point picks its
    // triangle by where a number in [0, total) falls.
    std::vector<double> ends(tris.size());
    double total = 0.0;
    for (size_t t = 0; t < tris.size(); ++t) {
        total += 0.5 * static_cast<double>(length(cross(P[tris[t].b] - P[tris[t].a], P[tris[t].c] - P[tris[t].a])));
        ends[t] = total;
    }
    // How many to try: the count, or the density to a square metre of the area.
    const double wanted = r.density >= 0.0 ? std::min(total * r.density, 1e7) : static_cast<double>(r.count);
    size_t n = tris.empty() || total <= 0.0 ? 0 : static_cast<size_t>(std::llround(wanted));
    if (n == 0) return out;
    const uint32_t seed = r.seed;

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
    thinScattered(src, tris, r, which, weights);
    n = which.size();
    if (n == 0) return out;

    out->addPoints(n);
    // The point attributes of the corners, blended.
    for (const std::string& name : src.points().names()) {
        const AttributeArray& from = *src.points().find(name);
        AttributeArray& to = out->points().create(name, from.type());
        switch (from.type()) {
            case AttrType::Float: blendCorners<float>(from, to, tris, which, weights); break;
            case AttrType::Vec2: blendCorners<Vec2>(from, to, tris, which, weights); break;
            case AttrType::Vec3: blendCorners<Vec3>(from, to, tris, which, weights); break;
            case AttrType::Vec4: blendCorners<Vec4>(from, to, tris, which, weights); break;
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
    if (!src.points().contains("N")) {
        auto N = out->points().create("N", AttrType::Vec3).write<Vec3>();
        for (size_t i = 0; i < n; ++i) {
            const Triangle& t = tris[which[i]];
            N[i] = normalize(cross(P[t.b] - P[t.a], P[t.c] - P[t.a]));
        }
    }
    return out;
}

void registerSurfaceNodes() {
    auto& r = NodeRegistry::instance();
    r.add("file", [](const std::string& n) { return std::make_unique<FileNode>(n); });
    r.add("scatter", [](const std::string& n) { return std::make_unique<ScatterNode>(n); });
    r.add("normal", [](const std::string& n) { return std::make_unique<NormalNode>(n); });
    r.add("copytopoints", [](const std::string& n) { return std::make_unique<CopyToPointsNode>(n); });
    r.add("unpack", [](const std::string& n) { return std::make_unique<UnpackNode>(n); });
    r.add("color", [](const std::string& n) { return std::make_unique<ColorNode>(n); });
    r.add("material", [](const std::string& n) { return std::make_unique<MaterialNode>(n); });
    r.add("uvproject", [](const std::string& n) { return std::make_unique<UvProjectNode>(n); });
}

}  // namespace pg
