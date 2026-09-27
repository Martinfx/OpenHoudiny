#pragma once
//
// The geometry nodes of a network, cooked by the core's cook engine
// (pg/core/CookEngine.h): each node of the Geometry category is mirrored by
// a node of a pg::Graph -- made, wired and set as the network says each time
// it changes. A parameter is set only when its value changed, so only what it
// feeds cooks again: move a slider at the end of a chain of fifty nodes and
// one node cooks.
//
//   [Box] -> [Transform] -> [Scatter] ...      the network (Network.h)
//     n3 ------> n4 -------> n7                 the core's graph, node "n<id>"
//
// Bypassed nodes are left out of the wiring: whatever feeds them feeds what
// they fed. A File parameter is read from the network's folder, and read
// again when the file changes. An animated parameter is bound as an
// expression of the core (its keys at the frame cooked): the node varies
// with time, and its geometry is cached frame by frame.
//
// Three nodes bring a simulation back as geometry -- Liquid Points, Rain
// Points, Gas Volume -- from the frame being cooked, which a FrameSource
// hands over: the editor's cache of frames, or the frame the command line
// has just simulated.
//
#include "pg/core/CookEngine.h"
#include "pg/core/Graph.h"
#include "pg/sim/Frame.h"
#include "pg/sim/Network.h"

#include <functional>
#include <map>
#include <memory>
#include <string>

namespace pg::sim {

class Network;

/// Frame n of the simulation, or null if there is none (yet).
using FrameSource = std::function<std::shared_ptr<const Frame>(int frame)>;

class GeometryGraph {
public:
    GeometryGraph();
    ~GeometryGraph();
    GeometryGraph(const GeometryGraph&) = delete;
    GeometryGraph& operator=(const GeometryGraph&) = delete;

    /// Makes the graph what `net`'s geometry nodes say. `folder`: where a
    /// relative path of a File parameter is read from. Cheap when nothing
    /// changed.
    void sync(const Network& net, const std::string& folder = {});

    /// What geometry node `id` puts out at `frame` -- time frame x timeStep
    /// -- cooking only what is out of date. A bypassed node puts out what
    /// comes into it. Null for a node that is not a geometry node.
    GeometryPtr cook(int id, int frame, float timeStep = 1.0f / 30.0f);

    /// What went wrong the last time node `id` cooked; empty if nothing.
    std::string error(int id) const;
    /// True for a geometry node of the network last synced.
    bool contains(int id) const { return nodes_.count(id) > 0; }
    /// The core's node that cooks node `id`; null if there is none.
    const pg::Node* coreNode(int id) const {
        const auto it = nodes_.find(id);
        return it == nodes_.end() ? nullptr : it->second.node;
    }

    /// Where the nodes that bring a simulation back get its frames.
    void setFrames(FrameSource frames) { frames_ = std::move(frames); }

    CookEngine& engine() { return engine_; }

private:
    struct Mirror {
        pg::Node* node = nullptr;
        std::string type;    ///< the network's type
        bool bypass = false;
        int input = 0;       ///< for a bypassed node: what comes into it, 0 if nothing
        std::string file;    ///< the file it reads, if any: looked at again at each sync
        std::map<std::string, std::vector<Key>> keys;  ///< the animated parameters, as last bound
    };
    /// Follows bypassed nodes up to the one whose output counts.
    int resolve(int id) const;

    Graph graph_;
    CookEngine engine_;
    std::map<int, Mirror> nodes_;
    FrameSource frames_;
    const Network* synced_ = nullptr;
    uint64_t revision_ = ~0ull;
    std::string folder_;
};

/// Registers liquid_points, rain_points and gas_volume with the core's
/// NodeRegistry: the nodes that bring a simulation back as geometry. Idempotent.
void registerSimGeometryNodes();

/// A node that makes its geometry from a frame of the simulation: the one
/// set last, for the frame about to be cooked. What it made of each frame
/// stays in the cook cache, as long as that frame stays the same -- a frame
/// simulated again dirties it, as a parameter would.
class FrameNode : public pg::Node {
public:
    using pg::Node::Node;
    void setFrame(int number, std::shared_ptr<const Frame> frame);
    bool isTimeDependentSelf() const override { return true; }

protected:
    std::shared_ptr<const Frame> frame_;

private:
    struct Seen {
        std::weak_ptr<const Frame> frame;
        bool any = false;  ///< there was one: an expired `frame` is not none
    };
    std::map<int, Seen> seen_;  ///< the frame each number was cooked from
};

}  // namespace pg::sim
