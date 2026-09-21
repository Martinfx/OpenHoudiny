#pragma once
//
// The geometry container.
//
// Invariant I3: there is exactly ONE geometry type. Polygons, open curves and
// loose points all live in this container and every node accepts it. Splitting
// into MeshGeometry / CurveGeometry is what destroys composability, so it is
// not done here and must not be done later.
//
// Copying a Geometry copies no element data: the attribute buffers, the
// topology and the group masks are all shared and clone on first write.
//
#include "pg/core/Attribute.h"

#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace pg {

/// A named subset of one attribute class. Copy-on-write like everything else.
class Group {
public:
    Group() = default;
    Group(AttrClass cls, size_t size);

    AttrClass classOf() const { return class_; }
    size_t size() const { return mask_ ? mask_->size() : 0; }

    bool contains(size_t i) const {
        return mask_ && i < mask_->size() && (*mask_)[i] != 0;
    }
    void set(size_t i, bool member);
    void resize(size_t n);
    /// Number of members.
    size_t memberCount() const;

    const void* bufferId() const { return mask_.get(); }
    std::span<const uint8_t> mask() const {
        return mask_ ? std::span<const uint8_t>(*mask_) : std::span<const uint8_t>();
    }

private:
    std::vector<uint8_t>& maskForWrite();

    std::shared_ptr<std::vector<uint8_t>> mask_;
    AttrClass class_ = AttrClass::Point;
};

class Geometry {
public:
    Geometry();

    // --- attribute access ---------------------------------------------------

    AttributeSet& detail() { return detail_; }
    AttributeSet& points() { return points_; }
    AttributeSet& vertices() { return vertices_; }
    AttributeSet& primitives() { return primitives_; }

    const AttributeSet& detail() const { return detail_; }
    const AttributeSet& points() const { return points_; }
    const AttributeSet& vertices() const { return vertices_; }
    const AttributeSet& primitives() const { return primitives_; }

    AttributeSet& attributes(AttrClass c);
    const AttributeSet& attributes(AttrClass c) const;

    size_t pointCount() const { return points_.elementCount(); }
    size_t vertexCount() const { return vertices_.elementCount(); }
    size_t primitiveCount() const { return primitives_.elementCount(); }
    size_t elementCount(AttrClass c) const { return attributes(c).elementCount(); }

    /// `P` is created by the constructor and is always present.
    std::span<const Vec3> positions() const { return points_.find("P")->read<Vec3>(); }
    std::span<Vec3> positionsForWrite() { return points_.find("P")->write<Vec3>(); }

    // --- building -----------------------------------------------------------

    /// Appends `n` points. Returns the index of the first one.
    size_t addPoints(size_t n);

    /// Appends a primitive over the given point indices.
    /// Returns the primitive index.
    size_t addPrimitive(std::span<const uint32_t> pointIndices, bool closed = true);

    /// Appends all of `other`. Point indices in the incoming topology are
    /// rebased; attributes are merged by name.
    void append(const Geometry& other);

    // --- topology -----------------------------------------------------------

    /// Point indices of primitive `prim`, one per corner.
    std::span<const uint32_t> primitivePoints(size_t prim) const;
    /// Index of the first vertex (corner) of `prim`.
    size_t primitiveVertexStart(size_t prim) const;
    size_t primitiveVertexCount(size_t prim) const;
    bool primitiveClosed(size_t prim) const;

    /// Point index of vertex (corner) `vertex`.
    uint32_t vertexPoint(size_t vertex) const;

    // --- groups -------------------------------------------------------------

    Group* findGroup(const std::string& name);
    const Group* findGroup(const std::string& name) const;
    Group& createGroup(const std::string& name, AttrClass cls);
    bool eraseGroup(const std::string& name);
    /// Sorted, for order-stable traversal.
    std::vector<std::string> groupNames() const;

    // --- whole-geometry operations ------------------------------------------

    /// Keeps only the points selected by `keep` (size == pointCount) and drops
    /// every primitive that referenced a removed point. Point order is
    /// preserved, so the result is deterministic.
    void deletePoints(std::span<const uint8_t> keep);

    /// Order-stable content hash over every attribute, the topology and the
    /// groups. Two geometries with the same hash are byte-identical in content.
    uint64_t hash() const;

    size_t memoryUsage() const;

private:
    struct Topology {
        std::vector<uint32_t> vertexPoint;  ///< corner -> point
        std::vector<uint32_t> primStart;    ///< primitive -> first corner
        std::vector<uint32_t> primCount;    ///< primitive -> corner count
        std::vector<uint8_t> primClosed;
    };

    const Topology& topology() const { return *topo_; }
    Topology& topologyForWrite();

    AttributeSet detail_;
    AttributeSet points_;
    AttributeSet vertices_;
    AttributeSet primitives_;
    std::map<std::string, Group> groups_;
    std::shared_ptr<Topology> topo_;
};

using GeometryPtr = std::shared_ptr<const Geometry>;

/// Mutable copy of `in` that shares all of its buffers (invariant I1).
/// This is how every node starts its cook.
std::shared_ptr<Geometry> editableCopy(const GeometryPtr& in);

}  // namespace pg
