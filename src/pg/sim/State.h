#pragma once
//
// A solver's state as bytes, and back: what a checkpoint holds -- everything
// a simulation needs to go on from a frame exactly as it would have, had it
// never stopped. Not what a Frame keeps (half floats, for playing back): the
// floats of the fields, the particles to the bit, the counters that number
// what is made next.
//
// Little-endian, as the machines it runs on are; a reader checks every length
// against what is left, so a cut or foreign file is refused, not read past.
//
#include "pg/core/Types.h"
#include "pg/sim/Grid.h"
#include "pg/sim/SparseGrid.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace pg::sim {

class StateWriter {
public:
    template <class T>
    void pod(const T& v) {
        static_assert(std::is_trivially_copyable_v<T>);
        bytes_.append(reinterpret_cast<const char*>(&v), sizeof v);
    }
    template <class T>
    void list(const std::vector<T>& v) {
        static_assert(std::is_trivially_copyable_v<T>);
        pod(static_cast<uint64_t>(v.size()));
        if (!v.empty()) bytes_.append(reinterpret_cast<const char*>(v.data()), v.size() * sizeof(T));
    }
    void grid(const Grid& g);
    /// Its values; the tiles are the caller's to write once for all the
    /// fields that share them.
    void values(const SparseGrid& g) { list(g.values()); }
    void tiles(const Tiles& t);

    const std::string& bytes() const { return bytes_; }
    std::string take() { return std::move(bytes_); }

private:
    std::string bytes_;
};

class StateReader {
public:
    explicit StateReader(std::string_view bytes) : data_(bytes) {}

    template <class T>
    bool pod(T& v) {
        static_assert(std::is_trivially_copyable_v<T>);
        if (!take(sizeof v)) return false;
        std::memcpy(&v, data_.data() + at_ - sizeof v, sizeof v);
        return true;
    }
    template <class T>
    bool list(std::vector<T>& v) {
        static_assert(std::is_trivially_copyable_v<T>);
        uint64_t n = 0;
        if (!pod(n) || n > (data_.size() - at_) / sizeof(T)) return fail();
        v.resize(static_cast<size_t>(n));
        if (n > 0) std::memcpy(v.data(), data_.data() + at_, static_cast<size_t>(n) * sizeof(T));
        at_ += static_cast<size_t>(n) * sizeof(T);
        return true;
    }
    /// A grid as grid() wrote it.
    bool grid(Grid& g);
    /// Values into `g`, which already has the tiles they were written with:
    /// as many as it stores, or the state is not this solver's.
    bool values(SparseGrid& g);
    bool tiles(std::shared_ptr<const Tiles>& t);

    bool ok() const { return ok_; }
    bool done() const { return ok_ && at_ == data_.size(); }
    /// Marks the state bad; false, for `return reader.fail();`.
    bool fail() {
        ok_ = false;
        return false;
    }

private:
    bool take(size_t n) {
        if (!ok_ || n > data_.size() - at_) return fail();
        at_ += n;
        return true;
    }

    std::string_view data_;
    size_t at_ = 0;
    bool ok_ = true;
};

}  // namespace pg::sim
