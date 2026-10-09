#pragma once
//
// The viewport drawn on a thread of its own: a gl::VolumeRenderer there, in
// an OpenGL context of its own that shares the window's -- what is drawn
// handed over in textures of that share group. The window's thread says
// what changes (jobs, run on the view's thread in the order they came)
// and asks for pictures; it shows the latest one finished. A scene that
// takes 100 ms to draw no longer holds the window -- the node editor, the
// sliders -- to 10 frames a second: they go on at the screen's rate, and
// the viewport shows each picture as it is done.
//
// Without a context of its own (sharedContext() none: no window, as in a
// test), the same calls run on the calling thread, at once -- as the
// renderer did before it had a thread.
//
// The window's thread keeps what it reads of the renderer itself: the
// geometry drawn, the look, the plate; the box round the geometry and its
// preparation the view's thread hands back after each batch of jobs.
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

class ViewThread {
public:
    /// The context the view's thread draws in, sharing the window's: made
    /// on the main thread (GLFW wants windows made there), hidden. None:
    /// the viewport draws on the window's thread.
    static void setSharedContext(GLFWwindow* context);
    static GLFWwindow* sharedContext();
    /// Waits for each picture asked for, however long it takes: for
    /// screenshots and scripts, whose pictures must be the ones asked for.
    static void setPatient(bool patient);

    explicit ViewThread(const gl::Api& gl);
    ~ViewThread();
    ViewThread(const ViewThread&) = delete;
    ViewThread& operator=(const ViewThread&) = delete;

    /// Whether it draws on a thread of its own.
    bool threaded() const { return context_ != nullptr; }

    /// Compiles the renderer's shaders, on its thread; waits. False, with
    /// the driver's message.
    bool init(std::string& log);

    /// Runs `job` with the renderer on its thread, after those before it;
    /// returns at once.
    void post(std::function<void(gl::VolumeRenderer&)> job);
    /// ... and waits for it.
    void run(const std::function<void(gl::VolumeRenderer&)>& job);

    // What changes, as the renderer has it (gl::VolumeRenderer) -- the
    // data handed over held until the job has run.
    void setFrame(std::shared_ptr<const sim::Frame> frame);
    void setFrame(std::shared_ptr<const sim::Frame> frame, std::shared_ptr<const sim::PreparedVolumes> prepared);
    void clearFrame();
    void setDomain(const sim::Domain& domain);
    void setSolids(std::vector<sim::Solid> solids);
    void setHighlight(std::vector<int> selected, int hovered);
    void setLines(gl::Lines lines);
    void setOverlay(gl::Overlay overlay, int layer = 0);
    void setOverlayHidden(float alpha);
    void setGeometry(const GeometryPtr& geometry);
    void setPrepared(std::shared_ptr<const sim::PreparedGeometry> prepared);
    void setPreparedPieces(std::shared_ptr<const sim::PreparedBodies> pieces);
    /// The plate (VolumeRenderer::setPlate): read on the view's thread, and
    /// waited for -- whether it could be read is wanted at once.
    bool setPlate(const std::string& file, const sim::Camera& camera, std::string& error);
    void clearPlate();
    bool hasPlate() const { return hasPlate_; }
    void setLook(const sim::Look& look);
    const sim::Look& look() const { return look_; }

    /// The geometry drawn, as the window's thread last set it.
    const GeometryPtr& geometry() const { return geometry_; }
    /// As the view's thread has them after the jobs it has run.
    std::shared_ptr<const sim::PreparedGeometry> prepared() const;
    bool geometryBounds(Vec3& lo, Vec3& hi) const;

    /// The view the window's thread turns; drawn from at the next draw().
    gl::Orbit orbit;

    /// Asks for a picture `width` x `height` from `orbit`: drawn on the
    /// view's thread after the jobs before it. Waits for it as long as the
    /// window can spare (or for good, patient); the one before stays
    /// shown meanwhile.
    void draw(int width, int height);
    /// The latest picture finished, to show this frame -- a texture of the
    /// share group, 0 before the first. Call once a frame, before showing
    /// it: the one shown before goes back to be drawn into once the GPU is
    /// done showing it.
    unsigned picture();
    /// Seconds the last picture took to draw, on the view's thread.
    double drawSeconds() const;

private:
    struct Slot {
        unsigned texture = 0, fbo = 0;
        int width = 0, height = 0;
        enum class State { Free, Drawing, Ready, Shown } state = State::Free;
        gl::GLsync released = nullptr;  ///< the window's GPU done showing it
    };
    void loop();
    void runJobs(std::deque<std::function<void(gl::VolumeRenderer&)>>& jobs);
    void publish(gl::VolumeRenderer& r);
    void handBack(gl::VolumeRenderer& r);

    const gl::Api& gl_;
    GLFWwindow* context_ = nullptr;
    std::unique_ptr<gl::VolumeRenderer> renderer_;

    // The window's thread's own.
    sim::Look look_;
    GeometryPtr geometry_;
    bool hasPlate_ = false;
    int shown_ = -1;

    mutable std::mutex mutex_;
    std::condition_variable wake_, done_;
    std::deque<std::function<void(gl::VolumeRenderer&)>> jobs_;
    uint64_t posted_ = 0, ran_ = 0;  ///< jobs given, jobs run
    struct Ask {
        gl::Orbit orbit;
        int width = 0, height = 0;
        uint64_t serial = 0;
    } ask_;
    uint64_t asked_ = 0, drawn_ = 0;  ///< pictures asked for, finished
    Slot slots_[3];
    int ready_ = -1;
    std::shared_ptr<const sim::PreparedGeometry> prepared_;
    bool hasBounds_ = false;
    Vec3 lo_{0.0f}, hi_{0.0f};
    double drawSeconds_ = 0.0;
    bool quit_ = false;
    std::thread thread_;
};

}  // namespace pg::editor
