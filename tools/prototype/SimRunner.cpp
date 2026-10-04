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

void SimRunner::prepareFor(const sim::World& world) {
    // The pieces and the cloth of this world -- and what is worked out from
    // them, kept from one frame to the next: a frame read back from disk
    // has their poses, not their geometry.
    struct Memo {
        sim::RigidScene rigid;
        sim::ClothScene cloth;
        std::shared_ptr<const sim::RigidLayout> layout;
        std::shared_ptr<const sim::RigidRebar> rebar;
        std::shared_ptr<const sim::RigidGlue> glue;
        std::shared_ptr<const sim::RigidBroken> broken;
    };
    auto memo = std::make_shared<Memo>();
    memo->rigid = world.rigid;
    memo->cloth = world.cloth;
    store_.setPrepare([memo](sim::Frame& f) {
        sim::adoptPieces(f, memo->rigid, &memo->layout, &memo->rebar, &memo->glue, &memo->broken);
        sim::adoptCloth(f, memo->cloth);
    });
}

void SimRunner::set(const sim::World& world, int frames) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        const sim::World safe = world.sanitized();
        if (!started_ || !(safe == world_)) {
            started_ = true;
            world_ = safe;
            fresh_ = true;
            folder_.clear();
            prepareFor(safe);
            store_.clear();
            ++generation_;
            domain_ = safe.hasGas ? safe.gas.solver.domain() : sim::Domain();
        }
        frames_ = std::max(1, frames);
        if (store_.count() > frames_) store_.truncate(frames_);
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
        frames_ = std::max(1, frames);
        folder_ = folder;
        prepareFor(safe);
        store_.stream(folder, 0);
        ++generation_;  // a frame simulated meanwhile belongs to no one
        domain_ = safe.hasGas ? safe.gas.solver.domain() : sim::Domain();
    }
    refresh();
    wake_.notify_all();
}

void SimRunner::refresh() {
    std::string folder;
    int frames = 0;
    {
        std::lock_guard<std::mutex> lock(mu_);
        folder = folder_;
        frames = frames_;
    }
    if (folder.empty()) return;
    sim::CacheInfo info;
    std::string error;
    const int count = sim::readCacheInfo(folder, info, error) ? info.frames : 0;
    std::lock_guard<std::mutex> lock(mu_);
    if (folder != folder_) return;  // streaming another meanwhile
    store_.found(std::clamp(count, 0, frames));
}

std::string SimRunner::folder() const {
    std::lock_guard<std::mutex> lock(mu_);
    return folder_;
}

bool SimRunner::fromDisk() const { return store_.streaming(); }

void SimRunner::clear() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        started_ = false;
        fresh_ = false;
        world_ = sim::World();
        frames_ = 0;
        folder_.clear();
        store_.clear();
        ++generation_;  // a frame simulated meanwhile belongs to no one
        stepMs_ = 0.0;
        domain_ = sim::Domain();
    }
    wake_.notify_all();
}

bool SimRunner::more() const {
    return fresh_ || (!store_.streaming() && store_.count() < frames_);
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
    store_.setBudget(bytes);
    wake_.notify_all();
}

size_t SimRunner::budget() const { return store_.budget(); }

void SimRunner::setSpill(bool on) {
    store_.setSpill(on, size_t(64) << 30);
    wake_.notify_all();
}

bool SimRunner::spill() const { return store_.spilling(); }

int SimRunner::cached() const { return store_.count(); }

std::shared_ptr<const sim::Frame> SimRunner::frame(int number) const { return store_.get(number); }

std::shared_ptr<const sim::Frame> SimRunner::ready(int number) const { return store_.ready(number); }

std::vector<std::pair<int, int>> SimRunner::inMemory() const { return store_.inMemory(); }

void SimRunner::setPlayhead(int frame, int direction) { store_.setPlayhead(frame, direction); }

bool SimRunner::busy() const {
    std::lock_guard<std::mutex> lock(mu_);
    return running_ && !hold_ && more() && store_.room();
}

bool SimRunner::full() const {
    std::lock_guard<std::mutex> lock(mu_);
    return !fresh_ && !store_.streaming() && store_.count() < frames_ && !store_.room();
}

size_t SimRunner::bytes() const { return store_.memoryBytes(); }

int SimRunner::spilledFrames() const { return store_.spilledFrames(); }

size_t SimRunner::spilledBytes() const { return store_.spilledBytes(); }

std::string SimRunner::spillError() const { return store_.spillError(); }

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
        if (!running_ || hold_ || !more() || !store_.room()) return false;
        // Frames cut short and wanted again: the solver is past them, and
        // goes from the start.
        if (!fresh_ && solver_ && solver_->frame() != store_.count()) {
            store_.clear();
            ++generation_;
            fresh_ = true;
        }
        if (fresh_) {
            world = world_;
            restart = true;
            fresh_ = false;
        }
        generation = store_.generation();
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
    // Kept -- spilled to disk, past the budget -- unless the world changed
    // while it was simulated: then it belongs to no one.
    if (store_.add(std::move(f), generation)) {
        std::lock_guard<std::mutex> lock(mu_);
        stepMs_ = ms;
    }
    return true;
}

void SimRunner::loop() {
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(mu_);
            // Waiting on the store's room too: a budget raised, spilling let
            // on, wakes it; a second at most between looks.
            wake_.wait_for(lock, std::chrono::seconds(1), [&] {
                return quit_ || (running_ && !hold_ && more() && store_.room());
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
