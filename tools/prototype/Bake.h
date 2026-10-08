#pragma once
//
// A bake: the simulation of a network run to the end by a process of its
// own -- `prototype sim NETWORK - --cache FOLDER --checkpoint K` -- into a
// cache folder, what Houdini calls Save to Disk in Background. The editor
// stays free: it plays the frames as they land (SimRunner::stream), says how
// far the bake has got and how long it has to go, and can stop it; closed,
// it leaves the bake running. The cache folder holds the network baked
// (network.pgsim) and what the process said (bake.log) beside the frames.
//
#include "pg/sim/Cache.h"

#include <sys/types.h>

#include <string>
#include <vector>

namespace pg::editor {

class Bake {
public:
    Bake() = default;
    ~Bake();
    Bake(const Bake&) = delete;
    Bake& operator=(const Bake&) = delete;

    /// Bakes network `text` into `folder` -- its relative paths read from
    /// `networkFolder` -- `frames` frames, the state saved every `checkpoint`
    /// frames. `resume`: on from the folder's checkpoint; else the folder's
    /// cache is thrown away first. `card`: the GPU its gas steps on, as
    /// PG_GPU names one; empty: the editor's own environment. False, with
    /// why, if it cannot start.
    bool start(const std::string& text, const std::string& networkFolder, const std::string& folder, int frames,
               int checkpoint, bool resume, std::string& error, const std::string& card = "");
    /// Stops it: the frames written so far stay, and the last checkpoint.
    void cancel();

    /// Looks at the process and at its cache.txt: call it now and then.
    void poll();
    bool running() const { return pid_ > 0; }
    /// It has ended since it started: `failed` when not with every frame --
    /// cancelled, or with an error (`why`: the last line it wrote).
    bool ended() const { return ended_; }
    bool failed() const { return failed_; }
    bool cancelled() const { return cancelled_; }
    const std::string& why() const { return why_; }
    const std::string& folder() const { return folder_; }
    int frames() const { return frames_; }
    /// What its cache.txt said last.
    const sim::CacheInfo& progress() const { return progress_; }
    /// Seconds it has run, and a guess of how many are left: 0 until a frame
    /// is done.
    double seconds() const;
    double secondsLeft() const;

    /// The folder holds a bake cut short that `text` could go on with: its
    /// checkpoint, of this network.
    static bool canResume(const std::string& folder, const std::string& text);
    /// "300 ms", "6.2 s", "4 min 12 s", "1 h 3 min"
    static std::string duration(double seconds);

private:
    pid_t pid_ = 0;
    std::string folder_;
    int frames_ = 0;
    bool ended_ = false, failed_ = false, cancelled_ = false;
    std::string why_;
    sim::CacheInfo progress_;
    double started_ = 0.0;  ///< seconds, on a steady clock
    double finished_ = 0.0;
    int startFrame_ = 0;    ///< the frame it went on from: 0 from the start
};

}  // namespace pg::editor
