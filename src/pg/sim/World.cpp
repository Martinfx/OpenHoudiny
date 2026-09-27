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
    w.rigid.solver.timeStep = w.timeStep;
    w.rigid = w.rigid.sanitized();
    return w;
}

WorldSolver::WorldSolver(const World& world) : world_(world.sanitized()) {
    if (world_.hasGas) gas_ = std::make_unique<PyroSolver>(world_.gas);
    if (world_.hasWater) water_ = std::make_unique<LiquidSolver>(world_.water);
    if (world_.hasRain) rain_ = std::make_unique<RainSolver>(world_.rain);
    if (world_.hasRigid) rigid_ = std::make_unique<RigidSolver>(world_.rigid);
}

void WorldSolver::step() {
    const bool animated = !world_.animation.empty();
    // The frame this step makes: its sources, forces and solids.
    const World& now = world_.at(frame_ + 1);
    // The pieces first: where they fall to, the water and the gas go round.
    if (rigid_) {
        if (animated) rigid_->setColliders(now.rigid.colliders);
        rigid_->step();
    }
    const RigidScene& rigid = world_.rigid;
    const bool piecesIntoGas = rigid_ && (rigid.intoGas || rigid.dustIntoGas);
    const bool piecesIntoWater = rigid_ && rigid.intoWater;
    const bool piecesIntoRain = rigid_ && rigid.intoRain;
    // The pieces where they are now, as colliders -- once for all.
    std::vector<Collider> pieces;
    if (rigid_ && (rigid.intoGas || piecesIntoWater || piecesIntoRain)) pieces = rigid_->colliders();
    if (gas_ && (animated || piecesIntoGas)) {
        Scene gas = now.gas;
        gas.solver.size = world_.gas.solver.size;
        gas.solver.resolution = world_.gas.solver.resolution;
        gas.solver.timeStep = world_.timeStep;
        if (rigid_ && rigid.intoGas) gas.colliders.insert(gas.colliders.end(), pieces.begin(), pieces.end());
        if (rigid_ && rigid.dustIntoGas) {
            // Where the glue broke: a puff of dust, fading.
            for (const auto& [at, left] : rigid_->dust()) {
                Emitter e;
                e.shape = Shape::Sphere;
                e.center = at;
                e.size = Vec3(1.0f, 1.0f, 1.0f) * rigid.solver.dustSize;
                e.fuel = 0.0f;
                e.smoke = rigid.solver.dust * left * 4.0f;
                e.heat = 0.1f * left;
                e.velocity = Vec3();
                e.flicker = 0.0f;
                e.node = rigid.node;
                gas.emitters.push_back(e);
            }
        }
        gas_->setScene(gas);
    }
    if (water_ && (animated || piecesIntoWater)) {
        LiquidScene water = now.water;
        water.solver.timeStep = world_.timeStep;
        if (piecesIntoWater) water.colliders.insert(water.colliders.end(), pieces.begin(), pieces.end());
        water_->setScene(water);
    }
    if (rain_ && (animated || piecesIntoRain)) {
        RainScene rain = now.rain;
        rain.rain.timeStep = world_.timeStep;
        if (piecesIntoRain) rain.colliders.insert(rain.colliders.end(), pieces.begin(), pieces.end());
        rain_->setScene(rain);
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
    if (rigid_) f.rigid = rigid_->capture();
    f.number = frame_;
    f.time = time_;
    return f;
}

}  // namespace pg::sim
