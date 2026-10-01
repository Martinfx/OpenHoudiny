#include "pg/core/Geometry.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace pg {
namespace {

// FNV-1a. Chosen because it is trivially reproducible across platforms, which
// matters: the hash is the basis of the golden-file regression suite.
constexpr uint64_t kFnvOffset = 1469598103934665603ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;

inline void hashBytes(uint64_t& h, const void* data, size_t n) {
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= kFnvPrime;
    }
}

inline void hashU64(uint64_t& h, uint64_t v) { hashBytes(h, &v, sizeof(v)); }

void hashAttributeSet(uint64_t& h, const AttributeSet& set) {
    hashU64(h, set.elementCount());
    hashU64(h, set.count());
    for (const auto& name : set.names()) {  // sorted: order-stable
        const AttributeArray* a = set.find(name);
        hashBytes(h, name.data(), name.size());
        hashU64(h, static_cast<uint64_t>(a->type()));
        hashU64(h, a->size());
        if (a->type() == AttrType::String) {
            // Hash the strings themselves, not the table-local indices, so the
            // hash is invariant to how the table happens to be ordered.
            auto idx = a->read<int32_t>();
            for (int32_t i : idx) {
                const std::string& s = a->stringValue(i);
                hashBytes(h, s.data(), s.size());
                hashU64(h, s.size());
            }
        } else if (a->size() > 0) {
            hashBytes(h, a->rawRead(), a->byteSize());
        }
    }
}

}  // namespace

// --- Group -----------------------------------------------------------------

Group::Group(AttrClass cls, size_t size) : class_(cls) {
    if (size > 0) mask_ = std::make_shared<std::vector<uint8_t>>(size, 0);
}

std::vector<uint8_t>& Group::maskForWrite() {
    if (!mask_) {
        mask_ = std::make_shared<std::vector<uint8_t>>();
    } else if (mask_.use_count() > 1) {
        mask_ = std::make_shared<std::vector<uint8_t>>(*mask_);
    }
    return *mask_;
}

void Group::set(size_t i, bool member) {
    auto& m = maskForWrite();
    if (i >= m.size()) m.resize(i + 1, 0);
    m[i] = member ? 1 : 0;
}

void Group::resize(size_t n) {
    if (mask_ && mask_->size() == n) return;
    maskForWrite().resize(n, 0);
}

size_t Group::memberCount() const {
    if (!mask_) return 0;
    size_t n = 0;
    for (uint8_t v : *mask_) n += (v != 0);
    return n;
}

// --- Volume ----------------------------------------------------------------

float Volume::at(int i, int j, int k) const {
    if (!values || i < 0 || j < 0 || k < 0 || i >= res[0] || j >= res[1] || k >= res[2]) return 0.0f;
    return (*values)[static_cast<size_t>(i) +
                     static_cast<size_t>(res[0]) * (static_cast<size_t>(j) + static_cast<size_t>(res[1]) * static_cast<size_t>(k))];
}

float Volume::sample(const Vec3& p) const {
    if (!values || voxel <= 0.0f) return 0.0f;
    // In voxel units, from the middle of voxel 0.
    float g[3];
    int c[3];
    float f[3];
    for (int a = 0; a < 3; ++a) {
        g[a] = (p[a] - origin[a]) / voxel - 0.5f;
        if (g[a] < -0.5f || g[a] > static_cast<float>(res[a]) - 0.5f) return 0.0f;
        const float fl = std::floor(g[a]);
        c[a] = static_cast<int>(fl);
        f[a] = g[a] - fl;
    }
    // The nearest voxel stands in for one past the edge.
    auto v = [&](int i, int j, int k) {
        return at(std::clamp(i, 0, res[0] - 1), std::clamp(j, 0, res[1] - 1), std::clamp(k, 0, res[2] - 1));
    };
    const float x00 = v(c[0], c[1], c[2]) + (v(c[0] + 1, c[1], c[2]) - v(c[0], c[1], c[2])) * f[0];
    const float x10 = v(c[0], c[1] + 1, c[2]) + (v(c[0] + 1, c[1] + 1, c[2]) - v(c[0], c[1] + 1, c[2])) * f[0];
    const float x01 = v(c[0], c[1], c[2] + 1) + (v(c[0] + 1, c[1], c[2] + 1) - v(c[0], c[1], c[2] + 1)) * f[0];
    const float x11 = v(c[0], c[1] + 1, c[2] + 1) + (v(c[0] + 1, c[1] + 1, c[2] + 1) - v(c[0], c[1] + 1, c[2] + 1)) * f[0];
    const float y0 = x00 + (x10 - x00) * f[1], y1 = x01 + (x11 - x01) * f[1];
    return y0 + (y1 - y0) * f[2];
}

Volume Volume::make(std::string name, const Vec3& origin, float voxel, int nx, int ny, int nz,
                    std::vector<float> data) {
    Volume v;
    v.name = std::move(name);
    v.origin = origin;
    v.voxel = voxel;
    v.res[0] = std::max(nx, 0);
    v.res[1] = std::max(ny, 0);
    v.res[2] = std::max(nz, 0);
    data.resize(v.count(), 0.0f);
    v.values = std::make_shared<const std::vector<float>>(std::move(data));
    return v;
}

// --- Geometry --------------------------------------------------------------

Geometry::Geometry() : topo_(std::make_shared<Topology>()) {
    // P is mandatory. Every node may assume it exists.
    points_.create("P", AttrType::Vec3);
    // The detail is one element: the geometry itself.
    detail_.setElementCount(1);
}

const std::vector<Volume>& Geometry::volumes() const {
    static const std::vector<Volume> none;
    return volumes_ ? *volumes_ : none;
}

void Geometry::addVolume(Volume volume) {
    if (!volumes_) volumes_ = std::make_shared<std::vector<Volume>>();
    else if (volumes_.use_count() > 1) volumes_ = std::make_shared<std::vector<Volume>>(*volumes_);
    volumes_->push_back(std::move(volume));
}

const std::vector<GeometryPtr>& Geometry::prototypes() const {
    static const std::vector<GeometryPtr> none;
    return prototypes_ ? *prototypes_ : none;
}

size_t Geometry::addPrototype(GeometryPtr prototype) {
    if (!prototypes_) prototypes_ = std::make_shared<std::vector<GeometryPtr>>();
    else if (prototypes_.use_count() > 1) prototypes_ = std::make_shared<std::vector<GeometryPtr>>(*prototypes_);
    prototypes_->push_back(std::move(prototype));
    return prototypes_->size() - 1;
}

const Volume* Geometry::findVolume(const std::string& name) const {
    for (const Volume& v : volumes()) {
        if (v.name == name) return &v;
    }
    return nullptr;
}

Geometry::Topology& Geometry::topologyForWrite() {
    if (topo_.use_count() > 1) topo_ = std::make_shared<Topology>(*topo_);
    return *topo_;
}

AttributeSet& Geometry::attributes(AttrClass c) {
    switch (c) {
        case AttrClass::Detail:    return detail_;
        case AttrClass::Point:     return points_;
        case AttrClass::Vertex:    return vertices_;
        case AttrClass::Primitive: return primitives_;
    }
    return points_;
}

const AttributeSet& Geometry::attributes(AttrClass c) const {
    return const_cast<Geometry*>(this)->attributes(c);
}

namespace {

/// The integer attribute `instance` of these points, if there is one.
AttributeArray* instanceAttribute(AttributeSet& points) {
    AttributeArray* a = points.find("instance");
    return a && a->type() == AttrType::Int ? a : nullptr;
}

}  // namespace

size_t Geometry::addPoints(size_t n) {
    const size_t first = points_.elementCount();
    points_.setElementCount(first + n);
    for (auto& [name, g] : groups_) {
        if (g.classOf() == AttrClass::Point) g.resize(first + n);
    }
    // A new point stands for nothing: 0 would be the first prototype.
    if (AttributeArray* instance = prototypeCount() > 0 && n > 0 ? instanceAttribute(points_) : nullptr) {
        auto k = instance->write<int32_t>();
        std::fill(k.begin() + static_cast<std::ptrdiff_t>(first), k.end(), -1);
    }
    return first;
}

size_t Geometry::addPrimitive(std::span<const uint32_t> pointIndices, bool closed) {
    Topology& t = topologyForWrite();
    const size_t prim = primitives_.elementCount();
    const size_t vstart = t.vertexPoint.size();

    t.vertexPoint.insert(t.vertexPoint.end(), pointIndices.begin(), pointIndices.end());
    t.primStart.push_back(static_cast<uint32_t>(vstart));
    t.primCount.push_back(static_cast<uint32_t>(pointIndices.size()));
    t.primClosed.push_back(closed ? 1 : 0);

    vertices_.setElementCount(t.vertexPoint.size());
    primitives_.setElementCount(prim + 1);
    for (auto& [name, g] : groups_) {
        if (g.classOf() == AttrClass::Primitive) g.resize(prim + 1);
        else if (g.classOf() == AttrClass::Vertex) g.resize(t.vertexPoint.size());
    }
    return prim;
}

size_t Geometry::addPrimitives(std::span<const uint32_t> points, std::span<const uint32_t> counts,
                               std::span<const uint8_t> closed) {
    Topology& t = topologyForWrite();
    const size_t first = primitives_.elementCount();
    t.vertexPoint.insert(t.vertexPoint.end(), points.begin(), points.end());
    t.primStart.reserve(t.primStart.size() + counts.size());
    t.primCount.reserve(t.primCount.size() + counts.size());
    size_t start = t.vertexPoint.size() - points.size();
    for (const uint32_t n : counts) {
        t.primStart.push_back(static_cast<uint32_t>(start));
        t.primCount.push_back(n);
        start += n;
    }
    if (closed.size() == counts.size()) {
        t.primClosed.insert(t.primClosed.end(), closed.begin(), closed.end());
    } else {
        t.primClosed.resize(t.primClosed.size() + counts.size(), !closed.empty() && closed[0] ? 1 : 0);
    }
    vertices_.setElementCount(t.vertexPoint.size());
    primitives_.setElementCount(first + counts.size());
    for (auto& [name, g] : groups_) {
        if (g.classOf() == AttrClass::Primitive) g.resize(first + counts.size());
        else if (g.classOf() == AttrClass::Vertex) g.resize(t.vertexPoint.size());
    }
    return first;
}

std::span<const uint32_t> Geometry::primitivePoints(size_t prim) const {
    const Topology& t = topology();
    if (prim >= t.primStart.size()) return {};
    return std::span<const uint32_t>(t.vertexPoint.data() + t.primStart[prim], t.primCount[prim]);
}

size_t Geometry::primitiveVertexStart(size_t prim) const {
    const Topology& t = topology();
    return prim < t.primStart.size() ? t.primStart[prim] : 0;
}

size_t Geometry::primitiveVertexCount(size_t prim) const {
    const Topology& t = topology();
    return prim < t.primCount.size() ? t.primCount[prim] : 0;
}

bool Geometry::primitiveClosed(size_t prim) const {
    const Topology& t = topology();
    return prim < t.primClosed.size() && t.primClosed[prim] != 0;
}

uint32_t Geometry::vertexPoint(size_t vertex) const {
    const Topology& t = topology();
    return vertex < t.vertexPoint.size() ? t.vertexPoint[vertex] : 0;
}

void Geometry::append(const Geometry& other) {
    const size_t pointOffset = pointCount();
    const size_t vertexOffset = vertexCount();
    const size_t primOffset = primitiveCount();
    // Whose points stand for prototypes: theirs are renumbered to follow
    // ours; the points of a side without any stand for none -- the zeros
    // the attribute is filled with would make them the first prototype.
    const size_t prototypeOffset = prototypeCount();
    const bool ourInstances = prototypeOffset > 0 && instanceAttribute(points_) != nullptr;
    const bool theirInstances =
        other.prototypeCount() > 0 && other.points_.find("instance") && other.points_.find("instance")->type() == AttrType::Int;

    // What sizes and tints instances: a side without it was drawn at 1.
    const bool ourScale = points_.find("pscale") && points_.find("pscale")->type() == AttrType::Float;
    const bool theirScale = other.points_.find("pscale") && other.points_.find("pscale")->type() == AttrType::Float;
    const bool ourTint = points_.find("tint") && points_.find("tint")->type() == AttrType::Vec3;
    const bool theirTint = other.points_.find("tint") && other.points_.find("tint")->type() == AttrType::Vec3;

    points_.append(other.points_);
    if (ourInstances || theirInstances) {
        const AttributeArray* k = instanceAttribute(points_);
        auto ones = [&](const char* name, AttrType type, size_t from, size_t to) {
            AttributeArray* a = points_.find(name);
            if (!a || a->type() != type || !k) return;
            const auto which = k->read<int32_t>();
            if (type == AttrType::Float) {
                auto v = a->write<float>();
                for (size_t p = from; p < to; ++p) {
                    if (which[p] >= 0) v[p] = 1.0f;
                }
            } else {
                auto v = a->write<Vec3>();
                for (size_t p = from; p < to; ++p) {
                    if (which[p] >= 0) v[p] = Vec3(1.0f, 1.0f, 1.0f);
                }
            }
        };
        if (AttributeArray* instance = instanceAttribute(points_)) {
            auto k = instance->write<int32_t>();
            const auto theirs = k.begin() + static_cast<std::ptrdiff_t>(pointOffset);
            if (!ourInstances) std::fill(k.begin(), theirs, -1);
            if (!theirInstances) {
                std::fill(theirs, k.end(), -1);
            } else {
                for (auto it = theirs; it != k.end(); ++it) {
                    if (*it >= 0) *it += static_cast<int32_t>(prototypeOffset);
                }
            }
        }
        for (const GeometryPtr& prototype : other.prototypes()) addPrototype(prototype);
        const size_t end = pointCount();
        if (ourScale != theirScale) ones("pscale", AttrType::Float, ourScale ? pointOffset : 0, ourScale ? end : pointOffset);
        if (ourTint != theirTint) ones("tint", AttrType::Vec3, ourTint ? pointOffset : 0, ourTint ? end : pointOffset);
    }
    vertices_.append(other.vertices_);
    primitives_.append(other.primitives_);

    // Detail attributes: ours win, theirs are added -- with their values --
    // only if we lack them.
    for (const auto& [name, attr] : other.detail_) {
        if (!detail_.contains(name)) detail_.assign(name, attr);
    }

    Topology& t = topologyForWrite();
    const Topology& o = *other.topo_;
    t.vertexPoint.reserve(t.vertexPoint.size() + o.vertexPoint.size());
    for (uint32_t vp : o.vertexPoint) {
        t.vertexPoint.push_back(vp + static_cast<uint32_t>(pointOffset));
    }
    for (size_t i = 0; i < o.primStart.size(); ++i) {
        t.primStart.push_back(o.primStart[i] + static_cast<uint32_t>(vertexOffset));
        t.primCount.push_back(o.primCount[i]);
        t.primClosed.push_back(o.primClosed[i]);
    }

    for (const Volume& v : other.volumes()) addVolume(v);

    for (auto& [name, g] : groups_) {
        g.resize(elementCount(g.classOf()));
    }
    for (const auto& [name, og] : other.groups_) {
        Group& g = createGroup(name, og.classOf());
        g.resize(elementCount(og.classOf()));
        size_t offset = 0;
        switch (og.classOf()) {
            case AttrClass::Point:     offset = pointOffset; break;
            case AttrClass::Vertex:    offset = vertexOffset; break;
            case AttrClass::Primitive: offset = primOffset; break;
            case AttrClass::Detail:    offset = 0; break;
        }
        auto m = og.mask();
        for (size_t i = 0; i < m.size(); ++i) {
            if (m[i]) g.set(offset + i, true);
        }
    }
}

Group* Geometry::findGroup(const std::string& name) {
    auto it = groups_.find(name);
    return it == groups_.end() ? nullptr : &it->second;
}

const Group* Geometry::findGroup(const std::string& name) const {
    auto it = groups_.find(name);
    return it == groups_.end() ? nullptr : &it->second;
}

Group& Geometry::createGroup(const std::string& name, AttrClass cls) {
    auto it = groups_.find(name);
    if (it != groups_.end() && it->second.classOf() == cls) return it->second;
    groups_[name] = Group(cls, elementCount(cls));
    return groups_[name];
}

bool Geometry::eraseGroup(const std::string& name) { return groups_.erase(name) > 0; }

std::vector<std::string> Geometry::groupNames() const {
    std::vector<std::string> out;
    out.reserve(groups_.size());
    for (const auto& [name, g] : groups_) out.push_back(name);
    return out;
}

void Geometry::deletePoints(std::span<const uint8_t> keep) {
    const size_t np = pointCount();
    if (keep.size() != np) return;

    std::vector<uint32_t> keptPoints;
    std::vector<uint32_t> remap(np, ~0u);
    keptPoints.reserve(np);
    for (size_t i = 0; i < np; ++i) {
        if (keep[i]) {
            remap[i] = static_cast<uint32_t>(keptPoints.size());
            keptPoints.push_back(static_cast<uint32_t>(i));
        }
    }

    // A primitive survives only if every one of its points survives.
    const Topology& t = topology();
    std::vector<uint32_t> keptPrims, keptVertices;
    for (size_t p = 0; p < t.primStart.size(); ++p) {
        const uint32_t start = t.primStart[p], count = t.primCount[p];
        bool alive = true;
        for (uint32_t v = start; v < start + count; ++v) {
            if (remap[t.vertexPoint[v]] == ~0u) { alive = false; break; }
        }
        if (!alive) continue;
        keptPrims.push_back(static_cast<uint32_t>(p));
        for (uint32_t v = start; v < start + count; ++v) keptVertices.push_back(v);
    }

    Topology next;
    next.vertexPoint.reserve(keptVertices.size());
    for (uint32_t v : keptVertices) next.vertexPoint.push_back(remap[t.vertexPoint[v]]);
    uint32_t cursor = 0;
    for (uint32_t p : keptPrims) {
        next.primStart.push_back(cursor);
        next.primCount.push_back(t.primCount[p]);
        next.primClosed.push_back(t.primClosed[p]);
        cursor += t.primCount[p];
    }

    points_.gather(keptPoints);
    vertices_.gather(keptVertices);
    primitives_.gather(keptPrims);
    topo_ = std::make_shared<Topology>(std::move(next));

    for (auto& [name, g] : groups_) {
        const std::vector<uint32_t>* idx = nullptr;
        switch (g.classOf()) {
            case AttrClass::Point:     idx = &keptPoints; break;
            case AttrClass::Vertex:    idx = &keptVertices; break;
            case AttrClass::Primitive: idx = &keptPrims; break;
            case AttrClass::Detail:    continue;
        }
        Group next(g.classOf(), idx->size());
        auto m = g.mask();
        for (size_t i = 0; i < idx->size(); ++i) {
            const uint32_t from = (*idx)[i];
            if (from < m.size() && m[from]) next.set(i, true);
        }
        g = std::move(next);
    }
}

void Geometry::deletePrimitives(std::span<const uint8_t> keep, bool unusedPoints) {
    const Topology& t = topology();
    const size_t nprims = t.primStart.size();
    if (keep.size() != nprims) return;

    std::vector<uint32_t> keptPrims, keptVertices;
    keptPrims.reserve(nprims);
    // 1: used by a kept primitive; 2: only by deleted ones.
    std::vector<uint8_t> use(unusedPoints ? pointCount() : 0, 0);
    for (size_t p = 0; p < nprims; ++p) {
        const uint32_t start = t.primStart[p], count = t.primCount[p];
        if (keep[p]) {
            keptPrims.push_back(static_cast<uint32_t>(p));
            for (uint32_t v = start; v < start + count; ++v) {
                keptVertices.push_back(v);
                if (unusedPoints) use[t.vertexPoint[v]] = 1;
            }
        } else if (unusedPoints) {
            for (uint32_t v = start; v < start + count; ++v) {
                uint8_t& u = use[t.vertexPoint[v]];
                if (u == 0) u = 2;
            }
        }
    }
    if (keptPrims.size() == nprims) return;

    Topology next;
    next.vertexPoint.reserve(keptVertices.size());
    for (uint32_t v : keptVertices) next.vertexPoint.push_back(t.vertexPoint[v]);
    uint32_t cursor = 0;
    for (uint32_t p : keptPrims) {
        next.primStart.push_back(cursor);
        next.primCount.push_back(t.primCount[p]);
        next.primClosed.push_back(t.primClosed[p]);
        cursor += t.primCount[p];
    }
    vertices_.gather(keptVertices);
    primitives_.gather(keptPrims);
    topo_ = std::make_shared<Topology>(std::move(next));

    for (auto& [name, g] : groups_) {
        const std::vector<uint32_t>* idx = nullptr;
        switch (g.classOf()) {
            case AttrClass::Vertex:    idx = &keptVertices; break;
            case AttrClass::Primitive: idx = &keptPrims; break;
            default:                   continue;
        }
        Group next(g.classOf(), idx->size());
        auto m = g.mask();
        for (size_t i = 0; i < idx->size(); ++i) {
            const uint32_t from = (*idx)[i];
            if (from < m.size() && m[from]) next.set(i, true);
        }
        g = std::move(next);
    }

    if (unusedPoints) {
        std::vector<uint8_t> keepPoints(use.size(), 1);
        bool any = false;
        for (size_t i = 0; i < use.size(); ++i) {
            if (use[i] == 2) {
                keepPoints[i] = 0;
                any = true;
            }
        }
        if (any) deletePoints(keepPoints);
    }
}

uint64_t Geometry::hash() const {
    uint64_t h = kFnvOffset;
    hashAttributeSet(h, detail_);
    hashAttributeSet(h, points_);
    hashAttributeSet(h, vertices_);
    hashAttributeSet(h, primitives_);

    const Topology& t = topology();
    hashU64(h, t.vertexPoint.size());
    if (!t.vertexPoint.empty()) {
        hashBytes(h, t.vertexPoint.data(), t.vertexPoint.size() * sizeof(uint32_t));
    }
    hashU64(h, t.primStart.size());
    if (!t.primCount.empty()) {
        hashBytes(h, t.primCount.data(), t.primCount.size() * sizeof(uint32_t));
        hashBytes(h, t.primClosed.data(), t.primClosed.size());
    }

    for (const auto& name : groupNames()) {  // sorted
        const Group* g = findGroup(name);
        hashBytes(h, name.data(), name.size());
        hashU64(h, static_cast<uint64_t>(g->classOf()));
        auto m = g->mask();
        hashU64(h, m.size());
        if (!m.empty()) hashBytes(h, m.data(), m.size());
    }

    hashU64(h, volumeCount());
    for (const Volume& v : volumes()) {
        hashBytes(h, v.name.data(), v.name.size());
        hashU64(h, v.name.size());
        hashBytes(h, &v.origin, sizeof(Vec3));
        hashBytes(h, &v.voxel, sizeof(float));
        hashBytes(h, v.res, sizeof(v.res));
        if (v.values && !v.values->empty()) hashBytes(h, v.values->data(), v.values->size() * sizeof(float));
    }
    hashU64(h, prototypeCount());
    for (const GeometryPtr& prototype : prototypes()) hashU64(h, prototype ? prototype->hash() : 0);
    return h;
}

size_t Geometry::memoryUsage() const {
    size_t bytes = detail_.memoryUsage() + points_.memoryUsage() +
                   vertices_.memoryUsage() + primitives_.memoryUsage();
    const Topology& t = topology();
    bytes += t.vertexPoint.size() * sizeof(uint32_t);
    bytes += t.primStart.size() * sizeof(uint32_t) * 2 + t.primClosed.size();
    for (const auto& [name, g] : groups_) bytes += g.size();
    for (const Volume& v : volumes()) bytes += v.values ? v.values->size() * sizeof(float) : 0;
    for (const GeometryPtr& prototype : prototypes()) bytes += prototype ? prototype->memoryUsage() : 0;
    return bytes;
}

std::shared_ptr<Geometry> editableCopy(const GeometryPtr& in) {
    // The copy constructor shares every buffer; nothing here is O(elements).
    return in ? std::make_shared<Geometry>(*in) : std::make_shared<Geometry>();
}

void setPrimitiveString(Geometry& geo, const std::string& name, const std::string& value, std::span<const uint8_t> mask) {
    const bool had = geo.primitives().find(name) && geo.primitives().find(name)->type() == AttrType::String;
    AttributeArray& a = geo.primitives().create(name, AttrType::String);
    // A new one's elements are 0: "" before anything else.
    if (!had) a.internString("");
    const int32_t id = a.internString(value);
    auto w = a.write<int32_t>();
    for (size_t p = 0; p < w.size(); ++p) {
        if (mask.empty() || (p < mask.size() && mask[p])) w[p] = id;
    }
}

const std::string& primitiveString(const Geometry& geo, const std::string& name, size_t p) {
    static const std::string none;
    const AttributeArray* a = geo.primitives().find(name);
    if (!a || a->type() != AttrType::String || p >= a->size()) return none;
    return a->stringValue(a->read<int32_t>()[p]);
}

}  // namespace pg
