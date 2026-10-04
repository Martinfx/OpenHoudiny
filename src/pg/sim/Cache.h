#pragma once
//
// Frames of a simulation on disk: a folder with a file a frame, and a note
// of what they are. Written once, they are played and rendered again without
// simulating -- with another look or camera, on another machine -- and read
// by the nodes that bring a simulation back as geometry (the editor's Save
// Cache and Load Cache, `prototype sim --cache` and `--from-cache`).
//
//   <folder>/cache.txt            "pgcache 1", frames N, fps F, network HASH;
//                                 while a bake runs, also of M (the frames it
//                                 is making), ms T (a frame, on average) and
//                                 checkpoint K (the frame of its state)
//   <folder>/frame.0001.pgframe   binary, little-endian, as sim::Frame holds it:
//                                 the gas as half floats (runs of zeros
//                                 packed), the water, the particles, the
//                                 rain, the poses of the rigid bodies
//                                 and their grit, the points of the cloth
//
//   <folder>/checkpoint.pgstate   the state of the simulation at frame K
//                                 (WorldSolver::saveState): a bake cut short
//                                 goes on from there
//
// Every file is written beside itself first and renamed into place: a
// reader -- the editor playing a bake as it runs -- finds each whole or not
// at all, and a bake killed halfway leaves what it had finished.
//
// The network's hash says whether a cache belongs to the network at hand.
// The pieces of the rigid bodies at rest are not in the files -- they are
// the network's, cooked when it compiles -- so a frame read back gets them
// from the world it is played in (adoptPieces).
//
#include "pg/sim/Frame.h"
#include "pg/sim/Rigid.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace pg::sim {

struct CacheInfo {
    int frames = 0;        ///< frames 1 .. frames are there
    float fps = 30.0f;
    uint64_t network = 0;  ///< networkHash() of the network that made them
    int of = 0;            ///< the frames a bake is making; 0: as many as there are -- it is done
    double stepMs = 0.0;   ///< how long a frame took, on average; 0: not said
    int checkpoint = 0;    ///< the frame checkpoint.pgstate is of; 0: none

    /// Every frame asked for is there.
    bool done() const { return of <= frames; }
};

/// A network's text (Network::save()) as a number -- where its nodes sit on
/// the canvas left out.
uint64_t networkHash(std::string_view text);

/// A frame as its file holds it, and back. parseFrame is false, with why,
/// for bytes that are not a frame.
std::string formatFrame(const Frame& frame);
bool parseFrame(std::string_view data, Frame& frame, std::string& error);

/// Gives a frame read back the pieces of the rigid bodies at rest: the
/// world's, when the frame's poses are theirs -- as many as the pieces make
/// bodies. Otherwise the frame's rigid bodies stay without geometry, and
/// nothing is drawn of them. The bars in them too (RigidFrame::rebar), when
/// the frame says what became of as many stretches of bar as the world's
/// bars make; and the joints of their glue (RigidFrame::glue), when it says
/// what became of as many as the world's pieces and network make. `memo`,
/// `rebarMemo` and `glueMemo`, if given, keep the bodies, the bars and the
/// joints worked out from one frame to the next. Where pieces broke as it
/// ran (RigidFrame::shatters), their fragments are made again from the
/// world's pieces (rigidBroken) -- `brokenMemo`, if given, keeps them from
/// one frame to the next, and goes on from them when a frame broke more.
void adoptPieces(Frame& frame, const RigidScene& scene, std::shared_ptr<const RigidLayout>* memo = nullptr,
                 std::shared_ptr<const RigidRebar>* rebarMemo = nullptr,
                 std::shared_ptr<const RigidGlue>* glueMemo = nullptr,
                 std::shared_ptr<const RigidBroken>* brokenMemo = nullptr);

/// Gives a frame read back its cloth's geometry -- the scene's, when the
/// frame has as many points; else its cloth stays without and is not drawn.
void adoptCloth(Frame& frame, const ClothScene& scene);

/// "<folder>/frame.0007.pgframe"
std::string frameFile(const std::string& folder, int number);
/// Writes the frame to frameFile(folder, frame.number), making the folder.
bool writeFrame(const Frame& frame, const std::string& folder, std::string& error);
bool readFrame(const std::string& folder, int number, Frame& frame, std::string& error);

bool writeCacheInfo(const std::string& folder, const CacheInfo& info, std::string& error);
bool readCacheInfo(const std::string& folder, CacheInfo& info, std::string& error);

/// "<folder>/checkpoint.pgstate"
std::string checkpointFile(const std::string& folder);
/// The state of a simulation (WorldSolver::saveState) into checkpointFile,
/// in place of the one there; and back.
bool writeCheckpoint(const std::string& folder, std::string_view state, std::string& error);
bool readCheckpoint(const std::string& folder, std::string& state, std::string& error);

/// Writes `bytes` to `path` whole: beside it first, then renamed into place.
bool writeWhole(const std::string& path, std::string_view bytes, std::string& error);

}  // namespace pg::sim
