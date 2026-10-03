#pragma once
//
// Runs a simulation on a thread of its own and keeps its frames: the cache
// the timeline plays and scrubs. A scene that differs starts it again from
// frame 1; until the cache holds as many frames as asked for it simulates
// ahead.
//
// The frames are kept by a sim::FrameStore, within a budget of memory: past
// it, those furthest from the play head are spilled to disk and read back
// as they are played -- so a simulation longer than the memory still plays
// whole -- or, with spilling off, the simulation waits there. A cache on
// disk is played the same way (stream()): read ahead of the play head on a
// thread of the store's own -- a cache bigger than the memory, a bake still
// writing it. The timeline asks with ready(), which never waits; what needs
// a frame now -- an export, a render -- with frame().
//
// Frames are kept as half floats (sim::Frame), shared: the renderer reads
// one while the thread adds the next.
//
#include "pg/sim/Frame.h"
#include "pg/sim/FrameStore.h"
#include "pg/sim/World.h"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
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
    /// The frames of a cache folder as the world's frames, read from disk
    /// as they are played, and nothing simulated, until set() brings
    /// another world. A bake may still be writing them: refresh() finds
    /// those written since.
    void stream(const sim::World& world, int frames, const std::string& folder);
    /// Streaming: the frames on disk now, as its cache.txt says.
    void refresh();
    /// The folder streamed from; empty when not streaming.
    std::string folder() const;
    /// The frames are read from a cache on disk, not simulated.
    bool fromDisk() const;
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
    size_t budget() const;
    /// Past the budget, frames go to disk (on), or the simulation waits.
    void setSpill(bool on);
    bool spill() const;

    /// Frames 1 .. cached() are there: in memory or on disk.
    int cached() const;
    /// Frame `number` (1-based), read from disk now if it must be; null if
    /// it is not there.
    std::shared_ptr<const sim::Frame> frame(int number) const;
    /// Frame `number` if it is in memory; else null, and it is read next.
    /// Never waits: what the timeline plays.
    std::shared_ptr<const sim::Frame> ready(int number) const;
    /// The runs of frames in memory, first and last of each.
    std::vector<std::pair<int, int>> inMemory() const;
    /// Where the timeline is, and which way it plays: what is kept, and
    /// read ahead.
    void setPlayhead(int frame, int direction);
    /// True while there is more to simulate and room for it.
    bool busy() const;
    /// The memory is full, and nothing may go to disk: the simulation waits.
    bool full() const;
    /// Memory the frames take; and what was spilled to disk, frames and bytes.
    size_t bytes() const;
    int spilledFrames() const;
    size_t spilledBytes() const;
    /// Why spilling stopped; empty if it did not.
    std::string spillError() const;
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
    /// What frames of `world` read from disk need: its pieces and cloth.
    void prepareFor(const sim::World& world);

    const bool synchronous_;
    mutable std::mutex mu_;
    std::condition_variable wake_;
    std::thread thread_;
    bool quit_ = false;

    // Guarded by mu_.
    sim::World world_;
    bool started_ = false;  // set() was called
    bool fresh_ = false;    // world_ differs from what the solver runs
    int frames_ = 0;
    unsigned generation_ = 0;
    double stepMs_ = 0.0;
    sim::Domain domain_;
    std::string folder_;  // streaming: the cache's

    std::atomic<bool> running_{true};
    std::atomic<bool> hold_{false};
    std::unique_ptr<sim::WorldSolver> solver_;  // the thread's (or the caller's in synchronous mode)
    mutable sim::FrameStore store_;             // the frames
};

}  // namespace pg::editor
