#pragma once
//
// Geometry cooked on a thread of its own: the editor hands over the network
// as it is -- a copy -- with the frame and the nodes it wants to see, and
// goes on drawing what it has; when the cook is done, it takes the new
// geometry. A new request gives up the cook under way (CookContext::
// interrupt): a slider dragged cooks the value it is at, not every value it
// went through, and the window never waits.
//
//   window:  submit(net', frame) ... submit(net'', frame) ... take() -> geometry of net''
//   cooker:     cook net' --- given up ---> cook net'' ------------> done
//
// Levels: inside an asset, the inside cooks with the inputs of the instance
// gone into -- the networks from the scene down, each with the instance of
// the next; the inputs of each instance cooked in the level above it. Each
// level keeps its graph, and so what it cooked, while the ones below come
// and go.
//
#include "pg/sim/GeometryGraph.h"

#include <atomic>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pg::sim {

class Cooker {
public:
    struct Level {
        std::shared_ptr<const Network> net;
        std::string folder;   ///< where its relative paths are read from
        int instance = 0;     ///< the asset node the next level is the inside of
    };
    struct Request {
        std::vector<Level> levels;  ///< the scene first; the last is the one cooked
        int frame = 1;
        float timeStep = 1.0f / 30.0f;
        std::vector<int> nodes;     ///< the nodes of the last level wanted
    };
    struct Result {
        uint64_t serial = 0;                                ///< the request's
        std::map<int, GeometryPtr> geometry;                ///< each node wanted that is a geometry node
        std::map<int, std::string> errors, warnings, logs;  ///< the last level's geometry nodes
        double ms = 0.0;                                    ///< how long it took
    };

    /// `frames`: where the nodes that bring a simulation back get its frames.
    explicit Cooker(FrameSource frames = {});
    ~Cooker();
    Cooker(const Cooker&) = delete;
    Cooker& operator=(const Cooker&) = delete;

    /// Asks for a cook, giving up the one under way. Its serial number.
    uint64_t submit(Request request);
    /// The newest result finished since the last take; false if none.
    bool take(Result& out);
    /// Until the last request is done.
    void wait();
    /// A request not done yet.
    bool busy() const;

private:
    void loop();
    Result cook(const Request& r);

    FrameSource frames_;
    mutable std::mutex mu_;
    std::condition_variable wake_, done_;
    bool stop_ = false;
    bool pending_ = false;
    Request next_;
    uint64_t submitted_ = 0;  ///< the serial of the last request
    uint64_t finished_ = 0;   ///< the serial of the last one done (or given up for a newer)
    bool ready_ = false;
    Result result_;
    std::atomic<bool> interrupt_{false};

    // The worker's own.
    std::vector<std::unique_ptr<GeometryGraph>> graphs_;
    std::vector<Level> held_;  ///< the networks the graphs last synced with: kept alive
    std::thread thread_;
};

}  // namespace pg::sim
