#pragma once
//
// The Render tab's renderer, on a thread of its own while the window goes
// on: Cycles, Blender's renderer (render/Cycles.h) -- its first pictures of
// fewer, larger pixels, sharper as the samples add up, the noise taken out of
// each -- or our path tracer (render/PathTracer.h), pass after pass. Asked
// for a picture of something else -- another frame, the network edited, the
// view turned -- it starts again; but a scene it has started on shows a
// picture first, so that while the simulation plays the tab shows frame
// after frame, as fast as they render, never only "building the scene".
//
#include "pg/render/Cycles.h"
#include "pg/render/PathTracer.h"
#include "pg/sim/Frame.h"

#include <atomic>
#include <chrono>
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
    enum class Engine : uint8_t {
        Cycles,      ///< Blender's renderer, when the build has it
        PathTracer,  ///< ours
    };
    /// Cycles when the build has it, else the path tracer.
    static Engine defaultEngine();
    static const char* engineName(Engine engine);

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
    /// Renders with `engine` from now on: the same scene, from nothing.
    void setEngine(Engine engine);
    Engine engine() const;

    /// A newer picture than the last taken: RGBA, 8 bits, the top row first.
    bool takePicture(std::vector<uint8_t>& rgba, int& width, int& height);

    struct Status {
        int samples = 0, of = 0;
        double seconds = 0.0;
        bool building = false;  ///< making the scene
        bool rendering = false;
        uint64_t paths = 0;     ///< the path tracer's; 0 with Cycles
        std::string error;
    };
    Status status() const;

    /// What has been rendered, into `path` (render/Save.h).
    bool save(const std::string& path, const std::string& comment, std::string& error);

private:
    void run();
    /// The scene of `request`, built off the lock.
    std::shared_ptr<const render::Scene> build(Request& request);
    void publish(const render::Image& image, float exposure, render::Settings::View view);

    std::thread thread_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::atomic<bool> interrupt_{false};
    bool quit_ = false, paused_ = false, pending_ = false, restart_ = false, building_ = false;
    Engine engine_ = defaultEngine();
    bool engineChanged_ = false;
    Request next_;
    uint64_t scene_ = ~0ull;
    render::SceneBuilder builder_;
    // Path tracer: its scene and settings changed under mutex_.
    render::PathTracer tracer_;
    // Cycles: what it renders, how; made at first need.
    std::unique_ptr<render::CyclesRender> cycles_;
    std::shared_ptr<const render::Scene> cyclesScene_;
    render::Settings cyclesSettings_;
    Request last_;    // the scene rendered now, for a restart with another engine
    bool shown_ = true;  // the scene rendered now has shown a picture
    std::chrono::steady_clock::time_point started_;
    std::vector<uint8_t> picture_;
    int pictureWidth_ = 0, pictureHeight_ = 0;
    bool fresh_ = false;
    std::string error_;
    // Seconds the last noise taken out took, and the passes since took.
    double denoiseCost_ = 0.0, sinceDenoise_ = 0.0;
};

}  // namespace pg::editor
