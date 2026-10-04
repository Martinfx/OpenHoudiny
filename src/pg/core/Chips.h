#pragma once
//
// The shapes of grit, as the renderers draw it and the USD carries it: each
// bit a chip of stone -- a box flattened and stretched, its corners and
// edges broken off by planes, as a bit of a break is -- or a sliver of
// glass, of three to five sides as thick as a pane. A dozen shapes of
// stone, half a dozen of glass; which one a bit is goes by its number, so
// that it keeps its shape as it flies, and so does its shade: stones are
// not all alike.
//
#include "pg/core/Types.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace pg {

inline constexpr size_t kChipShapes = 12;   ///< of stone
inline constexpr size_t kSliverShapes = 6;  ///< of glass

/// A face of a chip: its corners round the way it faces, counter-clockwise
/// seen from outside.
using ChipFace = std::vector<Vec3>;

/// The faces of the chip of stone -- or the sliver of glass -- `shape`
/// (below kChipShapes, kSliverShapes), flat, a unit from its middle to its
/// farthest corner.
std::vector<ChipFace> chipFaces(size_t shape, bool glass);

/// The `k`-th number of `seed`, 0 to 1: what the chips are chosen, tinted
/// and turned by.
float chipRandom(uint32_t seed, uint32_t k);
/// The shape the bit numbered `seed` is, of those of stone or of glass.
size_t chipShapeOf(uint32_t seed, bool glass);
/// Its colour: `color`, a shade of its own -- lighter or darker, some
/// greyer -- for stone; as it is for glass.
Vec3 chipTint(const Vec3& color, uint32_t seed, bool glass);
/// How it is turned without a turn of its own: any way as likely as any
/// other (Shoemake's uniform rotation), a quaternion x, y, z, w.
Vec4 chipTurn(uint32_t seed);

}  // namespace pg
