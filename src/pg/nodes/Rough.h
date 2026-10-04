#pragma once
//
// The rough faces of a crack: the faces a cut made, cut into triangles and
// moved by a noise -- the same on both sides of the crack and the same
// triangles, so the pieces still fit -- less and less towards the faces
// that stay, so nothing pokes out of the object. What Concrete Fracture,
// Wood Fracture and the RBD Solver's pieces that break as they are knocked
// make of a plain cut. Where each point was before stays in the point
// attribute proxy: a rigid body solver simulates that and draws the rest.
//
//   concrete  bumps: three octaves of smooth 3D noise, Amount in and out,
//             Scale apart
//   wood      splinters: a face across the fibres (Grain) torn into spikes
//             along them -- bundles of fibres broken at different lengths,
//             Splinter long at most, Splinter Size across; a face along
//             them grooved as the fibres run, Amount deep
//
#include "pg/core/Geometry.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace pg {

/// Perlin's improved gradient noise: smooth, about -1 to 1, the same on
/// every machine.
float gradientNoise(const Vec3& p, uint32_t seed);

/// How a point of a rough concrete face moves, each component between -1
/// and 1: three octaves of noise, bumps `scale` apart and finer ones on
/// them, gently saturated so that none goes further than 1.
Vec3 roughBumps(const Vec3& p, float scale, uint32_t seed);

/// How the faces of a crack are made rough.
struct RoughCut {
    float amount = 0.02f;  ///< metres, as far as a point goes -- each way
    float scale = 0.3f;    ///< metres between the bumps
    float detail = 0.03f;  ///< metres: the triangles no longer
    uint32_t seed = 1;
    /// Wood: the way its fibres run, of unit length; zero: not wood.
    Vec3 grain;
    float splinter = 0.0f;       ///< metres: how far the spikes of a face across the fibres reach
    float splinterSize = 0.01f;  ///< metres: how wide a bundle of fibres is
};

/// The faces nothing roughens -- the outside -- of all the pieces cut out of
/// one thing, as triangles in a sparse grid: how far a point is from them,
/// up to `reach`. The faces of group `rough` are not of them, unless they
/// are of group `flat` too.
class RoughAnchors {
public:
    RoughAnchors(const std::vector<std::shared_ptr<Geometry>>& pieces, const std::string& rough, const std::string& flat,
                 float reach);
    /// How far `p` is from the nearest of them -- `reach` when none is nearer.
    float distance(const Vec3& p) const;

private:
    int at(float x) const;
    static uint64_t key(int i, int j, int k);

    float reach_, cell_ = 1.0f;
    std::vector<std::array<Vec3, 3>> tris_;
    std::unordered_map<uint64_t, std::vector<uint32_t>> cells_;
};

/// `piece` with the faces of group `rough` -- but those of group `flat` --
/// made rough as `r` says, how far from the outside measured by `anchors`
/// (made of all the pieces, so both sides of a crack agree). The plain
/// positions in the point attribute proxy -- where the piece has one
/// already, it is kept, carried onto the new points.
std::shared_ptr<Geometry> roughenCuts(const Geometry& piece, const std::string& rough, const std::string& flat,
                                      const RoughCut& r, const RoughAnchors& anchors);

/// The Voronoi cells of `seeds` in the closed `mesh` (VoronoiCells, the cut
/// faces in group `inside`), measured with the way `grain` -- of unit
/// length; zero: as they are -- `stretch` times shorter than the others:
/// cells that much longer along it, the splints wood breaks into. Made in
/// parallel; those with nothing in them left out, the rest in the order of
/// their seeds. Squeezed and stretched back exactly as the planes cut: the
/// cells together are the mesh.
std::vector<std::shared_ptr<Geometry>> grainCells(const std::shared_ptr<const Geometry>& mesh, const std::vector<Vec3>& seeds,
                                                  const Vec3& grain, float stretch, const std::string& inside);

}  // namespace pg
