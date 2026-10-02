#pragma once
//
// A frame of the shot rendered to the end -- by Cycles or the path tracer,
// the Render tab's renderer, to the samples the Output's Render section
// asks for -- on a thread of its own while the window goes on: what Render
// Video and Render Frames with Cycles make of each frame, as the command
// line's `prototype sim OUT.mp4 --renderer cycles` does. The window asks
// how far it got and takes the picture once it is done; a stop ends it as
// soon as the renderer can.
//
#include "RenderView.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pg::editor {

class FrameRender {
public:
    FrameRender() = default;
    ~FrameRender();
    FrameRender(const FrameRender&) = delete;
    FrameRender& operator=(const FrameRender&) = delete;

    /// Renders `request` with `engine`, from nothing: the scene made -- the
    /// frame's pieces, cloth and water into meshes -- then rendered to the
    /// end. What it rendered before, it stops.
    void start(RenderView::Engine engine, RenderView::Request request);
    /// Stops as soon as it can; nothing comes of it.
    void cancel();

    enum class State : uint8_t {
        Idle,       ///< nothing asked, or the last picture taken
        Building,   ///< making the scene
        Rendering,  ///< the samples adding up
        Done,       ///< a picture to take -- or why there is none
    };
    struct Progress {
        State state = State::Idle;
        int samples = 0, of = 0;
        double seconds = 0.0;  ///< since it started
    };
    Progress progress() const;

    /// Once it is done: the picture, 8-bit RGB with the top row first, as
    /// the screen shows it -- or false with why there is none. Idle after.
    bool take(std::vector<uint8_t>& rgb, std::string& error);

private:
    void run(RenderView::Engine engine, RenderView::Request request);

    std::thread thread_;
    mutable std::mutex mutex_;
    std::atomic<bool> stop_{false};
    State state_ = State::Idle;
    int samples_ = 0, of_ = 0;  // the path tracer's; Cycles is asked
    /// The Cycles render under way, while it is: asked how far it got, and
    /// stopped, from the window's thread.
    render::CyclesRender* cycles_ = nullptr;
    std::chrono::steady_clock::time_point started_;
    std::vector<uint8_t> rgb_;
    std::string error_;
    // The thread's alone: the scene of the last frame kept for the next
    // (what did not change is not made again), the path tracer.
    render::SceneBuilder builder_;
    render::PathTracer tracer_;
};

}  // namespace pg::editor
