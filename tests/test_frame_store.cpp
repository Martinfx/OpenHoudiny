//
// The frames a timeline plays (src/pg/sim/FrameStore.h): kept in memory
// within a budget, spilled to disk past it and read back the same, a cache
// on disk read ahead of the play head on a thread of its own, ready() that
// never waits -- and all of it from several threads at once.
//
#include "pg/sim/Cache.h"
#include "pg/sim/FrameStore.h"

#include "test_framework.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

using namespace pg;
using namespace pg::sim;
namespace fs = std::filesystem;

namespace {

/// Frame `n`: gas over 32^3 cells, its values its own -- some 200 kB.
std::shared_ptr<const Frame> frameOf(int n) {
    auto f = std::make_shared<Frame>();
    f->number = n;
    f->time = static_cast<float>(n) / 30.0f;
    f->domain.cells[0] = f->domain.cells[1] = f->domain.cells[2] = 32;
    f->domain.voxel = 0.05f;
    f->fields.resize(3 * f->domain.cellCount());
    for (size_t i = 0; i < f->fields.size(); ++i) {
        f->fields[i] = toHalf(static_cast<float>(n) + 0.001f * static_cast<float>(i % 997));
    }
    return f;
}

/// Waits -- at most 20 s -- until `done` says so.
bool waitFor(const std::function<bool()>& done) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!done()) {
        if (std::chrono::steady_clock::now() > until) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

/// A folder of its own in the temporary one, empty.
fs::path scratch(const char* name) {
    const fs::path dir = fs::temp_directory_path() / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

}  // namespace

TEST(frame_store_keeps_what_fits_in_memory) {
    FrameStore store;
    const unsigned g = store.generation();
    std::vector<std::shared_ptr<const Frame>> frames;
    for (int n = 1; n <= 10; ++n) {
        frames.push_back(frameOf(n));
        CHECK(store.add(frames.back(), g));
    }
    CHECK_EQ(store.count(), 10);
    CHECK_EQ(store.memoryFrames(), 10);
    CHECK(store.get(5) == frames[4]);
    CHECK(store.ready(10) == frames[9]);
    CHECK(store.spillFolder().empty());
    CHECK(store.room());
    // Only the next frame, and only of this generation.
    CHECK(!store.add(frameOf(12), g));
    CHECK(!store.add(frameOf(11), g + 1));
    store.truncate(4);
    CHECK_EQ(store.count(), 4);
    CHECK(store.get(5) == nullptr);
    CHECK(store.add(frameOf(5), g));
}

TEST(frame_store_spills_past_its_budget_and_reads_back) {
    FrameStore store;
    const size_t size = frameOf(1)->bytes();
    store.setBudget(size * 7 / 2);  // three frames and a half
    std::atomic<int> prepared{0};
    store.setPrepare([&](Frame&) { ++prepared; });
    std::vector<std::shared_ptr<const Frame>> frames;
    const unsigned g = store.generation();
    for (int n = 1; n <= 12; ++n) {
        frames.push_back(frameOf(n));
        CHECK(store.add(frames.back(), g));
        CHECK(store.memoryBytes() <= size * 7 / 2);
        CHECK(store.room());
    }
    // The play head stays at 1: what is kept is what is nearest it.
    CHECK(store.inMemory(1) && store.inMemory(2) && store.inMemory(3));
    CHECK(!store.inMemory(12));
    CHECK(store.spilledFrames() >= 9);
    const std::string folder = store.spillFolder();
    CHECK(!folder.empty() && fs::is_regular_file(frameFile(folder, 12)));
    CHECK(store.spilledBytes() > 9 * size / 2);
    // Every frame back as it went in -- read from disk and prepared.
    for (int n = 1; n <= 12; ++n) {
        const std::shared_ptr<const Frame> f = store.get(n);
        CHECK(f != nullptr);
        if (f) CHECK(formatFrame(*f) == formatFrame(*frames[static_cast<size_t>(n - 1)]));
    }
    CHECK(prepared.load() >= 9);
    CHECK(store.memoryBytes() <= size * 9 / 2);  // what was read last, and the play head's
    // Thrown away: what was spilled too.
    store.clear();
    CHECK(!fs::exists(folder));
    CHECK_EQ(store.count(), 0);
}

TEST(frame_store_without_spilling_is_full) {
    FrameStore store;
    const size_t size = frameOf(1)->bytes();
    store.setBudget(size * 7 / 2);
    store.setSpill(false, 0);
    const unsigned g = store.generation();
    int added = 0;
    while (store.room() && added < 20) CHECK(store.add(frameOf(++added), g));
    CHECK_EQ(added, 4);
    CHECK_EQ(store.memoryFrames(), 4);  // nothing to let them go to
    CHECK(store.spillFolder().empty());
    // More memory: room again.
    store.setBudget(size * 10);
    CHECK(store.room());
}

TEST(frame_store_reads_a_cache_ahead_of_the_play_head) {
    const fs::path dir = scratch("pg_test_frame_store_stream");
    std::vector<std::shared_ptr<const Frame>> frames;
    std::string error;
    for (int n = 1; n <= 30; ++n) {
        frames.push_back(frameOf(n));
        CHECK(writeFrame(*frames.back(), dir.string(), error));
    }
    FrameStore store;
    const size_t size = frameOf(1)->bytes();
    store.setBudget(size * 6);
    store.stream(dir.string(), 20);
    CHECK_EQ(store.count(), 20);
    CHECK(!store.room());  // nothing is added to a cache read from disk
    // Ahead of the play head, as far as the budget holds -- and a frame
    // behind for every two ahead: six frames, 3 to 8.
    store.setPlayhead(5, 1);
    CHECK(waitFor([&] {
        bool all = store.memoryFrames() == 6;
        for (int n = 3; n <= 8; ++n) all = all && store.inMemory(n);
        return all;
    }));
    CHECK(!store.inMemory(2) && !store.inMemory(9) && !store.inMemory(20));
    CHECK(store.memoryBytes() <= size * 6);
    // Played on: the frames it has left go, the next come.
    store.setPlayhead(9, 1);
    CHECK(waitFor([&] { return store.inMemory(12) && !store.inMemory(5); }));
    // Asked for, a frame comes first -- ready() says no at once, and soon yes.
    CHECK(store.ready(17) == nullptr || store.inMemory(17));
    std::shared_ptr<const Frame> got;  // held: the store may let it go again
    CHECK(waitFor([&] { return (got = store.ready(17)) != nullptr; }));
    if (got) CHECK(formatFrame(*got) == formatFrame(*frames[16]));
    // Played backward from 18: what is behind it is read, what is ahead goes.
    store.setPlayhead(18, -1);
    CHECK(waitFor([&] { return store.inMemory(18) && store.inMemory(15); }));
    CHECK(waitFor([&] { return !store.inMemory(5); }));
    // More frames on disk: the cache grows.
    store.found(30);
    CHECK_EQ(store.count(), 30);
    got = store.get(30);
    CHECK(got != nullptr);
    if (got) CHECK(formatFrame(*got) == formatFrame(*frames[29]));
    // A frame that is not there cannot be read, and is not asked for again.
    fs::remove(frameFile(dir.string(), 25));
    CHECK(store.get(25) == nullptr);
    CHECK(store.get(25) == nullptr);
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST(frame_store_takes_frames_and_questions_from_several_threads) {
    // A simulation adding frames past the budget, a timeline asking and
    // moving its play head, an export reading them all -- then everything
    // thrown away while reads and writes are under way.
    FrameStore store;
    const size_t size = frameOf(1)->bytes();
    store.setBudget(size * 5);
    store.setPrepare([](Frame& f) { f.stepMs = 1.0; });
    const unsigned g = store.generation();
    std::atomic<bool> stop{false};
    std::thread simulation([&] {
        for (int n = 1; n <= 60 && !stop; ++n) store.add(frameOf(n), g);
    });
    std::thread timeline([&] {
        for (int i = 0; !stop && i < 4000; ++i) {
            const int n = 1 + (i * 7) % 60;
            store.setPlayhead(n, (i / 100) % 2 ? -1 : 1);
            store.ready(n);
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    });
    simulation.join();
    CHECK_EQ(store.count(), 60);
    for (int n = 1; n <= 60; ++n) {
        const std::shared_ptr<const Frame> f = store.get(n);
        CHECK(f != nullptr && f->number == n);
        if (f) CHECK(f->fields[0] == toHalf(static_cast<float>(n)));
    }
    stop = true;
    timeline.join();
    const std::string folder = store.spillFolder();
    store.setPlayhead(1, 1);
    store.ready(40);
    store.clear();
    // (A frame being spilled as it was cleared is deleted once written.)
    CHECK(folder.empty() || waitFor([&] { return !fs::exists(folder); }));
    CHECK(!store.add(frameOf(1), g));
    CHECK(store.add(frameOf(1), store.generation()));
}

TEST(frame_store_deletes_the_spills_of_programs_no_longer_running) {
    namespace fs = std::filesystem;
    const fs::path tmp = fs::temp_directory_path();
#ifdef _WIN32
    const long self = static_cast<long>(_getpid());
#else
    const long self = static_cast<long>(getpid());
#endif
    // A crashed program's, long untouched; one's that runs -- this; one
    // crashed a moment ago, which may yet be another's elsewhere.
    const fs::path dead = tmp / "prototype-frames-2147483-1";
    const fs::path live = tmp / ("prototype-frames-" + std::to_string(self) + "-999");
    const fs::path fresh = tmp / "prototype-frames-2147483-2";
    const auto old = fs::file_time_type::clock::now() - std::chrono::minutes(30);
    for (const fs::path& p : {dead, live, fresh}) {
        fs::create_directories(p);
        std::ofstream(p / "frame.0001.pgf") << "frame";
    }
    fs::last_write_time(dead, old);
    fs::last_write_time(live, old);
    CHECK(FrameStore::removeLeftSpills(10) >= 1);
    CHECK(!fs::exists(dead));
    CHECK(fs::exists(live));
    CHECK(fs::exists(fresh));
    std::error_code ec;
    fs::remove_all(live, ec);
    fs::remove_all(fresh, ec);
}
