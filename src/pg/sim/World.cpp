#include "pg/sim/World.h"

#include "pg/sim/State.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

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

namespace {

using Clock = std::chrono::steady_clock;

float msSince(Clock::time_point t0) {
    return std::chrono::duration<float, std::milli>(Clock::now() - t0).count();
}

}  // namespace

void WorldSolver::step() {
    profile_ = Frame::Profile();
    const PyroSolver::Times before = gas_ ? gas_->times() : PyroSolver::Times();
    Clock::time_point t0 = Clock::now();
    prepare();
    profile_.scenes = msSince(t0) - profile_.rigid;
    if (gas_) {
        t0 = Clock::now();
        gas_->step();
        profile_.gas = msSince(t0);
    }
    if (water_) {
        t0 = Clock::now();
        water_->step();
        profile_.water = msSince(t0);
    }
    if (rain_) {
        t0 = Clock::now();
        rain_->step(water_.get());
        profile_.rain = msSince(t0);
    }
    if (gas_) {
        // The stages, this step's: the solids found when the pieces moved
        // (setScene) among them -- their time counted in the gas's, not the
        // scenes'.
        const PyroSolver::Times& now = gas_->times();
        const double stages[8] = {now.solids - before.solids,   now.tiles - before.tiles,
                                  now.emit - before.emit,       now.advect - before.advect,
                                  now.combust - before.combust, now.forces - before.forces,
                                  now.project - before.project, now.dissipate - before.dissipate};
        for (int s = 0; s < 8; ++s) profile_.gasStages[s] = static_cast<float>(stages[s]);
        const float solidsInScenes = std::min(profile_.scenes, static_cast<float>(stages[0]));
        profile_.scenes -= solidsInScenes;
        profile_.gas += solidsInScenes;
    }
    ++frame_;
    time_ += world_.timeStep;
}

void WorldSolver::prepare() {
    const bool animated = !world_.animation.empty();
    // The frame this step makes: its sources, forces and solids.
    const World& now = world_.at(frame_ + 1);
    // The pieces first: where they fall to, the water and the gas go round.
    if (rigid_) {
        if (animated) {
            rigid_->setColliders(now.rigid.colliders);
            rigid_->setGuide(now.rigid.guide, now.rigid.solver.guideStrength);
        }
        const auto t0 = Clock::now();
        rigid_->step();
        profile_.rigid = msSince(t0);
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
            // Where the glue broke and pieces knocked: a puff of dust,
            // fading, carried the way the pieces went -- barely warm: it
            // rolls out along the ground more than it rises, pushed by the
            // air the crushing squeezes out.
            for (const RigidDust& d : rigid_->dust()) {
                if (d.amount <= 0.0f) continue;
                // Along the way it went this frame, a ball every quarter of
                // its size, sharing its dust: one ball a frame leaves a
                // string of beads behind a fast puff, and on a fine grid each
                // shows as a shell of its own.
                const Vec3 travel = d.velocity * world_.timeStep;
                const int balls = std::clamp(static_cast<int>(std::ceil(length(travel) / (0.25f * d.size))), 1, 8);
                const float share = 1.0f / static_cast<float>(balls);
                for (int b = 0; b < balls; ++b) {
                    Emitter e;
                    e.shape = Shape::Sphere;
                    e.center = d.at - travel * (static_cast<float>(b) * share);
                    e.size = Vec3(1.0f, 1.0f, 1.0f) * d.size;
                    e.fuel = 0.0f;
                    e.smoke = 4.0f * d.amount * share;
                    e.heat = 0.02f * d.amount * share;
                    e.velocity = d.velocity;
                    e.expansion = d.expansion * share;
                    // In clumps, not an even ball: the lumps a cloud of dust
                    // billows in.
                    e.flicker = 1.0f;
                    e.flickerSize = 0.35f * d.size;
                    e.node = rigid.node;
                    gas.emitters.push_back(e);
                }
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
}

// The state: "pgstate", a version, the frame, then each part there is --
// the gas, the water, the rain -- as its saveState() writes it. The pieces'
// is not: they are stepped again.
namespace {
constexpr char kStateMagic[8] = {'p', 'g', 's', 't', 'a', 't', 'e', '\0'};
constexpr uint32_t kStateVersion = 1;
}  // namespace

std::string WorldSolver::saveState() const {
    StateWriter out;
    out.pod(kStateMagic);
    out.pod(kStateVersion);
    out.pod(static_cast<int32_t>(frame_));
    out.pod(time_);
    const uint8_t parts = (gas_ ? 1u : 0u) | (water_ ? 2u : 0u) | (rain_ ? 4u : 0u) | (rigid_ ? 8u : 0u);
    out.pod(parts);
    if (gas_) gas_->saveState(out);
    if (water_) water_->saveState(out);
    if (rain_) rain_->saveState(out);
    return out.take();
}

bool WorldSolver::loadState(std::string_view bytes, std::string& error) {
    if (frame_ != 0) {
        error = "a state goes into a solver that has not stepped yet";
        return false;
    }
    StateReader in(bytes);
    char magic[8] = {};
    uint32_t version = 0;
    int32_t frame = 0;
    float time = 0.0f;
    uint8_t parts = 0;
    if (!in.pod(magic) || std::memcmp(magic, kStateMagic, sizeof magic) != 0 || !in.pod(version)) {
        error = "not a simulation state";
        return false;
    }
    if (version != kStateVersion) {
        error = "a simulation state of another version (" + std::to_string(version) + ", this one reads " +
                std::to_string(kStateVersion) + ")";
        return false;
    }
    const uint8_t mine = (gas_ ? 1u : 0u) | (water_ ? 2u : 0u) | (rain_ ? 4u : 0u) | (rigid_ ? 8u : 0u);
    if (!in.pod(frame) || !in.pod(time) || !in.pod(parts) || frame < 0) {
        error = "the simulation state is cut short";
        return false;
    }
    if (parts != mine) {
        error = "the simulation state is of another world: it simulates other parts";
        return false;
    }
    // The pieces -- and the scenes they give the rest -- to the frame, the
    // way step() takes them there; the rest is read.
    for (int f = 0; f < frame; ++f) {
        prepare();
        ++frame_;
        time_ += world_.timeStep;
    }
    const bool read = (!gas_ || gas_->loadState(in)) && (!water_ || water_->loadState(in)) &&
                      (!rain_ || rain_->loadState(in));
    if (!read || !in.done()) {
        error = "the simulation state does not fit this world: another grid, or a file cut short";
        return false;
    }
    frame_ = frame;
    time_ = time;
    return true;
}

World preview(const World& world, float fraction) {
    World w = world;
    fraction = std::isfinite(fraction) ? std::clamp(fraction, 0.05f, 1.0f) : 1.0f;
    if (fraction >= 1.0f) return w;
    auto scaled = [&](int resolution) {
        return std::max(16, static_cast<int>(std::lround(static_cast<float>(resolution) * fraction)));
    };
    w.gas.solver.resolution = scaled(w.gas.solver.resolution);
    w.water.solver.resolution = scaled(w.water.solver.resolution);
    return w;
}

Frame WorldSolver::capture() const {
    Frame f;
    if (gas_) f = sim::capture(*gas_);
    if (water_) f.water = sim::capture(*water_, world_.keepParticles);
    if (rain_) f.rain = sim::capture(*rain_);
    if (rigid_) f.rigid = rigid_->capture();
    f.number = frame_;
    f.time = time_;
    f.profile = profile_;
    return f;
}

}  // namespace pg::sim
