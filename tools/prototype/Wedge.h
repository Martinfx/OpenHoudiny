#pragma once
//
// A wedge: one parameter of a network set to several values, each variant
// baked to the end into a folder of its own, one after another -- or, given
// several cards, one on each at once -- what a
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

#include <memory>
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
        std::string card;      ///< the GPU it baked on, as PG_GPU names it; empty: the editor's
    };

    /// The values from `from` to `to`, `count` of them evenly -- whole
    /// numbers for an Int parameter, none twice.
    static std::vector<float> values(float from, float to, int count, bool whole);

    /// Bakes `net` with parameter `param` of node `node` (a Float or an Int)
    /// set to each of `values`, into folders under `root`: `frames` frames,
    /// the network's relative paths read from `networkFolder`. `cards`: the
    /// GPUs to bake on, a variant on each at once (as PG_GPU names them);
    /// none: one variant at a time, on what the editor's environment says.
    /// False, with why, if it cannot start.
    bool start(const sim::Network& net, int node, const std::string& param, const std::vector<float>& values,
               const std::string& root, const std::string& networkFolder, int frames, std::string& error,
               const std::vector<std::string>& cards = {});
    /// Stops the variants baking and those waiting.
    void cancel();
    /// Looks at the bakes running, and starts the next where one has ended:
    /// call it now and then. True when a variant has ended since.
    bool poll();

    bool running() const;
    bool any() const { return !variants_.empty(); }
    const std::vector<Variant>& variants() const { return variants_; }
    /// How many variants are baking, and how many have ended.
    int baking() const;
    int ended() const;
    /// The bake of variant `i` while it bakes; null when it does not.
    const Bake* bakeOf(size_t i) const;
    int node() const { return node_; }
    const std::string& nodeName() const { return nodeName_; }
    const std::string& param() const { return param_; }
    const std::string& root() const { return root_; }
    int frames() const { return frames_; }
    /// The network of variant `i`, as its bake got it.
    const std::string& text(size_t i) const { return texts_[i]; }

private:
    /// Starts the next variants waiting on the lanes free, if there are.
    void next();

    /// A card and the bake on it: the variant it bakes, -1 for none.
    struct Lane {
        std::string card;
        std::unique_ptr<Bake> bake = std::make_unique<Bake>();
        int variant = -1;
    };
    std::vector<Lane> lanes_;
    std::vector<Variant> variants_;
    std::vector<std::string> texts_;
    int node_ = 0;
    std::string nodeName_, param_, root_, networkFolder_;
    int frames_ = 0;
    bool cancelled_ = false;
};

}  // namespace pg::editor
