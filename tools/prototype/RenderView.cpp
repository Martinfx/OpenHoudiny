#include "RenderView.h"

#include "pg/render/Save.h"
#include "pg/sim/WaterMesh.h"

namespace pg::editor {

namespace {

using Clock = std::chrono::steady_clock;

/// A newer scene than the one rendered waits for its first picture no
/// longer than this: one slow to come does not hold the tab still.
constexpr auto kFirstPicture = std::chrono::seconds(3);

}  // namespace

RenderView::Engine RenderView::defaultEngine() {
    return render::cyclesAvailable() ? Engine::Cycles : Engine::PathTracer;
}

const char* RenderView::engineName(Engine engine) { return engine == Engine::Cycles ? "Cycles" : "Path tracer"; }

// Started once everything it uses is made: the members after thread_ are
// not yet while thread_ is.
RenderView::RenderView() { thread_ = std::thread([this] { run(); }); }

RenderView::~RenderView() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        quit_ = true;
    }
    interrupt_ = true;
    wake_.notify_all();
    thread_.join();
}

void RenderView::request(Request request) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        next_ = std::move(request);
        pending_ = true;
        // A scene that has not shown a picture yet finishes its first.
        if (shown_) interrupt_ = true;
    }
    wake_.notify_all();
}

void RenderView::setPaused(bool paused) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        paused_ = paused;
        if (cycles_) cycles_->setPaused(paused);
    }
    if (paused) interrupt_ = true;
    wake_.notify_all();
}

void RenderView::restart() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        restart_ = true;
    }
    interrupt_ = true;
    wake_.notify_all();
}

void RenderView::setEngine(Engine engine) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (engine_ == engine) return;
        engine_ = engine;
        engineChanged_ = true;
    }
    interrupt_ = true;
    wake_.notify_all();
}

RenderView::Engine RenderView::engine() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return engine_;
}

bool RenderView::takePicture(std::vector<uint8_t>& rgba, int& width, int& height) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!fresh_) return false;
    rgba = picture_;
    width = pictureWidth_;
    height = pictureHeight_;
    fresh_ = false;
    return true;
}

RenderView::Status RenderView::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    Status s;
    s.building = building_;
    if (engine_ == Engine::Cycles) {
        s.of = cyclesSettings_.samples;
        if (cycles_ && cyclesScene_) {
            s.samples = cycles_->samples();
            s.seconds = cycles_->seconds();
            s.rendering = !paused_ && !building_ && !cycles_->done();
            s.error = cycles_->error();
        }
    } else {
        s.samples = tracer_.samples();
        s.of = tracer_.settings().samples;
        s.seconds = tracer_.seconds();
        s.paths = tracer_.paths();
        s.rendering = !paused_ && !building_ && tracer_.scene() && !tracer_.done();
    }
    if (s.error.empty()) s.error = error_;
    return s;
}

bool RenderView::save(const std::string& path, const std::string& comment, std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (engine_ == Engine::Cycles) {
        if (!cycles_ || !cyclesScene_) {
            error = "nothing rendered yet";
            return false;
        }
        render::Rendered r;
        r.beauty = cycles_->beauty();
        r.albedo = cycles_->albedo();
        r.normal = cycles_->normal();
        r.depth = cycles_->depth();
        r.exposure = cyclesScene_->look.exposure;
        r.view = cyclesSettings_.view;
        return render::savePicture(r, path, comment, error);
    }
    return render::savePicture(tracer_, path, tracer_.settings().denoise, comment, error);
}

std::shared_ptr<const render::Scene> RenderView::build(Request& request) {
    // The pieces, the cloth and the water of the frame made into meshes,
    // then the hierarchies.
    render::SceneInput& in = request.input;
    if (request.frame) {
        in.bodies = sim::drawnBodies(*request.frame, in.look);
        if (!request.frame->water.empty() && in.look.waterSurface) {
            in.water = sim::waterMesh(request.frame->water, &request.frame->rain);
        }
        in.frame = request.frame;
    }
    return builder_.build(in);
}

void RenderView::publish(const render::Image& image, float exposure, render::Settings::View view) {
    std::vector<uint8_t> picture = render::toDisplay(image, exposure, view);
    std::lock_guard<std::mutex> lock(mutex_);
    if (restart_) return;
    picture_ = std::move(picture);
    pictureWidth_ = image.width;
    pictureHeight_ = image.height;
    fresh_ = true;
    shown_ = true;
}

void RenderView::run() {
    for (;;) {
        Request request;
        bool rebuild = false, again = false;
        Engine engine;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            // A newer scene is taken once the one rendered has shown a
            // picture -- or has taken too long to.
            auto takeNext = [&] { return pending_ && (shown_ || paused_ || Clock::now() - started_ > kFirstPicture); };
            auto work = [&] {
                if (quit_ || restart_ || engineChanged_ || takeNext()) return true;
                if (paused_) return false;
                return engine_ == Engine::PathTracer && tracer_.scene() && !tracer_.done();
            };
            if (engine_ == Engine::Cycles && cycles_ && cyclesScene_) {
                // Cycles renders on threads of its own: its pictures are
                // taken as they come.
                wake_.wait_for(lock, std::chrono::milliseconds(25), work);
            } else if (pending_ && !shown_) {
                wake_.wait_for(lock, std::chrono::milliseconds(50), work);
            } else {
                wake_.wait(lock, work);
            }
            if (quit_) {
                if (cycles_) cycles_->cancel();
                return;
            }
            interrupt_ = false;
            if (engineChanged_) {
                // The same scene again, with the other.
                engineChanged_ = false;
                if (!pending_ && last_.scene != 0) {
                    next_ = last_;
                    pending_ = true;
                }
                scene_ = ~0ull;
                if (cycles_) cycles_->cancel();
                cyclesScene_.reset();
                tracer_.setScene(nullptr);
                shown_ = true;
            }
            engine = engine_;
            if (takeNext()) {
                request = std::move(next_);
                pending_ = false;
                const bool rendering = engine == Engine::Cycles ? cyclesScene_ != nullptr : tracer_.scene() != nullptr;
                rebuild = request.scene != scene_ || !rendering;
                if (!rebuild) {
                    // The same scene, other settings.
                    if (engine == Engine::PathTracer) {
                        tracer_.setSettings(request.settings);
                    } else if (!(request.settings == cyclesSettings_)) {
                        cyclesSettings_ = request.settings;
                        again = true;
                    }
                    last_.settings = request.settings;
                    request = Request();
                }
            }
            if (restart_) {
                restart_ = false;
                if (engine == Engine::PathTracer) tracer_.restart();
                else again = true;
            }
            if (rebuild) building_ = true;
        }
        if (rebuild) {
            Request kept = request;
            std::shared_ptr<const render::Scene> scene = build(request);
            if (engine == Engine::Cycles) {
                if (!cycles_) {
                    auto made = std::make_unique<render::CyclesRender>(true);
                    std::lock_guard<std::mutex> lock(mutex_);
                    cycles_ = std::move(made);
                }
                // Only this thread starts it: off the lock.
                cycles_->start(scene, request.settings);
            }
            std::lock_guard<std::mutex> lock(mutex_);
            building_ = false;
            if (engine == Engine::PathTracer) {
                tracer_.setSettings(request.settings);
                tracer_.setScene(std::move(scene));
            } else {
                cyclesScene_ = std::move(scene);
                cyclesSettings_ = request.settings;
                if (paused_) cycles_->setPaused(true);
            }
            scene_ = request.scene;
            last_ = std::move(kept);
            shown_ = false;
            started_ = Clock::now();
            error_.clear();
            continue;
        }
        if (engine == Engine::Cycles) {
            if (again && cycles_) {
                std::shared_ptr<const render::Scene> scene;
                render::Settings s;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    scene = cyclesScene_;
                    s = cyclesSettings_;
                    shown_ = false;
                    started_ = Clock::now();
                }
                if (scene) cycles_->start(scene, s);
            }
            render::Image image;
            if (cycles_ && cycles_->takePicture(image)) {
                float exposure = 1.0f;
                render::Settings::View view;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (cyclesScene_) exposure = cyclesScene_->look.exposure;
                    view = cyclesSettings_.view;
                }
                publish(image, exposure, view);
            }
            continue;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (paused_ || !tracer_.scene() || tracer_.done() || restart_) continue;
            if (pending_ && shown_) continue;
        }
        // A pass -- the first of a scene not stopped for a newer one; the
        // picture of all the passes so far, the noise taken out after the
        // first pass, the last, and in between when the passes since the
        // last took as long as taking the noise out does: never more than
        // half the time. Between them the last clean picture stays.
        const bool first = !shown_;
        const double before = tracer_.seconds();
        if (!tracer_.pass(first ? nullptr : &interrupt_)) continue;
        // Only this thread changes the tracer: its picture is taken off the lock.
        const int n = tracer_.samples();
        const render::Settings s = tracer_.settings();
        if (n <= 1) sinceDenoise_ = 0.0;
        sinceDenoise_ += tracer_.seconds() - before;
        const bool denoise = s.denoise && (n <= 1 || n >= s.samples || sinceDenoise_ >= denoiseCost_);
        if (s.denoise && !denoise) continue;
        const auto start = Clock::now();
        const render::Image image = denoise ? tracer_.denoised() : tracer_.beauty();
        if (denoise) {
            denoiseCost_ = std::chrono::duration<double>(Clock::now() - start).count();
            sinceDenoise_ = 0.0;
        }
        publish(image, tracer_.scene() ? tracer_.scene()->look.exposure : 1.0f, s.view);
    }
}

}  // namespace pg::editor
