#include "pg/core/Parallel.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

namespace pg {

// Bound on chunk count. Keeps scheduling overhead sane for huge inputs while
// staying a pure function of `count` (so still deterministic).
static constexpr size_t kMaxChunks = 4096;

std::vector<std::pair<size_t, size_t>> chunkRanges(size_t count, size_t grain) {
    std::vector<std::pair<size_t, size_t>> out;
    if (count == 0) return out;
    if (grain == 0) grain = 1;

    size_t chunks = (count + grain - 1) / grain;
    if (chunks > kMaxChunks) {
        chunks = kMaxChunks;
        grain = (count + chunks - 1) / chunks;
        chunks = (count + grain - 1) / grain;
    }

    out.reserve(chunks);
    for (size_t begin = 0; begin < count; begin += grain) {
        out.emplace_back(begin, std::min(begin + grain, count));
    }
    return out;
}

struct TaskPool::Batch {
    std::function<void(size_t)> fn;
    size_t count = 0;
    std::atomic<size_t> next{0};
    std::atomic<size_t> done{0};
};

struct TaskPool::Impl {
    std::mutex mu;
    std::condition_variable cv;
    std::vector<std::shared_ptr<Batch>> batches;
    std::vector<std::thread> workers;
    std::atomic<bool> stop{false};
};

TaskPool::TaskPool() : impl_(std::make_unique<Impl>()) {
    unsigned hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 1;
    startWorkers(hw > 1 ? hw - 1 : 0);
}

TaskPool::~TaskPool() { stopWorkers(); }

TaskPool& TaskPool::instance() {
    static TaskPool pool;
    return pool;
}

void TaskPool::startWorkers(unsigned workers) {
    impl_->stop.store(false);
    workerCount_ = workers;
    impl_->workers.reserve(workers);
    for (unsigned i = 0; i < workers; ++i) {
        impl_->workers.emplace_back([this] { workerLoop(); });
    }
}

void TaskPool::stopWorkers() {
    impl_->stop.store(true);
    impl_->cv.notify_all();
    for (auto& t : impl_->workers) {
        if (t.joinable()) t.join();
    }
    impl_->workers.clear();
    workerCount_ = 0;
}

void TaskPool::setThreadCount(unsigned n) {
    if (n == 0) n = 1;
    if (n == threadCount()) return;
    stopWorkers();
    startWorkers(n - 1);
}

bool TaskPool::tryRunOne() {
    std::shared_ptr<Batch> batch;
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        for (auto& b : impl_->batches) {
            if (b->next.load(std::memory_order_relaxed) < b->count) {
                batch = b;
                break;
            }
        }
    }
    if (!batch) return false;

    const size_t i = batch->next.fetch_add(1, std::memory_order_relaxed);
    if (i >= batch->count) return false;  // lost the race, try again

    batch->fn(i);
    batch->done.fetch_add(1, std::memory_order_release);
    return true;
}

void TaskPool::workerLoop() {
    using namespace std::chrono_literals;
    while (!impl_->stop.load(std::memory_order_relaxed)) {
        if (tryRunOne()) continue;
        std::unique_lock<std::mutex> lk(impl_->mu);
        // Timed wait rather than a strict predicate: a spurious extra wakeup
        // costs one cheap scan, a missed wakeup would cost a hang.
        impl_->cv.wait_for(lk, 1ms);
    }
}

void TaskPool::run(size_t n, const std::function<void(size_t)>& task) {
    if (n == 0) return;
    if (n == 1 || workerCount_ == 0) {
        for (size_t i = 0; i < n; ++i) task(i);
        return;
    }

    auto batch = std::make_shared<Batch>();
    batch->fn = task;
    batch->count = n;

    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        impl_->batches.push_back(batch);
    }
    impl_->cv.notify_all();

    // The calling thread helps instead of blocking. This is what makes nested
    // run() calls safe: no thread can be parked waiting on work it could run.
    while (batch->done.load(std::memory_order_acquire) < n) {
        if (!tryRunOne()) std::this_thread::yield();
    }

    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        impl_->batches.erase(
            std::remove(impl_->batches.begin(), impl_->batches.end(), batch),
            impl_->batches.end());
    }
}

void parallelFor(size_t count, size_t grain,
                 const std::function<void(size_t, size_t)>& body) {
    const auto chunks = chunkRanges(count, grain);
    if (chunks.empty()) return;
    if (chunks.size() == 1) {
        body(chunks[0].first, chunks[0].second);
        return;
    }
    TaskPool::instance().run(chunks.size(), [&](size_t c) {
        body(chunks[c].first, chunks[c].second);
    });
}

}  // namespace pg
