#pragma once
//
// Frames rendered one after another -- into a video (pg/io/Video.h) or a
// folder of numbered PNGs -- a few each frame of the window, with a modal
// that shows how far it got and can stop it: Render Frames and Render Video
// in the editor. A frame that is not ready yet (the simulation has not got
// there, Cycles is still rendering it) is waited for.
//
#include "pg/io/Video.h"

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace pg::editor {

class RenderJob {
public:
    /// Draws `frame` into `rgb` (width x height, the top row first): true
    /// when drawn; false with no error while it cannot be yet; false with
    /// why to stop.
    using Draw = std::function<bool(int frame, std::vector<uint8_t>& rgb, std::string& error)>;

    /// Frames first..last of width x height, at `fps`, into `target`: a
    /// video when its extension is a video's, else a folder -- made if need
    /// be -- of <stem>_0001.png, <stem>_0002.png... `shown`: the target as
    /// the messages name it. False, with why, if it cannot start.
    bool start(const std::string& target, const std::string& shown, const std::string& stem, int width, int height,
               double fps, int first, int last, Draw draw, std::string& error);
    bool running() const { return running_; }
    /// Renders for about `budgetMs`: at least a frame, when one is ready.
    void step(double budgetMs = 40.0);
    /// A line under the title, from start() on: how it renders -- the
    /// renderer, the size, the samples.
    void setDetail(std::string detail) { detail_ = std::move(detail); }
    /// While the frame it is on is not drawn yet: what that waits on, as
    /// the modal says it, and how far it is, 0 to 1 -- told by the drawing
    /// as it returns false. Nothing told: the simulation. Forgotten once
    /// the frame is drawn.
    void tell(std::string what, float done = 0.0f);
    /// The modal, while it runs: the progress, the last frame drawn when
    /// there is one to show (`preview`, a GL texture of it), and Stop.
    void draw(unsigned preview = 0, int previewWidth = 0, int previewHeight = 0);
    /// Once, after it stopped: what came of it, whether it went wrong, and
    /// the file or folder it wrote.
    bool takeResult(std::string& message, bool& failed, std::string& path);

private:
    void finish(const std::string& why, bool failed);

    bool running_ = false, cancel_ = false, waiting_ = false, done_ = false;
    std::string target_, shown_, stem_, detail_;
    std::string told_;     // what the frame waits on (tell)
    float toldDone_ = 0.0f;
    int width_ = 0, height_ = 0, first_ = 1, last_ = 0, next_ = 1, written_ = 0;
    double fps_ = 30.0, drawnMs_ = 0.0;
    std::chrono::steady_clock::time_point frameStarted_;  // the first try at the frame it is on
    Draw draw_;
    std::unique_ptr<io::VideoWriter> video_;
    std::vector<uint8_t> rgb_;
    std::chrono::steady_clock::time_point started_;
    std::string message_, path_;
    bool failed_ = false;
};

}  // namespace pg::editor
