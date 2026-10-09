#include "ViewThread.h"

#include <GLFW/glfw3.h>

#include <chrono>
#include <utility>

namespace pg::editor {
namespace {

GLFWwindow* gShared = nullptr;
bool gPatient = false;

/// How long the window's thread waits for a picture it asks for: one that
/// comes this soon shows the same frame, as it did when it was drawn
/// there; a slower one, at the next frame after it is done.
constexpr double kWaitSeconds = 0.012;

}  // namespace

void ViewThread::setSharedContext(GLFWwindow* context) { gShared = context; }
GLFWwindow* ViewThread::sharedContext() { return gShared; }
void ViewThread::setPatient(bool patient) { gPatient = patient; }

ViewThread::ViewThread(const gl::Api& gl) : gl_(gl), context_(gShared) {
    if (!context_) {
        renderer_ = std::make_unique<gl::VolumeRenderer>(gl);
    } else {
        // Made in the view's context: its vertex arrays are of the context
        // they are made in, not shared.
        thread_ = std::thread([this] { loop(); });
        std::unique_lock<std::mutex> lock(mutex_);
        done_.wait(lock, [&] { return renderer_ != nullptr; });
    }
    look_ = renderer_->look;
    orbit = renderer_->orbit;
}

ViewThread::~ViewThread() {
    if (!thread_.joinable()) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        quit_ = true;
    }
    wake_.notify_all();
    thread_.join();
}

void ViewThread::loop() {
    glfwMakeContextCurrent(context_);
    {
        auto made = std::make_unique<gl::VolumeRenderer>(gl_);
        std::lock_guard<std::mutex> lock(mutex_);
        renderer_ = std::move(made);
        done_.notify_all();
    }
    std::deque<std::function<void(gl::VolumeRenderer&)>> jobs;
    for (;;) {
        Ask ask;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [&] { return quit_ || !jobs_.empty() || ask_.serial > drawn_; });
            if (quit_) break;
            jobs.swap(jobs_);
            ask = ask_;
        }
        runJobs(jobs);
        // Only the latest picture asked for -- those asked before it passed
        // by -- of the jobs that came before it.
        if (ask.serial > drawn_ && ask.width > 0 && ask.height > 0) {
            const auto start = std::chrono::steady_clock::now();
            renderer_->orbit = ask.orbit;
            renderer_->render(ask.width, ask.height);
            publish(*renderer_);
            std::lock_guard<std::mutex> lock(mutex_);
            drawSeconds_ = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            drawn_ = ask.serial;
            done_.notify_all();
        }
    }
    // The renderer's GL objects, and the pictures', go in its context.
    renderer_.reset();
    for (Slot& s : slots_) {
        if (s.released) gl_.DeleteSync(s.released);
        if (s.fbo) gl_.DeleteFramebuffers(1, &s.fbo);
        if (s.texture) gl_.DeleteTextures(1, &s.texture);
    }
    gl_.Finish();
    glfwMakeContextCurrent(nullptr);
}

void ViewThread::runJobs(std::deque<std::function<void(gl::VolumeRenderer&)>>& jobs) {
    if (jobs.empty()) return;
    const size_t n = jobs.size();
    for (auto& job : jobs) job(*renderer_);
    jobs.clear();
    handBack(*renderer_);
    std::lock_guard<std::mutex> lock(mutex_);
    ran_ += n;
    done_.notify_all();
}

void ViewThread::handBack(gl::VolumeRenderer& r) {
    Vec3 lo(0.0f), hi(0.0f);
    const bool bounds = r.geometryBounds(lo, hi);
    std::lock_guard<std::mutex> lock(mutex_);
    prepared_ = r.prepared();
    hasBounds_ = bounds;
    lo_ = lo;
    hi_ = hi;
}

void ViewThread::publish(gl::VolumeRenderer& r) {
    // A picture neither shown nor about to be: drawn into once the window's
    // GPU is done showing it.
    int k = -1;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (int i = 0; i < 3 && k < 0; ++i) {
            if (slots_[i].state == Slot::State::Free) k = i;
        }
        if (k < 0) return;  // cannot be: one shown, one ready at most
        slots_[k].state = Slot::State::Drawing;
    }
    Slot& s = slots_[k];
    if (s.released) {
        gl_.WaitSync(s.released, 0, gl::TIMEOUT_IGNORED);
        gl_.DeleteSync(s.released);
        s.released = nullptr;
    }
    const int w = r.width(), h = r.height();
    if (!s.texture) gl_.GenTextures(1, &s.texture);
    if (!s.fbo) gl_.GenFramebuffers(1, &s.fbo);
    if (s.width != w || s.height != h) {
        gl_.BindTexture(gl::TEXTURE_2D, s.texture);
        gl_.TexImage2D(gl::TEXTURE_2D, 0, static_cast<int>(gl::RGBA8), w, h, 0, gl::RGBA, gl::UNSIGNED_BYTE, nullptr);
        gl_.TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MIN_FILTER, static_cast<int>(gl::LINEAR));
        gl_.TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MAG_FILTER, static_cast<int>(gl::LINEAR));
        gl_.BindTexture(gl::TEXTURE_2D, 0);
        gl_.BindFramebuffer(gl::FRAMEBUFFER, s.fbo);
        gl_.FramebufferTexture2D(gl::FRAMEBUFFER, gl::COLOR_ATTACHMENT0, gl::TEXTURE_2D, s.texture, 0);
        s.width = w;
        s.height = h;
    }
    // The renderer's picture copied over: its own framebuffer stays its.
    GLuint read = 0;
    gl_.GenFramebuffers(1, &read);
    gl_.BindFramebuffer(gl::READ_FRAMEBUFFER, read);
    gl_.FramebufferTexture2D(gl::READ_FRAMEBUFFER, gl::COLOR_ATTACHMENT0, gl::TEXTURE_2D, r.colorTexture(), 0);
    gl_.BindFramebuffer(gl::DRAW_FRAMEBUFFER, s.fbo);
    gl_.BlitFramebuffer(0, 0, w, h, 0, 0, w, h, gl::COLOR_BUFFER_BIT, gl::NEAREST);
    gl_.BindFramebuffer(gl::READ_FRAMEBUFFER, 0);
    gl_.BindFramebuffer(gl::DRAW_FRAMEBUFFER, 0);
    gl_.DeleteFramebuffers(1, &read);
    // Done on the GPU before the window's thread may show it.
    gl_.Finish();
    std::lock_guard<std::mutex> lock(mutex_);
    if (ready_ >= 0) slots_[ready_].state = Slot::State::Free;  // never shown: passed by
    ready_ = k;
    s.state = Slot::State::Ready;
}

unsigned ViewThread::picture() {
    if (!context_) return renderer_->colorTexture();
    std::lock_guard<std::mutex> lock(mutex_);
    if (ready_ >= 0) {
        if (shown_ >= 0) {
            // Drawn into again once what the window's GPU was told up to
            // now -- showing it, the frame before -- is done.
            Slot& old = slots_[shown_];
            old.released = gl_.FenceSync(gl::SYNC_GPU_COMMANDS_COMPLETE, 0);
            gl_.Flush();
            old.state = Slot::State::Free;
        }
        shown_ = ready_;
        ready_ = -1;
        slots_[shown_].state = Slot::State::Shown;
    }
    return shown_ >= 0 ? slots_[shown_].texture : 0;
}

double ViewThread::drawSeconds() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return drawSeconds_;
}

void ViewThread::draw(int width, int height) {
    if (!context_) {
        renderer_->orbit = orbit;
        const auto start = std::chrono::steady_clock::now();
        renderer_->render(width, height);
        drawSeconds_ = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        return;
    }
    std::unique_lock<std::mutex> lock(mutex_);
    ask_.orbit = orbit;
    ask_.width = width;
    ask_.height = height;
    ask_.serial = ++asked_;
    const uint64_t serial = ask_.serial;
    wake_.notify_all();
    if (gPatient) {
        done_.wait(lock, [&] { return drawn_ >= serial; });
    } else {
        done_.wait_for(lock, std::chrono::duration<double>(kWaitSeconds), [&] { return drawn_ >= serial; });
    }
}

bool ViewThread::init(std::string& log) {
    bool ok = false;
    run([&](gl::VolumeRenderer& r) { ok = r.init(log); });
    return ok;
}

void ViewThread::post(std::function<void(gl::VolumeRenderer&)> job) {
    if (!context_) {
        job(*renderer_);
        handBack(*renderer_);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        jobs_.push_back(std::move(job));
        ++posted_;
    }
    wake_.notify_all();
}

void ViewThread::run(const std::function<void(gl::VolumeRenderer&)>& job) {
    if (!context_) {
        job(*renderer_);
        handBack(*renderer_);
        return;
    }
    // Waited for: what it refers to lives on the caller's stack.
    bool finished = false;
    post([&](gl::VolumeRenderer& r) {
        job(r);
        std::lock_guard<std::mutex> lock(mutex_);
        finished = true;
    });
    std::unique_lock<std::mutex> lock(mutex_);
    done_.wait(lock, [&] { return finished; });
}

std::shared_ptr<const sim::PreparedGeometry> ViewThread::prepared() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return prepared_;
}

bool ViewThread::geometryBounds(Vec3& lo, Vec3& hi) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!hasBounds_) return false;
    lo = lo_;
    hi = hi_;
    return true;
}

void ViewThread::setFrame(std::shared_ptr<const sim::Frame> frame) {
    post([frame = std::move(frame)](gl::VolumeRenderer& r) { r.setFrame(*frame); });
}

void ViewThread::setFrame(std::shared_ptr<const sim::Frame> frame, std::shared_ptr<const sim::PreparedVolumes> prepared) {
    post([frame = std::move(frame), prepared = std::move(prepared)](gl::VolumeRenderer& r) { r.setFrame(*frame, *prepared); });
}

void ViewThread::clearFrame() {
    post([](gl::VolumeRenderer& r) { r.clearFrame(); });
}

void ViewThread::setDomain(const sim::Domain& domain) {
    post([domain](gl::VolumeRenderer& r) { r.setDomain(domain); });
}

void ViewThread::setSolids(std::vector<sim::Solid> solids) {
    post([solids = std::move(solids)](gl::VolumeRenderer& r) { r.setSolids(solids); });
}

void ViewThread::setHighlight(std::vector<int> selected, int hovered) {
    post([selected = std::move(selected), hovered](gl::VolumeRenderer& r) { r.setHighlight(selected, hovered); });
}

void ViewThread::setLines(gl::Lines lines) {
    post([lines = std::move(lines)](gl::VolumeRenderer& r) { r.setLines(lines); });
}

void ViewThread::setOverlay(gl::Overlay overlay, int layer) {
    post([overlay = std::move(overlay), layer](gl::VolumeRenderer& r) { r.setOverlay(overlay, layer); });
}

void ViewThread::setOverlayHidden(float alpha) {
    post([alpha](gl::VolumeRenderer& r) { r.setOverlayHidden(alpha); });
}

void ViewThread::setGeometry(const GeometryPtr& geometry) {
    geometry_ = geometry;
    post([geometry](gl::VolumeRenderer& r) { r.setGeometry(geometry); });
}

void ViewThread::setPrepared(std::shared_ptr<const sim::PreparedGeometry> prepared) {
    geometry_ = prepared ? prepared->geometry : nullptr;
    post([prepared = std::move(prepared)](gl::VolumeRenderer& r) { r.setPrepared(prepared); });
}

void ViewThread::setPreparedPieces(std::shared_ptr<const sim::PreparedBodies> pieces) {
    post([pieces = std::move(pieces)](gl::VolumeRenderer& r) { r.setPreparedPieces(pieces); });
}

bool ViewThread::setPlate(const std::string& file, const sim::Camera& camera, std::string& error) {
    bool ok = false, has = false;
    run([&](gl::VolumeRenderer& r) {
        ok = r.setPlate(file, camera, error);
        has = r.hasPlate();
    });
    hasPlate_ = has;
    return ok;
}

void ViewThread::clearPlate() {
    hasPlate_ = false;
    post([](gl::VolumeRenderer& r) { r.clearPlate(); });
}

void ViewThread::setLook(const sim::Look& look) {
    look_ = look;
    post([look](gl::VolumeRenderer& r) { r.look = look; });
}

}  // namespace pg::editor
