#pragma once
//
// Volumes to OpenVDB files (.vdb): what Houdini, Blender and the renderers
// read smoke from. Written without the library, as OpenVDB 10
// writes them -- file version 224:
//
//   header      magic, versions, a UUID (from what the file holds: the same
//               volumes give the same bytes), file metadata ("creator")
//   each grid   its name, "Tree_float_5_4_3", where it starts and ends;
//               active-mask compression (no zip); metadata ("class",
//               "name", the bounding box and the voxel count); a uniform
//               scale and a translation that puts voxel (i, j, k) at the
//               volume's voxel centre; the tree -- root, 32^3 and 16^3
//               internal nodes, 8^3 leaves -- as topology, then the leaves'
//               values
//
// A voxel that is not 0 is active; the rest are the background, 0. Only the
// blocks of 8^3 voxels that hold an active one are written. A volume whose
// values are all at least 0 is a fog volume -- smoke, as the renderers draw it.
//
#include "pg/core/Geometry.h"

#include <string>
#include <vector>

namespace pg::io {

/// The file's bytes: a float grid for each volume, named as it is (a name
/// that comes twice gets a suffix, "density_2").
std::string formatVdb(const std::vector<Volume>& volumes);
/// Writes them to `path`. False, with why, if the file cannot be written or
/// there is no volume.
bool writeVdb(const std::vector<Volume>& volumes, const std::string& path, std::string& error);

}  // namespace pg::io
