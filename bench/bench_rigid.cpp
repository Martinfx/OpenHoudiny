//
// The rigid bodies against the ROADMAP's step 4: the tower of the demolition
// example -- and the same tower cut in ten times the pieces -- fractured,
// made into bodies, and stepped through its collapse, on one thread and on
// all there are. The dust is left out: what is measured is the RBD Solver
// -- and where its step's time goes (RigidSolver::times). That the frames
// come out the same on any number of threads is checked as it goes.
//
#include "pg/core/Parallel.h"
#include "pg/sim/Network.h"
#include "pg/sim/Rigid.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

using namespace pg;
using Clock = std::chrono::steady_clock;

namespace {

double since(Clock::time_point t0) { return std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); }

void header(const std::string& title) {
    std::printf("\n%s\n%s\n", title.c_str(), std::string(title.size(), '-').c_str());
}

/// The demolition example's tower, its seeds `size` metres apart (1.6 as
/// the example has it), without the dust. Its scene, and how long the
/// network took to compile -- to fracture it, above all.
bool tower(float size, sim::RigidScene& scene, double& compileMs) {
    sim::Network net;
    std::string error;
    if (!sim::Network::load(sim::Network::exampleText("demolition"), net, error)) {
        std::printf("the demolition example did not load: %s\n", error.c_str());
        return false;
    }
    const sim::Node* seeds = net.named("seeds");
    if (!seeds) return false;
    const int id = seeds->id;
    net.setParam(id, "size", std::to_string(size));
    net.setParam(id, "low_size", std::to_string(size * 0.5625f));
    for (const char* name : {"dust", "dust_look"}) {
        if (const sim::Node* n = net.named(name)) net.remove(n->id);
    }
    const auto t0 = Clock::now();
    const sim::Compiled c = net.compile(PG_SIM_EXAMPLES_DIR);
    compileMs = since(t0);
    if (!c.ok || !c.world.hasRigid) {
        std::printf("the tower did not compile\n");
        return false;
    }
    scene = c.world.rigid;
    return true;
}

struct Run {
    double makeMs = 0.0, meanMs = 0.0, worstMs = 0.0;
    sim::RigidSolver::Times spent;  ///< summed over the steps
    std::vector<sim::RigidFrame> frames;
};

Run run(const sim::RigidScene& scene, unsigned threads, int frames) {
    TaskPool::instance().setThreadCount(threads);
    Run r;
    auto t0 = Clock::now();
    sim::RigidSolver solver(scene);
    r.makeMs = since(t0);
    double sum = 0.0;
    for (int f = 0; f < frames; ++f) {
        t0 = Clock::now();
        solver.step();
        const double ms = since(t0);
        sum += ms;
        r.worstMs = std::max(r.worstMs, ms);
        const sim::RigidSolver::Times& t = solver.times();
        r.spent.jolt += t.jolt;
        r.spent.glue += t.glue;
        r.spent.grit += t.grit;
        r.spent.rest += t.rest;
        r.frames.push_back(solver.capture());
    }
    r.meanMs = sum / std::max(frames, 1);
    return r;
}

bool same(const std::vector<sim::RigidFrame>& a, const std::vector<sim::RigidFrame>& b) {
    if (a.size() != b.size()) return false;
    for (size_t k = 0; k < a.size(); ++k) {
        if (a[k].poses != b[k].poses || a[k].broken != b[k].broken || a[k].debris != b[k].debris) return false;
    }
    return true;
}

void bench(int number, float size, int frames) {
    sim::RigidScene scene;
    double compileMs = 0.0;
    const unsigned all = TaskPool::instance().threadCount();
    if (!tower(size, scene, compileMs)) return;
    const Run one = run(scene, 1, frames);
    const Run many = run(scene, all, frames);
    TaskPool::instance().setThreadCount(all);
    const sim::RigidFrame& last = one.frames.back();
    int pieces = 0;
    sim::pieceOfPrimitives(*scene.pieces, scene.attribute, pieces);
    header(std::to_string(number) + ". The tower, seeds " + std::to_string(size).substr(0, 4) + " m apart: " +
           std::to_string(pieces) + " pieces, " + std::to_string(last.poses.size()) + " bodies");
    std::printf("  network compiled (tower fractured)  %8.0f ms\n", compileMs);
    std::printf("  solver made (bodies, glue, hulls)   %8.0f ms on 1 thread, %.0f ms on %u\n", one.makeMs, many.makeMs, all);
    std::printf("  %d frames stepped, 1 thread         %8.1f ms a frame (worst %.0f ms)\n", frames, one.meanMs, one.worstMs);
    std::printf("  %d frames stepped, %u threads        %8.1f ms a frame (worst %.0f ms)\n", frames, all, many.meanMs,
                many.worstMs);
    const double spent = std::max(one.spent.total(), 1e-9);
    std::printf("  where it goes, 1 thread: Jolt %.0f%%, the glue %.0f%%, the grit %.0f%%, the rest %.0f%%\n",
                100.0 * one.spent.jolt / spent, 100.0 * one.spent.glue / spent, 100.0 * one.spent.grit / spent,
                100.0 * one.spent.rest / spent);
    std::printf("  joints broken %zu, grit %zu, the same frames on 1 and %u threads: %s\n", last.broken,
                last.debris.size() / 4, all, same(one.frames, many.frames) ? "yes" : "NO");
}

}  // namespace

int main() {
    std::printf("rigid bodies -- benchmarks\n");
    std::printf("hardware threads available: %u\n", TaskPool::instance().threadCount());
    if (!sim::rigidAvailable()) {
        std::printf("built without rigid bodies (PG_WITH_JOLT=OFF)\n");
        return 0;
    }
    bench(1, 1.6f, 180);
    bench(2, 0.75f, 180);
    return 0;
}
