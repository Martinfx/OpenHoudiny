#include "pg/sim/World.h"

#include <algorithm>
#include <cmath>

namespace pg::sim {

bool Animation::operator==(const Animation& other) const {
    if (frames == other.frames) return true;
    if (empty() || other.empty()) return empty() && other.empty();
    return *frames == *other.frames;
}

const World& World::at(int frame) const {
    if (animation.empty()) return *this;
    const std::vector<World>& f = *animation.frames;
    return f[static_cast<size_t>(std::clamp(frame, 1, static_cast<int>(f.size())) - 1)];
}

World World::sanitized() const {
    World w = *this;
    if (!std::isfinite(w.timeStep)) w.timeStep = World().timeStep;
    w.timeStep = std::clamp(w.timeStep, 1e-4f, 1.0f);
    w.gas.solver.timeStep = w.timeStep;
    w.gas = w.gas.sanitized();
    w.water.solver.timeStep = w.timeStep;
    w.water = w.water.sanitized();
    w.rain.rain.timeStep = w.timeStep;
    w.rain = w.rain.sanitized();
    return w;
}

WorldSolver::WorldSolver(const World& world) : world_(world.sanitized()) {
    if (world_.hasGas) gas_ = std::make_unique<PyroSolver>(world_.gas);
    if (world_.hasWater) water_ = std::make_unique<LiquidSolver>(world_.water);
    if (world_.hasRain) rain_ = std::make_unique<RainSolver>(world_.rain);
}

void WorldSolver::step() {
    if (!world_.animation.empty()) {
        // The frame this step makes: its sources, forces and solids.
        const World& now = world_.at(frame_ + 1);
        if (gas_) {
            Scene gas = now.gas;
            gas.solver.size = world_.gas.solver.size;
            gas.solver.resolution = world_.gas.solver.resolution;
            gas.solver.timeStep = world_.timeStep;
            gas_->setScene(gas);
        }
        if (water_) {
            LiquidScene water = now.water;
            water.solver.timeStep = world_.timeStep;
            water_->setScene(water);
        }
        if (rain_) {
            RainScene rain = now.rain;
            rain.rain.timeStep = world_.timeStep;
            rain_->setScene(rain);
        }
    }
    if (gas_) gas_->step();
    if (water_) water_->step();
    if (rain_) rain_->step(water_.get());
    ++frame_;
    time_ += world_.timeStep;
}

Frame WorldSolver::capture() const {
    Frame f;
    if (gas_) f = sim::capture(*gas_);
    if (water_) f.water = sim::capture(*water_, world_.keepParticles);
    if (rain_) f.rain = sim::capture(*rain_);
    f.number = frame_;
    f.time = time_;
    return f;
}

}  // namespace pg::sim
