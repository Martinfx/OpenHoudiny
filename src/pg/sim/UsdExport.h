#pragma once
//
// A simulated shot out to USD (pg/io/Usda.h): what the Output shows, frame
// by frame, as one stage -- what Houdini, Blender or usdview open and a
// renderer renders:
//
//   /World/<node>    the displayed geometry: its polygons a Mesh, its open
//                    polylines BasisCurves, its loose points Points, in the
//                    colour of Cd (displayColor), its other point attributes
//                    as primvars
//   /World/pieces    each body of the RBD Solver an Xform over a Mesh of its
//                    shape about its middle: the Xform moves and turns it
//                    (translate, orient), so the shape is written once;
//                    blown to dust, it turns invisible. The faces a fracture
//                    cut are a GeomSubset "inside", for a material of their
//                    own
//   /World/grit      the grit: Points as wide as it is, with ids and velocities
//   /World/rebar     the steel bars in the pieces, where the pieces have taken
//                    them -- bent between two, torn -- linear BasisCurves as
//                    thick as they are, with velocities
//   /World/cloth     the Cloth Solver's cloth where its points are, with their
//                    normals and velocities: its faces a Mesh, its ropes
//                    BasisCurves; torn, as it is torn -- the points split off,
//                    the faces on them
//   /World/water     the water's surface: a closed Mesh with normals,
//                    velocities and primvars:foam (WaterMesh.h) -- unless
//                    the Water Look hides it
//   /World/rain      the drops and the droplets of the splashes: Points with
//                    ids and velocities, what a renderer draws streaks by
//   /World/gas       the gas: a Volume over OpenVDB files beside the stage,
//                    one a frame -- density, temperature, flame
//   /World/camera    the network's camera; its lens in tenths of a scene
//                    unit, as USD has it (38 mm: 0.38)
//   /World/sun, sky  the look's light as a DistantLight, its sky as a
//                    DomeLight; /World/ground the floor
//   /World/Looks     materials: one that takes its colour from displayColor,
//                    the water's, the rain's
//   /World/Materials the displayed geometry's materials (render/MaterialGraph.h)
//                    -- MaterialX, and a UsdPreviewSurface for what reads
//                    none -- its faces and its prototypes' bound to them, a
//                    GeomSubset a material; their pictures beside the stage
//                    (shot_textures), all of them as shot.mtlx too
//
// What is large and new every frame -- the water, the rain, the grit, the
// bars, the cloth, the displayed geometry when it changes -- goes to a layer of its own for each
// frame, written as the frame comes (shot_frames/shot.0001.usda): the stage
// takes their values from them as USD's value clips, declared in
// shot_frames/shot.manifest.usda. A shot of any length never has to fit in
// memory, and the stage stays small. What does not change is written once,
// in the stage; so is what moves but stays small -- the bodies, the camera.
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
    /// The stage goes to `path` (.usda), the frames' layers and the gas to
    /// folders beside it, named after it: shot.usda, shot_frames/shot.0001.usda,
    /// shot_gas/shot_gas.0001.vdb. The displayed geometry is the prim
    /// `geometryName`; `fps` time codes a second.
    explicit UsdExport(std::string path, std::string geometryName = "geometry", float fps = 30.0f);
    ~UsdExport();
    UsdExport(const UsdExport&) = delete;
    UsdExport& operator=(const UsdExport&) = delete;

    /// Each frame, in order: `geometry` the displayed node's (null: none),
    /// `camera` null without one. False, with why, when a file of it
    /// cannot be written.
    bool add(const Frame& frame, const GeometryPtr& geometry, const Camera* camera, const Look& look,
             std::string& error);
    /// The stage as it is written.
    io::usda::Stage stage() const;
    /// ... written to the path, with the rest of the frames' layers and
    /// their manifest. False, with why.
    bool finish(std::string& error);

    const std::string& path() const;
    int frames() const;      ///< added
    int bodies() const;      ///< of the RBD Solver
    int gasFiles() const;    ///< written beside the stage
    int frameFiles() const;  ///< the frames' layers, written beside the stage
    /// The layer of `frame`'s values, as the stage finds it: "./shot_frames/shot.0001.usda".
    std::string frameFile(int frame) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// Whether `path` names a USD stage: .usda or .usd, in any case.
bool isUsdPath(const std::string& path);

/// Geometry out to `path` as its extension says -- as io::writeGeometry
/// writes it, but a USD stage (.usda) with its materials, its faces bound
/// to them (render/MaterialGraph.h) in the scope Materials under its prim,
/// and .mtlx its materials alone, as a MaterialX document. Their pictures go
/// beside it, to a folder of the name the file has (without the frame of a
/// sequence: field_textures for field.0007.usda). False, with why.
bool exportGeometry(const Geometry& geo, const std::string& path, std::string& error);
/// The extensions exportGeometry() knows, with their dots.
const char* const* exportExtensions();

}  // namespace pg::sim
