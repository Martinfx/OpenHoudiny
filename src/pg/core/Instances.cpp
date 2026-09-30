#include "pg/core/Instances.h"

#include "pg/core/Parallel.h"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <utility>

namespace pg {

Vec4 quatMultiply(const Vec4& a, const Vec4& b) { return vec4Of(quatOf(a) * quatOf(b)); }

Vec3 quatRotate(const Vec4& q, const Vec3& v) { return quatOf(q) * v; }

Vec4 quatFromAxes(const Vec3& X, const Vec3& Y, const Vec3& Z) { return vec4Of(glm::quat_cast(Mat3(X, Y, Z))); }

Vec4 quatUpTo(const Vec3& n) {
    const float c = n.y;  // dot(+y, n)
    if (c > 0.999999f) return Vec4(0.0f, 0.0f, 0.0f, 1.0f);
    if (c < -0.999999f) return Vec4(1.0f, 0.0f, 0.0f, 0.0f);  // half a turn about x
    const Vec3 axis = normalize(cross(Vec3(0.0f, 1.0f, 0.0f), n));
    const float s = std::sqrt(std::max(0.0f, 0.5f * (1.0f - c))), w = std::sqrt(std::max(0.0f, 0.5f * (1.0f + c)));
    return Vec4(axis.x * s, axis.y * s, axis.z * s, w);
}

Vec3 turnUpTo(const Vec3& n, const Vec3& v) { return quatRotate(quatUpTo(n), v); }

namespace {

const AttributeArray* instanceAttribute(const Geometry& geo) {
    if (geo.prototypeCount() == 0) return nullptr;
    const AttributeArray* a = geo.points().find("instance");
    return a && a->type() == AttrType::Int ? a : nullptr;
}

const AttributeArray* ofType(const AttributeSet& set, const char* name, AttrType type) {
    const AttributeArray* a = set.find(name);
    return a && a->type() == type ? a : nullptr;
}


}  // namespace

int32_t instanceOf(const Geometry& geo, size_t p) {
    const AttributeArray* a = instanceAttribute(geo);
    if (!a || p >= a->size()) return -1;
    const int32_t k = a->read<int32_t>()[p];
    return k >= 0 && static_cast<size_t>(k) < geo.prototypeCount() ? k : -1;
}

std::vector<std::vector<uint32_t>> instancesByPrototype(const Geometry& geo) {
    std::vector<std::vector<uint32_t>> out(geo.prototypeCount());
    if (const AttributeArray* a = instanceAttribute(geo)) {
        const auto k = a->read<int32_t>();
        for (size_t p = 0; p < k.size(); ++p) {
            if (k[p] >= 0 && static_cast<size_t>(k[p]) < out.size()) out[static_cast<size_t>(k[p])].push_back(static_cast<uint32_t>(p));
        }
    }
    return out;
}

size_t instanceCount(const Geometry& geo) {
    size_t n = 0;
    for (const auto& points : instancesByPrototype(geo)) n += points.size();
    return n;
}

std::vector<Placement> placementsOf(const Geometry& geo, float scale, bool align) {
    std::vector<Placement> out(geo.pointCount());
    const auto P = geo.positions();
    const AttributeArray* orient = ofType(geo.points(), "orient", AttrType::Vec4);
    const AttributeArray* N = align && !orient ? ofType(geo.points(), "N", AttrType::Vec3) : nullptr;
    const AttributeArray* pscale = ofType(geo.points(), "pscale", AttrType::Float);
    parallelFor(out.size(), 4096, [&](size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i) {
            Placement& p = out[i];
            p.at = P[i];
            p.scale = scale * (pscale ? pscale->read<float>()[i] : 1.0f);
            if (orient) {
                p.orient = unitQuat(orient->read<Vec4>()[i]);
            } else if (N) {
                const Vec3 n = normalize(N->read<Vec3>()[i]);
                if (length(n) > 0.5f) p.orient = quatUpTo(n);
            }
        }
    });
    return out;
}

std::shared_ptr<Geometry> copiesOnPoints(const Geometry& tplIn, const Geometry& pts, std::span<const uint32_t> which,
                                         float scale, bool align, bool colours) {
    auto out = std::make_shared<Geometry>();
    // What stands on the template's own points is made first.
    std::shared_ptr<const Geometry> unpacked;
    if (tplIn.prototypeCount() > 0) unpacked = unpackInstances(tplIn);
    const Geometry& tpl = unpacked ? *unpacked : tplIn;
    const size_t copies = which.size(), tp = tpl.pointCount(), tv = tpl.vertexCount(), tprims = tpl.primitiveCount();
    if (copies == 0 || tp == 0) return out;

    // The corners first, the attributes after: each copy's primitives over
    // its own points.
    out->addPoints(copies * tp);
    {
        const auto vertexPoint = tpl.vertexPoints();
        const auto sizes = tpl.primitiveSizes();
        const auto closedFlags = tpl.primitiveClosedFlags();
        std::vector<uint32_t> corners(copies * tv), counts(copies * tprims);
        std::vector<uint8_t> closed(copies * tprims);
        parallelFor(copies, 64, [&](size_t begin, size_t end) {
            for (size_t c = begin; c < end; ++c) {
                const uint32_t offset = static_cast<uint32_t>(c * tp);
                for (size_t v = 0; v < tv; ++v) corners[c * tv + v] = vertexPoint[v] + offset;
                std::copy(sizes.begin(), sizes.end(), counts.begin() + static_cast<std::ptrdiff_t>(c * tprims));
                std::copy(closedFlags.begin(), closedFlags.end(), closed.begin() + static_cast<std::ptrdiff_t>(c * tprims));
            }
        });
        if (tprims > 0) out->addPrimitives(corners, counts, closed);
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
    for (const std::string& name : tpl.points().names()) out->points().assign(name, tpl.points().find(name)->gather(byPoint));
    for (const std::string& name : tpl.vertices().names()) out->vertices().assign(name, tpl.vertices().find(name)->gather(byVertex));
    for (const std::string& name : tpl.primitives().names()) {
        out->primitives().assign(name, tpl.primitives().find(name)->gather(byPrim));
    }
    for (const std::string& name : tpl.detail().names()) out->detail().assign(name, *tpl.detail().find(name));
    // Its groups, in every copy.
    for (const std::string& name : tpl.groupNames()) {
        const Group* g = tpl.findGroup(name);
        const size_t count = tpl.elementCount(g->classOf());
        const auto mask = g->mask();
        uint8_t* to = out->createGroup(name, g->classOf()).writableMask();
        if (g->classOf() == AttrClass::Detail) {
            if (!mask.empty()) to[0] = mask[0];
            continue;
        }
        for (size_t c = 0; c < copies; ++c) std::copy(mask.begin(), mask.end(), to + c * count);
    }

    // The points' attributes onto their copies: their Cd in place of the
    // copies' colours.
    std::vector<uint32_t> owner(copies * tp);
    for (size_t c = 0; c < copies; ++c) {
        std::fill(owner.begin() + static_cast<std::ptrdiff_t>(c * tp), owner.begin() + static_cast<std::ptrdiff_t>((c + 1) * tp),
                  which[c]);
    }
    for (const std::string& name : pts.points().names()) {
        if (name == "P" || name == "N" || name == "pscale" || name == "orient" || name == "instance" || name == "tint") continue;
        if (name == "Cd" && !colours) continue;
        out->points().assign(name, pts.points().find(name)->gather(owner));
    }
    if (colours && ofType(pts.points(), "Cd", AttrType::Vec3)) {
        out->vertices().erase("Cd");
        out->primitives().erase("Cd");
        out->detail().erase("Cd");
    }
    // Their tint times the copies' colours -- wherever those are.
    if (const AttributeArray* tintAttr = ofType(pts.points(), "tint", AttrType::Vec3)) {
        const auto tint = tintAttr->read<Vec3>();
        auto times = [&](AttributeArray* cd, size_t count) {
            auto C = cd->write<Vec3>();
            parallelFor(copies, 64, [&](size_t begin, size_t end) {
                for (size_t c = begin; c < end; ++c) {
                    const Vec3 t = tint[which[c]];
                    for (size_t i = c * count; i < (c + 1) * count; ++i) C[i] = C[i] * t;
                }
            });
        };
        AttributeArray* cd = nullptr;
        if ((cd = out->points().find("Cd")) && cd->type() == AttrType::Vec3) {
            times(cd, tp);
        } else if ((cd = out->vertices().find("Cd")) && cd->type() == AttrType::Vec3) {
            times(cd, tv);
        } else if ((cd = out->primitives().find("Cd")) && cd->type() == AttrType::Vec3) {
            times(cd, tprims);
        } else {
            // A colour of the whole, or the viewport's grey: onto the points.
            Vec3 base(0.72f, 0.72f, 0.74f);
            if (const AttributeArray* d = ofType(out->detail(), "Cd", AttrType::Vec3)) base = d->read<Vec3>()[0];
            out->detail().erase("Cd");
            auto C = out->points().create("Cd", AttrType::Vec3).write<Vec3>();
            for (size_t c = 0; c < copies; ++c) {
                std::fill(C.begin() + static_cast<std::ptrdiff_t>(c * tp), C.begin() + static_cast<std::ptrdiff_t>((c + 1) * tp),
                          base * tint[which[c]]);
            }
        }
    }

    // Moved, sized and turned.
    const auto at = pts.positions();
    const AttributeArray* pscaleAttr = ofType(pts.points(), "pscale", AttrType::Float);
    const AttributeArray* orientAttr = ofType(pts.points(), "orient", AttrType::Vec4);
    const AttributeArray* normalAttr = !orientAttr && align ? ofType(pts.points(), "N", AttrType::Vec3) : nullptr;
    AttributeArray* nOut = out->points().find("N");
    const bool turnNormals = nOut && nOut->type() == AttrType::Vec3;
    auto P = out->positionsForWrite();
    std::span<Vec3> N;
    if (turnNormals) N = nOut->write<Vec3>();
    parallelFor(copies, 256, [&](size_t begin, size_t end) {
        for (size_t c = begin; c < end; ++c) {
            const uint32_t q = which[c];
            const float s = scale * (pscaleAttr ? pscaleAttr->read<float>()[q] : 1.0f);
            if (orientAttr) {
                const Vec4 o = unitQuat(orientAttr->read<Vec4>()[q]);
                for (size_t i = c * tp; i < (c + 1) * tp; ++i) {
                    P[i] = at[q] + quatRotate(o, P[i] * s);
                    if (turnNormals) N[i] = quatRotate(o, N[i]);
                }
                continue;
            }
            const Vec3 up = normalAttr ? normalize(normalAttr->read<Vec3>()[q]) : Vec3(0.0f, 1.0f, 0.0f);
            const bool turn = normalAttr && length(up) > 0.5f;
            for (size_t i = c * tp; i < (c + 1) * tp; ++i) {
                const Vec3 local = P[i] * s;
                P[i] = at[q] + (turn ? turnUpTo(up, local) : local);
                if (turnNormals && turn) N[i] = turnUpTo(up, N[i]);
            }
        }
    });
    return out;
}

std::shared_ptr<Geometry> withoutInstances(const Geometry& geo) {
    auto out = std::make_shared<Geometry>(geo);
    if (geo.prototypeCount() == 0) return out;
    std::vector<uint8_t> keep(geo.pointCount(), 1);
    for (const auto& points : instancesByPrototype(geo)) {
        for (const uint32_t p : points) keep[p] = 0;
    }
    out->clearPrototypes();
    out->deletePoints(keep);
    out->points().erase("instance");
    // What only placed the instances -- merged in, zero on every point
    // left -- goes with them: no point is turned by a zero quaternion, sized
    // to nothing, tinted black or faces no way.
    for (const auto& [name, type] : {std::pair<const char*, AttrType>{"orient", AttrType::Vec4},
                                     {"pscale", AttrType::Float}, {"tint", AttrType::Vec3}, {"N", AttrType::Vec3}}) {
        const AttributeArray* a = out->points().find(name);
        if (!a || a->type() != type) continue;
        const std::byte* bytes = a->rawRead();
        if (!bytes || std::all_of(bytes, bytes + a->byteSize(), [](std::byte b) { return b == std::byte{0}; })) {
            out->points().erase(name);
        }
    }
    return out;
}

namespace {

void grow(Vec3& lo, Vec3& hi, const Vec3& p) {
    for (int a = 0; a < 3; ++a) {
        lo[a] = std::min(lo[a], p[a]);
        hi[a] = std::max(hi[a], p[a]);
    }
}

}  // namespace

void instancesBox(const Geometry& geo, Vec3& lo, Vec3& hi) {
    lo = Vec3(1e30f, 1e30f, 1e30f);
    hi = Vec3(-1e30f, -1e30f, -1e30f);
    const auto byPrototype = instancesByPrototype(geo);
    if (byPrototype.empty()) return;
    const std::vector<Placement> places = placementsOf(geo);
    for (size_t k = 0; k < byPrototype.size(); ++k) {
        const GeometryPtr& prototype = geo.prototypes()[k];
        if (byPrototype[k].empty() || !prototype) continue;
        Vec3 plo, phi;
        drawnBox(*prototype, plo, phi);
        if (plo.x > phi.x) continue;
        for (const uint32_t p : byPrototype[k]) {
            for (int c = 0; c < 8; ++c) {
                grow(lo, hi, places[p].point(Vec3(c & 1 ? phi.x : plo.x, c & 2 ? phi.y : plo.y, c & 4 ? phi.z : plo.z)));
            }
        }
    }
}

void drawnBox(const Geometry& geo, Vec3& lo, Vec3& hi) {
    instancesBox(geo, lo, hi);
    std::vector<uint8_t> instance(geo.pointCount(), 0);
    for (const auto& points : instancesByPrototype(geo)) {
        for (const uint32_t p : points) instance[p] = 1;
    }
    const auto P = geo.positions();
    for (size_t p = 0; p < P.size(); ++p) {
        if (!instance[p]) grow(lo, hi, P[p]);
    }
}

std::shared_ptr<Geometry> unpackInstances(const Geometry& geo) {
    if (geo.prototypeCount() == 0) return std::make_shared<Geometry>(geo);
    const auto byPrototype = instancesByPrototype(geo);
    // What is no instance, as it is.
    auto out = withoutInstances(geo);
    // Then each prototype's copies.
    for (size_t k = 0; k < byPrototype.size(); ++k) {
        const GeometryPtr& prototype = geo.prototypes()[k];
        if (byPrototype[k].empty() || !prototype) continue;
        out->append(*copiesOnPoints(*prototype, geo, byPrototype[k], 1.0f, true, false));
    }
    return out;
}

}  // namespace pg
