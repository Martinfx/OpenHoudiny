#pragma once
//
// The Render tab's renderer: the path tracer (render/PathTracer.h) on a
// thread of its own, pass after pass, while the window goes on. Asked for a
// picture of something else -- another frame, the network edited, the view
// turned -- it stops the pass it is in and starts again; the window takes
// the picture as it gets less noisy.
//
#include "pg/render/PathTracer.h"
#include "pg/sim/Frame.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pg::editor {

class RenderView {
public:
    /// What to render: the scene -- the frame's pieces, cloth and water
    /// made into meshes on the thread -- and how.
    struct Request {
        render::SceneInput input;
        std::shared_ptr<const sim::Frame> frame;  ///< for its pieces, cloth and water
        render::Settings settings;
        uint64_t scene = 0;  ///< another number: another scene; the same, only the settings changed
    };

    RenderView();
    ~RenderView();

    void request(Request request);
    /// Stopped where it is -- or going on.
    void setPaused(bool paused);
    bool paused() const { return paused_; }
    /// From nothing, the same scene.
    void restart();

    /// A newer picture than the last taken: RGBA, 8 bits, the top row first.
    bool takePicture(std::vector<uint8_t>& rgba, int& width, int& height);

    struct Status {
        int samples = 0, of = 0;
        double seconds = 0.0;
        bool building = false;  ///< making the scene
        bool rendering = false;
        uint64_t paths = 0;
        std::string error;
    };
    Status status() const;

    /// What has been rendered, into `path` (render/Save.h).
    bool save(const std::string& path, const std::string& comment, std::string& error);

private:
    void run();

    std::thread thread_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::atomic<bool> interrupt_{false};
    bool quit_ = false, paused_ = false, pending_ = false, restart_ = false, building_ = false;
    Request next_;
    uint64_t scene_ = ~0ull;
    render::SceneBuilder builder_;
    render::PathTracer tracer_;  // its scene and settings changed under mutex_
    std::vector<uint8_t> picture_;
    int pictureWidth_ = 0, pictureHeight_ = 0;
    bool fresh_ = false;
    std::string error_;
};

}  // namespace pg::editor
