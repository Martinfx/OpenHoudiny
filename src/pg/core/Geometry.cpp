#include "pg/core/Geometry.h"

#include <algorithm>
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

// --- Geometry --------------------------------------------------------------

Geometry::Geometry() : topo_(std::make_shared<Topology>()) {
    // P is mandatory. Every node may assume it exists.
    points_.create("P", AttrType::Vec3);
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

size_t Geometry::addPoints(size_t n) {
    const size_t first = points_.elementCount();
    points_.setElementCount(first + n);
    for (auto& [name, g] : groups_) {
        if (g.classOf() == AttrClass::Point) g.resize(first + n);
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

    points_.append(other.points_);
    vertices_.append(other.vertices_);
    primitives_.append(other.primitives_);

    // Detail attributes: ours win, theirs are added only if we lack them.
    for (const auto& [name, attr] : other.detail_) {
        if (!detail_.contains(name)) {
            detail_.create(name, attr.type());
        }
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
    return h;
}

size_t Geometry::memoryUsage() const {
    size_t bytes = detail_.memoryUsage() + points_.memoryUsage() +
                   vertices_.memoryUsage() + primitives_.memoryUsage();
    const Topology& t = topology();
    bytes += t.vertexPoint.size() * sizeof(uint32_t);
    bytes += t.primStart.size() * sizeof(uint32_t) * 2 + t.primClosed.size();
    for (const auto& [name, g] : groups_) bytes += g.size();
    return bytes;
}

std::shared_ptr<Geometry> editableCopy(const GeometryPtr& in) {
    // The copy constructor shares every buffer; nothing here is O(elements).
    return in ? std::make_shared<Geometry>(*in) : std::make_shared<Geometry>();
}

}  // namespace pg
