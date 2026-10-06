#include "pg/sim/Cooker.h"

#include <chrono>

namespace pg::sim {

Cooker::Cooker(FrameSource frames) : frames_(std::move(frames)) {
    thread_ = std::thread([this] { loop(); });
}

Cooker::~Cooker() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        stop_ = true;
    }
    interrupt_.store(true, std::memory_order_relaxed);
    wake_.notify_all();
    thread_.join();
}

uint64_t Cooker::submit(Request request) {
    uint64_t serial;
    {
        std::lock_guard<std::mutex> lock(mu_);
        next_ = std::move(request);
        pending_ = true;
        serial = ++submitted_;
    }
    interrupt_.store(true, std::memory_order_relaxed);  // what is under way is not wanted now
    wake_.notify_all();
    return serial;
}

bool Cooker::take(Result& out) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!ready_) return false;
    out = std::move(result_);
    ready_ = false;
    return true;
}

void Cooker::wait() {
    std::unique_lock<std::mutex> lock(mu_);
    done_.wait(lock, [&] { return stop_ || finished_ == submitted_; });
}

bool Cooker::busy() const {
    std::lock_guard<std::mutex> lock(mu_);
    return finished_ != submitted_;
}

void Cooker::loop() {
    for (;;) {
        Request request;
        uint64_t serial = 0;
        {
            std::unique_lock<std::mutex> lock(mu_);
            wake_.wait(lock, [&] { return stop_ || pending_; });
            if (stop_) return;
            request = std::move(next_);
            pending_ = false;
            serial = submitted_;
            interrupt_.store(false, std::memory_order_relaxed);
        }
        const auto start = std::chrono::steady_clock::now();
        Result result = cook(request);
        result.serial = serial;
        result.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        {
            std::lock_guard<std::mutex> lock(mu_);
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

Cooker::Result Cooker::cook(const Request& r) {
    Result out;
    if (r.levels.empty()) return out;
    const std::atomic<bool>* stop = &interrupt_;
    // The graphs of the levels that stay; a new one for a level that is new.
    if (graphs_.size() > r.levels.size()) graphs_.resize(r.levels.size());
    while (graphs_.size() < r.levels.size()) {
        graphs_.push_back(std::make_unique<GeometryGraph>());
        graphs_.back()->setFrames(frames_);
    }
    for (size_t i = 0; i < r.levels.size(); ++i) {
        const Level& level = r.levels[i];
        GeometryGraph& g = *graphs_[i];
        if (i > 0) {
            // What comes into the instance gone into, cooked a level up.
            const Level& up = r.levels[i - 1];
            const Node* inst = up.net->node(up.instance);
            const NodeType* t = inst ? findNodeType(inst->type) : nullptr;
            std::vector<GeometryPtr> inputs;
            for (size_t k = 0; t && k < t->inputs.size(); ++k) {
                const std::vector<Link> in = up.net->linksInto(up.instance, t->inputs[k].name);
                inputs.push_back(in.empty() ? nullptr : graphs_[i - 1]->cook(in.front().from, r.frame, r.timeStep, stop));
            }
            if (interrupt_.load(std::memory_order_relaxed)) return out;
            g.setInputs(std::move(inputs));
        }
        g.sync(*level.net, level.folder);
    }
    held_ = r.levels;
    GeometryGraph& last = *graphs_.back();
    for (const int id : r.nodes) {
        if (!last.contains(id) || out.geometry.count(id)) continue;
        GeometryPtr g = last.cook(id, r.frame, r.timeStep, stop);
        if (interrupt_.load(std::memory_order_relaxed)) return out;
        out.geometry[id] = std::move(g);
    }
    // What is shown, made ready to draw: not given up halfway -- thrown
    // away with the rest if a newer request came meanwhile.
    if (r.prepare) {
        const auto shown = out.geometry.find(r.prepare);
        if (shown != out.geometry.end() && shown->second) out.prepared = preparer_.prepare(shown->second);
    }
    for (const Node& n : r.levels.back().net->nodes()) {
        if (!last.contains(n.id)) continue;
        std::string e = last.error(n.id);
        if (!e.empty()) out.errors[n.id] = std::move(e);
        std::string w = last.warning(n.id);
        if (!w.empty()) out.warnings[n.id] = std::move(w);
        std::string l = last.log(n.id);
        if (!l.empty()) out.logs[n.id] = std::move(l);
    }
    return out;
}

}  // namespace pg::sim
