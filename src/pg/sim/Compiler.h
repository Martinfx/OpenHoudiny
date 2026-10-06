#pragma once
//
// Networks compiled on a thread of their own: the editor hands over the
// network as it is -- a copy -- and goes on with what it compiled before;
// when the compile is done, it takes the new one. As the Cooker does for the
// geometry shown (Cooker.h), the window never waits on geometry cooked for
// the solvers.
//
// Requests made while a compile is under way wait, the newest in place of
// the rest. The one under way makes way for it -- given up, through
// Network::compile's interrupt -- once it has gone on for kPatience: a
// compile of a moment comes back while a gizmo is dragged, and what is drawn
// of the objects follows it; a slider dragged upstream of a fracture
// compiles the value it is at, not every value it went through.
//
//   window:    submit(net') . submit(net'') ............ take() -> net'' compiled
//   compiler:     compile net' --- given up ---> compile net'' --> done
//
// The thread keeps a GeometryGraph of its own, synced with each network it
// is handed: what feeds the solvers cooks again only where it changed.
//
#include "pg/sim/GeometryGraph.h"
#include "pg/sim/Network.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace pg::sim {

class Compiler {
public:
    /// How long a compile under way goes on before a newer request gives it up.
    static constexpr double kPatience = 0.25;

    struct Result {
        uint64_t serial = 0;    ///< the request's
        uint64_t revision = 0;  ///< the network's, when it was handed over
        Compiled compiled;
        double ms = 0.0;        ///< how long it took
    };

    /// `frames`: where the nodes that bring a simulation back get its frames.
    explicit Compiler(FrameSource frames = {});
    ~Compiler();
    Compiler(const Compiler&) = delete;
    Compiler& operator=(const Compiler&) = delete;

    /// Asks for `net` compiled, its relative paths read from `folder`: in
    /// place of a request still waiting. Its serial number.
    uint64_t submit(std::shared_ptr<const Network> net, std::string folder);
    /// Gives up the compile under way if a newer request waits and it has
    /// gone on for kPatience: once a frame, while one is busy().
    void giveUpIfStale();
    /// The newest compile finished since the last take; false if none.
    bool take(Result& out);
    /// Until the last request is done -- one under way that a newer request
    /// waits behind given up at once, what is waited for being the newest;
    /// or, given `seconds`, at most that long. True once it is done.
    bool wait(double seconds = -1.0);
    /// A request not done yet.
    bool busy() const;

private:
    void loop();
    /// giveUpIfStale(), mu_ held.
    void giveUpIfStaleLocked();

    mutable std::mutex mu_;
    std::condition_variable wake_, done_;
    bool stop_ = false;
    bool pending_ = false;
    std::shared_ptr<const Network> next_;
    std::string nextFolder_;
    uint64_t submitted_ = 0;  ///< the serial of the last request
    uint64_t finished_ = 0;   ///< the serial of the last one done (or given up for a newer)
    bool ready_ = false;
    Result result_;
    bool working_ = false;  ///< a compile is under way, since started_
    std::chrono::steady_clock::time_point started_;
    std::atomic<bool> interrupt_{false};

    // The worker's own.
    GeometryGraph graph_;
    std::shared_ptr<const Network> held_;  ///< the network the graph last synced with: kept alive
    std::thread thread_;
};

}  // namespace pg::sim
