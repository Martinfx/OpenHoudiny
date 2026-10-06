#pragma once
//
// What a renderer draws of a geometry, made ahead of the drawing -- on the
// thread that cooked the geometry -- so that the window has little more to
// do than send it to the GPU (gl::VolumeRenderer::setPrepared). Made once,
// it is shared: the viewport and the thumbnails of the whole scene draw the
// same.
//
//   cooker's thread:  cook -> GeometryPreparer::prepare -> PreparedGeometry
//   window's thread:  VolumeRenderer::setPrepared -> buffers, textures
//
// Its parts are those of Display.h: the polygons indexed (DisplayMesher),
// the rest as flat arrays (displayOf), what stands on the points
// (instancesOf) with each prototype indexed at each level of detail, and
// the pictures laid on the faces, read and squared.
//
#include "pg/core/Geometry.h"
#include "pg/sim/Display.h"

#include <cstdint>
#include <map>
#include <memory>
#include <utility>
#include <vector>

namespace pg::sim {

/// How many pixels square a picture laid on a face is in the viewport.
inline constexpr int kDisplayPictureSize = 512;

/// A picture laid on faces, as the viewport sends it to the GPU:
/// kDisplayPictureSize square, four bytes a pixel -- its colour as shown
/// with its alpha; its normal map, green as OpenGL has it, or flat.
struct PictureBytes {
    std::vector<uint8_t> color, normal;
};
/// Null when its colour cannot be read.
std::shared_ptr<const PictureBytes> pictureBytes(const DisplayPicture& picture, int size = kDisplayPictureSize);

struct PreparedGeometry {
    GeometryPtr geometry;  ///< what it was made of
    /// Its polygons, glass apart, indexed. Meshes of one `topology` differ
    /// only in their vertices' places, normals and velocities: a renderer
    /// that sent one sends of the next only those.
    std::shared_ptr<const DisplayMesh> mesh;
    uint64_t topology = 0;
    /// It has more to draw than the polygons (DisplayMesher::hasRest).
    bool rest = false;
    /// The rest of it -- glass, lines, loose points, volumes as dots
    /// (displayOf, its faces apart); empty, as large as the mesh, when there
    /// is none.
    DisplayGeometry display;
    /// What stands on its points, and each prototype of it indexed at each
    /// of its levels of detail -- a plant at kDetailLevels, anything else at
    /// one -- in order, the levels of one after another.
    DisplayInstances instances;
    struct Prototype {
        size_t which = 0;  ///< its place in instances.prototypes
        int level = 0;
        int levels = 1;
        std::shared_ptr<const DisplayMesh> mesh;
    };
    std::vector<Prototype> prototypes;
    /// The pictures the meshes lay on their faces, read.
    std::vector<std::pair<DisplayPicture, std::shared_ptr<const PictureBytes>>> pictures;

    /// What `picture` is, read; null if it is not among them.
    std::shared_ptr<const PictureBytes> bytesOf(const DisplayPicture& picture) const;
};

/// Prepares one geometry after another. What it made last is the start of
/// the next: when only the points moved -- under a brush, a handle -- the
/// mesh is found again quickly, of the same topology; a prototype, a
/// picture prepared already is not prepared again.
class GeometryPreparer {
public:
    /// Null for null; the same again for the geometry prepared last.
    std::shared_ptr<const PreparedGeometry> prepare(const GeometryPtr& geometry);

private:
    DisplayMesher mesher_;
    std::shared_ptr<const PreparedGeometry> last_;
    /// Each prototype's meshes, a level each -- those of the last geometry,
    /// the prototype held: its pointer names it.
    std::map<const Geometry*, std::pair<GeometryPtr, std::vector<std::shared_ptr<const DisplayMesh>>>> prototypes_;
};

/// Whether a plant has foliage to thin far away (core/Lod.h): faces that
/// let light through.
bool hasFoliage(const Geometry& geo);

}  // namespace pg::sim
