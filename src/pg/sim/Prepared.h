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
// So too the bodies of the simulation's frames -- pieces, cloth, grains --
// as the look draws them: on a thread of their own (BodiesPreparer), the
// frame at the play head first, then those it plays next.
//
#include "pg/core/Geometry.h"
#include "pg/sim/Display.h"
#include "pg/sim/Frame.h"
#include "pg/sim/Look.h"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
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

/// Geometry prepared on a thread of its own, for drawings apart from the
/// viewport -- the nodes' thumbnails: those wanted, the first first; what
/// was made kept, the no longer wanted going first past kKept.
class GeometryPreparerThread {
public:
    static constexpr size_t kKept = 4;

    GeometryPreparerThread();
    ~GeometryPreparerThread();
    GeometryPreparerThread(const GeometryPreparerThread&) = delete;
    GeometryPreparerThread& operator=(const GeometryPreparerThread&) = delete;

    /// What is wanted now, in place of what was: the first not made yet is
    /// made next.
    void want(std::vector<GeometryPtr> wanted);
    /// `geometry` prepared, if it is; null if not (yet).
    std::shared_ptr<const PreparedGeometry> find(const GeometryPtr& geometry) const;
    /// Until everything wanted is made.
    void wait();

private:
    void loop();
    /// mu_ held: the prepared `geometry`; the first wanted not made yet, -1
    /// if none.
    std::shared_ptr<const PreparedGeometry> madeLocked(const GeometryPtr& geometry) const;
    int nextLocked() const;

    mutable std::mutex mu_;
    std::condition_variable wake_, done_;
    bool stop_ = false, working_ = false;
    std::vector<GeometryPtr> wanted_;
    std::deque<std::shared_ptr<const PreparedGeometry>> made_;  ///< the newest last
    std::thread thread_;
};

/// Whether a plant has foliage to thin far away (core/Lod.h): faces that
/// let light through.
bool hasFoliage(const Geometry& geo);

/// The pieces, cloth and grains of a frame as a look draws them
/// (drawnBodies), made ready to draw: the triangles, lines and dots the
/// viewport sends. Empty when it draws none of them.
struct PreparedBodies {
    DisplayGeometry display;
};
/// What of a look the bodies drawn of a frame depend on: the same for two
/// looks that draw them alike.
std::string bodiesKey(const Look& look);
/// Whether `look` draws anything of the bodies of `frame`.
bool drawsBodies(const Frame& frame, const Look& look);
std::shared_ptr<const PreparedBodies> prepareBodies(const Frame& frame, const Look& look);

/// The bodies of frames made ready to draw on a thread of its own: those
/// wanted, the most wanted first -- the frame at the play head, those it
/// plays next, the thumbnails'. What was made is kept, the frames no longer
/// wanted going first past kKept, as long as their frame is.
class BodiesPreparer {
public:
    struct Want {
        std::shared_ptr<const Frame> frame;
        Look look;
    };
    static constexpr size_t kKept = 4;

    BodiesPreparer();
    ~BodiesPreparer();
    BodiesPreparer(const BodiesPreparer&) = delete;
    BodiesPreparer& operator=(const BodiesPreparer&) = delete;

    /// What is wanted now, in place of what was: the first not made yet is
    /// made next.
    void want(std::vector<Want> wanted);
    /// `frame` as `look` draws it, if it is made; null if not (yet).
    std::shared_ptr<const PreparedBodies> find(const std::shared_ptr<const Frame>& frame, const Look& look) const;
    /// Until everything wanted is made.
    void wait();

private:
    struct Made {
        std::weak_ptr<const Frame> frame;  ///< what it was made of: another frame where it was, no match
        std::string key;
        std::shared_ptr<const PreparedBodies> bodies;
    };
    void loop();
    /// mu_ held: what was made of `frame` with `key`; the first wanted not
    /// made yet, -1 if none; whether a made one is wanted.
    const Made* madeLocked(const std::shared_ptr<const Frame>& frame, const std::string& key) const;
    int nextLocked() const;
    bool wantedLocked(const Made& made) const;

    mutable std::mutex mu_;
    std::condition_variable wake_, done_;
    bool stop_ = false, working_ = false;
    std::vector<Want> wanted_;
    std::vector<std::string> wantedKeys_;
    std::deque<Made> made_;  ///< the newest last
    std::thread thread_;
};

}  // namespace pg::sim
