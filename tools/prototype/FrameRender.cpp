#include "FrameRender.h"

#include "pg/render/Save.h"
#include "pg/sim/WaterMesh.h"

#include <memory>

namespace pg::editor {

FrameRender::~FrameRender() {
    cancel();
    if (thread_.joinable()) thread_.join();
}

void FrameRender::start(RenderView::Engine engine, RenderView::Request request) {
    cancel();
    if (thread_.joinable()) thread_.join();
    stop_ = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_ = State::Building;
        samples_ = 0;
        of_ = request.settings.samples;
        rgb_.clear();
        error_.clear();
        started_ = std::chrono::steady_clock::now();
    }
    thread_ = std::thread([this, engine, r = std::move(request)]() mutable { run(engine, std::move(r)); });
}

void FrameRender::cancel() {
    stop_ = true;
    // Cycles stops on its own threads: told here, its wait() returns.
    std::lock_guard<std::mutex> lock(mutex_);
    if (cycles_) cycles_->cancel();
}

FrameRender::Progress FrameRender::progress() const {
    std::lock_guard<std::mutex> lock(mutex_);
    Progress p;
    p.state = state_;
    p.samples = cycles_ ? cycles_->samples() : samples_;
    p.of = of_;
    p.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started_).count();
    return p;
}

bool FrameRender::take(std::vector<uint8_t>& rgb, std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != State::Done) return false;
    state_ = State::Idle;
    if (!error_.empty() || rgb_.empty()) {
        error = error_.empty() ? "no picture" : error_;
        return false;
    }
    rgb = std::move(rgb_);
    rgb_.clear();
    return true;
}

void FrameRender::run(RenderView::Engine engine, RenderView::Request request) {
    // The pieces, the cloth and the water of the frame made into meshes,
    // then the hierarchies -- as the Render tab makes its scene.
    render::SceneInput& in = request.input;
    if (request.frame) {
        in.bodies = sim::drawnBodies(*request.frame, in.look);
        if (!request.frame->water.empty() && in.look.waterSurface) {
            in.water = sim::waterMesh(request.frame->water, &request.frame->rain);
        }
        in.frame = request.frame;
    }
    const std::shared_ptr<const render::Scene> scene = builder_.build(in);
    const render::Settings& s = request.settings;
    std::vector<uint8_t> rgb;
    std::string error;
    if (!stop_ && engine == RenderView::Engine::Cycles) {
        // A session of its own, to the end, as the command line renders:
        // made here, off the lock -- the window asks the one under way.
        auto cycles = std::make_unique<render::CyclesRender>(false);
        cycles->start(scene, s);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            cycles_ = cycles.get();
            state_ = State::Rendering;
        }
        if (stop_) cycles->cancel();  // a stop that came while it started
        cycles->wait();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            samples_ = cycles->samples();
            cycles_ = nullptr;
        }
        if (!stop_) {
            const render::Rendered out = cycles->rendered();
            if (!cycles->error().empty()) error = "Cycles: " + cycles->error();
            else if (out.beauty.width != s.width || out.beauty.height != s.height) error = "Cycles gave no picture";
            else rgb = render::displayRgb(out);
        }
    } else if (!stop_) {
        tracer_.setSettings(s);
        tracer_.setScene(scene);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            state_ = State::Rendering;
        }
        while (!stop_ && tracer_.samples() < s.samples) {
            if (!tracer_.pass(&stop_)) break;
            std::lock_guard<std::mutex> lock(mutex_);
            samples_ = tracer_.samples();
        }
        if (!stop_) rgb = render::displayRgb(tracer_, s.denoise);
        tracer_.setScene(nullptr);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    state_ = State::Done;
    rgb_ = std::move(rgb);
    error_ = stop_ ? std::string("stopped") : error;
}

}  // namespace pg::editor
