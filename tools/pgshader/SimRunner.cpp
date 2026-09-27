#include "SimRunner.h"

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

void SimRunner::set(const sim::Scene& scene, int frames) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        const sim::Scene safe = scene.sanitized();
        if (!started_ || !(safe == scene_)) {
            started_ = true;
            scene_ = safe;
            fresh_ = true;
            cache_.clear();
            bytes_ = 0;
            ++generation_;
            domain_ = safe.solver.domain();
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

void SimRunner::setRunning(bool on) {
    running_ = on;
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
    return static_cast<int>(cache_.size());
}

std::shared_ptr<const sim::Frame> SimRunner::frame(int number) const {
    std::lock_guard<std::mutex> lock(mu_);
    if (number < 1 || number > static_cast<int>(cache_.size())) return nullptr;
    return cache_[static_cast<size_t>(number - 1)];
}

bool SimRunner::busy() const {
    std::lock_guard<std::mutex> lock(mu_);
    return running_ && (fresh_ || static_cast<int>(cache_.size()) < frames_) && bytes_ < budget_;
}

bool SimRunner::full() const {
    std::lock_guard<std::mutex> lock(mu_);
    return bytes_ >= budget_ && static_cast<int>(cache_.size()) < frames_;
}

size_t SimRunner::bytes() const {
    std::lock_guard<std::mutex> lock(mu_);
    return bytes_;
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
    sim::Scene scene;
    {
        std::lock_guard<std::mutex> lock(mu_);
        const bool more = fresh_ || static_cast<int>(cache_.size()) < frames_;
        if (!running_ || !more || bytes_ >= budget_) return false;
        if (fresh_) {
            scene = scene_;
            restart = true;
            fresh_ = false;
        }
        generation = generation_;
    }
    // A new scene: from the start, with a solver made off the lock -- it
    // allocates the grids, which takes a moment.
    if (restart) solver_ = std::make_unique<sim::PyroSolver>(scene);
    if (!solver_) return false;
    const auto t0 = std::chrono::steady_clock::now();
    solver_->step();
    auto f = std::make_shared<sim::Frame>(sim::capture(*solver_));
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    f->stepMs = ms;
    std::lock_guard<std::mutex> lock(mu_);
    // The scene changed while this frame was simulated: it belongs to no one.
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
                return quit_ ||
                       (running_ && (fresh_ || static_cast<int>(cache_.size()) < frames_) && bytes_ < budget_);
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
