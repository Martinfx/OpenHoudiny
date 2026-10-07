#pragma once
//
// Smoke and fire: a gas simulated on a 3D grid -- the idea behind Houdini's
// Pyro, small enough to read. It runs a Scene (Scene.h): the domain and the
// solver's settings, the sources, the forces and the solids.
//
// Each step:
//   1. emit      the sources add fuel, smoke and heat, and push the gas;
//   2. advect    the flow carries everything along, itself included
//                (semi-Lagrangian; MacCormack for smoke, heat, fuel and flame,
//                which keeps them sharper);
//   3. combust   fuel burns into heat, soot and flame, and the gas expands;
//   4. forces    heat rises, soot weighs down, vorticity confinement puts
//                back the swirls a coarse grid smooths away, then the scene's
//                forces: turbulence, wind, vortices, attractors, drag;
//   5. project   pressure makes the flow incompressible -- except where the
//                burning gas expands (multigrid: Poisson.h). The sides and the
//                top are open, the floor a wall if the scene says so, and no
//                gas flows into a solid;
//   6. dissipate smoke thins out, heat cools, flames die; where the gas
//                swells, what it carries thins out with it.
//
// Water in the gas (setWater) -- the particles of a Liquid Solver, the drops
// of a Rain on their way down -- puts the fire out, after the gas is
// carried and before it burns: each cell it is in cools by a share of its
// heat, its fuel soaks and its flame goes out, as fast as there is water in
// it; the heat it takes makes steam. A source of fire the water falls on
// soaks too, and gives less and less: a campfire in the rain dies down, and
// stays out.
//
// Steam is a field of its own, as smoke is: carried by the flow, white, it
// rises -- lighter than the air, and warm (steamLift) -- and thins out into
// clear air as it goes (steamFade). Without water there is none, and the
// solver does not carry it.
//
// Heat and flame are separate, as in Houdini: heat lifts the gas and fades
// slowly, the flame -- fuel burning -- shows for a fraction of a second. A
// renderer draws the fire from the flame, coloured by the heat; drawn from the
// heat alone, it would be a glowing column as tall as the plume.
//
// Smoke, heat and fuel sit at the cell centres. The velocity is staggered (a
// MAC grid): each component lives on the faces it flows through -- x on the
// (nx+1) x ny x nz faces between cells along x, and so on. The divergence of a
// cell is then exactly what flows in and out through its six faces, and the
// projection can remove it exactly.
//
// Sparse (SolverSettings::sparse): every field is kept in tiles of 8 x 8 x 8
// cells (SparseGrid.h), and only the tiles the gas is in are worked on --
// and those round them, as far as it can move in a step. Before each step
// a tile whose smoke, heat, fuel and flame are all below the cutoff is let
// go, and the tiles round what is left, round the sources and round moving
// solids are taken on. The rest of the domain is still, empty air, where the
// pressure is 0: a big domain costs what its gas does. With every tile taken
// on, the solver is the dense one, to the bit.
//
// Deterministic (invariant I5): every loop is a pg::parallelFor over tiles or
// rows of cells, each cell written by exactly one chunk and no sums across
// chunks, so the same scene gives the same bits on any number of threads.
//
#include "pg/sim/Grid.h"
#include "pg/sim/Poisson.h"
#include "pg/sim/Scene.h"
#include "pg/sim/SparseGrid.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace pg::sim {

class StateReader;
class StateWriter;

/// The temperature above which the gas boils away the water in it
/// (SolverSettings::evaporate): warm smoke leaves it be, the flames take it.
inline constexpr float kBoil = 0.5f;

class PyroGpu;

class PyroSolver {
public:
    explicit PyroSolver(const Scene& scene = Scene::fire());
    ~PyroSolver();
    PyroSolver(PyroSolver&&) noexcept;
    PyroSolver& operator=(PyroSolver&&) noexcept;

    /// Empty domain, time 0.
    void reset();
    /// Advances by the time step, in the scene's substeps.
    void step();

    /// The scene, sanitized.
    const Scene& scene() const { return scene_; }
    /// Takes effect with the next step. A new domain -- size or resolution --
    /// starts again; the rest carries on with the gas as it is.
    void setScene(const Scene& scene);
    const Domain& domain() const { return domain_; }

    const SparseGrid& density() const { return density_; }  ///< smoke, soot
    /// Steam the water made of the heat it took: white, rising, thinning.
    const SparseGrid& steam() const { return steam_; }
    /// Whether there is any steam: water has quenched the gas.
    bool steamy() const { return steamy_; }
    const SparseGrid& temperature() const { return temperature_; }
    const SparseGrid& fuel() const { return fuel_; }
    /// Fuel burnt within the last flameLife seconds: where the fire is.
    const SparseGrid& flame() const { return flame_; }
    /// Above 0 in the cells the colliders take -- 1 + the collider's index
    /// -- 0 elsewhere.
    const SparseGrid& solid() const { return solid_; }
    /// Velocity component `axis` on its faces, world units per second: value
    /// (i, j, k) of axis 0 is on the face between cells i-1 and i.
    const SparseGrid& velocity(int axis) const { return vel_[axis]; }
    SparseGrid& velocity(int axis) { return vel_[axis]; }
    /// The tiles worked on: every one, without sparse.
    const Tiles& tiles() const { return *cells_; }
    /// Cells in them.
    size_t activeCells() const { return cells_->activeCells(); }
    /// The velocity at a position in cell units, interpolated from the faces.
    void velocityAt(float x, float y, float z, float out[3]) const;
    /// ... at a world point; still air outside the domain.
    Vec3 flowAt(const Vec3& p) const;
    /// The temperature at a world point, between the cells' middles; 0
    /// outside the domain.
    float heatAt(const Vec3& p) const;

    /// Water in the gas for the next step: where it is (World gives it).
    struct Water {
        std::vector<Vec3> particles;     ///< a Liquid Solver's, each `particleVolume` of water
        float particleVolume = 0.0f;     ///< m^3
        /// A Rain's drops: the way each goes through the air this step.
        std::vector<Vec3> dropFrom, dropTo;
        bool empty() const { return particles.empty() && dropFrom.empty(); }
    };
    void setWater(const Water& water);
    /// How soaked each source is (by its index in the scene): it gives
    /// exp(-soaked) of what it would. Empty: none is.
    const std::vector<float>& soaked() const { return soaked_; }
    /// Cells the water is in this step, in the order of their numbers.
    size_t wetCells() const { return wet_.size(); }

    int nx() const { return nx_; }
    int ny() const { return ny_; }
    int nz() const { return nz_; }
    int frame() const { return frame_; }
    float time() const { return time_; }
    /// Edge of a cell, world units.
    float cellSize() const { return domain_.voxel; }
    /// The world position of a point given in cell units.
    Vec3 worldAt(float x, float y, float z) const;

    // The stages of a step -- public for tests.
    void emit(float dt);
    void advect(float dt);
    void combust(float dt);
    /// The water cools the gas, soaks its fuel and its sources (setWater).
    void quench(float dt);
    void addForces(float dt);
    void project();
    void dissipate(float dt);
    /// Mean |divergence of the velocity - expansion| over the cells that are
    /// not solid, in 1/s: what project() removes.
    double meanDivergence() const;

    /// Milliseconds each stage has taken, summed over the steps since the
    /// domain was made -- where the time of a step goes. `solids`: finding
    /// the cells the colliders take, whenever they change; `tiles`: choosing
    /// the tiles to work on (sparse).
    struct Times {
        double solids = 0.0, tiles = 0.0, emit = 0.0, advect = 0.0, combust = 0.0, forces = 0.0, project = 0.0,
               dissipate = 0.0;
        double total() const { return solids + tiles + emit + advect + combust + forces + project + dissipate; }
    };
    const Times& times() const { return times_; }
    /// With the solver's gpu setting: what does the GPU's share, or why
    /// nothing does -- empty before the first step that asked.
    const std::string& gpuNote() const { return gpuNote_; }
    /// The GPU at work; null when the CPU does everything.
    const PyroGpu* gpu() const { return gpu_.get(); }

    /// Sparse: lets go of the tiles the gas has left and takes on those it
    /// may reach in a step of dt -- step() does, before each step.
    void updateTiles(float dt);

    /// All the next step needs of what it has come to (State.h): the fields
    /// to the bit, the tiles, the solids found, the frame and the time. Not
    /// the scene: the solver it is loaded into has that already.
    void saveState(StateWriter& out) const;
    /// Takes on a state saveState() wrote -- of a solver of this grid; false,
    /// with the solver as it was, when it is not one.
    bool loadState(StateReader& in);

private:
    friend class PyroGpu;
    /// Every field onto the tiles `cells` (and their faces).
    void retile(std::shared_ptr<const Tiles> cells);
    /// advect's share on the GPU, if the settings ask for it and there is
    /// one: false, the CPU to do it.
    bool advectOnGpu(float dt);
    /// What advect does after the fields are carried: solids empty, walls.
    void finishAdvect();
    void updateSolids();
    /// Zero velocity on every face the gas cannot flow through: at solids,
    /// and at the floor when it is closed.
    void enforceWalls();
    void advectScalar(SparseGrid& field);
    void advectVelocity(int axis, float cells);
    /// The velocity at face (i, j, k) of component `axis`: its own value, and
    /// the other two averaged from the four faces around it.
    void faceVelocity(int axis, int i, int j, int k, float out[3]) const;
    float divergence(int i, int j, int k) const;
    void addVorticity(float dt);
    void addForce(const Force& force, size_t index, float dt);
    /// How much a force acts on face (i, j, k) of `axis`, by its mask.
    float maskAt(Mask mask, int axis, int i, int j, int k) const;

    Scene scene_;
    Domain domain_;
    int nx_ = 0, ny_ = 0, nz_ = 0;
    std::shared_ptr<const Tiles> cells_, faces_[3];  // the tiles worked on
    SparseGrid vel_[3], velNext_[3];
    SparseGrid density_, temperature_, fuel_, flame_, solid_, steam_;
    bool steamy_ = false;              // there is steam: it is carried, it rises and thins
    bool anySolid_ = false;
    std::vector<size_t> solidCells_;   // where the solid cells are in the fields
    std::vector<size_t> blocked_[3];   // faces next to or inside a solid, per axis
    std::vector<float> blockedVel_[3]; // ... and the velocity a moving solid gives each (0 when none moves)
    // Scratch, shared by the stages, which run one after another: advect's
    // are the swirls' (addVorticity) and the pressure's right-hand side.
    SparseGrid back_[3], forward_[3];  // where each cell's gas came from / goes to, cell units
    SparseGrid predicted_, hi_, corrected_;
    SparseGrid expansion_;             // divergence the burning asks for, 1/s
    SparseGrid pressure_, divergence_;
    PoissonSolver poisson_;
    std::vector<std::array<Grid, 3>> noise_;  // a turbulence force's coarse lattices
    // The water of this step: each wet cell's number and how fast its water
    // puts it out, 1/s, in the order of the numbers.
    std::vector<std::pair<uint64_t, float>> wet_;
    std::vector<float> soaked_;                // each source's
    int frame_ = 0;
    float time_ = 0.0f;
    Times times_;
    std::unique_ptr<PyroGpu> gpu_;
    bool gpuTried_ = false;  // a device was looked for; gpuNote_ says how it went
    std::string gpuNote_;
};

/// Transmittance from each cell towards a light: exp(-optical depth) through
/// `density` along `towardsLight`, at 1/`divisor` of the resolution. What a
/// volume renderer shades smoke with -- its shadows. (The editor's renderer
/// does the same on the GPU; this is the reference, and for tests.)
Grid lightTransmittance(const SparseGrid& density, const float towardsLight[3], float extinctionPerCell, int divisor);

}  // namespace pg::sim
