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
#include <vector>

namespace pg::sim {

class StateReader;
class StateWriter;

class PyroSolver {
public:
    explicit PyroSolver(const Scene& scene = Scene::fire());

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
    /// Every field onto the tiles `cells` (and their faces).
    void retile(std::shared_ptr<const Tiles> cells);
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
    SparseGrid density_, temperature_, fuel_, flame_, solid_;
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
    int frame_ = 0;
    float time_ = 0.0f;
    Times times_;
};

/// Transmittance from each cell towards a light: exp(-optical depth) through
/// `density` along `towardsLight`, at 1/`divisor` of the resolution. What a
/// volume renderer shades smoke with -- its shadows. (The editor's renderer
/// does the same on the GPU; this is the reference, and for tests.)
Grid lightTransmittance(const SparseGrid& density, const float towardsLight[3], float extinctionPerCell, int divisor);

}  // namespace pg::sim
