#pragma once
//
// Work done frame by frame on a thread of its own -- saving the cache to
// disk, exporting a geometry's frames, the shot as USD or Alembic -- with a
// modal that shows how far it got and stops it, as a render's does: the
// window goes on drawing, and nothing in the network changes under it.
//
// What a frame's work touches must be its own or safe from another thread:
// a GeometryGraph of its own, the runner's frames (SimRunner is), a copy of
// what was compiled -- never the editor's.
//
#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace pg::editor {

class FrameJob {
public:
    /// Frame `frame`'s work: false, with why in `error`, to stop there.
    using Step = std::function<bool(int frame, std::string& error)>;
    /// After the last frame -- or the one it stopped at: `done` frames done,
    /// `stopped` by Stop. What to tell in `message`; false if it failed.
    using Finish = std::function<bool(int done, bool stopped, std::string& message)>;

    FrameJob() = default;
    /// Stops what runs, and waits for it.
    ~FrameJob();
    FrameJob(const FrameJob&) = delete;
    FrameJob& operator=(const FrameJob&) = delete;

    /// `title` -- "Saving the cache" -- over frames first..last of `what`
    /// (a path, as the modal shows it). False if one runs already.
    bool start(std::string title, std::string what, int first, int last, Step step, Finish finish);
    bool running() const { return running_; }
    /// Asks it to stop after the frame it is on.
    void stop() { stop_ = true; }
    /// The modal, while it runs: the progress, the time left, and Stop
    /// (or Escape).
    void draw();
    /// Once, after it ended: what came of it.
    bool takeResult(std::string& message, bool& failed);

private:
    void join();

    std::thread thread_;
    std::atomic<bool> running_{false}, stop_{false}, ended_{false};
    std::atomic<int> done_{0};
    int first_ = 1, last_ = 0;
    std::string title_, what_;
    std::chrono::steady_clock::time_point started_;
    std::mutex mu_;  // the result
    std::string message_;
    bool failed_ = false;
};

}  // namespace pg::editor
