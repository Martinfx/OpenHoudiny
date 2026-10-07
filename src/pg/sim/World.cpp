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
    w.upres = w.upres.sanitized();
    w.water.solver.timeStep = w.timeStep;
    w.water = w.water.sanitized();
    w.rain.rain.timeStep = w.timeStep;
    w.rain = w.rain.sanitized();
    w.rigid.solver.timeStep = w.timeStep;
    w.rigid = w.rigid.sanitized();
    w.cloth.solver.timeStep = w.timeStep;
    w.cloth = w.cloth.sanitized();
    w.grains.solver.timeStep = w.timeStep;
    w.grains = w.grains.sanitized();
    return w;
}

Domain sceneDomain(const World& world) {
    const World safe = world.sanitized();
    Vec3 size;
    bool any = false;
    auto take = [&](const Domain& d) {
        const Vec3 e = d.size();
        size = any ? Vec3(std::max(size.x, e.x), std::max(size.y, e.y), std::max(size.z, e.z)) : e;
        any = true;
    };
    if (safe.hasGas) take(safe.gas.solver.domain());
    if (safe.hasWater) take(safe.water.solver.domain());
    // Rain alone: some ground to fall on, not the sky it falls from -- and
    // the same however the cloud is sized, or the camera would follow it.
    if (!any && safe.hasRain) take(Domain::ofBox(Vec3(3.0f, 1.5f, 3.0f), 64));
    if (!any) return Scene().solver.domain();
    return Domain::ofBox(size, 64);
}

WorldSolver::WorldSolver(const World& world) : world_(world.sanitized()) {
    playback_ = world_.hasGas && world_.vdbGas.any();
    if (world_.hasGas && !playback_) gas_ = std::make_unique<PyroSolver>(world_.gas);
    if (playback_) gasDomain_ = world_.gas.solver.domain();
    if (gas_ && world_.hasUpres) upres_ = std::make_unique<UpresSolver>(world_.upres);
    if (world_.hasWater) water_ = std::make_unique<LiquidSolver>(world_.water);
    if (world_.hasRain) rain_ = std::make_unique<RainSolver>(world_.rain);
    if (world_.hasRigid) rigid_ = std::make_unique<RigidSolver>(world_.rigid);
    if (world_.hasCloth) cloth_ = std::make_unique<ClothSolver>(world_.cloth);
    if (world_.hasGrains) grains_ = std::make_unique<GrainSolver>(world_.grains);
    // The water holds the pieces up and drags them, the gas blows the grit
    // and them about -- when there are any, and their share is not 0.
    if (rigid_) {
        const RigidSettings& r = world_.rigid.solver;
        fluidsPush_ = (water_ && (r.buoyancy > 0.0f || r.waterDrag > 0.0f)) || (gas_ && r.airDrag > 0.0f);
        // The cloth the pieces fall on holds them back.
        clothPushes_ = cloth_ && world_.rigid.intoCloth;
        // So do the grains.
        grainsPush_ = grains_ && world_.rigid.intoGrains;
        coupled_ = fluidsPush_ || clothPushes_ || grainsPush_;
    }
    // The water and the rain put the fire out -- and it boils them away.
    quenches_ = gas_ && (water_ || rain_) && world_.gas.solver.quench > 0.0f;
    evaporates_ = gas_ && (water_ || rain_) && world_.gas.solver.evaporate > 0.0f;
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
    const UpresSolver::Times upresBefore = upres_ ? upres_->times() : UpresSolver::Times();
    const LiquidSolver::Times waterBefore = water_ ? water_->times() : LiquidSolver::Times();
    Clock::time_point t0 = Clock::now();
    prepare();
    profile_.scenes = msSince(t0) - profile_.rigid;
    if (cloth_) {
        t0 = Clock::now();
        cloth_->step();
        profile_.cloth = msSince(t0);
    }
    if (grains_) {
        t0 = Clock::now();
        grains_->step();
        profile_.grains = msSince(t0);
    }
    if (upres_) {
        // Before the gas: with the flow it is about to carry its own with.
        t0 = Clock::now();
        upres_->step(*gas_);
        profile_.upres = msSince(t0);
        const UpresSolver::Times& now = upres_->times();
        const double stages[6] = {now.tiles - upresBefore.tiles, now.solids - upresBefore.solids,
                                  now.emit - upresBefore.emit,   now.swirl - upresBefore.swirl,
                                  now.advect - upresBefore.advect, now.combust - upresBefore.combust};
        for (int s = 0; s < 6; ++s) profile_.upresStages[s] = static_cast<float>(stages[s]);
    }
    if (gas_) {
        t0 = Clock::now();
        gas_->step();
        profile_.gas = msSince(t0);
    }
    if (playback_) {
        // The gas of the frame this step makes, from its file.
        t0 = Clock::now();
        gasFileError_.clear();
        if (!vdbGasFrame(world_.vdbGas, gasDomain_, frame_ + 1, played_, gasFileError_)) {
            // A file that cannot be read: no gas this frame -- and why.
            played_ = Frame();
            played_.domain = gasDomain_;
            played_.gasTiles = {0};
            played_.fields.assign(3 * Tiles::kCells, 0);
        }
        profile_.gas = msSince(t0);
    }
    if (evaporates_) {
        // The fire boils away the water in it -- the particles, the drops --
        // as it is hotter round them than kBoil, before they move on.
        const PyroSolver* gas = gas_.get();
        const float boil = world_.gas.solver.evaporate;
        const auto rate = [gas, boil](const Vec3& p) { return boil * std::max(gas->heatAt(p) - kBoil, 0.0f); };
        const uint32_t seed = static_cast<uint32_t>(frame_) * 0x9E3779B9u + 0x7F4A7C15u;
        if (water_) water_->evaporate(rate, world_.timeStep, seed);
        if (rain_) rain_->evaporate(rate, world_.timeStep, seed ^ 0x85EBCA6Bu);
    }
    if (water_) {
        t0 = Clock::now();
        water_->step();
        profile_.water = msSince(t0);
    }
    if (rain_) {
        t0 = Clock::now();
        rain_->step(water_.get());
        // The drops that fell into the water add to it.
        if (water_ && !rain_->intoWater().empty()) {
            std::vector<Vec3> at, velocity;
            for (const RainParticle& d : rain_->intoWater()) {
                at.push_back(d.position);
                velocity.push_back(d.velocity);
            }
            water_->pour(at, velocity, rain_->dropVolume());
        }
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
        profile_.gpu = gas_->gpuNote();
        const float solidsInScenes = std::min(profile_.scenes, static_cast<float>(stages[0]));
        profile_.scenes -= solidsInScenes;
        profile_.gas += solidsInScenes;
    }
    if (water_) {
        // The same for the water: its solids are found as the pieces move.
        const LiquidSolver::Times& now = water_->times();
        const double stages[9] = {now.solids - waterBefore.solids,         now.sort - waterBefore.sort,
                                  now.emit - waterBefore.emit,             now.toGrid - waterBefore.toGrid,
                                  now.extrapolate - waterBefore.extrapolate, now.forces - waterBefore.forces,
                                  now.project - waterBefore.project,       now.toParticles - waterBefore.toParticles,
                                  now.advect - waterBefore.advect};
        for (int s = 0; s < 9; ++s) profile_.waterStages[s] = static_cast<float>(stages[s]);
        const float solidsInScenes = std::min(profile_.scenes, static_cast<float>(stages[0]));
        profile_.scenes -= solidsInScenes;
        profile_.water += solidsInScenes;
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
        if (coupled_) {
            // The flows of this step: those it took before, when stepped
            // again from a checkpoint; else felt now, and kept.
            const size_t at = static_cast<size_t>(frame_);
            if (at < flows_.size()) {
                rigid_->setFlow(flows_[at]);
            } else {
                RigidFlow flow = fluidsPush_ ? rigid_->feel(fluids()) : RigidFlow();
                auto pushed = [&](uint32_t piece, const Vec3& shift, const Vec3& velocity, const Vec3& spin) {
                    RigidFlow::Push push;
                    push.piece = piece;
                    push.shift = shift;
                    push.velocity = velocity;
                    push.spin = spin;
                    flow.pushes.push_back(push);
                };
                if (clothPushes_) {
                    for (const ClothSolver::Reaction& r : cloth_->reactions()) pushed(r.piece, r.shift, r.velocity, r.spin);
                }
                if (grainsPush_) {
                    for (const GrainSolver::Reaction& r : grains_->reactions()) pushed(r.piece, r.shift, r.velocity, r.spin);
                }
                flows_.push_back(std::move(flow));
                rigid_->setFlow(flows_.back());
            }
        }
        rigid_->step();
        profile_.rigid = msSince(t0);
        // The grit that came out of the pieces is the grains' now: they take
        // it into this step. (Stepped again from a checkpoint, the grains
        // read afterwards are what they had.)
        if (grains_ && world_.rigid.gritIntoGrains && !rigid_->thrown().empty()) {
            const std::vector<RigidBit>& bits = rigid_->thrown();
            std::vector<Vec3> at(bits.size()), velocity(bits.size());
            std::vector<float> radius(bits.size());
            for (size_t i = 0; i < bits.size(); ++i) {
                at[i] = bits[i].at;
                velocity[i] = bits[i].velocity;
                radius[i] = 0.5f * bits[i].size;
            }
            grains_->add(at, velocity, radius, &world_.rigid.gritColor);
        }
    }
    const RigidScene& rigid = world_.rigid;
    const bool piecesIntoGas = rigid_ && (rigid.intoGas || rigid.dustIntoGas);
    const bool piecesIntoWater = rigid_ && rigid.intoWater;
    const bool piecesIntoRain = rigid_ && rigid.intoRain;
    const bool piecesIntoCloth = rigid_ && rigid.intoCloth;
    const bool piecesIntoGrains = rigid_ && rigid.intoGrains;
    // The pieces where they are now, as colliders -- once for all.
    std::vector<Collider> pieces;
    if (rigid_ && (rigid.intoGas || piecesIntoWater || piecesIntoRain || piecesIntoCloth || piecesIntoGrains)) {
        pieces = rigid_->colliders();
    }
    // The cloth: its objects, the pieces where they are, where its pins go;
    // the gas as it was at the end of the last step blows it.
    if (cloth_) {
        ClothScene cloth = now.cloth;
        cloth.solver.timeStep = world_.timeStep;
        if (piecesIntoCloth) cloth.colliders.insert(cloth.colliders.end(), pieces.begin(), pieces.end());
        cloth_->setScene(cloth);
        if (gas_) {
            const PyroSolver* gas = gas_.get();
            cloth_->setAir([gas](const Vec3& p) { return gas->flowAt(p); });
        }
    }
    // The grains: their objects, the pieces where they are; the gas as it
    // was at the end of the last step blows them.
    if (grains_) {
        GrainScene grains = now.grains;
        grains.solver.timeStep = world_.timeStep;
        if (piecesIntoGrains) grains.colliders.insert(grains.colliders.end(), pieces.begin(), pieces.end());
        grains_->setScene(grains);
        if (gas_) {
            const PyroSolver* gas = gas_.get();
            grains_->setAir([gas](const Vec3& p) { return gas->flowAt(p); });
        }
    }
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
    if (quenches_) {
        // The water in the gas this step: the water's particles and the
        // drops' way down, as they were at the end of the last -- in the
        // gas's box.
        PyroSolver::Water wet;
        const Domain& d = gas_->domain();
        const Vec3 lo = d.origin(), hi = d.origin() + d.size();
        auto inside = [&](const Vec3& p) {
            return p.x >= lo.x && p.y >= lo.y && p.z >= lo.z && p.x < hi.x && p.y < hi.y && p.z < hi.z;
        };
        if (water_) {
            const float h = water_->cellSize();
            wet.particleVolume = 0.125f * h * h * h;
            for (const Vec3& p : water_->positions()) {
                if (inside(p)) wet.particles.push_back(p);
            }
        }
        if (rain_) {
            for (const RainParticle& r : rain_->drops()) {
                const Vec3 to = r.position + r.velocity * world_.timeStep;
                if (!inside(r.position) && !inside(to)) continue;
                wet.dropFrom.push_back(r.position);
                wet.dropTo.push_back(to);
            }
        }
        gas_->setWater(wet);
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

RigidFluids WorldSolver::fluids() const {
    RigidFluids f;
    if (water_) {
        auto level = std::make_shared<const WaterLevel>(water_->waterLevel());
        const LiquidSolver* water = water_.get();
        f.waterLevel = [level](float x, float z) { return level->at(x, z); };
        f.waterVelocity = [water](const Vec3& p) { return water->velocityAt(p); };
        f.waterCell = water->cellSize();
    }
    if (gas_) {
        const PyroSolver* gas = gas_.get();
        f.airVelocity = [gas](const Vec3& p) { return gas->flowAt(p); };
        f.airCell = gas->cellSize();
    }
    return f;
}

// The state: "pgstate", a version, the frame; what the water, the gas and
// the cloth did to the pieces in each step so far (version 2); then each
// part there is -- the gas, the water (its grids on their tiles, version 4;
// the rain poured into it that is not a particle yet, version 5),
// the rain, the cloth, torn or not (version 3), the gas's upres, the grains
// -- as its saveState() writes it; the gas with how soaked its sources are
// (version 5) and its steam (version 6); the cloth with the shape it holds
// as it has given way (version 7). The pieces' is not: they are stepped
// again, with those flows.
namespace {
constexpr char kStateMagic[8] = {'p', 'g', 's', 't', 'a', 't', 'e', '\0'};
constexpr uint32_t kStateVersion = 7;
}  // namespace

std::string WorldSolver::saveState() const {
    StateWriter out;
    out.pod(kStateMagic);
    out.pod(kStateVersion);
    out.pod(static_cast<int32_t>(frame_));
    out.pod(time_);
    const uint8_t parts = (gas_ ? 1u : 0u) | (water_ ? 2u : 0u) | (rain_ ? 4u : 0u) | (rigid_ ? 8u : 0u) |
                          (cloth_ ? 16u : 0u) | (upres_ ? 32u : 0u) | (grains_ ? 64u : 0u);
    out.pod(parts);
    out.pod(static_cast<uint64_t>(flows_.size()));
    for (const RigidFlow& f : flows_) {
        out.list(f.pushes);
        out.list(f.gritFlow);
        out.list(f.gritWet);
    }
    if (gas_) gas_->saveState(out);
    if (water_) water_->saveState(out);
    if (rain_) rain_->saveState(out);
    if (cloth_) cloth_->saveState(out);
    if (upres_) upres_->saveState(out);
    if (grains_) grains_->saveState(out);
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
    const uint8_t mine = (gas_ ? 1u : 0u) | (water_ ? 2u : 0u) | (rain_ ? 4u : 0u) | (rigid_ ? 8u : 0u) |
                         (cloth_ ? 16u : 0u) | (upres_ ? 32u : 0u) | (grains_ ? 64u : 0u);
    if (!in.pod(frame) || !in.pod(time) || !in.pod(parts) || frame < 0) {
        error = "the simulation state is cut short";
        return false;
    }
    if (parts != mine) {
        error = "the simulation state is of another world: it simulates other parts";
        return false;
    }
    // What the water and the gas did to the pieces: a step's worth each, if
    // they push them in this world; none else.
    uint64_t steps = 0;
    if (!in.pod(steps) || steps > static_cast<uint64_t>(frame)) {
        error = "the simulation state is cut short";
        return false;
    }
    std::vector<RigidFlow> flows(static_cast<size_t>(steps));
    for (RigidFlow& f : flows) {
        if (!in.list(f.pushes) || !in.list(f.gritFlow) || !in.list(f.gritWet)) {
            error = "the simulation state is cut short";
            return false;
        }
    }
    if (steps != (coupled_ ? static_cast<uint64_t>(frame) : 0u)) {
        error = "the simulation state is of another world: the water, the gas, the cloth and the grains push its pieces "
                "otherwise";
        return false;
    }
    flows_ = std::move(flows);
    // The pieces -- and the scenes they give the rest -- to the frame, the
    // way step() takes them there, with the flows they took; the rest is read.
    for (int f = 0; f < frame; ++f) {
        prepare();
        ++frame_;
        time_ += world_.timeStep;
    }
    const bool read = (!gas_ || gas_->loadState(in)) && (!water_ || water_->loadState(in)) &&
                      (!rain_ || rain_->loadState(in)) && (!cloth_ || cloth_->loadState(in)) &&
                      (!upres_ || upres_->loadState(in, *gas_)) && (!grains_ || grains_->loadState(in));
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
    if (upres_) {
        f = sim::capture(*upres_);
        // The steam and the velocity: the solver's, the upres carries none
        // of its own -- the velocity on the upres's tiles, the steam's
        // taken in.
        addCoarseSteam(f, *gas_, world_.upres.scale);
        addVelocity(f, *gas_);
    } else if (gas_) {
        f = sim::capture(*gas_);
        addVelocity(f, *gas_);
    } else if (playback_) {
        f.domain = played_.domain;
        f.fields = played_.fields;
        f.gasTiles = played_.gasTiles;
        f.steam = played_.steam;
        f.velocity = played_.velocity;
    }
    if (water_) f.water = sim::capture(*water_, world_.keepParticles);
    if (rain_) f.rain = sim::capture(*rain_);
    if (rigid_) f.rigid = rigid_->capture();
    if (cloth_) f.cloth = cloth_->capture();
    if (grains_) f.grains = grains_->capture();
    f.number = frame_;
    f.time = time_;
    f.profile = profile_;
    return f;
}

}  // namespace pg::sim
