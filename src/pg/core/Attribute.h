#pragma once
//
// Attribute storage: the single most important data structure in the system.
//
// Two invariants from ARCHITECTURE.md are enforced here:
//
//   I1  Copy-on-write per attribute array. Copying an AttributeArray (or a
//       whole AttributeSet, or a whole Geometry) copies *no* element data.
//       The clone happens on the first write, and only for the array written.
//
//   I2  Struct-of-arrays. One attribute is one contiguous typed buffer. There
//       is no `struct Point { Vec3 P; Vec3 N; ... }` anywhere.
//
// Thread safety: an AttributeArray is either (a) owned exclusively by the node
// currently building it, or (b) immutable and shared downstream. `rawWrite()`
// uses use_count() to decide whether to clone; if two threads race on two
// *distinct* AttributeArray objects that share one buffer, both observe
// "shared" and both clone. That wastes a copy but can never corrupt or alias.
//
#include "pg/core/Types.h"

#include <cassert>
#include <cstddef>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

namespace pg {

/// Process-wide counter of *element-data* buffer allocations.
/// Tests use it to prove that copy-on-write actually avoids copies.
uint64_t attributeAllocationCount();
void resetAttributeAllocationCount();

class AttributeArray {
public:
    AttributeArray() = default;
    AttributeArray(AttrType type, size_t count);

    AttrType type() const { return type_; }
    size_t size() const { return count_; }
    size_t byteSize() const { return count_ * attrSize(type_); }
    bool empty() const { return count_ == 0; }

    /// True while more than one AttributeArray references the element buffer.
    bool isShared() const { return buffer_ && buffer_.use_count() > 1; }

    /// Identity of the underlying element buffer. Equal pointers mean the data
    /// is genuinely shared, not copied. Exposed for tests and diagnostics.
    const void* bufferId() const { return buffer_.get(); }

    // --- raw access ---------------------------------------------------------

    /// Read path. Never clones, never allocates.
    const std::byte* rawRead() const {
        return buffer_ ? buffer_->bytes.data() : nullptr;
    }

    /// Write path. Clones the buffer iff it is shared. This is the COW hinge.
    std::byte* rawWrite();

    // --- typed access -------------------------------------------------------

    template <class T>
    std::span<const T> read() const {
        assert(holds<T>() && "attribute type mismatch");
        return std::span<const T>(reinterpret_cast<const T*>(rawRead()), count_);
    }

    template <class T>
    std::span<T> write() {
        assert(holds<T>() && "attribute type mismatch");
        return std::span<T>(reinterpret_cast<T*>(rawWrite()), count_);
    }

    /// Element count change. Goes through the write path.
    void resize(size_t count);

    /// Appends `other`'s elements. Types must match. Write path.
    void append(const AttributeArray& other);

    /// New array holding the elements at `indices`, in that order. Shares the
    /// string table rather than rebuilding it. This is the single primitive
    /// behind every reordering and compaction (delete, sort, extract).
    AttributeArray gather(std::span<const uint32_t> indices) const;

    // --- string attributes --------------------------------------------------
    //
    // String attributes store int32 indices into a per-array table that is
    // itself copy-on-write. Element data stays fixed-width, so the SoA layout
    // and every bulk operation below work unchanged.

    /// Returns the index for `s`, adding it to the table if new. Write path.
    int32_t internString(const std::string& s);
    /// Empty string for an out-of-range index.
    const std::string& stringValue(int32_t index) const;
    size_t stringTableSize() const { return strings_ ? strings_->size() : 0; }

    /// Bytes of element data owned by this array, ignoring sharing.
    size_t memoryUsage() const;

private:
    /// True if T is the element type. String arrays store int32 indices into
    /// their table, so int32_t is their element type too.
    template <class T>
    bool holds() const {
        return type_ == AttrTypeOf<T>::value ||
               (std::is_same_v<T, int32_t> && type_ == AttrType::String);
    }

    struct Buffer {
        std::vector<std::byte> bytes;
        Buffer() = default;
        explicit Buffer(size_t n) : bytes(n) {}
    };
    using StringTable = std::vector<std::string>;

    std::shared_ptr<Buffer> buffer_;
    std::shared_ptr<StringTable> strings_;
    AttrType type_ = AttrType::Float;
    size_t count_ = 0;
};

/// The attributes of one AttrClass. Copying shares every element buffer.
class AttributeSet {
public:
    /// Number of elements each attribute in this set holds.
    size_t elementCount() const { return elementCount_; }

    /// Sets the element count and resizes every attribute to match.
    void setElementCount(size_t n);

    AttributeArray* find(const std::string& name);
    const AttributeArray* find(const std::string& name) const;

    /// Creates the attribute, or returns the existing one if the type matches.
    /// A type mismatch replaces the attribute.
    AttributeArray& create(const std::string& name, AttrType type);

    bool erase(const std::string& name);
    bool contains(const std::string& name) const { return find(name) != nullptr; }
    size_t count() const { return attrs_.size(); }

    /// Attribute names in sorted order. Sorted because every traversal that
    /// feeds a hash or a file must be order-stable (invariant I5).
    std::vector<std::string> names() const;

    /// Appends every matching attribute of `other`; attributes missing on
    /// either side are zero-filled so both sides stay rectangular.
    void append(const AttributeSet& other);

    /// Reorders/compacts every attribute to the elements at `indices`.
    void gather(std::span<const uint32_t> indices);

    size_t memoryUsage() const;

    auto begin() { return attrs_.begin(); }
    auto end() { return attrs_.end(); }
    auto begin() const { return attrs_.begin(); }
    auto end() const { return attrs_.end(); }

private:
    // std::map, not unordered_map: iteration order is part of the contract.
    std::map<std::string, AttributeArray> attrs_;
    size_t elementCount_ = 0;
};

}  // namespace pg
