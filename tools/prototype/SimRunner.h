#pragma once
//
// Runs a simulation on a thread of its own and keeps its frames: the cache
// the timeline plays and scrubs. A scene that differs starts it again from
// frame 1; until the cache holds as many frames as asked for -- or as many as
// its budget of memory allows -- it simulates ahead.
//
// Frames are kept as half floats (sim::Frame), shared: the renderer reads
// one while the thread adds the next. Frames read from a cache on disk take
// the place of simulated ones (adopt()) -- or stay on disk, read as they are
// asked for (stream()): a cache bigger than the memory, a bake that is still
// writing it.
//
#include "pg/sim/Frame.h"
#include "pg/sim/World.h"

#include <atomic>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pg::editor {

class SimRunner {
public:
    /// `synchronous`: no thread; step() simulates a frame on the caller's --
    /// what screenshots use, so that N frames of the window are N steps.
    explicit SimRunner(bool synchronous = false);
    ~SimRunner();
    SimRunner(const SimRunner&) = delete;
    SimRunner& operator=(const SimRunner&) = delete;

    /// What to simulate, and how many frames. Another world throws the
    /// frames away and starts again; only another count keeps them.
    void set(const sim::World& world, int frames);
    /// Frames from elsewhere -- a cache on disk -- as the world's frames
    /// 1, 2, 3...: they are shown in place of simulated ones, and nothing is
    /// simulated after them -- there is no solver to go on from -- until
    /// set() brings another world.
    void adopt(const sim::World& world, int frames, std::vector<std::shared_ptr<const sim::Frame>> loaded);
    /// The frames of a cache folder as the world's frames, read from disk
    /// when asked for -- the latest read kept, as many as the budget holds --
    /// and nothing simulated, until set() brings another world. A bake may
    /// still be writing them: refresh() finds those written since.
    void stream(const sim::World& world, int frames, const std::string& folder);
    /// Streaming: the frames on disk now, as its cache.txt says.
    void refresh();
    /// The folder streamed from; empty when not streaming.
    std::string folder() const;
    /// The frames are adopted or streamed ones, not simulated.
    bool adopted() const;
    /// Nothing to simulate: the frames and the world go, and the next set()
    /// starts again whatever it brings -- another network, or none.
    void clear();
    /// Simulate ahead or not.
    void setRunning(bool on);
    bool running() const { return running_; }
    /// While held it starts nothing: a scene still changing under the mouse
    /// -- a gizmo being dragged -- is not worth simulating until it is let go.
    void hold(bool on);
    /// The most memory the frames may take.
    void setBudget(size_t bytes);

    /// Frames 1 .. cached() are ready.
    int cached() const;
    /// Frame `number` (1-based), or null if it is not ready.
    std::shared_ptr<const sim::Frame> frame(int number) const;
    /// True while there is more to simulate and room for it.
    bool busy() const;
    bool full() const;
    size_t bytes() const;
    double stepMs() const;
    /// Bumped whenever the frames are thrown away.
    unsigned generation() const;
    /// The grid of the gas being simulated; the default when there is none.
    sim::Domain domain() const;

    /// Synchronous mode: simulates one frame, if there is one to simulate.
    void step();

private:
    /// Frames still to simulate. Called with mu_ held.
    bool more() const;
    void loop();
    /// Simulates the next frame; false if there was nothing to do.
    bool advance();
    /// Streaming: frame `number` read from disk into the frames kept, the
    /// least recently asked for let go past the budget.
    std::shared_ptr<const sim::Frame> load(int number) const;

    const bool synchronous_;
    mutable std::mutex mu_;
    std::condition_variable wake_;
    std::thread thread_;
    bool quit_ = false;

    // Guarded by mu_.
    sim::World world_;
    bool started_ = false;  // set() was called
    bool fresh_ = false;    // world_ differs from what the solver runs
    bool adopted_ = false;  // the frames came from adopt()
    int frames_ = 0;
    std::vector<std::shared_ptr<const sim::Frame>> cache_;
    size_t bytes_ = 0, budget_ = size_t(1536) << 20;
    unsigned generation_ = 0;
    double stepMs_ = 0.0;
    sim::Domain domain_;
    // Streaming: the folder, the frames on disk, those read -- by number, and
    // when last asked for -- and the pieces the frames get (adoptPieces).
    std::string folder_;
    int onDisk_ = 0;
    mutable std::map<int, std::shared_ptr<const sim::Frame>> read_;
    mutable std::map<int, uint64_t> askedAt_;
    mutable uint64_t asks_ = 0;
    mutable size_t readBytes_ = 0;
    mutable std::mutex loading_;  // one read at a time; guards the memos
    mutable std::shared_ptr<const sim::RigidLayout> layout_;
    mutable std::shared_ptr<const sim::RigidRebar> rebar_;
    mutable std::shared_ptr<const sim::RigidGlue> glue_;

    std::atomic<bool> running_{true};
    std::atomic<bool> hold_{false};
    std::unique_ptr<sim::WorldSolver> solver_;  // the thread's (or the caller's in synchronous mode)
};

}  // namespace pg::editor
