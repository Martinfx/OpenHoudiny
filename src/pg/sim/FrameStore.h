#pragma once
//
// The frames of a simulation as a timeline plays them, within a budget of
// memory: frames 1 to count(), each in memory, on disk, or both.
//
//   - Frames added as they are simulated stay in memory. Past the budget
//     the ones furthest from the play head go -- written first to a folder of
//     the store's own when they are nowhere else (spilled) -- so a
//     simulation longer than the memory still plays whole, read back as it
//     is played. Without spilling, or past its budget of disk, the store is
//     full: room() is false, and nothing more is to be added.
//   - A cache on disk (stream()) is read as it is asked for.
//   - A thread of its own reads ahead of the play head, the way it plays: a
//     timeline asks with ready(), which never waits, and plays on as the
//     frames arrive. get() waits: for what needs the frame now -- an export,
//     a render, geometry cooked from it.
//
// Furthest from the play head: ahead of it, by how many frames; behind it,
// twice as many -- a frame just played is less likely wanted again than
// the next. What is read ahead is what the budget holds of the frames
// nearest it, and no more than kAhead.
//
// Thread-safe: frames are added on one thread, asked for on others.
//
#include "pg/sim/Frame.h"

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pg::sim {

class FrameStore {
public:
    /// What a frame read from disk needs before it is used -- the world's
    /// pieces and cloth (adoptPieces, adoptCloth). Called on the thread that
    /// read it, one frame at a time.
    using Prepare = std::function<void(Frame&)>;

    /// Frames read ahead of the play head at most; behind it, a fifth.
    static constexpr int kAhead = 240;

    FrameStore();
    ~FrameStore();
    FrameStore(const FrameStore&) = delete;
    FrameStore& operator=(const FrameStore&) = delete;

    /// The folders frames were spilled to by programs no longer running --
    /// a crash, a kill, which ~FrameStore never saw -- deleted, those of
    /// the last `minutes` spared. How many went.
    static int removeLeftSpills(int minutes = 10);

    /// Nothing kept; what was spilled deleted.
    void clear();
    /// In place of what it kept: the frames of the cache in `folder`,
    /// `count` of them there now, read as they are asked for.
    void stream(const std::string& folder, int count);
    /// Streaming: the frames there now, if more.
    void found(int count);
    /// Simulated, the next frame: frame count() + 1. Past the budget what is
    /// furthest from the play head goes (spilled, if it must be): on the
    /// caller's thread. False -- nothing added -- for a frame of another
    /// number, or when `generation` is not the store's: it was cleared since.
    bool add(std::shared_ptr<const Frame> frame, unsigned generation);
    /// Frames 1 .. count kept, the rest let go.
    void truncate(int count);

    void setPrepare(Prepare prepare);
    void setBudget(size_t bytes);
    /// Spill past the budget of memory, writing at most `disk` bytes.
    void setSpill(bool on, size_t disk);
    bool spilling() const;
    /// Where the timeline is, and which way it goes: +1 forward, -1
    /// backward, 0 standing (as forward).
    void setPlayhead(int frame, int direction);

    /// Bumped whenever the frames are thrown away: clear(), stream(),
    /// truncate().
    unsigned generation() const;
    int count() const;
    /// Frame `n`: from memory, or read from disk now. Null if there is none.
    std::shared_ptr<const Frame> get(int n);
    /// Frame `n` if it is in memory; else null -- and it is read next.
    std::shared_ptr<const Frame> ready(int n);
    bool inMemory(int n) const;
    /// The runs of frames in memory, first and last of each.
    std::vector<std::pair<int, int>> inMemory() const;
    /// Another frame can be added: there is room in memory, or room on disk
    /// to spill to.
    bool room() const;
    bool streaming() const;
    size_t budget() const;
    size_t memoryBytes() const;
    int memoryFrames() const;
    /// What was spilled, and where; empty until something is.
    size_t spilledBytes() const;
    int spilledFrames() const;
    std::string spillFolder() const;
    /// Why spilling stopped -- a disk full, a folder not writable; empty if
    /// it did not.
    std::string spillError() const;

private:
    struct Slot {
        std::shared_ptr<const Frame> frame;  // in memory, or null
        size_t bytes = 0;                    // in memory
        bool onDisk = false;                 // in the stream's folder, or spilled
        bool busy = false;                   // being read or written
        bool failed = false;                 // could not be read
    };

    /// How far frame `n` is from the play head, the way it plays.
    long cost(int n) const;
    /// The frame to read next: the one asked for, else the nearest the play
    /// head that is on disk only and that the budget can hold; 0 if none.
    int nextToRead() const;
    /// Reads frame `n` with the lock let go, and keeps it.
    void readInto(std::unique_lock<std::mutex>& lock, int n);
    /// Lets go of the frames furthest from the play head -- spilling those
    /// on disk nowhere -- until the memory is within the budget; never
    /// `keep` nor the play head's.
    void makeRoom(std::unique_lock<std::mutex>& lock, int keep);
    bool canSpill() const;
    void dropSpill();
    void readLoop();

    mutable std::mutex mu_;
    std::condition_variable wake_;
    std::mutex io_;  // one read at a time: Prepare's memos are not shared
    std::vector<Slot> slots_;
    std::string folder_;  // streaming: the cache's
    bool streaming_ = false;
    Prepare prepare_;
    size_t budget_ = size_t(1536) << 20, memory_ = 0;
    bool spill_ = true;
    size_t diskBudget_ = size_t(64) << 30, spilled_ = 0;
    int spilledFrames_ = 0;
    std::string spillFolder_, spillError_;
    int playhead_ = 1, direction_ = 1, wanted_ = 0;
    unsigned generation_ = 0;
    bool quit_ = false;
    std::thread reader_;
};

}  // namespace pg::sim
