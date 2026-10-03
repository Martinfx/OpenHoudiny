#pragma once
//
// Water: a liquid carried by particles, made incompressible on a grid -- FLIP
// (Zhu and Bridson 2005), what Houdini's FLIP solver is built on, small
// enough to read. It runs a LiquidScene: the domain and the solver's
// settings, the sources of water, the forces and the solids.
//
// The water is particles, eight to a cell where it is full; they carry its
// velocity. Each substep:
//
//   1. emit       a source fills its shape with water once, or keeps pouring
//                 it out at its velocity;
//   2. to grid    the particles' velocities are averaged onto the faces of a
//                 MAC grid (as the gas's, Pyro.h), and each cell gets the
//                 signed distance to the water's surface from the particles
//                 round it (Zhu and Bridson's averaged spheres): the cells
//                 inside are the water;
//   3. forces     gravity, then the scene's forces -- a wind carries the
//                 spray, and barely moves the body of the water: air, a
//                 thousandth as heavy, pushes on its surface alone;
//   4. project    the pressure that keeps the water's volume, 0 at its
//                 surface, nothing flowing into solids (FreeSurface.h):
//                 water falls, splashes, piles up and flows round objects;
//   5. to particles  each particle takes what the grid's velocity changed by
//                 (FLIP), blended with the grid's velocity itself (PIC) by
//                 `flip`: FLIP keeps splashes lively, PIC smooths them and
//                 slows everything down;
//   6. move       through the grid's velocity (second-order Runge-Kutta), out
//                 of any solid a particle ends up in, and away when it leaves
//                 the domain through an open side.
//
// Solids cut the cells they pass through: each face is as open as a solid
// leaves it (from the solids' distance at the grid's corners), so water runs
// down a tilted board rather than a staircase. The floor is always there;
// the sides are walls when the domain is a tank.
//
// A substep moves no particle more than two cells; a frame takes as many as
// that needs, at least `substeps`.
//
// Sparse (LiquidSettings::sparse): the grids are kept in tiles of 8 x 8 x 8
// cells (SparseGrid.h), only those with water in them, of a source about to
// pour, and the tiles round those: the reach of everything a substep does --
// the particles' kernels, the velocity carried out past the water, the
// pressure's neighbours -- is a few cells, well inside a tile. The air above
// a lake and the empty half of a flood's domain take neither memory nor time.
// What happens there never reaches the particles: the water is the dense
// one, to the bit. The solids are kept in tiles of their own, round each
// collider, whatever the water does.
//
// Deterministic (invariant I5): the particles are sorted into their cells (a
// stable counting sort, in the order of the cells of the whole grid -- x
// fastest, then y, then z -- whatever the tiles) and each face and cell
// gathers from the particles round it in that order; a new particle's place
// is a hash of its cell and the substep. The same scene gives the same bits
// on any number of threads.
//
#include "pg/sim/FreeSurface.h"
#include "pg/sim/Grid.h"
#include "pg/sim/Scene.h"
#include "pg/sim/SparseGrid.h"

#include <array>
#include <cstdint>
#include <vector>

namespace pg::sim {

class StateReader;
class StateWriter;

/// How a water source gives water.
enum class WaterMode : uint8_t {
    Fill,  ///< its shape is filled with water once, when it starts
    Flow,  ///< water keeps coming out of it at its velocity, while it is on
};

/// A place water comes from: a shape (Shape.h) placed in the world.
struct WaterSource {
    Shape shape = Shape::Box;
    Vec3 center{0.0f, 0.3f, 0.0f};
    Vec3 rotation;                  ///< degrees about x, then y, then z
    Vec3 size{0.4f, 0.6f, 0.4f};    ///< along its own axes
    WaterMode mode = WaterMode::Fill;
    /// The water's velocity as it leaves, along the source's own axes.
    Vec3 velocity;
    float start = 0.0f;             ///< seconds
    float end = 0.0f;               ///< seconds; at or before start: a flow never stops
    /// How fast it is carried, when it is animated: a flow's water leaves
    /// with that velocity too.
    Vec3 moving;
    uint32_t seed = 1;
    std::shared_ptr<const MeshShape> mesh;  ///< Shape::Mesh: what it is
    int node = 0;                   ///< the network node it came from, 0 if none

    bool activeAt(float t) const { return t >= start && (end <= start || t < end); }
    ShapeInstance instance() const { return {shape, center, rotation, size, mesh}; }
    bool operator==(const WaterSource&) const = default;
};

struct LiquidSettings {
    Vec3 size{2.0f, 1.0f, 1.2f};   ///< the domain, world units: it stands on the floor, centred
    /// Cells along the longest side, 16 to 256; the counts are multiples of 8.
    int resolution = 64;
    /// Walls round the four sides: a tank. Without them the water runs off
    /// the sides and is gone.
    bool closedSides = true;
    /// Only the tiles round the water kept and worked on; off: every tile of
    /// the domain -- the same water, to the bit, in more memory and time.
    bool sparse = true;

    float timeStep = 1.0f / 30.0f;
    int substeps = 1;              ///< at least this many a frame
    /// 1: all FLIP -- lively, splashy, a little noisy; 0: all PIC -- smooth,
    /// thick, slowing down.
    float flip = 0.95f;
    float gravity = 9.81f;         ///< m/s^2, downwards
    uint32_t seed = 1;

    Domain domain() const { return Domain::ofBox(size, resolution); }
    bool operator==(const LiquidSettings&) const = default;
};

struct LiquidScene {
    LiquidSettings solver;
    std::vector<WaterSource> sources;
    std::vector<Force> forces;
    std::vector<Collider> colliders;

    /// Every number in a range the solver can work with (as Scene::sanitized).
    LiquidScene sanitized() const;

    /// A block of water in the corner of a tank, let go: for tests and code
    /// without a network.
    static LiquidScene damBreak();

    bool operator==(const LiquidScene&) const = default;
};

/// The height of the water's surface over the floor plan of a liquid's
/// grid: a value a column of cells, at its middle -- world y of the top of
/// the body of water in it (spray above it aside). Where a solid lies on the
/// water -- a floating piece -- the level is that round it: what the piece
/// floats in, not where it pushed the water down to.
struct WaterLevel {
    static constexpr float kNone = -1e30f;  ///< no water in the column
    Vec3 origin;             ///< the corner at the least x and z (y unused)
    float cell = 0.0f;       ///< edge of a column, world units
    int nx = 0, nz = 0;
    std::vector<float> height;  ///< x fastest; kNone where there is no water

    bool empty() const { return height.empty(); }
    /// The level at a world point's x and z: interpolated between the
    /// columns that have water; kNone where none about it has.
    float at(float x, float z) const;
};

class LiquidSolver {
public:
    explicit LiquidSolver(const LiquidScene& scene = LiquidScene::damBreak());

    /// Advances by the time step, in as many substeps as it takes.
    void step();
    /// Takes on another scene as it runs -- sources, forces, solids and the
    /// settings that are not the grid: the water stays. The domain stays
    /// as it was (its size, resolution and sides, sparse or not).
    void setScene(const LiquidScene& scene);

    /// The scene, sanitized.
    const LiquidScene& scene() const { return scene_; }
    const Domain& domain() const { return domain_; }
    int frame() const { return frame_; }
    float time() const { return time_; }
    /// Substeps the last step took, and CG iterations its pressure solves took in all.
    int lastSubsteps() const { return lastSubsteps_; }
    int lastIterations() const { return lastIterations_; }

    /// Milliseconds each stage has taken, summed over the steps since the
    /// solver was made -- where the time of a step goes. `solids`: the
    /// colliders' distance, the open faces and their velocity, whenever they
    /// move; `sort`: the particles into their cells, and the tiles round
    /// them; `toGrid`: their velocities onto the faces and the surface into
    /// the cells; `extrapolate`: the velocity carried out past the water,
    /// twice a substep.
    struct Times {
        double solids = 0.0, sort = 0.0, emit = 0.0, toGrid = 0.0, extrapolate = 0.0, forces = 0.0, project = 0.0,
               toParticles = 0.0, advect = 0.0;
        double total() const {
            return solids + sort + emit + toGrid + extrapolate + forces + project + toParticles + advect;
        }
    };
    const Times& times() const { return times_; }

    /// All the next step needs of what it has come to (State.h); not the
    /// scene, which the solver it is loaded into has already.
    void saveState(StateWriter& out) const;
    /// Takes on a state saveState() wrote -- of a solver of this grid; false,
    /// with the solver as it was, when it is not one.
    bool loadState(StateReader& in);

    size_t particleCount() const { return position_.size(); }
    /// World units, and world units per second.
    const std::vector<Vec3>& positions() const { return position_; }
    const std::vector<Vec3>& velocities() const { return velocity_; }
    /// How white each particle is, 0 to 1: spray, and water thrown about fast.
    const std::vector<float>& foam() const { return foam_; }
    /// Each particle's own number, from the first made: the same as long as
    /// it is there, whatever order the particles are kept in.
    const std::vector<uint32_t>& ids() const { return id_; }
    /// Litres of water: the particles' share of the cells they fill.
    double volume() const;

    /// The tiles of cells the grids keep: those round the water, or all.
    const Tiles& tiles() const { return *tiles_; }
    /// The signed distance to the water's surface at each cell centre, world
    /// units, below 0 in the water -- from the particles as they were at the
    /// start of the last substep. Far from the water: as far as the particles
    /// reach (the background).
    const SparseGrid& surface() const { return phi_; }
    /// surface() at a world point, interpolated; outside the domain, no water.
    float distanceToSurface(const Vec3& p) const;
    /// Velocity component `axis` on its faces, world units per second, as
    /// PyroSolver::velocity(); 0 on the faces of tiles not kept.
    const SparseGrid& velocity(int axis) const { return vel_[axis]; }
    const SparseGrid& pressure() const { return pressure_; }
    /// How open face (i, j, k) of `axis` is, 0 to 1: what the solids and the
    /// walls leave of it.
    float open(int axis, int i, int j, int k) const;
    /// The distance to the nearest solid -- the colliders, not the floor or
    /// the walls -- at a world point, world units, below 0 inside one.
    float solidDistance(const Vec3& p) const;
    /// The velocity of the collider nearest a world point; 0 when none moves.
    Vec3 solidVelocity(const Vec3& p) const;

    /// The water's surface on a grid `factor` times finer than the solver's,
    /// for drawing: the signed distance at its cell centres, world units,
    /// clamped to [-band, band]; and the foam there, 0 to 1.
    void surfaceField(int factor, float band, Grid& distance, Grid& foam) const;

    float cellSize() const { return domain_.voxel; }
    /// The world position of a point given in cell units.
    Vec3 worldAt(float x, float y, float z) const;
    /// The grid's velocity at a world point, interpolated from the faces.
    Vec3 velocityAt(const Vec3& p) const;
    /// The height of the water's surface, column by column (WaterLevel),
    /// as the particles were at the start of the last substep.
    WaterLevel waterLevel() const;
    float maxSpeed() const;

private:
    void updateSolids();
    void substep(float dt);
    /// Sorts the particles into their cells, and keeps the tiles round them
    /// and round the sources about to pour: the grids follow (retile()).
    void sortParticles();
    /// The tiles of cells the grids are kept in from now on: what the next
    /// step reads goes on where the tiles do, the rest is made again.
    void retile(std::shared_ptr<const Tiles> tiles);
    /// The cell (i, j, k) holds the centre of a solid.
    bool solidCellAt(int i, int j, int k) const;
    /// The velocity of the solid on face (i, j, k) of `axis`: 0 but where a
    /// moving solid covers it.
    float solidVelocityAt(int axis, int i, int j, int k) const;
    /// The cells of a shape's box, inside the grid: [c0, c1) along each axis.
    void boxCells(const ShapeInstance& shape, int c0[3], int c1[3]) const;
    void emit();
    void toGrid();
    void addForces(float dt);
    /// Particles in the 3 x 3 x 3 cells round each kept cell, as
    /// SparseGrid::index() has them.
    std::vector<float> crowdOfCells() const;
    void project(float dt);
    /// Carries the velocity of the valid faces out to their neighbours, a
    /// layer of faces at a time.
    void extrapolate(int layers);
    void toParticles(float dt);
    void advect(float dt);
    /// A world point in cell units, as the grid sees it.
    Vec3 toCells(const Vec3& p) const;
    Vec3 sampleVelocity(const SparseGrid* vel, const Vec3& cells) const;
    /// The particles' spheres averaged onto the centres of a grid `factor`
    /// (1 or 2) times finer than the solver's (Zhu and Bridson): at each, the
    /// sum of the kernel's weights, the weighted sum of the particles'
    /// positions (cell units of the solver), of their radii -- with `radius`,
    /// spray drawn wider; without, all alike -- and of their foam. On the
    /// tiles of that grid under the solver's: `tiles` itself, or each made
    /// eight (finer()).
    void splat(int factor, const std::shared_ptr<const Tiles>& tiles, SparseGrid& weight, SparseGrid* centre,
               SparseGrid* radius, SparseGrid& foam) const;
    /// The distance, cells, from `x` to the averaged sphere, from the sums at
    /// a point; outside the reach of every particle, the reach.
    float sphereDistance(const Vec3& x, float weight, const Vec3& centre, float radius) const;

    LiquidScene scene_;
    Domain domain_;
    int n_[3] = {0, 0, 0};
    std::vector<Vec3> position_, velocity_;
    std::vector<float> foam_;
    std::vector<uint32_t> id_;
    uint32_t made_ = 0;                // particles made so far: the next one's number
    std::vector<Vec3> cellPos_;        // the positions in cell units, as of the last sort
    // The grids' tiles: of the cells, and of the faces along each axis.
    std::shared_ptr<const Tiles> tiles_, faces_[3];
    // Per kept cell (as SparseGrid::index() has them): its particles are
    // [cellStart_, cellStart_ + cellCount_). layerStart_[k]: the first
    // particle of the layer of cells k (and the end of the last, at nz).
    std::vector<uint32_t> cellStart_, cellCount_, layerStart_;
    SparseGrid vel_[3], old_[3], weight_[3];
    std::vector<uint8_t> valid_[3];    // per kept face
    SparseGrid phi_, pressure_, rhs_;
    SparseGrid sphereWeight_, sphereCentre_[3], sphereFoam_;  // splat() at the solver's resolution
    std::vector<uint8_t> cells_;       // FreeSurfaceSolver::Cell of each kept cell
    // The solids, in tiles of their own round the colliders: their distance
    // at the grid's corners -- (nx+1) x (ny+1) x (nz+1), far where not kept
    // -- and, on the tiles of cells that reach those corners, how open each
    // face is (1 where not kept: the walls are open() 's), which cells hold
    // a solid's centre, and the velocity of a moving solid on its faces.
    SparseGrid solidPhi_;
    std::shared_ptr<const Tiles> solidTiles_;
    SparseGrid solidOpen_[3], solidVel_[3];
    std::vector<uint8_t> solidCell_;   // per kept cell of solidTiles_
    bool anySolid_ = false;
    bool movingSolid_ = false;         // a collider moves: solidVel_ holds its velocity
    std::vector<ShapeInstance> shapes_;  // the colliders, placed
    FreeSurfaceSolver pressureSolver_;
    std::vector<std::array<Grid, 3>> noise_;  // a turbulence force's lattices
    std::vector<uint8_t> filled_;      // a fill source has filled its shape
    uint32_t substepCount_ = 0;
    int frame_ = 0;
    float time_ = 0.0f;
    int lastSubsteps_ = 0, lastIterations_ = 0;
    Times times_;
};

}  // namespace pg::sim
