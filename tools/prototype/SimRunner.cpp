#include "SimRunner.h"

#include "pg/sim/Cache.h"

#include <algorithm>
#include <chrono>

namespace pg::editor {

SimRunner::SimRunner(bool synchronous) : synchronous_(synchronous) {
    if (!synchronous_) thread_ = std::thread([this] { loop(); });
}

SimRunner::~SimRunner() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        quit_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void SimRunner::set(const sim::World& world, int frames) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        const sim::World safe = world.sanitized();
        if (!started_ || !(safe == world_)) {
            started_ = true;
            world_ = safe;
            fresh_ = true;
            adopted_ = false;
            cache_.clear();
            bytes_ = 0;
            folder_.clear();
            read_.clear();
            askedAt_.clear();
            readBytes_ = 0;
            ++generation_;
            domain_ = safe.hasGas ? safe.gas.solver.domain() : sim::Domain();
        }
        frames_ = std::max(1, frames);
        if (static_cast<int>(cache_.size()) > frames_) {
            cache_.resize(static_cast<size_t>(frames_));
            bytes_ = 0;
            for (const auto& f : cache_) bytes_ += f->bytes();
        }
    }
    wake_.notify_all();
}

void SimRunner::adopt(const sim::World& world, int frames, std::vector<std::shared_ptr<const sim::Frame>> loaded) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        const sim::World safe = world.sanitized();
        started_ = true;
        world_ = safe;
        fresh_ = false;  // the thread's solver stays as it is, unused
        adopted_ = true;
        frames_ = std::max(1, frames);
        if (static_cast<int>(loaded.size()) > frames_) loaded.resize(static_cast<size_t>(frames_));
        cache_ = std::move(loaded);
        bytes_ = 0;
        for (const auto& f : cache_) bytes_ += f->bytes();
        folder_.clear();
        read_.clear();
        askedAt_.clear();
        readBytes_ = 0;
        ++generation_;  // a frame simulated meanwhile belongs to no one
        domain_ = safe.hasGas ? safe.gas.solver.domain() : sim::Domain();
    }
    wake_.notify_all();
}

void SimRunner::clear() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        started_ = false;
        fresh_ = false;
        adopted_ = false;
        world_ = sim::World();
        frames_ = 0;
        cache_.clear();
        bytes_ = 0;
        folder_.clear();
        read_.clear();
        askedAt_.clear();
        readBytes_ = 0;
        ++generation_;  // a frame simulated meanwhile belongs to no one
        stepMs_ = 0.0;
        domain_ = sim::Domain();
    }
    wake_.notify_all();
}

void SimRunner::stream(const sim::World& world, int frames, const std::string& folder) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        const sim::World safe = world.sanitized();
        started_ = true;
        world_ = safe;
        fresh_ = false;  // the thread's solver stays as it is, unused
        adopted_ = true;
        frames_ = std::max(1, frames);
        cache_.clear();
        bytes_ = 0;
        folder_ = folder;
        onDisk_ = 0;
        read_.clear();
        askedAt_.clear();
        readBytes_ = 0;
        ++generation_;  // a frame simulated meanwhile belongs to no one
        domain_ = safe.hasGas ? safe.gas.solver.domain() : sim::Domain();
    }
    {
        std::lock_guard<std::mutex> lock(loading_);
        layout_.reset();
        rebar_.reset();
        glue_.reset();
    }
    refresh();
    wake_.notify_all();
}

void SimRunner::refresh() {
    std::string folder;
    {
        std::lock_guard<std::mutex> lock(mu_);
        folder = folder_;
    }
    if (folder.empty()) return;
    sim::CacheInfo info;
    std::string error;
    const int count = sim::readCacheInfo(folder, info, error) ? info.frames : 0;
    std::lock_guard<std::mutex> lock(mu_);
    if (folder != folder_) return;  // streaming another meanwhile
    onDisk_ = std::clamp(count, 0, frames_);
}

std::string SimRunner::folder() const {
    std::lock_guard<std::mutex> lock(mu_);
    return folder_;
}

std::shared_ptr<const sim::Frame> SimRunner::load(int number) const {
    std::string folder;
    sim::RigidScene rigid;
    sim::ClothScene cloth;
    unsigned generation = 0;
    {
        std::lock_guard<std::mutex> lock(mu_);
        folder = folder_;
        rigid = world_.rigid;
        cloth = world_.cloth;
        generation = generation_;
    }
    // One read at a time: two asking for the same frame read it once.
    std::lock_guard<std::mutex> reading(loading_);
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (generation != generation_) return nullptr;
        if (auto it = read_.find(number); it != read_.end()) return it->second;
    }
    auto frame = std::make_shared<sim::Frame>();
    std::string error;
    if (!sim::readFrame(folder, number, *frame, error)) return nullptr;
    frame->number = number;
    sim::adoptPieces(*frame, rigid, &layout_, &rebar_, &glue_);
    sim::adoptCloth(*frame, cloth);
    std::lock_guard<std::mutex> lock(mu_);
    if (generation != generation_) return nullptr;  // another world or folder meanwhile
    read_[number] = frame;
    askedAt_[number] = ++asks_;
    readBytes_ += frame->bytes();
    // Past the budget: the frames asked for longest ago go -- never the one
    // just read.
    while (readBytes_ > budget_ && read_.size() > 1) {
        auto oldest = askedAt_.begin();
        for (auto it = askedAt_.begin(); it != askedAt_.end(); ++it) {
            if (it->second < oldest->second) oldest = it;
        }
        if (oldest->first == number) break;
        readBytes_ -= read_[oldest->first]->bytes();
        read_.erase(oldest->first);
        askedAt_.erase(oldest);
    }
    return frame;
}

bool SimRunner::adopted() const {
    std::lock_guard<std::mutex> lock(mu_);
    return adopted_;
}

bool SimRunner::more() const {
    return fresh_ || (!adopted_ && static_cast<int>(cache_.size()) < frames_);
}

void SimRunner::setRunning(bool on) {
    running_ = on;
    wake_.notify_all();
}

void SimRunner::hold(bool on) {
    if (hold_ == on) return;
    hold_ = on;
    wake_.notify_all();
}

void SimRunner::setBudget(size_t bytes) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        budget_ = bytes;
    }
    wake_.notify_all();
}

int SimRunner::cached() const {
    std::lock_guard<std::mutex> lock(mu_);
    return folder_.empty() ? static_cast<int>(cache_.size()) : onDisk_;
}

std::shared_ptr<const sim::Frame> SimRunner::frame(int number) const {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (folder_.empty()) {
            if (number < 1 || number > static_cast<int>(cache_.size())) return nullptr;
            return cache_[static_cast<size_t>(number - 1)];
        }
        if (number < 1 || number > onDisk_) return nullptr;
        if (auto it = read_.find(number); it != read_.end()) {
            askedAt_[number] = ++asks_;
            return it->second;
        }
    }
    return load(number);
}

bool SimRunner::busy() const {
    std::lock_guard<std::mutex> lock(mu_);
    return running_ && !hold_ && more() && bytes_ < budget_;
}

bool SimRunner::full() const {
    std::lock_guard<std::mutex> lock(mu_);
    return bytes_ >= budget_ && !adopted_ && static_cast<int>(cache_.size()) < frames_;
}

size_t SimRunner::bytes() const {
    std::lock_guard<std::mutex> lock(mu_);
    return folder_.empty() ? bytes_ : readBytes_;
}

double SimRunner::stepMs() const {
    std::lock_guard<std::mutex> lock(mu_);
    return stepMs_;
}

unsigned SimRunner::generation() const {
    std::lock_guard<std::mutex> lock(mu_);
    return generation_;
}

sim::Domain SimRunner::domain() const {
    std::lock_guard<std::mutex> lock(mu_);
    return domain_;
}

bool SimRunner::advance() {
    unsigned generation = 0;
    bool restart = false;
    sim::World world;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!running_ || hold_ || !more() || bytes_ >= budget_) return false;
        if (fresh_) {
            world = world_;
            restart = true;
            fresh_ = false;
        }
        generation = generation_;
    }
    // A new world: from the start, with a solver made off the lock -- it
    // allocates the grids, which takes a moment.
    if (restart) solver_ = std::make_unique<sim::WorldSolver>(world);
    if (!solver_) return false;
    const auto t0 = std::chrono::steady_clock::now();
    solver_->step();
    auto f = std::make_shared<sim::Frame>(solver_->capture());
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    f->stepMs = ms;
    std::lock_guard<std::mutex> lock(mu_);
    // The world changed while this frame was simulated: it belongs to no one.
    if (generation != generation_ || fresh_) return true;
    bytes_ += f->bytes();
    cache_.push_back(std::move(f));
    stepMs_ = ms;
    return true;
}

void SimRunner::loop() {
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(mu_);
            wake_.wait(lock, [&] {
                return quit_ || (running_ && !hold_ && more() && bytes_ < budget_);
            });
            if (quit_) return;
        }
        advance();
    }
}

void SimRunner::step() {
    if (synchronous_) advance();
}

}  // namespace pg::editor
