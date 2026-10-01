#include "RenderView.h"

#include "pg/render/Save.h"
#include "pg/sim/WaterMesh.h"

namespace pg::editor {

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
    }
    interrupt_ = true;
    wake_.notify_all();
}

void RenderView::setPaused(bool paused) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        paused_ = paused;
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
    s.samples = tracer_.samples();
    s.of = tracer_.settings().samples;
    s.seconds = tracer_.seconds();
    s.paths = tracer_.paths();
    s.building = building_;
    s.rendering = !paused_ && !building_ && tracer_.scene() && !tracer_.done();
    s.error = error_;
    return s;
}

bool RenderView::save(const std::string& path, const std::string& comment, std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    return render::savePicture(tracer_, path, tracer_.settings().denoise, comment, error);
}

void RenderView::run() {
    for (;;) {
        Request request;
        bool rebuild = false;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            // Waits while there is nothing to do: paused, done, or no scene.
            wake_.wait(lock, [&] {
                return quit_ || pending_ || restart_ || (!paused_ && tracer_.scene() && !tracer_.done());
            });
            if (quit_) return;
            interrupt_ = false;
            if (pending_) {
                request = std::move(next_);
                pending_ = false;
                rebuild = request.scene != scene_ || !tracer_.scene();
                if (!rebuild) {
                    tracer_.setSettings(request.settings);
                    request = Request();
                }
            }
            if (restart_) {
                restart_ = false;
                tracer_.restart();
            }
            if (rebuild) building_ = true;
        }
        if (rebuild) {
            // The scene, off the lock: the pieces, the cloth and the water
            // of the frame made into meshes, then the hierarchies.
            render::SceneInput& in = request.input;
            if (request.frame) {
                in.bodies = sim::drawnBodies(*request.frame, in.look);
                if (!request.frame->water.empty() && in.look.waterSurface) {
                    in.water = sim::waterMesh(request.frame->water, &request.frame->rain);
                }
                in.gas = request.frame;
            }
            std::shared_ptr<const render::Scene> scene = builder_.build(in);
            std::lock_guard<std::mutex> lock(mutex_);
            building_ = false;
            // Asked for another while this was made: that one next.
            if (pending_) continue;
            tracer_.setSettings(request.settings);
            tracer_.setScene(std::move(scene));
            scene_ = request.scene;
            error_.clear();
            continue;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (paused_ || !tracer_.scene() || tracer_.done() || pending_ || restart_) continue;
        }
        // A pass; the picture of all the passes so far, the noise taken out
        // -- of a large one now and then: the first passes, every eighth, the last.
        if (!tracer_.pass(&interrupt_)) continue;
        // Only this thread changes the tracer: its picture is taken off the lock.
        const int n = tracer_.samples();
        const render::Settings s = tracer_.settings();
        const bool small = static_cast<int64_t>(s.width) * s.height <= 1 << 20;
        const bool denoise = s.denoise && (small || n <= 4 || n % 8 == 0 || n >= s.samples);
        std::vector<uint8_t> picture = tracer_.display(denoise);
        std::lock_guard<std::mutex> lock(mutex_);
        if (pending_ || restart_) continue;
        picture_ = std::move(picture);
        pictureWidth_ = s.width;
        pictureHeight_ = s.height;
        fresh_ = true;
    }
}

}  // namespace pg::editor
