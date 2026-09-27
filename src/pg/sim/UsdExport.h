#pragma once
//
// A simulated shot out to USD (pg/io/Usda.h): what the Output shows, frame
// by frame, as one stage -- what Houdini, Blender or usdview open and a
// renderer renders:
//
//   /World/<node>    the displayed geometry: its polygons a Mesh, its open
//                    polylines BasisCurves, its loose points Points, in the
//                    colour of Cd (displayColor); a value a frame only when
//                    it changes
//   /World/pieces    each body of the RBD Solver an Xform over a Mesh of its
//                    shape about its middle: the Xform moves and turns it
//                    (translate, orient), so the shape is written once;
//                    blown to dust, it turns invisible. The faces a fracture
//                    cut are a GeomSubset "inside", for a material of their
//                    own
//   /World/grit      the grit: Points as wide as it is
//   /World/gas       the gas: a Volume over OpenVDB files beside the stage,
//                    one a frame -- density, temperature, flame
//   /World/camera    the network's camera; its lens in tenths of a scene
//                    unit, as USD has it (38 mm: 0.38)
//   /World/sun, sky  the look's light as a DistantLight, its sky as a
//                    DomeLight; /World/ground the floor
//   /World/Looks     a material that takes its colour from displayColor
//
// Y is up and a unit a metre; a time code is a frame.
//
#include "pg/core/Geometry.h"
#include "pg/io/Usda.h"
#include "pg/sim/Camera.h"
#include "pg/sim/Frame.h"
#include "pg/sim/Look.h"

#include <memory>
#include <string>

namespace pg::sim {

class UsdExport {
public:
    /// The stage goes to `path` (.usda) and the gas to a folder beside it,
    /// named after it: shot.usda, shot_gas/shot_gas.0001.vdb. The displayed
    /// geometry is the prim `geometryName`.
    explicit UsdExport(std::string path, std::string geometryName = "geometry");
    ~UsdExport();
    UsdExport(const UsdExport&) = delete;
    UsdExport& operator=(const UsdExport&) = delete;

    /// Each frame, in order: `geometry` the displayed node's (null: none),
    /// `camera` null without one. False, with why, when a file of the gas
    /// cannot be written.
    bool add(const Frame& frame, const GeometryPtr& geometry, const Camera* camera, const Look& look,
             std::string& error);
    /// The stage as it is written: `fps` time codes a second.
    io::usda::Stage stage(float fps) const;
    /// ... written to the path. False, with why.
    bool finish(float fps, std::string& error);

    const std::string& path() const;
    int frames() const;    ///< added
    int bodies() const;    ///< of the RBD Solver
    int gasFiles() const;  ///< written beside the stage

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// Whether `path` names a USD stage: .usda or .usd, in any case.
bool isUsdPath(const std::string& path);

}  // namespace pg::sim
