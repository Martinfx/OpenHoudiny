//
// The gas against the ROADMAP's step 5: the dust of the demolition example
// -- the tower, its pieces and the dust they raise, stepped as the example
// has it -- at the resolutions asked for. How long a frame of the Pyro
// Solver takes and where its time goes, how much memory the whole run holds,
// and how much of the domain the dust and its air take: what a sparse grid,
// which keeps only that, would store.
//
//   pgbench_pyro [RESOLUTION...] [--frames N] [--dense] [--set PARAM=VALUE]... [--example NAME]
//                [--upres SCALE]
//
// --example steps another example's gas instead: its first Pyro Solver.
// --set changes a parameter of that Pyro Solver. --upres puts a Pyro Upres
// of that scale after it: how long its frames take, where their time goes
// and how much of the fine grid it works on.
//
#include "pg/core/Parallel.h"
#include "pg/sim/Network.h"
#include "pg/sim/World.h"

#include <sys/resource.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace pg;
using Clock = std::chrono::steady_clock;

namespace {

double since(Clock::time_point t0) { return std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); }

void header(const std::string& title) {
    std::printf("\n%s\n%s\n", title.c_str(), std::string(title.size(), '-').c_str());
}

/// The most memory the process has held so far, MB.
double peakMb() {
    rusage u{};
    getrusage(RUSAGE_SELF, &u);
    return static_cast<double>(u.ru_maxrss) / 1024.0;
}

/// Of the 8^3 tiles of the domain: how many hold smoke or heat, and how many
/// air that moves -- faster than `slow`, world units a second.
struct Share {
    size_t tiles = 0, gas = 0, moving = 0;
};

Share share(const sim::PyroSolver& s, float slow) {
    const int tx = (s.nx() + 7) / 8, ty = (s.ny() + 7) / 8, tz = (s.nz() + 7) / 8;
    Share r;
    r.tiles = static_cast<size_t>(tx) * static_cast<size_t>(ty) * static_cast<size_t>(tz);
    std::vector<char> gas(r.tiles, 0), moving(r.tiles, 0);
    for (int k = 0; k < s.nz(); ++k) {
        for (int j = 0; j < s.ny(); ++j) {
            for (int i = 0; i < s.nx(); ++i) {
                const size_t t = static_cast<size_t>(i / 8) +
                                 static_cast<size_t>(tx) * (static_cast<size_t>(j / 8) + static_cast<size_t>(ty) * static_cast<size_t>(k / 8));
                if (s.density().at(i, j, k) > 1e-4f || s.temperature().at(i, j, k) > 1e-4f) gas[t] = 1;
                const float u = std::fabs(s.velocity(0).at(i, j, k)), v = std::fabs(s.velocity(1).at(i, j, k)),
                            w = std::fabs(s.velocity(2).at(i, j, k));
                if (std::max({u, v, w}) > slow) moving[t] = 1;
            }
        }
    }
    for (size_t t = 0; t < r.tiles; ++t) {
        r.gas += gas[t] ? 1u : 0u;
        r.moving += moving[t] || gas[t] ? 1u : 0u;
    }
    return r;
}

/// Of the fine grid's tiles: how many are worked on, and how many of them
/// hold smoke, heat or flame.
Share share(const sim::UpresSolver& u) {
    Share r;
    const sim::Tiles& tiles = u.tiles();
    r.tiles = tiles.stored().size();
    for (size_t s = 0; s < tiles.stored().size(); ++s) {
        const size_t base = s * sim::Tiles::kCells;
        bool any = false;
        for (const sim::SparseGrid* g : {&u.density(), &u.temperature(), &u.flame()}) {
            for (int c = 0; c < sim::Tiles::kCells && !any; ++c) any = g->data()[base + static_cast<size_t>(c)] > 1e-4f;
        }
        r.gas += any ? 1u : 0u;
    }
    return r;
}

bool bench(const std::string& example, int resolution, int frames, bool sparse, const std::vector<std::string>& sets,
           int upres) {
    sim::Network net;
    std::string error;
    if (!sim::Network::load(sim::Network::exampleText(example), net, error)) {
        std::printf("the %s example did not load: %s\n", example.c_str(), error.c_str());
        return false;
    }
    const sim::Node* dust = nullptr;
    for (const sim::Node& n : net.nodes()) {
        if (n.type == "pyro_solver") {
            dust = &n;
            break;
        }
    }
    if (!dust) {
        std::printf("the %s example has no Pyro Solver\n", example.c_str());
        return false;
    }
    net.setParam(dust->id, "resolution", std::to_string(resolution));
    net.setParam(dust->id, "sparse", sparse ? "1" : "0");
    for (const std::string& set : sets) {
        const size_t eq = set.find('=');
        if (eq != std::string::npos) net.setParam(dust->id, set.substr(0, eq), set.substr(eq + 1));
    }
    if (upres > 0) {
        // The Pyro Upres between the solver and whatever takes its gas.
        const int id = dust->id;
        const int node = net.add("pyro_upres");
        net.setParam(node, "scale", std::to_string(upres));
        for (const sim::Link& l : std::vector<sim::Link>(net.links())) {
            if (l.from == id && l.output == "gas") net.connect(node, "gas", l.to, l.input);
        }
        net.connect(id, "gas", node, "gas");
    }
    const sim::Compiled c = net.compile(PG_SIM_EXAMPLES_DIR);
    if (!c.ok || !c.world.hasGas) {
        std::printf("the %s example did not compile\n", example.c_str());
        return false;
    }
    sim::WorldSolver world(c.world);
    const sim::PyroSolver& gas = *world.gas();
    const sim::Domain& d = gas.domain();
    header("Resolution " + std::to_string(resolution) + ": " + std::to_string(d.cells[0]) + " x " +
           std::to_string(d.cells[1]) + " x " + std::to_string(d.cells[2]) + " cells of " +
           std::to_string(d.voxel).substr(0, 5) + " m, " + std::to_string(d.cellCount() / 1000000) + "." +
           std::to_string(d.cellCount() / 100000 % 10) + " M voxels");
    std::printf("  %5s %10s %10s %8s %8s %8s %8s %8s\n", "frame", "gas ms", "world ms", "gas", "air", "worked", "MB",
                "div 1/s");
    double gasMs = 0.0, worldMs = 0.0;
    sim::PyroSolver::Times before = gas.times();
    for (int f = 1; f <= frames; ++f) {
        const auto t0 = Clock::now();
        world.step();
        const double ms = since(t0);
        worldMs += ms;
        const double g = gas.times().total() - before.total();
        before = gas.times();
        gasMs += g;
        if (f % 10 == 0 || f == frames) {
            const Share s = share(gas, 0.05f);
            std::printf("  %5d %10.0f %10.0f %7.1f%% %7.1f%% %7.1f%% %8.0f %8.4f\n", f, g, ms,
                        100.0 * static_cast<double>(s.gas) / static_cast<double>(s.tiles),
                        100.0 * static_cast<double>(s.moving) / static_cast<double>(s.tiles),
                        100.0 * static_cast<double>(gas.activeCells()) / static_cast<double>(d.cellCount()), peakMb(),
                        gas.meanDivergence());
        }
    }
    if (const sim::UpresSolver* u = world.upres()) {
        const sim::Domain& fine = u->domain();
        const sim::UpresSolver::Times& t = u->times();
        const Share s = share(*u);
        std::printf("  upres %d: %d x %d x %d fine cells; %.0f ms a frame; %zu tiles worked on, %.0f%% with gas, "
                    "%.1f%% of the fine grid\n",
                    fine.cells[0] / std::max(d.cells[0], 1), fine.cells[0], fine.cells[1], fine.cells[2],
                    t.total() / frames, s.tiles, 100.0 * static_cast<double>(s.gas) / static_cast<double>(std::max<size_t>(s.tiles, 1)),
                    100.0 * static_cast<double>(u->activeCells()) / static_cast<double>(fine.cellCount()));
        std::printf("  the upres's time: tiles %.0f%%, solids %.0f%%, emit %.0f%%, swirl %.0f%%, advect %.0f%%, "
                    "combust %.0f%%\n",
                    100.0 * t.tiles / t.total(), 100.0 * t.solids / t.total(), 100.0 * t.emit / t.total(),
                    100.0 * t.swirl / t.total(), 100.0 * t.advect / t.total(), 100.0 * t.combust / t.total());
        uint64_t print = 1469598103934665603ull;
        for (const sim::SparseGrid* g : {&u->density(), &u->temperature(), &u->flame()}) {
            for (const float v : g->values()) {
                uint32_t b;
                std::memcpy(&b, &v, sizeof b);
                print = (print ^ b) * 1099511628211ull;
            }
        }
        std::printf("  fingerprint of the upres's last frame: %016llx\n", static_cast<unsigned long long>(print));
    }
    const sim::PyroSolver::Times& t = gas.times();
    std::printf("  a frame: gas %.0f ms, the whole world %.0f ms\n", gasMs / frames, worldMs / frames);
    std::printf("  the gas's time: solids %.0f%%, emit %.0f%%, advect %.0f%%, combust %.0f%%, forces %.0f%%, "
                "project %.0f%%, dissipate %.0f%%\n",
                100.0 * t.solids / t.total(), 100.0 * t.emit / t.total(), 100.0 * t.advect / t.total(),
                100.0 * t.combust / t.total(), 100.0 * t.forces / t.total(), 100.0 * t.project / t.total(),
                100.0 * t.dissipate / t.total());
    // What the run came to, as one number: a change meant to be only faster
    // must leave it as it is.
    uint64_t print = 1469598103934665603ull;
    auto mix = [&](const auto& g) {
        for (int k = 0; k < g.nz(); ++k) {
            for (int j = 0; j < g.ny(); ++j) {
                for (int i = 0; i < g.nx(); ++i) {
                    const float v = g.at(i, j, k);
                    uint32_t b;
                    std::memcpy(&b, &v, sizeof b);
                    print = (print ^ b) * 1099511628211ull;
                }
            }
        }
    };
    mix(gas.density());
    mix(gas.temperature());
    for (int a = 0; a < 3; ++a) mix(gas.velocity(a));
    std::printf("  fingerprint of the last frame: %016llx\n", static_cast<unsigned long long>(print));
    std::printf("  memory held at most: %.0f MB\n", peakMb());
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<int> resolutions;
    int frames = 120, upres = 0;
    bool sparse = true;
    std::string example = "demolition";
    std::vector<std::string> sets;
    for (int a = 1; a < argc; ++a) {
        const std::string s = argv[a];
        if (s == "--frames" && a + 1 < argc) frames = std::max(1, std::atoi(argv[++a]));
        else if (s == "--example" && a + 1 < argc) example = argv[++a];
        else if (s == "--set" && a + 1 < argc) sets.push_back(argv[++a]);
        else if (s == "--dense") sparse = false;
        else if (s == "--upres" && a + 1 < argc) upres = std::atoi(argv[++a]);
        else resolutions.push_back(std::atoi(argv[a]));
    }
    if (resolutions.empty()) resolutions = {96, 176};
    std::printf("gas -- benchmarks\n");
    std::printf("hardware threads available: %u\n", TaskPool::instance().threadCount());
    for (const int r : resolutions) {
        if (!bench(example, r, frames, sparse, sets, upres)) return 1;
    }
    return 0;
}
