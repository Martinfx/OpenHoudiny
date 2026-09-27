#pragma once
//
// Frames rendered one after another -- into a video (pg/io/Video.h) or a
// folder of numbered PNGs -- a few each frame of the window, with a modal
// that shows how far it got and can stop it: Render Frames and Render Video
// in the editor. A frame that is not ready yet (the simulation has not got
// there) is waited for.
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
    /// The modal, while it runs: the progress, and Cancel.
    void draw();
    /// Once, after it stopped: what came of it, whether it went wrong, and
    /// the file or folder it wrote.
    bool takeResult(std::string& message, bool& failed, std::string& path);

private:
    void finish(const std::string& why, bool failed);

    bool running_ = false, cancel_ = false, waiting_ = false, done_ = false;
    std::string target_, shown_, stem_;
    int width_ = 0, height_ = 0, first_ = 1, last_ = 0, next_ = 1, written_ = 0;
    double fps_ = 30.0, drawnMs_ = 0.0;
    Draw draw_;
    std::unique_ptr<io::VideoWriter> video_;
    std::vector<uint8_t> rgb_;
    std::chrono::steady_clock::time_point started_;
    std::string message_, path_;
    bool failed_ = false;
};

}  // namespace pg::editor
