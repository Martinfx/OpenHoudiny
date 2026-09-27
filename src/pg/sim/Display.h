#pragma once
//
// Geometry as the viewport draws it -- the network's displayed node:
//
//   polygons   triangles (a fan across each closed polygon), lit, in the
//              colour Cd of each corner -- the vertex's, else the point's,
//              the primitive's, the detail's, or a light grey -- bent by the
//              points' N where there is one, else by the faces round each
//              corner that bend less than 60 degrees from its own: round
//              things come out round, a box keeps its edges;
//   polylines  the open primitives, as line segments in their colour;
//   points     those no primitive uses, as dots: pscale wide where they have
//              one, else a few pixels;
//   volumes    a dot in each voxel that is not empty -- blue to yellow as
//              the value grows -- and the box round the volume.
//
// Worked out on the CPU, handed to the renderer as flat arrays of floats.
//
#include "pg/core/Geometry.h"

#include <array>
#include <cstddef>
#include <vector>

namespace pg::sim {

struct DisplayGeometry {
    /// Nine floats a corner, three corners a triangle: position, normal, colour.
    std::vector<float> triangles;
    /// Three floats a corner of `triangles`: its point's velocity v, world
    /// units a second -- what motion blur needs. Empty when the points have
    /// no v.
    std::vector<float> velocities;
    /// Seven floats a dot: position, colour, radius -- world units; 0 for a
    /// dot a few pixels wide.
    std::vector<float> dots;
    /// Seven floats an end, two ends a segment: position, colour, alpha.
    std::vector<float> lines;
    /// The box round it all; lo > hi when there is nothing.
    Vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
    /// Dots left out of volumes that had more than there is room for.
    size_t dotsLeftOut = 0;

    size_t triangleCount() const { return triangles.size() / 27; }
    size_t dotCount() const { return dots.size() / 7; }
    size_t segmentCount() const { return lines.size() / 14; }
    bool empty() const { return triangles.empty() && dots.empty() && lines.empty(); }
};

/// What `geo` looks like in the viewport. At most `maxDots` dots come from
/// its volumes: past that, every second voxel, every third...
DisplayGeometry displayOf(const Geometry& geo, size_t maxDots = 400000);

/// The normal of each corner of `triangles` (three point indices each), in
/// order: the faces round its point that bend less than `crease` degrees
/// from its own, averaged by area.
std::vector<Vec3> cornerNormals(const std::vector<Vec3>& positions, const std::vector<std::array<uint32_t, 3>>& triangles,
                                float crease = 60.0f);

}  // namespace pg::sim
