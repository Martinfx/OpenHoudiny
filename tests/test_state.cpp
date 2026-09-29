//
// Checkpoints: a simulation saved at a frame (WorldSolver::saveState) and
// loaded into a fresh solver goes on to the bit as if it had never stopped
// -- the gas sparse and dense, the water, the rain, the pieces stepped
// again -- and a state of another world or cut short is refused. And the
// preview of a world: its grids coarser, the rest as it is.
//
#include "pg/sim/Cache.h"
#include "pg/sim/Network.h"
#include "pg/sim/World.h"

#include "test_framework.h"

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
