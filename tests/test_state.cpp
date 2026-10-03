//
// Checkpoints: a simulation saved at a frame (WorldSolver::saveState) and
// loaded into a fresh solver goes on to the bit as if it had never stopped
// -- the gas sparse and dense, the water, the rain, the pieces stepped
// again -- and a state of another world or cut short is refused. And the
// preview of a world: its grids coarser, the rest as it is. And what a bake
// leaves in its cache folder: how far it has got, and its checkpoint.
//
#include "pg/sim/Cache.h"
#include "pg/sim/Network.h"
#include "pg/sim/World.h"

#include "test_framework.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>

using namespace pg;

namespace {

sim::World exampleWorld(const char* name) {
    sim::Network net;
    std::string error;
    CHECK(sim::Network::load(sim::Network::exampleText(name), net, error));
    const sim::Compiled c = net.compile(PG_SIM_EXAMPLES_DIR);
    CHECK(c.ok);
    return c.world;
}

/// The frames after `stop` of a run of `frames`, straight through and
/// resumed from a state saved at `stop`, as the cache writes them: they must
/// be the same bytes.
void checkResume(const sim::World& world, int stop, int frames) {
    sim::WorldSolver straight(world);
    std::string state;
    for (int f = 1; f <= stop; ++f) straight.step();
    state = straight.saveState();
    CHECK(!state.empty());

    sim::WorldSolver resumed(world);
    std::string error;
    CHECK(resumed.loadState(state, error));
    CHECK_EQ(error, std::string());
    CHECK_EQ(resumed.frame(), stop);
    CHECK_EQ(resumed.time(), straight.time());
    // Saved again at once, it is the same state.
    CHECK(resumed.saveState() == state);
    for (int f = stop + 1; f <= frames; ++f) {
        straight.step();
        resumed.step();
        CHECK(sim::formatFrame(straight.capture()) == sim::formatFrame(resumed.capture()));
    }
}

}  // namespace

TEST(state_resumes_the_sparse_gas_to_the_bit) {
    checkResume(sim::preview(exampleWorld("campfire"), 0.5f), 12, 20);
}

TEST(state_resumes_the_dense_gas_to_the_bit) {
    sim::World w = sim::preview(exampleWorld("campfire"), 0.5f);
    w.gas.solver.sparse = false;
    checkResume(w, 9, 15);
}

TEST(state_resumes_an_animated_source_to_the_bit) {
    checkResume(sim::preview(exampleWorld("fire_trail"), 0.4f), 10, 16);
}

TEST(state_resumes_the_water_to_the_bit) {
    checkResume(sim::preview(exampleWorld("dam_break"), 0.5f), 8, 14);
}

TEST(state_resumes_the_rain_on_the_water_to_the_bit) {
    checkResume(sim::preview(exampleWorld("rain_pond"), 0.5f), 10, 16);
}

TEST(state_resumes_the_pieces_and_their_dust_to_the_bit) {
    // The pieces are stepped again to the frame; the dust they raised is in
    // the gas that was saved.
    checkResume(sim::preview(exampleWorld("demolition"), 0.25f), 30, 36);
}

TEST(state_resumes_the_upres_to_the_bit) {
    // The fine gas, the noise its whirls are carried in -- across a layer
    // starting afresh -- and the solids it keeps out of, found again.
    sim::World w = sim::preview(exampleWorld("campfire"), 0.25f);
    w.hasUpres = true;
    w.upres.scale = 2;
    w.upres.swirlLife = 0.2f;
    sim::Collider ball;
    ball.center = Vec3(0.0f, 0.7f, 0.0f);
    ball.size = Vec3(0.3f);
    w.gas.colliders.push_back(ball);
    checkResume(w, 8, 15);
    // Without the upres, another world.
    sim::World plain = w;
    plain.hasUpres = false;
    sim::WorldSolver without(plain);
    without.step();
    sim::WorldSolver with(w);
    std::string error;
    CHECK(!with.loadState(without.saveState(), error));
    CHECK(error.find("other parts") != std::string::npos);
}

TEST(state_resumes_the_grains_and_the_pieces_they_push_to_the_bit) {
    // Gravel down a chute into two boxes it pushes: the grains, their
    // numbers and colours, and what they did to the pieces in each step --
    // the pieces stepped again with it.
    checkResume(exampleWorld("gravel_slide"), 26, 36);
    // Poured sand: grains still being made when it stopped.
    sim::World sand = exampleWorld("sand_pour");
    sand.grains.solver.substeps = 4;
    checkResume(sand, 6, 12);
}

TEST(state_of_another_world_or_cut_short_is_refused) {
    const sim::World fire = sim::preview(exampleWorld("campfire"), 0.5f);
    sim::WorldSolver solver(fire);
    for (int f = 0; f < 5; ++f) solver.step();
    const std::string state = solver.saveState();
    std::string error;

    // Another grid.
    {
        sim::WorldSolver other(sim::preview(exampleWorld("campfire"), 0.4f));
        CHECK(!other.loadState(state, error));
        CHECK(error.find("does not fit") != std::string::npos);
    }
    // Other parts.
    {
        sim::WorldSolver other(sim::preview(exampleWorld("dam_break"), 0.5f));
        CHECK(!other.loadState(state, error));
        CHECK(error.find("other parts") != std::string::npos);
    }
    // Cut short, anywhere.
    for (const size_t keep : {size_t(0), size_t(7), size_t(20), state.size() / 2, state.size() - 1}) {
        sim::WorldSolver other(fire);
        CHECK(!other.loadState(std::string_view(state).substr(0, keep), error));
    }
    // Not a state.
    {
        sim::WorldSolver other(fire);
        CHECK(!other.loadState("pgcache 1\nframes 3\n", error));
        CHECK_EQ(error, std::string("not a simulation state"));
    }
    // Into a solver that has stepped already.
    {
        sim::WorldSolver other(fire);
        other.step();
        CHECK(!other.loadState(state, error));
    }
}

TEST(preview_makes_the_grids_coarser_and_nothing_else) {
    const sim::World w = exampleWorld("rain_pond");
    const sim::World half = sim::preview(w, 0.5f);
    CHECK_EQ(half.water.solver.resolution, (w.water.solver.resolution + 1) / 2);
    CHECK_EQ(half.gas.solver.resolution, std::max(16, (w.gas.solver.resolution + 1) / 2));
    sim::World back = half;
    back.water.solver.resolution = w.water.solver.resolution;
    back.gas.solver.resolution = w.gas.solver.resolution;
    CHECK(back == w);
    // 1 and above: as it is; never below 16 cells.
    CHECK(sim::preview(w, 1.0f) == w);
    CHECK(sim::preview(w, 3.0f) == w);
    CHECK_EQ(sim::preview(w, 0.01f).water.solver.resolution, 16);
}

TEST(cache_note_says_how_far_a_bake_has_got_and_reads_back) {
    namespace fs = std::filesystem;
    const fs::path folder = fs::temp_directory_path() / ("pg_test_bake_" + std::to_string(std::random_device{}()));
    sim::CacheInfo info;
    info.frames = 43;
    info.fps = 24.0f;
    info.network = 0x0123456789abcdefull;
    info.of = 150;
    info.stepMs = 6212.34;
    info.checkpoint = 40;
    std::string error;
    CHECK(sim::writeCacheInfo(folder.string(), info, error));
    sim::CacheInfo back;
    CHECK(sim::readCacheInfo(folder.string(), back, error));
    CHECK_EQ(back.frames, 43);
    CHECK_EQ(back.of, 150);
    CHECK_NEAR(back.stepMs, 6212.3, 1e-2);
    CHECK_EQ(back.checkpoint, 40);
    CHECK_EQ(back.network, info.network);
    CHECK(!back.done());
    // Done: none of it is written, and a note without it reads as done.
    info.frames = 150;
    info.checkpoint = 0;
    CHECK(sim::writeCacheInfo(folder.string(), info, error));
    std::ifstream text(folder / "cache.txt");
    const std::string note((std::istreambuf_iterator<char>(text)), std::istreambuf_iterator<char>());
    CHECK(note.find("of ") == std::string::npos);
    CHECK(note.find("checkpoint") == std::string::npos);
    CHECK(sim::readCacheInfo(folder.string(), back, error));
    CHECK(back.done());
    // Nothing is left beside the files written.
    for (const fs::directory_entry& e : fs::directory_iterator(folder)) {
        CHECK(e.path().extension() != ".part");
    }
    fs::remove_all(folder);
}

TEST(checkpoint_file_holds_the_state_whole) {
    namespace fs = std::filesystem;
    const fs::path folder = fs::temp_directory_path() / ("pg_test_checkpoint_" + std::to_string(std::random_device{}()));
    const sim::World w = sim::preview(exampleWorld("campfire"), 0.4f);
    sim::WorldSolver solver(w);
    for (int f = 0; f < 6; ++f) solver.step();
    std::string error, state;
    CHECK(!sim::readCheckpoint(folder.string(), state, error));
    CHECK(sim::writeCheckpoint(folder.string(), solver.saveState(), error));
    CHECK(sim::readCheckpoint(folder.string(), state, error));
    CHECK(state == solver.saveState());
    sim::WorldSolver resumed(w);
    CHECK(resumed.loadState(state, error));
    CHECK_EQ(resumed.frame(), 6);
    // Written again: replaced whole.
    solver.step();
    CHECK(sim::writeCheckpoint(folder.string(), solver.saveState(), error));
    CHECK(sim::readCheckpoint(folder.string(), state, error));
    CHECK(state == solver.saveState());
    CHECK(!fs::exists(sim::checkpointFile(folder.string()) + ".part"));
    fs::remove_all(folder);
}

TEST(profile_says_where_the_time_of_a_step_went) {
    sim::WorldSolver solver(sim::preview(exampleWorld("demolition"), 0.25f));
    for (int f = 0; f < 3; ++f) solver.step();
    const sim::Frame f = solver.capture();
    const sim::Frame::Profile& p = f.profile;
    CHECK(p.gas > 0.0f);
    CHECK(p.rigid > 0.0f);
    CHECK_EQ(p.water, 0.0f);
    CHECK_EQ(p.rain, 0.0f);
    // The gas's stages are its time -- all but what goes round them.
    float stages = 0.0f;
    for (const float s : p.gasStages) {
        CHECK(s >= 0.0f);
        stages += s;
    }
    CHECK(stages <= p.gas * 1.01f + 0.5f);
    CHECK(stages >= p.gas * 0.5f);
    // Kept in memory, not in the cache.
    sim::Frame back;
    std::string error;
    CHECK(sim::parseFrame(sim::formatFrame(f), back, error));
    CHECK_EQ(back.profile.total(), 0.0f);
}

TEST(profile_says_where_the_water_spent_its_step) {
    // The flood with the crates: the water's stages are its time, the solids
    // found as the crates move among them.
    sim::WorldSolver solver(sim::preview(exampleWorld("flood_crates"), 0.5f));
    for (int f = 0; f < 3; ++f) solver.step();
    const sim::Frame::Profile& p = solver.capture().profile;
    CHECK(p.water > 0.0f);
    float stages = 0.0f;
    for (const float s : p.waterStages) {
        CHECK(s >= 0.0f);
        stages += s;
    }
    CHECK(p.waterStages[6] > 0.0f);  // the pressure
    CHECK(stages <= p.water * 1.01f + 0.5f);
    CHECK(stages >= p.water * 0.5f);
}
