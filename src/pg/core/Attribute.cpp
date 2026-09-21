#include "pg/core/Attribute.h"

#include <algorithm>
#include <atomic>
#include <cstring>

namespace pg {
namespace {

std::atomic<uint64_t> g_allocations{0};

const std::string& emptyString() {
    static const std::string s;
    return s;
}

}  // namespace

uint64_t attributeAllocationCount() {
    return g_allocations.load(std::memory_order_relaxed);
}

void resetAttributeAllocationCount() {
    g_allocations.store(0, std::memory_order_relaxed);
}

// --- AttributeArray --------------------------------------------------------

AttributeArray::AttributeArray(AttrType type, size_t count) : type_(type), count_(count) {
    if (count > 0) {
        g_allocations.fetch_add(1, std::memory_order_relaxed);
        buffer_ = std::make_shared<Buffer>(count * attrSize(type));
    }
}

std::byte* AttributeArray::rawWrite() {
    if (!buffer_) {
        g_allocations.fetch_add(1, std::memory_order_relaxed);
        buffer_ = std::make_shared<Buffer>(byteSize());
    } else if (buffer_.use_count() > 1) {
        // The COW clone. This is the only place element data is duplicated.
        g_allocations.fetch_add(1, std::memory_order_relaxed);
        buffer_ = std::make_shared<Buffer>(*buffer_);
    }
    return buffer_->bytes.data();
}

void AttributeArray::resize(size_t count) {
    if (count == count_) return;
    rawWrite();  // materialise an unshared buffer first
    buffer_->bytes.resize(count * attrSize(type_), std::byte{0});
    count_ = count;
}

void AttributeArray::append(const AttributeArray& other) {
    if (other.count_ == 0) return;
    assert(type_ == other.type_ && "append type mismatch");

    const size_t oldCount = count_;
    resize(oldCount + other.count_);

    if (type_ == AttrType::String) {
        // Indices are table-local, so they are remapped rather than copied.
        auto dst = write<int32_t>();
        auto src = other.read<int32_t>();
        for (size_t i = 0; i < other.count_; ++i) {
            dst[oldCount + i] = internString(other.stringValue(src[i]));
        }
        // internString may reallocate the table but never the element buffer.
        return;
    }

    std::memcpy(rawWrite() + oldCount * attrSize(type_),
                other.rawRead(),
                other.count_ * attrSize(type_));
}

AttributeArray AttributeArray::gather(std::span<const uint32_t> indices) const {
    AttributeArray out(type_, indices.size());
    out.strings_ = strings_;  // shared, not rebuilt: indices stay valid
    if (indices.empty() || count_ == 0) return out;

    const size_t esz = attrSize(type_);
    const std::byte* src = rawRead();
    std::byte* dst = out.rawWrite();
    for (size_t i = 0; i < indices.size(); ++i) {
        const size_t from = indices[i];
        assert(from < count_ && "gather index out of range");
        std::memcpy(dst + i * esz, src + from * esz, esz);
    }
    return out;
}

int32_t AttributeArray::internString(const std::string& s) {
    if (!strings_) {
        strings_ = std::make_shared<StringTable>();
    } else if (strings_.use_count() > 1) {
        strings_ = std::make_shared<StringTable>(*strings_);  // COW the table too
    }
    auto it = std::find(strings_->begin(), strings_->end(), s);
    if (it != strings_->end()) {
        return static_cast<int32_t>(std::distance(strings_->begin(), it));
    }
    strings_->push_back(s);
    return static_cast<int32_t>(strings_->size() - 1);
}

const std::string& AttributeArray::stringValue(int32_t index) const {
    if (!strings_ || index < 0 || static_cast<size_t>(index) >= strings_->size()) {
        return emptyString();
    }
    return (*strings_)[static_cast<size_t>(index)];
}

size_t AttributeArray::memoryUsage() const {
    size_t bytes = buffer_ ? buffer_->bytes.size() : 0;
    if (strings_) {
        for (const auto& s : *strings_) bytes += s.size() + sizeof(std::string);
    }
    return bytes;
}

// --- AttributeSet ----------------------------------------------------------

void AttributeSet::setElementCount(size_t n) {
    if (n == elementCount_) return;
    for (auto& [name, attr] : attrs_) attr.resize(n);
    elementCount_ = n;
}

AttributeArray* AttributeSet::find(const std::string& name) {
    auto it = attrs_.find(name);
    return it == attrs_.end() ? nullptr : &it->second;
}

const AttributeArray* AttributeSet::find(const std::string& name) const {
    auto it = attrs_.find(name);
    return it == attrs_.end() ? nullptr : &it->second;
}

AttributeArray& AttributeSet::create(const std::string& name, AttrType type) {
    auto it = attrs_.find(name);
    if (it != attrs_.end()) {
        if (it->second.type() == type) return it->second;
        attrs_.erase(it);  // type change replaces the attribute
    }
    auto [ins, ok] = attrs_.emplace(name, AttributeArray(type, elementCount_));
    (void)ok;
    return ins->second;
}

bool AttributeSet::erase(const std::string& name) {
    return attrs_.erase(name) > 0;
}

std::vector<std::string> AttributeSet::names() const {
    std::vector<std::string> out;
    out.reserve(attrs_.size());
    for (const auto& [name, attr] : attrs_) out.push_back(name);
    return out;  // std::map already iterates in sorted order
}

void AttributeSet::append(const AttributeSet& other) {
    const size_t oldCount = elementCount_;
    const size_t addCount = other.elementCount_;

    // Attributes present only on the incoming side: create and back-fill zeros
    // for the elements we already have, so the set stays rectangular.
    for (const auto& [name, attr] : other) {
        if (!contains(name)) create(name, attr.type()).resize(oldCount);
    }

    for (auto& [name, attr] : attrs_) {
        const AttributeArray* src = other.find(name);
        if (src && src->type() == attr.type()) {
            attr.append(*src);
        } else {
            attr.resize(oldCount + addCount);  // zero-fill the tail
        }
    }
    elementCount_ = oldCount + addCount;
}

void AttributeSet::gather(std::span<const uint32_t> indices) {
    for (auto& [name, attr] : attrs_) attr = attr.gather(indices);
    elementCount_ = indices.size();
}

size_t AttributeSet::memoryUsage() const {
    size_t bytes = 0;
    for (const auto& [name, attr] : attrs_) bytes += attr.memoryUsage();
    return bytes;
}

}  // namespace pg
