#pragma once
//
// A wedge: one parameter of a network set to several values, each variant
// baked to the end into a folder of its own, one after another -- what a
// Wedge TOP does in Houdini, to choose between versions of a simulation by
// looking at them. Each variant is a Bake (Bake.h): the network with the
// value set, at the full resolution. The folders sit side by side under one,
// with wedge.txt saying which value each holds:
//
//   <root>/wedge.txt      "pgwedge 1", the node and the parameter, then
//                         "variant FOLDER VALUE" a line
//   <root>/<param>_1/     a cache folder, as Bake to Disk makes it
//   <root>/<param>_2/ ...
//
#include "Bake.h"

#include "pg/sim/Network.h"

#include <string>
#include <vector>

namespace pg::editor {

class Wedge {
public:
    struct Variant {
        float value = 0.0f;
        std::string folder;
        enum class State { Waiting, Baking, Done, Failed, Cancelled } state = State::Waiting;
        std::string why;       ///< Failed: what the bake said
        double seconds = 0.0;  ///< how long its bake took
    };

    /// The values from `from` to `to`, `count` of them evenly -- whole
    /// numbers for an Int parameter, none twice.
    static std::vector<float> values(float from, float to, int count, bool whole);

    /// Bakes `net` with parameter `param` of node `node` (a Float or an Int)
    /// set to each of `values`, into folders under `root`: `frames` frames,
    /// the network's relative paths read from `networkFolder`. False, with
    /// why, if it cannot start.
    bool start(const sim::Network& net, int node, const std::string& param, const std::vector<float>& values,
               const std::string& root, const std::string& networkFolder, int frames, std::string& error);
    /// Stops the variant baking and those waiting.
    void cancel();
    /// Looks at the bake running, and starts the next when it has ended:
    /// call it now and then. True when a variant has ended since.
    bool poll();

    bool running() const;
    bool any() const { return !variants_.empty(); }
    const std::vector<Variant>& variants() const { return variants_; }
    /// The variant baking; -1 when none is.
    int baking() const { return baking_; }
    const Bake& bake() const { return bake_; }
    int node() const { return node_; }
    const std::string& nodeName() const { return nodeName_; }
    const std::string& param() const { return param_; }
    const std::string& root() const { return root_; }
    int frames() const { return frames_; }
    /// The network of variant `i`, as its bake got it.
    const std::string& text(size_t i) const { return texts_[i]; }

private:
    /// Starts the next variant waiting, if there is one.
    void next();

    Bake bake_;
    std::vector<Variant> variants_;
    std::vector<std::string> texts_;
    int baking_ = -1;
    int node_ = 0;
    std::string nodeName_, param_, root_, networkFolder_;
    int frames_ = 0;
    bool cancelled_ = false;
};

}  // namespace pg::editor
