#pragma once
//
// Smoke and fire: a gas simulated on a 3D grid -- the idea behind Houdini's
// Pyro, small enough to read.
//
// Each step:
//   1. emit      the source adds fuel (fire) or hot smoke, and pushes it up;
//   2. advect    the flow carries everything along, itself included
//                (semi-Lagrangian; MacCormack for smoke, heat and fuel, which
//                keeps them sharper);
//   3. combust   fuel burns into heat, soot and flame, and the gas expands;
//   4. forces    heat rises, soot weighs down, vorticity confinement puts
//                back the swirls a coarse grid smooths away, turbulence
//                breaks the flow up;
//   5. project   pressure makes the flow incompressible -- except where the
//                burning gas expands (multigrid, open boundaries: Poisson.h);
//   6. dissipate smoke thins out, heat cools, flames die.
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
// Deterministic (invariant I5): every loop is a pg::parallelFor over rows of
// cells, each cell written by exactly one chunk and no sums across chunks, so
// the same settings give the same bits on any number of threads.
//
#include "pg/sim/Grid.h"
#include "pg/sim/Poisson.h"

#include <cstdint>
#include <vector>

namespace pg::sim {

struct PyroSettings {
    /// Cells across, 8 to 256; the domain is 1 wide, 1.5 tall and 1 deep.
    /// Rounded up to a multiple of 8, so that multigrid can halve the grid a
    /// few times.
    int resolution = 64;
    float timeStep = 1.0f / 30.0f;
    int substeps = 1;

    // The source: a sphere near the floor, its output flickering with noise.
    float sourceRadius = 0.1f;   ///< domain units
    float sourceHeight = 0.12f;  ///< centre above the floor
    float sourceSpeed = 0.5f;    ///< upward velocity it gives the gas, domain units/s
    float fuelRate = 0.0f;       ///< fuel added per second -- fire
    float smokeRate = 0.0f;      ///< smoke added per second
    float heatRate = 0.0f;       ///< heat added per second
    float sourceNoise = 0.6f;    ///< 0 steady, 1 strongly flickering

    // Combustion.
    float burnRate = 4.0f;     ///< share of the fuel that burns per second
    float heatRelease = 3.0f;  ///< heat per unit of fuel burnt
    float sootRelease = 0.5f;  ///< smoke per unit of fuel burnt
    float expansion = 1.0f;    ///< expansion of the gas per unit of fuel burnt
    float flameLife = 0.12f;   ///< seconds the flame of burning fuel lasts

    // Forces.
    float buoyancy = 1.2f;     ///< lift per unit of heat
    float weight = 0.1f;       ///< sink per unit of smoke
    float vorticity = 0.35f;   ///< confinement strength: swirls
    float turbulence = 0.0f;   ///< noisy force where there is heat or fuel
    float turbulenceScale = 0.12f;  ///< size of its whirls, domain units

    // Dissipation, per second.
    float cooling = 1.0f;
    float smokeDecay = 0.05f;

    int pressureCycles = 2;  ///< multigrid V-cycles per step
    uint32_t seed = 1;

    static PyroSettings fire();
    static PyroSettings smoke();

    /// Every number in a range the solver can work with: resolution 8 to 256,
    /// a time step above 0 and at most 1 s, 1 to 16 substeps and pressure
    /// cycles, rates at least the minimum of their pyroParams() entry. What is
    /// not a number becomes the default. The solver takes its settings this
    /// way, so no input makes it divide by zero or allocate the machine away.
    PyroSettings sanitized() const;
};

/// The numbers of PyroSettings by name, with a range and a word of help: what
/// the editor shows as sliders and `pgshader pyro --set name=value` changes.
struct PyroParam {
    const char* name;
    const char* label;
    const char* group;  ///< Source, Combustion, Forces, Dissipation
    float PyroSettings::*member;
    float min, max;
    const char* help;
};
const std::vector<PyroParam>& pyroParams();

class PyroSolver {
public:
    explicit PyroSolver(const PyroSettings& settings = PyroSettings::fire());

    /// Empty domain, time 0.
    void reset();
    /// Advances by timeStep, in `substeps` steps.
    void step();

    const PyroSettings& settings() const { return settings_; }
    /// Takes effect with the next step; a new resolution resets.
    void setSettings(const PyroSettings& settings);

    const Grid& density() const { return density_; }  ///< smoke, soot
    const Grid& temperature() const { return temperature_; }
    const Grid& fuel() const { return fuel_; }
    /// Fuel burnt within the last flameLife seconds: where the fire is.
    const Grid& flame() const { return flame_; }
    /// Velocity component `axis` on its faces, in domain units per second:
    /// value (i, j, k) of axis 0 is on the face between cells i-1 and i.
    const Grid& velocity(int axis) const { return vel_[axis]; }
    Grid& velocity(int axis) { return vel_[axis]; }
    /// The velocity at a position in cell units, interpolated from the faces.
    void velocityAt(float x, float y, float z, float out[3]) const;

    int nx() const { return nx_; }
    int ny() const { return ny_; }
    int nz() const { return nz_; }
    int frame() const { return frame_; }
    float time() const { return time_; }
    /// Edge of a cell in domain units.
    float cellSize() const { return 1.0f / static_cast<float>(nx_); }

    // The stages of a step -- public for tests.
    void emit(float dt);
    void advect(float dt);
    void combust(float dt);
    void addForces(float dt);
    void project();
    void dissipate(float dt);
    /// Mean |divergence of the velocity - expansion| over the cells, in 1/s:
    /// what project() removes.
    double meanDivergence() const;

private:
    void advectScalar(Grid& field);
    void addTurbulence(float dt);
    void advectVelocity(int axis, float cells);
    /// The velocity at face (i, j, k) of component `axis`: its own value, and
    /// the other two averaged from the four faces around it.
    void faceVelocity(int axis, int i, int j, int k, float out[3]) const;
    float divergence(int i, int j, int k) const;

    PyroSettings settings_;
    int nx_ = 0, ny_ = 0, nz_ = 0;
    Grid vel_[3], velNext_[3];
    Grid density_, temperature_, fuel_, flame_;
    Grid back_[3], forward_[3];  // where each cell's gas came from / goes to, cell units
    Grid predicted_, lo_, hi_, corrected_;
    Grid expansion_;             // divergence the burning asks for, 1/s
    Grid pressure_, divergence_;
    PoissonSolver poisson_;
    Grid centre_[3], curl_[3], curlLength_;
    Grid noise_[3];              // the turbulence, on a coarse lattice
    int frame_ = 0;
    float time_ = 0.0f;
};

/// Transmittance from each cell towards a light: exp(-optical depth) through
/// `density` along `towardsLight`, at 1/`divisor` of the resolution. What a
/// volume renderer shades smoke with -- its shadows.
Grid lightTransmittance(const Grid& density, const float towardsLight[3], float extinctionPerCell, int divisor);

}  // namespace pg::sim
