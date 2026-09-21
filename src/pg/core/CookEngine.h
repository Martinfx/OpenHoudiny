#pragma once
//
// The cook engine: lazy, pull-based, dependency-driven evaluation.
//
// Asking for a node's output pulls its inputs recursively. Anything whose
// (node, version, frame) key is already in the cache is not recomputed. A
// parameter change bumps versions downstream only, so an edit halfway down a
// 100-node chain recooks 50 nodes, not 100.
//
#include "pg/core/Node.h"

#include <cstdint>
#include <list>
#include <mutex>
#include <unordered_map>

namespace pg {

/// Frame slot used by time-independent nodes: one entry covers all frames.
inline constexpr int kAnyFrame = INT32_MIN;

struct CacheKey {
    const Node* node = nullptr;
    uint64_t version = 0;
    int frame = kAnyFrame;

    bool operator==(const CacheKey& o) const {
        return node == o.node && version == o.version && frame == o.frame;
    }
};

struct CacheKeyHash {
    size_t operator()(const CacheKey& k) const noexcept {
        size_t h = std::hash<const void*>{}(k.node);
        h ^= std::hash<uint64_t>{}(k.version) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
        h ^= std::hash<int>{}(k.frame) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
        return h;
    }
};

/// LRU cache of cooked geometry, bounded by a hard memory budget.
///
/// Accounting caveat: an entry is charged Geometry::memoryUsage(), which counts
/// every buffer it references even when that buffer is shared with another
/// cached entry. The figure is therefore an upper bound on real footprint.
/// Exact accounting needs a buffer-level registry -- see ARCHITECTURE.md,
/// "Known simplifications".
class CookCache {
public:
    explicit CookCache(size_t budgetBytes);

    void setMemoryBudget(size_t bytes);
    size_t memoryBudget() const;
    size_t memoryUsed() const;
    size_t entryCount() const;

    uint64_t hits() const;
    uint64_t misses() const;
    uint64_t evictions() const;
    void resetStats();

    GeometryPtr find(const CacheKey& key);
    void insert(const CacheKey& key, const GeometryPtr& geo, size_t bytes);
    void clear();

private:
    struct Entry {
        CacheKey key;
        GeometryPtr geo;
        size_t bytes = 0;
    };

    void evictToBudgetLocked();

    mutable std::mutex mu_;
    std::list<Entry> lru_;  ///< front == most recently used
    std::unordered_map<CacheKey, std::list<Entry>::iterator, CacheKeyHash> index_;
    size_t budget_ = 0;
    size_t used_ = 0;
    uint64_t hits_ = 0, misses_ = 0, evictions_ = 0;
};

class CookEngine {
public:
    static constexpr size_t kDefaultBudget = 256ull * 1024 * 1024;

    explicit CookEngine(size_t cacheBudgetBytes = kDefaultBudget);

    /// Evaluates `node` at `ctx`, pulling only what is out of date.
    GeometryPtr cook(Node& node, const CookContext& ctx);

    CookCache& cache() { return cache_; }
    const CookCache& cache() const { return cache_; }

    /// Cooks independent inputs of a multi-input node concurrently.
    /// On by default. Note that with it on, a node reachable through two
    /// branches may be cooked twice (same result, wasted work) -- tests that
    /// assert exact cook counts on a diamond graph should turn it off.
    void setParallelBranches(bool on) { parallelBranches_ = on; }
    bool parallelBranches() const { return parallelBranches_; }

    /// True if `node` or anything upstream of it varies with time.
    bool isTimeDependent(const Node& node) const;

private:
    using TimeDepMap = std::unordered_map<const Node*, bool>;

    static bool computeTimeDependent(const Node& node, TimeDepMap& memo);
    GeometryPtr cookRecursive(Node& node, const CookContext& ctx, const TimeDepMap& td);

    CookCache cache_;
    bool parallelBranches_ = true;
};

}  // namespace pg
