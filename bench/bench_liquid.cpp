//
// The water against the ROADMAP's step 5: an example's Liquid Solver --
// flood_crates unless asked for another, the pieces in it stepped as the
// example has them -- at the resolutions asked for. How long a frame of the
// water takes and where its time goes, how much memory the whole run holds,
// and how much of the domain the water takes: the 8^3 tiles with particles
// in them, and the tiles the solver keeps -- those and the tiles round them
// (all of them with --set sparse=0).
//
//   pgbench_liquid [RESOLUTION...] [--frames N] [--set PARAM=VALUE]... [--example NAME] [--threads N]
//
// --set changes a parameter of the Liquid Solver; --threads runs on N
// threads (all there are by default) -- the same water on any number.
//
#include "pg/core/Parallel.h"
#include "pg/sim/Liquid.h"
#include "pg/sim/Network.h"
#include "pg/sim/World.h"

#include <sys/resource.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
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

/// Of the 8^3 tiles of the domain: how many hold a particle, and how many
/// the solver keeps.
struct Share {
    size_t tiles = 0, water = 0, reach = 0;
};

Share share(const sim::LiquidSolver& s) {
    const sim::Domain& d = s.domain();
    const int t[3] = {(d.cells[0] + 7) / 8, (d.cells[1] + 7) / 8, (d.cells[2] + 7) / 8};
    Share r;
    r.tiles = static_cast<size_t>(t[0]) * static_cast<size_t>(t[1]) * static_cast<size_t>(t[2]);
    std::vector<uint8_t> water(r.tiles, 0);
    const Vec3 lo = d.origin();
    for (const Vec3& p : s.positions()) {
        int c[3];
        for (int a = 0; a < 3; ++a) c[a] = std::clamp(static_cast<int>(std::floor((p[a] - lo[a]) / d.voxel)) / 8, 0, t[a] - 1);
        water[static_cast<size_t>(c[0]) + static_cast<size_t>(t[0]) * (static_cast<size_t>(c[1]) + static_cast<size_t>(t[1]) * static_cast<size_t>(c[2]))] = 1;
    }
    for (const uint8_t v : water) r.water += v;
    r.reach = s.tiles().stored().size();
    return r;
}

bool bench(const std::string& example, int resolution, int frames, const std::vector<std::string>& sets) {
    sim::Network net;
    std::string error;
    if (!sim::Network::load(sim::Network::exampleText(example), net, error)) {
        std::printf("the %s example did not load: %s\n", example.c_str(), error.c_str());
        return false;
    }
    const sim::Node* water = nullptr;
    for (const sim::Node& n : net.nodes()) {
        if (n.type == "liquid_solver") {
            water = &n;
            break;
        }
    }
    if (!water) {
        std::printf("the %s example has no Liquid Solver\n", example.c_str());
        return false;
    }
    net.setParam(water->id, "resolution", std::to_string(resolution));
    for (const std::string& set : sets) {
        const size_t eq = set.find('=');
        if (eq != std::string::npos) net.setParam(water->id, set.substr(0, eq), set.substr(eq + 1));
    }
    const sim::Compiled c = net.compile(PG_SIM_EXAMPLES_DIR);
    if (!c.ok || !c.world.hasWater) {
        std::printf("the %s example did not compile\n", example.c_str());
        return false;
    }
    sim::WorldSolver world(c.world);
    const sim::LiquidSolver& liquid = *world.water();
    const sim::Domain& d = liquid.domain();
    header("Resolution " + std::to_string(resolution) + ": " + std::to_string(d.cells[0]) + " x " +
           std::to_string(d.cells[1]) + " x " + std::to_string(d.cells[2]) + " cells of " +
           std::to_string(d.voxel).substr(0, 5) + " m, " + std::to_string(d.cellCount() / 1000000) + "." +
           std::to_string(d.cellCount() / 100000 % 10) + " M voxels");
    std::printf("  %5s %10s %10s %9s %8s %8s %9s %8s\n", "frame", "water ms", "world ms", "particles", "water",
                "kept", "substeps", "MB");
    double waterMs = 0.0, worldMs = 0.0;
    sim::LiquidSolver::Times before = liquid.times();
    for (int f = 1; f <= frames; ++f) {
        const auto t0 = Clock::now();
        world.step();
        const double ms = since(t0);
        worldMs += ms;
        const double w = liquid.times().total() - before.total();
        before = liquid.times();
        waterMs += w;
        if (f % 10 == 0 || f == frames) {
            const Share s = share(liquid);
            std::printf("  %5d %10.0f %10.0f %9zu %7.1f%% %7.1f%% %9d %8.0f\n", f, w, ms, liquid.particleCount(),
                        100.0 * static_cast<double>(s.water) / static_cast<double>(s.tiles),
                        100.0 * static_cast<double>(s.reach) / static_cast<double>(s.tiles), liquid.lastSubsteps(),
                        peakMb());
        }
    }
    const sim::LiquidSolver::Times& t = liquid.times();
    std::printf("  a frame: water %.0f ms, the whole world %.0f ms\n", waterMs / frames, worldMs / frames);
    std::printf("  the water's time: solids %.0f%%, sort %.0f%%, emit %.0f%%, to grid %.0f%%, extrapolate %.0f%%, "
                "forces %.0f%%, project %.0f%%, to particles %.0f%%, advect %.0f%%\n",
                100.0 * t.solids / t.total(), 100.0 * t.sort / t.total(), 100.0 * t.emit / t.total(),
                100.0 * t.toGrid / t.total(), 100.0 * t.extrapolate / t.total(), 100.0 * t.forces / t.total(),
                100.0 * t.project / t.total(), 100.0 * t.toParticles / t.total(), 100.0 * t.advect / t.total());
    // What the run came to, as one number: a change meant to be only faster
    // must leave it as it is.
    uint64_t print = 1469598103934665603ull;
    auto mix = [&](const std::vector<Vec3>& v) {
        for (const Vec3& p : v) {
            for (int a = 0; a < 3; ++a) {
                uint32_t b;
                std::memcpy(&b, &p[a], sizeof b);
                print = (print ^ b) * 1099511628211ull;
            }
        }
    };
    mix(liquid.positions());
    mix(liquid.velocities());
    std::printf("  fingerprint of the last frame: %016llx\n", static_cast<unsigned long long>(print));
    std::printf("  memory held at most: %.0f MB\n", peakMb());
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<int> resolutions;
    int frames = 120;
    std::string example = "flood_crates";
    std::vector<std::string> sets;
    for (int a = 1; a < argc; ++a) {
        const std::string s = argv[a];
        if (s == "--frames" && a + 1 < argc) frames = std::max(1, std::atoi(argv[++a]));
        else if (s == "--example" && a + 1 < argc) example = argv[++a];
        else if (s == "--set" && a + 1 < argc) sets.push_back(argv[++a]);
        else if (s == "--threads" && a + 1 < argc) TaskPool::instance().setThreadCount(static_cast<unsigned>(std::max(1, std::atoi(argv[++a]))));
        else resolutions.push_back(std::atoi(argv[a]));
    }
    if (resolutions.empty()) resolutions = {96, 192};
    std::printf("water -- benchmarks\n");
    std::printf("hardware threads available: %u\n", TaskPool::instance().threadCount());
    for (const int r : resolutions) {
        if (!bench(example, r, frames, sets)) return 1;
    }
    return 0;
}
