#pragma once
//
// Deterministic parallelism (invariant I5).
//
// The rule that makes results reproducible: work is split into a number of
// chunks derived ONLY from the element count, never from the thread count.
// Chunks then execute in any order, but
//
//   * parallelFor   writes to disjoint output ranges, so the result cannot
//                   depend on scheduling;
//   * parallelReduce accumulates one partial per chunk and folds the partials
//                   SEQUENTIALLY in chunk order, so float addition -- which is
//                   not associative -- is applied in a fixed order.
//
// A plain `#pragma omp parallel for reduction(+:x)` over floats violates this
// and silently produces thread-count-dependent output. That class of bug is
// found by studios, months later, on a render farm.
//
#include <cstddef>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace pg {

/// Fixed chunk decomposition of [0, count). Depends only on `count` and
/// `grain` -- deliberately not on hardware concurrency.
std::vector<std::pair<size_t, size_t>> chunkRanges(size_t count, size_t grain);

/// Thread pool. Threads that wait for a batch also execute pending tasks, so
/// nested parallelFor (a node parallelising inside a parallel branch cook)
/// cannot deadlock.
class TaskPool {
public:
    static TaskPool& instance();

    unsigned threadCount() const { return workerCount_ + 1; }
    /// Resizes the pool. `n` is the total number of participating threads
    /// (1 == fully serial). Tests use this to prove determinism.
    void setThreadCount(unsigned n);

    /// Runs task(i) for i in [0, n) and returns once all have completed.
    /// The calling thread participates rather than blocking idle.
    void run(size_t n, const std::function<void(size_t)>& task);

    ~TaskPool();

private:
    TaskPool();
    struct Batch;
    void workerLoop();
    bool tryRunOne();
    void stopWorkers();
    void startWorkers(unsigned workers);

    struct Impl;
    std::unique_ptr<Impl> impl_;
    unsigned workerCount_ = 0;
};

/// Parallel iteration over [0, count) in deterministic chunks.
/// `body(begin, end)` processes one half-open chunk.
void parallelFor(size_t count, size_t grain,
                 const std::function<void(size_t begin, size_t end)>& body);

/// Deterministic parallel reduction.
/// `map(begin, end) -> T` produces one partial per chunk.
/// `combine(T, T) -> T` folds partials, always in ascending chunk order.
template <class T, class MapFn, class CombineFn>
T parallelReduce(size_t count, size_t grain, const T& identity,
                 MapFn map, CombineFn combine) {
    const auto chunks = chunkRanges(count, grain);
    if (chunks.empty()) return identity;
    if (chunks.size() == 1) return map(chunks[0].first, chunks[0].second);

    std::vector<T> partials(chunks.size(), identity);
    TaskPool::instance().run(chunks.size(), [&](size_t c) {
        partials[c] = map(chunks[c].first, chunks[c].second);
    });

    T acc = partials[0];
    for (size_t i = 1; i < partials.size(); ++i) acc = combine(acc, partials[i]);
    return acc;
}

}  // namespace pg
