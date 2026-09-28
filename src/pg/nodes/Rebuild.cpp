// The blending of new elements from old ones (Rebuild.h).
#include "pg/nodes/Rebuild.h"

namespace pg {

int floatsOf(AttrType t) {
    switch (t) {
        case AttrType::Float: return 1;
        case AttrType::Vec2: return 2;
        case AttrType::Vec3: return 3;
        case AttrType::Vec4: return 4;
        default: return 0;
    }
}

/// The attributes of `from` for elements blended from its own.
AttributeSet blended(const AttributeSet& from, const Blends& b) {
    AttributeSet out;
    out.setElementCount(b.size());
    std::vector<uint32_t> pick(b.size(), 0);
    for (size_t e = 0; e < b.size(); ++e) pick[e] = static_cast<uint32_t>(std::max<int64_t>(b.heaviest(e), 0));
    for (const std::string& name : from.names()) {
        const AttributeArray& a = *from.find(name);
        const int k = floatsOf(a.type());
        if (k == 0) {
            // Integers and strings: the heaviest term's, its string table shared.
            if (a.size() == 0) out.create(name, a.type());
            else out.assign(name, a.gather(pick));
            continue;
        }
        AttributeArray& o = out.create(name, a.type());
        const float* src = reinterpret_cast<const float*>(a.rawRead());
        float* dst = reinterpret_cast<float*>(o.rawWrite());
        for (size_t e = 0; e < b.size(); ++e) {
            float acc[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            for (uint32_t t = b.start[e]; t < b.start[e + 1]; ++t) {
                if (b.index[t] >= a.size()) continue;
                const float* v = src + static_cast<size_t>(b.index[t]) * static_cast<size_t>(k);
                for (int c = 0; c < k; ++c) acc[c] += b.weight[t] * v[c];
            }
            for (int c = 0; c < k; ++c) dst[e * static_cast<size_t>(k) + static_cast<size_t>(c)] = acc[c];
        }
    }
    return out;
}

/// The attributes of `from` gathered: element e is from's `source[e]`.
AttributeSet gathered(const AttributeSet& from, std::span<const uint32_t> source) {
    AttributeSet out = from;
    if (from.elementCount() == 0) {
        out.setElementCount(source.size());
        return out;
    }
    out.gather(source);
    return out;
}

/// The groups of `src` onto `dst`, whose points are `points` blended from
/// src's (members where every term is one) and whose primitives are
/// `prims` gathered from src's.
void carryGroups(const Geometry& src, Geometry& dst, const Blends& points, std::span<const uint32_t> prims) {
    for (const std::string& name : src.groupNames()) {
        const Group* g = src.findGroup(name);
        if (g->classOf() == AttrClass::Point) {
            Group& o = dst.createGroup(name, AttrClass::Point);
            for (size_t e = 0; e < points.size(); ++e) {
                bool all = points.start[e + 1] > points.start[e];
                for (uint32_t t = points.start[e]; t < points.start[e + 1] && all; ++t) all = g->contains(points.index[t]);
                if (all) o.set(e, true);
            }
        } else if (g->classOf() == AttrClass::Primitive) {
            Group& o = dst.createGroup(name, AttrClass::Primitive);
            for (size_t e = 0; e < prims.size(); ++e) {
                if (g->contains(prims[e])) o.set(e, true);
            }
        }
    }
}

/// A geometry of `pointCount` points and the primitives `faces` (point
/// lists, `closed`), its attributes from `src`: points blended, vertices
/// blended, primitives gathered by `sourcePrim`; groups carried; detail and
/// volumes kept.
std::shared_ptr<Geometry> rebuild(const Geometry& src, const Blends& points, const std::vector<std::vector<uint32_t>>& faces,
                                  const std::vector<uint8_t>& closed, const Blends& vertices,
                                  const std::vector<uint32_t>& sourcePrim) {
    auto out = std::make_shared<Geometry>();
    out->addPoints(points.size());
    for (size_t f = 0; f < faces.size(); ++f) out->addPrimitive(faces[f], closed[f] != 0);
    out->points() = blended(src.points(), points);
    out->vertices() = blended(src.vertices(), vertices);
    out->primitives() = gathered(src.primitives(), sourcePrim);
    out->detail() = src.detail();
    carryGroups(src, *out, points, sourcePrim);
    for (const Volume& v : src.volumes()) out->addVolume(v);
    return out;
}

uint64_t edgeKey(uint32_t a, uint32_t b) {
    if (a > b) std::swap(a, b);
    return (static_cast<uint64_t>(a) << 32) | b;
}

}  // namespace pg
