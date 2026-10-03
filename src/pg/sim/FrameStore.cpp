#include "pg/sim/FrameStore.h"

#include "pg/sim/Cache.h"

#include <algorithm>
#include <atomic>
#include <climits>
#include <filesystem>
#include <system_error>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace pg::sim {

namespace fs = std::filesystem;

namespace {

long processId() {
#ifdef _WIN32
    return static_cast<long>(_getpid());
#else
    return static_cast<long>(getpid());
#endif
}

/// A folder of its own for a store's spilled frames, in the system's
/// temporary folder: the process and a count make it one no other store
/// uses.
std::string newSpillFolder() {
    static std::atomic<unsigned> made{0};
    std::error_code ec;
    fs::path root = fs::temp_directory_path(ec);
    if (ec) root = ".";
    return (root / ("prototype-frames-" + std::to_string(processId()) + "-" + std::to_string(++made))).string();
}

}  // namespace

FrameStore::FrameStore() : reader_([this] { readLoop(); }) {}

FrameStore::~FrameStore() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        quit_ = true;
        ++generation_;
    }
    wake_.notify_all();
    if (reader_.joinable()) reader_.join();
    dropSpill();
}

void FrameStore::dropSpill() {
    std::string folder;
    {
        std::lock_guard<std::mutex> lock(mu_);
        folder.swap(spillFolder_);
        spilled_ = 0;
        spilledFrames_ = 0;
        spillError_.clear();
    }
    if (folder.empty()) return;
    std::error_code ec;
    fs::remove_all(folder, ec);
}

// --- what is kept -------------------------------------------------------------------

void FrameStore::clear() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        ++generation_;
        slots_.clear();
        memory_ = 0;
        streaming_ = false;
        folder_.clear();
        wanted_ = 0;
    }
    dropSpill();
    wake_.notify_all();
}

void FrameStore::stream(const std::string& folder, int count) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        ++generation_;
        Slot onDisk;
        onDisk.onDisk = true;
        slots_.assign(static_cast<size_t>(std::max(count, 0)), onDisk);
        memory_ = 0;
        streaming_ = true;
        folder_ = folder;
        wanted_ = 0;
    }
    dropSpill();
    wake_.notify_all();
}

void FrameStore::found(int count) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!streaming_ || count <= static_cast<int>(slots_.size())) return;
        Slot onDisk;
        onDisk.onDisk = true;
        slots_.resize(static_cast<size_t>(count), onDisk);
    }
    wake_.notify_all();
}

bool FrameStore::add(std::shared_ptr<const Frame> frame, unsigned generation) {
    std::unique_lock<std::mutex> lock(mu_);
    if (!frame || generation != generation_ || streaming_ || frame->number != static_cast<int>(slots_.size()) + 1) {
        return false;
    }
    Slot s;
    s.bytes = frame->bytes();
    s.frame = std::move(frame);
    memory_ += s.bytes;
    slots_.push_back(std::move(s));
    wake_.notify_all();
    makeRoom(lock, 0);
    return true;
}

void FrameStore::truncate(int count) {
    std::lock_guard<std::mutex> lock(mu_);
    count = std::max(count, 0);
    if (count >= static_cast<int>(slots_.size())) return;
    for (size_t i = static_cast<size_t>(count); i < slots_.size(); ++i) memory_ -= slots_[i].bytes;
    slots_.resize(static_cast<size_t>(count));
}

void FrameStore::setPrepare(Prepare prepare) {
    std::lock_guard<std::mutex> lock(mu_);
    prepare_ = std::move(prepare);
}

void FrameStore::setBudget(size_t bytes) {
    {
        std::unique_lock<std::mutex> lock(mu_);
        budget_ = bytes;
        makeRoom(lock, 0);
    }
    wake_.notify_all();
}

void FrameStore::setSpill(bool on, size_t disk) {
    {
        std::unique_lock<std::mutex> lock(mu_);
        spill_ = on;
        diskBudget_ = disk;
        if (on) spillError_.clear();
        makeRoom(lock, 0);
    }
    wake_.notify_all();
}

bool FrameStore::spilling() const {
    std::lock_guard<std::mutex> lock(mu_);
    return spill_;
}

void FrameStore::setPlayhead(int frame, int direction) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        const int dir = direction < 0 ? -1 : 1;
        if (frame == playhead_ && dir == direction_) return;
        playhead_ = frame;
        direction_ = dir;
    }
    wake_.notify_all();
}

// --- frames ---------------------------------------------------------------------------

unsigned FrameStore::generation() const {
    std::lock_guard<std::mutex> lock(mu_);
    return generation_;
}

int FrameStore::count() const {
    std::lock_guard<std::mutex> lock(mu_);
    return static_cast<int>(slots_.size());
}

std::shared_ptr<const Frame> FrameStore::get(int n) {
    std::unique_lock<std::mutex> lock(mu_);
    for (;;) {
        if (n < 1 || n > static_cast<int>(slots_.size())) return nullptr;
        const Slot& s = slots_[static_cast<size_t>(n - 1)];
        if (s.frame) return s.frame;
        if (s.failed || !s.onDisk) return nullptr;
        if (s.busy) {
            // Being read: wait for it.
            wake_.wait(lock);
            continue;
        }
        readInto(lock, n);
    }
}

std::shared_ptr<const Frame> FrameStore::ready(int n) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (n < 1 || n > static_cast<int>(slots_.size())) return nullptr;
        const Slot& s = slots_[static_cast<size_t>(n - 1)];
        if (s.frame) return s.frame;
        if (wanted_ == n) return nullptr;
        wanted_ = n;
    }
    wake_.notify_all();
    return nullptr;
}

bool FrameStore::inMemory(int n) const {
    std::lock_guard<std::mutex> lock(mu_);
    return n >= 1 && n <= static_cast<int>(slots_.size()) && slots_[static_cast<size_t>(n - 1)].frame != nullptr;
}

std::vector<std::pair<int, int>> FrameStore::inMemory() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<std::pair<int, int>> runs;
    for (int n = 1; n <= static_cast<int>(slots_.size()); ++n) {
        if (!slots_[static_cast<size_t>(n - 1)].frame) continue;
        if (!runs.empty() && runs.back().second == n - 1) runs.back().second = n;
        else runs.emplace_back(n, n);
    }
    return runs;
}

bool FrameStore::room() const {
    std::lock_guard<std::mutex> lock(mu_);
    return !streaming_ && (memory_ < budget_ || canSpill());
}

bool FrameStore::streaming() const {
    std::lock_guard<std::mutex> lock(mu_);
    return streaming_;
}

size_t FrameStore::budget() const {
    std::lock_guard<std::mutex> lock(mu_);
    return budget_;
}

size_t FrameStore::memoryBytes() const {
    std::lock_guard<std::mutex> lock(mu_);
    return memory_;
}

int FrameStore::memoryFrames() const {
    std::lock_guard<std::mutex> lock(mu_);
    return static_cast<int>(std::count_if(slots_.begin(), slots_.end(), [](const Slot& s) { return s.frame != nullptr; }));
}

size_t FrameStore::spilledBytes() const {
    std::lock_guard<std::mutex> lock(mu_);
    return spilled_;
}

int FrameStore::spilledFrames() const {
    std::lock_guard<std::mutex> lock(mu_);
    return spilledFrames_;
}

std::string FrameStore::spillFolder() const {
    std::lock_guard<std::mutex> lock(mu_);
    return spillFolder_;
}

std::string FrameStore::spillError() const {
    std::lock_guard<std::mutex> lock(mu_);
    return spillError_;
}

// --- reading and letting go ----------------------------------------------------------------

long FrameStore::cost(int n) const {
    const long d = static_cast<long>(n - playhead_) * static_cast<long>(direction_);
    return d >= 0 ? d : -2 * d;
}

bool FrameStore::canSpill() const {
    return spill_ && !streaming_ && spillError_.empty() && spilled_ < diskBudget_;
}

int FrameStore::nextToRead() const {
    const int count = static_cast<int>(slots_.size());
    auto readable = [&](int n) {
        const Slot& s = slots_[static_cast<size_t>(n - 1)];
        return !s.frame && s.onDisk && !s.busy && !s.failed;
    };
    // The one asked for, whatever it costs.
    if (wanted_ >= 1 && wanted_ <= count && readable(wanted_)) return wanted_;
    // The nearest the play head, the way it plays: kAhead ahead of it, a
    // fifth of that behind.
    int best = 0;
    long bestCost = LONG_MAX;
    const int ahead = kAhead, behind = kAhead / 5;
    const int from = std::max(1, direction_ > 0 ? playhead_ - behind : playhead_ - ahead);
    const int to = std::min(count, direction_ > 0 ? playhead_ + ahead : playhead_ + behind);
    for (int n = from; n <= to; ++n) {
        if (!readable(n)) continue;
        const long c = cost(n);
        if (c < bestCost) {
            bestCost = c;
            best = n;
        }
    }
    if (!best) return 0;
    // Room for it: within the budget as it is -- a frame is about as big as
    // the last one kept --, or in place of one further from the play head.
    size_t typical = 0;
    for (int n = count; n >= 1 && !typical; --n) typical = slots_[static_cast<size_t>(n - 1)].bytes;
    if (memory_ + typical <= budget_) return best;
    for (int n = 1; n <= count; ++n) {
        const Slot& s = slots_[static_cast<size_t>(n - 1)];
        if (s.frame && !s.busy && n != playhead_ && cost(n) > bestCost && (s.onDisk || canSpill())) return best;
    }
    return 0;
}

void FrameStore::readInto(std::unique_lock<std::mutex>& lock, int n) {
    slots_[static_cast<size_t>(n - 1)].busy = true;
    const unsigned generation = generation_;
    const std::string folder = streaming_ ? folder_ : spillFolder_;
    const Prepare prepare = prepare_;
    lock.unlock();
    auto frame = std::make_shared<Frame>();
    std::string error;
    bool ok = false;
    {
        std::lock_guard<std::mutex> one(io_);
        ok = !folder.empty() && readFrame(folder, n, *frame, error);
        if (ok) {
            frame->number = n;
            if (prepare) prepare(*frame);
        }
    }
    lock.lock();
    // Thrown away, or cut short, meanwhile: it belongs to no one.
    if (generation != generation_ || n > static_cast<int>(slots_.size())) {
        wake_.notify_all();
        return;
    }
    Slot& s = slots_[static_cast<size_t>(n - 1)];
    s.busy = false;
    if (!ok) {
        s.failed = true;
    } else {
        s.bytes = frame->bytes();
        s.frame = std::move(frame);
        memory_ += s.bytes;
    }
    wake_.notify_all();
    if (ok) makeRoom(lock, n);
}

void FrameStore::makeRoom(std::unique_lock<std::mutex>& lock, int keep) {
    while (memory_ > budget_) {
        // The frame furthest from the play head that can go.
        int victim = 0;
        long worst = -1;
        for (int n = 1; n <= static_cast<int>(slots_.size()); ++n) {
            const Slot& s = slots_[static_cast<size_t>(n - 1)];
            if (!s.frame || s.busy || n == keep || n == playhead_) continue;
            if (!s.onDisk && !canSpill()) continue;
            const long c = cost(n);
            if (c > worst) {
                worst = c;
                victim = n;
            }
        }
        if (!victim) return;
        Slot& s = slots_[static_cast<size_t>(victim - 1)];
        if (s.onDisk) {
            memory_ -= s.bytes;
            s.bytes = 0;
            s.frame.reset();
            continue;
        }
        // Nowhere else: written first, with the lock let go -- the frame is
        // still there to be read meanwhile.
        if (spillFolder_.empty()) spillFolder_ = newSpillFolder();
        const std::string folder = spillFolder_;
        const unsigned generation = generation_;
        const std::shared_ptr<const Frame> frame = s.frame;
        s.busy = true;
        lock.unlock();
        std::string error;
        const bool ok = writeFrame(*frame, folder, error);
        std::error_code ec;
        const uintmax_t size = ok ? fs::file_size(frameFile(folder, victim), ec) : 0;
        lock.lock();
        if (generation != generation_) {
            // Thrown away meanwhile: so is what was written -- and the
            // folder, if that leaves it empty.
            fs::remove(frameFile(folder, victim), ec);
            fs::remove(folder, ec);
            wake_.notify_all();
            return;
        }
        if (victim > static_cast<int>(slots_.size())) continue;  // cut short meanwhile
        Slot& t = slots_[static_cast<size_t>(victim - 1)];
        t.busy = false;
        wake_.notify_all();
        if (!ok) {
            spillError_ = error;
            return;
        }
        t.onDisk = true;
        spilled_ += static_cast<size_t>(ec ? 0 : size);
        ++spilledFrames_;
    }
}

void FrameStore::readLoop() {
    std::unique_lock<std::mutex> lock(mu_);
    for (;;) {
        wake_.wait(lock, [&] { return quit_ || nextToRead() != 0; });
        if (quit_) return;
        const int n = nextToRead();
        if (n) readInto(lock, n);
    }
}

}  // namespace pg::sim
