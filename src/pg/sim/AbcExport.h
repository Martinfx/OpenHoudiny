#pragma once
//
// A simulated shot out to Alembic (pg/abc): what the Output shows, frame
// by frame, as one archive -- what Houdini, Maya, Blender, Nuke, Katana
// and the renderers read a cache from:
//
//   /<node>        the displayed geometry, under an Xform: its polygons a
//                  PolyMesh (N, uv, Cd, v), its open lines Curves, its loose
//                  points Points (ids, v, widths from pscale, Cd)
//   /pieces        each body of the RBD Solver an Xform over a PolyMesh of
//                  its shape about its middle, written once: the Xform
//                  moves and turns it every frame. A body blown to dust --
//                  or a fragment before it broke off -- is not visible
//                  then. The faces a fracture cut are a FaceSet "inside",
//                  for a material of their own
//   /grit          the grit: Points as wide as it is, numbered and moving
//   /grains        the Grain Solver's grains: Points, in their colours
//   /rebar         the steel bars where the pieces have taken them: Curves
//   /cloth         the cloth where its points are: a PolyMesh
//   /water         the water's surface: a PolyMesh of its own every frame,
//                  with normals, velocities and foam
//   /rain          the drops and the droplets: Points
//   /camera        an Xform over a Camera: the lens 24 mm high, as the
//                  program's (a film back of 2.4 cm by 2.4 x the picture's
//                  aspect)
//   the gas        OpenVDB files beside it, a frame each (shot_gas/), as the
//                  USD export writes them
//
// A frame f is at f / fps seconds. What is large and new every frame is
// written as the frame comes -- a shot of any length never has to fit in
// memory; what must be there from the first frame and is small -- the
// bodies' poses, the camera, what is visible when -- is kept and written at
// finish(). A sample the same as the one before is not written again: the
// still pieces' shapes, a still camera, cost one sample each.
//
// Y is up and a unit a metre.
//
#include "pg/core/Geometry.h"
#include "pg/sim/Camera.h"
#include "pg/sim/Frame.h"
#include "pg/sim/Look.h"

#include <memory>
#include <string>

namespace pg::sim {

class AbcExport {
public:
    /// The archive goes to `path` (.abc), the gas to a folder beside it
    /// (shot_gas/shot_gas.0001.vdb). The displayed geometry is the object
    /// `geometryName`; `fps` frames a second.
    explicit AbcExport(std::string path, std::string geometryName = "geometry", float fps = 30.0f);
    ~AbcExport();
    AbcExport(const AbcExport&) = delete;
    AbcExport& operator=(const AbcExport&) = delete;

    /// Each frame, in order: `geometry` the displayed node's (null: none),
    /// `camera` null without one. False, with why, when a file cannot be
    /// written.
    bool add(const Frame& frame, const GeometryPtr& geometry, const Camera* camera, const Look& look,
             std::string& error);
    /// The rest written, the archive closed. False, with why.
    bool finish(std::string& error);

    const std::string& path() const;
    int frames() const;    ///< added
    int bodies() const;    ///< of the RBD Solver
    int gasFiles() const;  ///< written beside the archive

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// Whether `path` names an Alembic archive: .abc, in any case.
bool isAlembicPath(const std::string& path);

}  // namespace pg::sim
