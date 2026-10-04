#pragma once
//
// OpenVDB files played back as a shot's gas (VDB Gas): smoke and fire
// simulated elsewhere -- Houdini, Blender, EmberGen -- drawn, rendered and
// exported as the Pyro Solver's would be, a file a frame. Each frame's
// density, temperature and flame grids -- the first of each list of names
// the file has -- and steam, if asked for, are laid on one domain that
// holds every frame's: as every domain, it stands on the floor centred on
// the y axis (Scene.h), so the files' voxels are put on its cells -- at
// most half a voxel from where they were -- and what is below the floor
// is not drawn. A shot the domain would be too fine for -- more cells
// along a side than Resolution -- is averaged down, 2 x 2 x 2 voxels into
// a cell or more.
//
#include "pg/sim/Frame.h"
#include "pg/sim/Scene.h"

#include <string>
#include <vector>

namespace pg::sim {

struct VdbGas {
    std::string file;  ///< an OpenVDB file, or a numbered sequence of them (smoke.$F4.vdb, smoke.####.vdb)
    int offset = 0;    ///< frame f reads the file of frame f + offset
    Vec3 move;         ///< added to where the files put the gas, world units
    bool zUp = false;  ///< the files' world has z up, as Blender's (io::VdbReadOptions::zUp)
    /// The grids each field is read from: names separated by spaces, the
    /// first one the file has. Empty: none.
    std::string density = "density", temperature = "temperature heat", flame = "flame flames fire", steam;
    /// The vector grid how fast the gas goes is read from (Frame::velocity):
    /// what the renderers blur it along. Empty: none.
    std::string velocity = "vel v velocity";
    float densityScale = 1.0f, temperatureScale = 1.0f, flameScale = 1.0f, velocityScale = 1.0f;
    int resolution = 512;  ///< the domain's cells along its longest side at the most
    /// What says the files are as they were when the domain was found --
    /// their sizes and times: other files, another world.
    std::string stamp;

    bool any() const { return !file.empty(); }
    bool operator==(const VdbGas&) const = default;
};

/// The domain that holds what the files of frames 1 to `frames` hold, its
/// cells as large as their voxels -- or `factor` of them a side --; and
/// `gas.stamp`. Notes for what is not as it was: frames with no file, gas
/// below the floor, voxels averaged down. False, with why, when there is
/// no file at all, or no grid of those named in any.
bool vdbGasDomain(VdbGas& gas, int frames, Domain& domain, int& factor, std::vector<std::string>& notes,
                  std::string& error);

/// Frame `frame`'s gas from its file -- sparse: the tiles of `domain` that
/// hold any -- as a frame keeps it (Frame::fields, gasTiles, steam). A
/// frame with no file has none. False, with why, for a file that cannot be
/// read.
bool vdbGasFrame(const VdbGas& gas, const Domain& domain, int frame, Frame& out, std::string& error);

}  // namespace pg::sim
