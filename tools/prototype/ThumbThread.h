#pragma once
//
// The network's thumbnails drawn on a thread of their own, as the viewport is
// (ViewThread.h): two renderers there -- one for a node's own thing, one for
// the whole scene -- in a hidden OpenGL context sharing the window's. The
// window's thread hands over what a picture is to show and goes on; the
// picture comes back, averaged down to Thumbnails::kWidth x kHeight, as a
// texture of the share group the window's thread takes as the node's
// (Thumbnails::put). A forest's picture that takes a second to draw holds
// the thumbnail alone, not the window.
//
// Without a context of its own (sharedContext() none), each picture is drawn
// on the calling thread as it is handed over.
//
#include "pg/gl/Gl.h"
#include "pg/gl/Volume.h"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct GLFWwindow;

namespace pg::editor {

class ThumbThread {
public:
    /// The context the thumbnails' thread draws in, sharing the window's:
    /// made on the main thread, hidden. None: they draw on the window's.
    static void setSharedContext(GLFWwindow* context);

    /// Draws a picture into the node's renderer or the scene's; the one it
    /// drew in.
    using Draw = std::function<gl::VolumeRenderer&(gl::VolumeRenderer& node, gl::VolumeRenderer& scene)>;

    /// A picture done: `texture` is the caller's to keep, or to delete.
    struct Done {
        int node = 0;
        uint64_t key = 0, live = 0, tag = 0;
        gl::GLuint texture = 0;
        double ms = 0.0;  ///< how long drawing it took
    };

    /// Both renderers with `texelBudget` (VolumeRenderer::texelBudget).
    ThumbThread(const gl::Api& gl, size_t texelBudget);
    ~ThumbThread();
    ThumbThread(const ThumbThread&) = delete;
    ThumbThread& operator=(const ThumbThread&) = delete;

    bool threaded() const { return context_ != nullptr; }
    /// Compiles both renderers' shaders; waits. False, with the driver's message.
    bool init(std::string& log);

    /// Draws `draw`'s picture of `node`, showing `key` and `live`, after
    /// those handed over before; `tag` comes back with it.
    void post(int node, uint64_t key, uint64_t live, uint64_t tag, Draw draw);
    /// Pictures handed over and not taken back yet.
    size_t pending() const;
    /// The pictures done since the last call, in the order handed over.
    std::vector<Done> take();
    /// Waits until every picture handed over is done.
    void wait();

private:
    struct Job {
        int node = 0;
        uint64_t key = 0, live = 0, tag = 0;
        Draw draw;
    };
    void loop();
    Done drawOne(Job& job);

    const gl::Api& gl_;
    const size_t texelBudget_;
    GLFWwindow* context_ = nullptr;
    std::unique_ptr<gl::VolumeRenderer> node_, scene_;
    gl::GLuint readFbo_ = 0, drawFbo_ = 0;

    mutable std::mutex mutex_;
    std::condition_variable wake_, done_;
    std::deque<Job> jobs_;
    std::vector<Done> finished_;
    size_t pending_ = 0;  ///< handed over, not taken back
    size_t running_ = 0;  ///< being drawn
    std::function<void()> call_;  ///< run on the thread, once (init)
    bool quit_ = false;
    std::thread thread_;
};

}  // namespace pg::editor
