//
// The gas against the ROADMAP's step 5: the dust of the demolition example
// -- the tower, its pieces and the dust they raise, stepped as the example
// has it -- at the resolutions asked for. How long a frame of the Pyro
// Solver takes and where its time goes, how much memory the whole run holds,
// and how much of the domain the dust and its air take: what a sparse grid,
// which keeps only that, would store.
//
//   pgbench_pyro [RESOLUTION...] [--frames N] [--dense]
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

bool bench(int resolution, int frames, bool sparse) {
    sim::Network net;
    std::string error;
    if (!sim::Network::load(sim::Network::exampleText("demolition"), net, error)) {
        std::printf("the demolition example did not load: %s\n", error.c_str());
        return false;
    }
    const sim::Node* dust = net.named("dust");
    if (!dust) return false;
    net.setParam(dust->id, "resolution", std::to_string(resolution));
    net.setParam(dust->id, "sparse", sparse ? "1" : "0");
    const sim::Compiled c = net.compile(PG_SIM_EXAMPLES_DIR);
    if (!c.ok || !c.world.hasGas) {
        std::printf("the demolition did not compile\n");
        return false;
    }
    sim::WorldSolver world(c.world);
    const sim::PyroSolver& gas = *world.gas();
    const sim::Domain& d = gas.domain();
    header("Resolution " + std::to_string(resolution) + ": " + std::to_string(d.cells[0]) + " x " +
           std::to_string(d.cells[1]) + " x " + std::to_string(d.cells[2]) + " cells of " +
           std::to_string(d.voxel).substr(0, 5) + " m, " + std::to_string(d.cellCount() / 1000000) + "." +
           std::to_string(d.cellCount() / 100000 % 10) + " M voxels");
    std::printf("  %5s %10s %10s %8s %8s %8s %8s\n", "frame", "gas ms", "world ms", "gas", "air", "worked", "MB");
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
            std::printf("  %5d %10.0f %10.0f %7.1f%% %7.1f%% %7.1f%% %8.0f\n", f, g, ms,
                        100.0 * static_cast<double>(s.gas) / static_cast<double>(s.tiles),
                        100.0 * static_cast<double>(s.moving) / static_cast<double>(s.tiles),
                        100.0 * static_cast<double>(gas.activeCells()) / static_cast<double>(d.cellCount()), peakMb());
        }
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
    int frames = 120;
    bool sparse = true;
    for (int a = 1; a < argc; ++a) {
        const std::string s = argv[a];
        if (s == "--frames" && a + 1 < argc) frames = std::max(1, std::atoi(argv[++a]));
        else if (s == "--dense") sparse = false;
        else resolutions.push_back(std::atoi(argv[a]));
    }
    if (resolutions.empty()) resolutions = {96, 176};
    std::printf("gas -- benchmarks\n");
    std::printf("hardware threads available: %u\n", TaskPool::instance().threadCount());
    for (const int r : resolutions) {
        if (!bench(r, frames, sparse)) return 1;
    }
    return 0;
}
