#include "pg/core/CookEngine.h"

#include "pg/core/Parallel.h"

namespace pg {

// --- CookCache -------------------------------------------------------------

CookCache::CookCache(size_t budgetBytes) : budget_(budgetBytes) {}

void CookCache::setMemoryBudget(size_t bytes) {
    std::lock_guard<std::mutex> lk(mu_);
    budget_ = bytes;
    evictToBudgetLocked();
}

size_t CookCache::memoryBudget() const {
    std::lock_guard<std::mutex> lk(mu_);
    return budget_;
}

size_t CookCache::memoryUsed() const {
    std::lock_guard<std::mutex> lk(mu_);
    return used_;
}

size_t CookCache::entryCount() const {
    std::lock_guard<std::mutex> lk(mu_);
    return lru_.size();
}

uint64_t CookCache::hits() const {
    std::lock_guard<std::mutex> lk(mu_);
    return hits_;
}

uint64_t CookCache::misses() const {
    std::lock_guard<std::mutex> lk(mu_);
    return misses_;
}

uint64_t CookCache::evictions() const {
    std::lock_guard<std::mutex> lk(mu_);
    return evictions_;
}

void CookCache::resetStats() {
    std::lock_guard<std::mutex> lk(mu_);
    hits_ = misses_ = evictions_ = 0;
}

GeometryPtr CookCache::find(const CacheKey& key) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = index_.find(key);
    if (it == index_.end()) {
        ++misses_;
        return nullptr;
    }
    lru_.splice(lru_.begin(), lru_, it->second);  // touch
    ++hits_;
    return it->second->geo;
}

void CookCache::insert(const CacheKey& key, const GeometryPtr& geo, size_t bytes) {
    std::lock_guard<std::mutex> lk(mu_);

    if (auto it = index_.find(key); it != index_.end()) {
        used_ -= it->second->bytes;
        lru_.erase(it->second);
        index_.erase(it);
    }

    lru_.push_front(Entry{key, geo, bytes});
    index_[key] = lru_.begin();
    used_ += bytes;
    evictToBudgetLocked();
}

void CookCache::evictToBudgetLocked() {
    // Never evict the entry that was just inserted: a single result larger than
    // the whole budget must still be returnable.
    while (used_ > budget_ && lru_.size() > 1) {
        Entry& victim = lru_.back();
        used_ -= victim.bytes;
        index_.erase(victim.key);
        lru_.pop_back();
        ++evictions_;
    }
}

void CookCache::clear() {
    std::lock_guard<std::mutex> lk(mu_);
    lru_.clear();
    index_.clear();
    used_ = 0;
}

// --- CookEngine ------------------------------------------------------------

CookEngine::CookEngine(size_t cacheBudgetBytes) : cache_(cacheBudgetBytes) {}

bool CookEngine::computeTimeDependent(const Node& node, TimeDepMap& memo) {
    if (auto it = memo.find(&node); it != memo.end()) return it->second;
    memo[&node] = false;  // guard against a malformed graph

    bool td = node.isTimeDependentSelf();
    for (size_t i = 0; i < node.inputCount(); ++i) {
        if (const Node* in = node.input(i)) {
            if (computeTimeDependent(*in, memo)) td = true;
        }
    }
    memo[&node] = td;
    return td;
}

bool CookEngine::isTimeDependent(const Node& node) const {
    TimeDepMap memo;
    return computeTimeDependent(node, memo);
}

GeometryPtr CookEngine::cook(Node& node, const CookContext& ctx) {
    // Resolved once, up front, and then read-only. Computing it lazily inside
    // the recursion would race as soon as branches cook in parallel.
    TimeDepMap td;
    computeTimeDependent(node, td);
    return cookRecursive(node, ctx, td);
}

GeometryPtr CookEngine::cookRecursive(Node& node, const CookContext& ctx,
                                      const TimeDepMap& td) {
    bool timeDep = true;
    if (auto it = td.find(&node); it != td.end()) timeDep = it->second;

    const CacheKey key{&node, node.version(), timeDep ? ctx.frame : kAnyFrame};
    if (GeometryPtr hit = cache_.find(key)) return hit;

    const size_t inputCount = node.inputCount();
    std::vector<GeometryPtr> inputs(inputCount);

    if (parallelBranches_ && inputCount > 1) {
        TaskPool::instance().run(inputCount, [&](size_t i) {
            if (Node* src = node.input(i)) inputs[i] = cookRecursive(*src, ctx, td);
        });
    } else {
        for (size_t i = 0; i < inputCount; ++i) {
            if (Node* src = node.input(i)) inputs[i] = cookRecursive(*src, ctx, td);
        }
    }

    GeometryPtr out = node.cookInstrumented(ctx, inputs);
    cache_.insert(key, out, out ? out->memoryUsage() : 0);
    return out;
}

}  // namespace pg
