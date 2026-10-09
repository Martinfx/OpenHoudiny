#include "ThumbThread.h"

#include "Thumbnails.h"

#include <GLFW/glfw3.h>

#include <chrono>
#include <utility>

namespace pg::editor {
namespace {

GLFWwindow* gShared = nullptr;

}  // namespace

void ThumbThread::setSharedContext(GLFWwindow* context) { gShared = context; }

ThumbThread::ThumbThread(const gl::Api& gl, size_t texelBudget) : gl_(gl), texelBudget_(texelBudget), context_(gShared) {
    if (!context_) {
        node_ = std::make_unique<gl::VolumeRenderer>(gl);
        scene_ = std::make_unique<gl::VolumeRenderer>(gl);
        node_->texelBudget = scene_->texelBudget = texelBudget;
        return;
    }
    // Made in the thread's context: vertex arrays are not shared.
    thread_ = std::thread([this] { loop(); });
    std::unique_lock<std::mutex> lock(mutex_);
    done_.wait(lock, [&] { return scene_ != nullptr; });
}

ThumbThread::~ThumbThread() {
    if (!thread_.joinable()) {
        if (readFbo_) gl_.DeleteFramebuffers(1, &readFbo_);
        if (drawFbo_) gl_.DeleteFramebuffers(1, &drawFbo_);
        for (const Done& d : finished_) gl_.DeleteTextures(1, &d.texture);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        quit_ = true;
    }
    wake_.notify_all();
    thread_.join();
}

bool ThumbThread::init(std::string& log) {
    bool ok = false;
    auto both = [&] { ok = node_->init(log) && scene_->init(log); };
    if (!context_) {
        both();
        return ok;
    }
    std::unique_lock<std::mutex> lock(mutex_);
    call_ = both;
    wake_.notify_all();
    done_.wait(lock, [&] { return !call_; });
    return ok;
}

void ThumbThread::loop() {
    glfwMakeContextCurrent(context_);
    {
        auto node = std::make_unique<gl::VolumeRenderer>(gl_);
        auto scene = std::make_unique<gl::VolumeRenderer>(gl_);
        node->texelBudget = scene->texelBudget = texelBudget_;
        std::lock_guard<std::mutex> lock(mutex_);
        node_ = std::move(node);
        scene_ = std::move(scene);
        done_.notify_all();
    }
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [&] { return quit_ || call_ || !jobs_.empty(); });
            if (quit_) break;
            if (call_) {
                call_();
                call_ = nullptr;
                done_.notify_all();
                continue;
            }
            job = std::move(jobs_.front());
            jobs_.pop_front();
            ++running_;
        }
        Done d = drawOne(job);
        job.draw = nullptr;  // what it holds -- frames, geometry -- let go of here
        std::lock_guard<std::mutex> lock(mutex_);
        --running_;
        finished_.push_back(d);
        done_.notify_all();
    }
    // The renderers' GL objects, and pictures never taken, go in this context.
    node_.reset();
    scene_.reset();
    if (readFbo_) gl_.DeleteFramebuffers(1, &readFbo_);
    if (drawFbo_) gl_.DeleteFramebuffers(1, &drawFbo_);
    for (const Done& d : finished_) gl_.DeleteTextures(1, &d.texture);
    gl_.Finish();
    glfwMakeContextCurrent(nullptr);
}

ThumbThread::Done ThumbThread::drawOne(Job& job) {
    using namespace gl;
    const auto start = std::chrono::steady_clock::now();
    const VolumeRenderer& r = job.draw(*node_, *scene_);
    Done d;
    d.node = job.node;
    d.key = job.key;
    d.live = job.live;
    d.tag = job.tag;
    constexpr int w = Thumbnails::kWidth, h = Thumbnails::kHeight;
    gl_.GenTextures(1, &d.texture);
    gl_.BindTexture(TEXTURE_2D, d.texture);
    gl_.TexImage2D(TEXTURE_2D, 0, static_cast<GLint>(RGBA8), w, h, 0, RGBA, UNSIGNED_BYTE, nullptr);
    gl_.TexParameteri(TEXTURE_2D, TEXTURE_MIN_FILTER, LINEAR);
    gl_.TexParameteri(TEXTURE_2D, TEXTURE_MAG_FILTER, LINEAR);
    gl_.TexParameteri(TEXTURE_2D, TEXTURE_WRAP_S, CLAMP_TO_EDGE);
    gl_.TexParameteri(TEXTURE_2D, TEXTURE_WRAP_T, CLAMP_TO_EDGE);
    gl_.BindTexture(TEXTURE_2D, 0);
    if (!readFbo_) gl_.GenFramebuffers(1, &readFbo_);
    if (!drawFbo_) gl_.GenFramebuffers(1, &drawFbo_);
    // Twice the size, averaged down: a linear blit at exactly 2:1 samples
    // each picture pixel between four of the source's.
    gl_.BindFramebuffer(READ_FRAMEBUFFER, readFbo_);
    gl_.FramebufferTexture2D(READ_FRAMEBUFFER, COLOR_ATTACHMENT0, TEXTURE_2D, r.colorTexture(), 0);
    gl_.BindFramebuffer(DRAW_FRAMEBUFFER, drawFbo_);
    gl_.FramebufferTexture2D(DRAW_FRAMEBUFFER, COLOR_ATTACHMENT0, TEXTURE_2D, d.texture, 0);
    gl_.BlitFramebuffer(0, 0, 2 * w, 2 * h, 0, 0, w, h, COLOR_BUFFER_BIT, static_cast<GLenum>(LINEAR));
    gl_.FramebufferTexture2D(READ_FRAMEBUFFER, COLOR_ATTACHMENT0, TEXTURE_2D, 0, 0);
    gl_.FramebufferTexture2D(DRAW_FRAMEBUFFER, COLOR_ATTACHMENT0, TEXTURE_2D, 0, 0);
    gl_.BindFramebuffer(READ_FRAMEBUFFER, 0);
    gl_.BindFramebuffer(DRAW_FRAMEBUFFER, 0);
    // Done on the GPU before the window's thread may show it.
    if (context_) gl_.Finish();
    d.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    return d;
}

void ThumbThread::post(int node, uint64_t key, uint64_t live, uint64_t tag, Draw draw) {
    Job job{node, key, live, tag, std::move(draw)};
    if (!context_) {
        ++pending_;
        finished_.push_back(drawOne(job));
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        jobs_.push_back(std::move(job));
        ++pending_;
    }
    wake_.notify_all();
}

size_t ThumbThread::pending() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pending_;
}

std::vector<ThumbThread::Done> ThumbThread::take() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Done> out;
    out.swap(finished_);
    pending_ -= out.size();
    return out;
}

void ThumbThread::wait() {
    if (!context_) return;
    std::unique_lock<std::mutex> lock(mutex_);
    done_.wait(lock, [&] { return jobs_.empty() && running_ == 0; });
}

}  // namespace pg::editor
