#include "pg/sim/Compiler.h"

#include <chrono>

namespace pg::sim {

Compiler::Compiler(FrameSource frames) {
    graph_.setFrames(std::move(frames));
    thread_ = std::thread([this] { loop(); });
}

Compiler::~Compiler() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        stop_ = true;
    }
    interrupt_.store(true, std::memory_order_relaxed);
    wake_.notify_all();
    thread_.join();
}

uint64_t Compiler::submit(std::shared_ptr<const Network> net, std::string folder) {
    uint64_t serial;
    {
        std::lock_guard<std::mutex> lock(mu_);
        next_ = std::move(net);
        nextFolder_ = std::move(folder);
        pending_ = true;
        serial = ++submitted_;
        giveUpIfStaleLocked();
    }
    wake_.notify_all();
    return serial;
}

void Compiler::giveUpIfStale() {
    std::lock_guard<std::mutex> lock(mu_);
    giveUpIfStaleLocked();
}

void Compiler::giveUpIfStaleLocked() {
    if (!pending_ || !working_) return;
    const double ran = std::chrono::duration<double>(std::chrono::steady_clock::now() - started_).count();
    if (ran >= kPatience) interrupt_.store(true, std::memory_order_relaxed);  // what is under way is not wanted now
}

bool Compiler::take(Result& out) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!ready_) return false;
    out = std::move(result_);
    ready_ = false;
    return true;
}

bool Compiler::wait(double seconds) {
    std::unique_lock<std::mutex> lock(mu_);
    auto done = [&] { return stop_ || finished_ == submitted_; };
    if (seconds < 0.0) {
        if (pending_ && working_) interrupt_.store(true, std::memory_order_relaxed);
        done_.wait(lock, done);
        return true;
    }
    return done_.wait_for(lock, std::chrono::duration<double>(seconds), done);
}

bool Compiler::busy() const {
    std::lock_guard<std::mutex> lock(mu_);
    return finished_ != submitted_;
}

void Compiler::loop() {
    for (;;) {
        std::shared_ptr<const Network> net;
        std::string folder;
        uint64_t serial = 0;
        {
            std::unique_lock<std::mutex> lock(mu_);
            wake_.wait(lock, [&] { return stop_ || pending_; });
            if (stop_) return;
            net = std::move(next_);
            folder = std::move(nextFolder_);
            pending_ = false;
            serial = submitted_;
            working_ = true;
            started_ = std::chrono::steady_clock::now();
            interrupt_.store(false, std::memory_order_relaxed);
        }
        const auto start = std::chrono::steady_clock::now();
        Result result;
        result.compiled = net->compile(folder, &graph_, &interrupt_);
        held_ = net;
        result.serial = serial;
        result.revision = net->revision();
        result.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        {
            std::lock_guard<std::mutex> lock(mu_);
            working_ = false;
            // Given up, it is not whole: thrown away. Whole, it is taken even
            // with a newer one waiting -- the window shows it meanwhile.
            if (!interrupt_.load(std::memory_order_relaxed)) {
                result_ = std::move(result);
                ready_ = true;
            }
            // Given up or not, it is out of the way: the next one, if any, is what counts.
            if (!pending_) finished_ = submitted_;
        }
        done_.notify_all();
    }
}

}  // namespace pg::sim
