#include "pg/sim/World.h"

#include <algorithm>
#include <cmath>

namespace pg::sim {

World World::sanitized() const {
    World w = *this;
    if (!std::isfinite(w.timeStep)) w.timeStep = World().timeStep;
    w.timeStep = std::clamp(w.timeStep, 1e-4f, 1.0f);
    w.gas.solver.timeStep = w.timeStep;
    w.gas = w.gas.sanitized();
    return w;
}

WorldSolver::WorldSolver(const World& world) : world_(world.sanitized()) {
    if (world_.hasGas) gas_ = std::make_unique<PyroSolver>(world_.gas);
}

void WorldSolver::step() {
    if (gas_) gas_->step();
    ++frame_;
    time_ += world_.timeStep;
}

Frame WorldSolver::capture() const {
    Frame f;
    if (gas_) f = sim::capture(*gas_);
    f.number = frame_;
    f.time = time_;
    return f;
}

}  // namespace pg::sim
