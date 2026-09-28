#pragma once
//
// Frames of a simulation on disk: a folder with a file a frame, and a note
// of what they are. Written once, they are played and rendered again without
// simulating -- with another look or camera, on another machine -- and read
// by the nodes that bring a simulation back as geometry (the editor's Save
// Cache and Load Cache, `prototype sim --cache` and `--from-cache`).
//
//   <folder>/cache.txt            "pgcache 1", frames N, fps F, network HASH
//   <folder>/frame.0001.pgframe   binary, little-endian, as sim::Frame holds it:
//                                 the gas as half floats (runs of zeros
//                                 packed), the water, the particles, the
//                                 rain, the poses of the rigid bodies
//                                 and their grit
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
/// bars make. `memo` and `rebarMemo`, if given, keep the bodies and the
/// bars worked out from one frame to the next.
void adoptPieces(Frame& frame, const RigidScene& scene, std::shared_ptr<const RigidLayout>* memo = nullptr,
                 std::shared_ptr<const RigidRebar>* rebarMemo = nullptr);

/// "<folder>/frame.0007.pgframe"
std::string frameFile(const std::string& folder, int number);
/// Writes the frame to frameFile(folder, frame.number), making the folder.
bool writeFrame(const Frame& frame, const std::string& folder, std::string& error);
bool readFrame(const std::string& folder, int number, Frame& frame, std::string& error);

bool writeCacheInfo(const std::string& folder, const CacheInfo& info, std::string& error);
bool readCacheInfo(const std::string& folder, CacheInfo& info, std::string& error);

}  // namespace pg::sim
