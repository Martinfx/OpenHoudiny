#pragma once
//
// OpenVDB files (.vdb): what Houdini, Blender, EmberGen and the renderers
// keep smoke, fire and distance fields in. Written and read without the
// library.
//
// Written as OpenVDB 10 writes them -- file version 224:
//
//   header      magic, versions, a UUID (from what the file holds: the same
//               volumes give the same bytes), file metadata ("creator")
//   each grid   its name, "Tree_float_5_4_3" (or "Tree_vec3s_5_4_3" for a
//               vector), where it starts and ends;
//               active-mask compression, the values in Blosc (LZ4, the
//               bytes shuffled) as Houdini writes them, or zipped, or as
//               they are (VdbCompression); metadata ("class",
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
// Read as OpenVDB 3 to 12 write them -- file versions 222 to 224: grids of
// float, double, int32, int64 and their vec3 kinds, kept as half floats or
// not, with their values as they are, zip- or Blosc-compressed (LZ4, zlib,
// BloscLZ: Blosc.h), the active ones alone or all; tiles at every level of
// the tree; instances of another grid's tree; the scale, translation and
// affine transforms. Each grid becomes a Volume: every voxel of the box its
// active ones are in, in the world -- the inactive ones too, as a level set
// keeps its inside below 0. A transform that is not a uniform scale along
// the axes (a turned grid, voxels longer one way) is resampled onto cubic
// voxels. Not read: bool, mask, string and point grids, frustum transforms.
//
#include "pg/core/Geometry.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace pg::io {

/// How a grid's values are kept: as they are, zipped (zlib, what every
/// OpenVDB reads), or in Blosc frames (LZ4 -- what Houdini writes; read by
/// an OpenVDB built with Blosc, as Houdini's, Blender's and the renderers'
/// are). Values too few to gain by it are kept as they are, as OpenVDB does.
enum class VdbCompression { None, Zip, Blosc };

/// The file's bytes: a float grid for each volume, named as it is (a name
/// that comes twice gets a suffix, "density_2") -- but for a vector's
/// parts, "vel.x", "vel.y" and "vel.z" in turn and laid out alike: one
/// vector grid of them, "vel" (vec3s), as Houdini writes a velocity.
std::string formatVdb(const std::vector<Volume>& volumes, VdbCompression compression = VdbCompression::Blosc);
/// Writes them to `path`. False, with why, if the file cannot be written or
/// there is no volume.
bool writeVdb(const std::vector<Volume>& volumes, const std::string& path, std::string& error,
              VdbCompression compression = VdbCompression::Blosc);

/// A grid of an OpenVDB file, as its header says (vdbGrids).
struct VdbGridInfo {
    std::string name;
    std::string type;       ///< its values: float, double, int32, int64, vec3s, vec3d, vec3i, bool ...
    std::string gridClass;  ///< "fog volume", "level set", "staggered", "unknown"
    bool half = false;      ///< kept as half floats
    bool readable = false;  ///< of a kind readVdb() reads
    float voxel = 0.0f;     ///< the shortest edge of a voxel, world units
    /// Where its active voxels are -- the box round their outer faces, in
    /// the world -- when the file says (OpenVDB writes it as metadata).
    bool bounded = false;
    Vec3 lo, hi;
};

/// The grids of an OpenVDB file, from their headers alone: what is in it
/// and where, without reading their values -- quick for a file of any
/// size. `zUp`: as readVdb() turns them. False, with why, for a file that
/// is not one.
bool vdbGrids(const std::string& path, std::vector<VdbGridInfo>& grids, std::string& error, bool zUp = false);

struct VdbReadOptions {
    std::vector<std::string> grids;  ///< which, by name; empty: every one there is that it reads
    /// A volume of more voxels than this is averaged down -- 2 x 2 x 2
    /// voxels into one, or 3 x 3 x 3 ... -- until it fits; a note says so.
    size_t maxVoxels = size_t(1) << 25;
    int downsample = 1;  ///< at least this many voxels a side averaged into one
    /// The file's world has z up, as Blender's: turned about x so that y is
    /// -- (x, y, z) to (x, z, -y) -- vectors with it.
    bool zUp = false;
};

/// What readVdb() read.
struct VdbVolumes {
    /// A volume for each grid read -- three for a vector grid: name.x,
    /// name.y, name.z -- in the order of the file.
    std::vector<Volume> volumes;
    std::vector<std::string> classes;  ///< each volume's grid's class: "level set" is a distance, inside below 0
    std::vector<int> components;       ///< each volume's grid's values: 1, or 3 for a vector
    std::vector<std::string> notes;    ///< what was not read, or not as it is
};

/// The grids of the OpenVDB file `path` as volumes. A grid with no active
/// voxel gives none. False, with why, for a file that is not one, is cut
/// short or broken; a grid of a kind not read is a note.
bool readVdb(const std::string& path, VdbVolumes& out, std::string& error, const VdbReadOptions& options = {});
/// The same from the file's bytes.
bool parseVdb(std::span<const uint8_t> bytes, VdbVolumes& out, std::string& error, const VdbReadOptions& options = {});

}  // namespace pg::io
