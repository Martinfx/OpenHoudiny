#pragma once
//
// Trees as they grow: a trunk, branches off it, branches off those -- each
// stem a curve thick at its base and thin at its tip, turned up to the light,
// bent by its own weight, wandering a little -- and leaves at the ends. After
// the model of Weber and Penn (Creation and Rendering of Realistic Trees,
// 1995): the crown's outline is how long the first branches are up the
// trunk; the rest grows level by level, each branch round its parent a
// golden angle on from the one before, as leaves and twigs are set.
//
// A tree is the same for the same settings and seed, whatever the number of
// threads. It has no more than 200 000 stems and a million leaves: past
// them it grows no more.
//
#include "pg/core/Geometry.h"

#include <array>
#include <cstdint>
#include <vector>

namespace pg {

struct TreeSettings {
    /// The crown's outline: how long the first branches are, from the
    /// crown's foot up.
    enum class Shape : uint8_t {
        Conical,        ///< longest at the foot: a spruce, a fir
        Spherical,      ///< longest halfway up: an oak, a lime, a maple
        Hemispherical,  ///< long at the foot, round over the top
        Cylindrical,    ///< all as long: a column, a poplar
        Flame,          ///< longest two thirds down: a birch, a pear
        Umbrella,       ///< longest at the top: an acacia, a stone pine
        Weeping,        ///< round, the twigs hanging: a willow
    };
    enum class Leaf : uint8_t {
        Broad,    ///< an oval blade
        Narrow,   ///< a long thin one: a willow's
        Needles,  ///< a toothed spray of needles: a conifer's
    };

    Shape shape = Shape::Spherical;
    float height = 6.0f;   ///< m, the trunk's length
    float radius = 0.16f;  ///< m, the trunk above its foot
    float tip = 0.08f;     ///< the trunk's radius at its top, a share of `radius`
    float flare = 0.35f;   ///< how much wider it is at the ground, a share
    float lean = 0.1f;     ///< how far the trunk leans and bows, 0 to 1
    float crown = 0.35f;   ///< where the branches begin, a share of the trunk's length
    int forks = 1;           ///< how many leaders the trunk parts into: 1 none, up to 5
    float forkHeight = 0.5f;  ///< where, a share of its length
    float forkAngle = 25.0f;  ///< degrees the leaders diverge from it
    int levels = 3;        ///< levels of branches, 0 to 3
    std::array<int, 3> branches{28, 7, 5};            ///< on each parent, at each level
    std::array<float, 3> angle{55.0f, 45.0f, 40.0f};  ///< degrees off the parent
    std::array<float, 3> length{0.5f, 0.45f, 0.4f};   ///< a share of the parent's length
    float thickness = 0.55f;  ///< a branch's radius at its base, a share of its parent's where it grows
    float gravity = 0.25f;    ///< how much the branches droop, 0 to 1
    float up = 0.25f;         ///< how much they turn up to the light, 0 to 1
    float wobble = 0.3f;      ///< how much they wander, 0 to 1
    int leaves = 10;          ///< on each branch of the last level
    float leafSize = 0.12f;   ///< m, a leaf's length
    Leaf leaf = Leaf::Broad;
    Vec3 barkColor{0.14f, 0.11f, 0.085f};
    Vec3 leafColor{0.1f, 0.23f, 0.05f};
    float variation = 0.3f;  ///< how much the leaves, and the trees, differ in shade
    int sides = 10;          ///< faces round the trunk; two fewer round each level of branches, never six
    float segment = 0.25f;   ///< m, how long a piece of the trunk is; branches in pieces as fine
    /// Pruning to an envelope, as Weber and Penn's: how much a branch that
    /// would reach out of it is cut back to it, 0 not at all to 1 to it.
    /// The envelope: round the trunk, `pruneWidth` of the height across at
    /// its widest, `prunePeak` up the crown from its foot; below and above
    /// that narrowing as the powers say (1 a cone, below 1 fuller, above 1
    /// leaner).
    float prune = 0.0f;
    float pruneWidth = 0.5f;
    float prunePeak = 0.5f;
    float prunePowerLow = 0.5f, prunePowerHigh = 0.5f;
    int roots = 0;            ///< roots out from the trunk's foot, above the ground then down into it
    float rootLength = 0.15f;  ///< how long, a share of the trunk's
};

/// A trunk or a branch: a curve from its base to its tip.
struct TreeStem {
    int level = 0;              ///< 0 the trunk, and its leaders where it forks
    int parent = -1;            ///< the stem it grows from
    float along = 0.0f;         ///< where on it, a share of its length
    std::vector<Vec3> points;   ///< base to tip
    std::vector<float> radius;  ///< at each point
    float length = 0.0f;        ///< m
    float path = 0.0f;          ///< m along the wood from the tree's foot to its base
    bool root = false;          ///< a root: no leaves, still in the wind
};

/// A leaf: its base on its stem, which way it points and which way it faces.
struct TreeLeaf {
    Vec3 at;
    Vec3 along;   ///< base to tip, of unit length
    Vec3 facing;  ///< its blade's normal, of unit length, square to `along`
    float size = 0.0f;  ///< m, base to tip
    Vec3 color;
    int stem = 0;
    float path = 0.0f;  ///< m along the wood from the tree's foot to its base
};

struct Tree {
    std::vector<TreeStem> stems;  ///< the trunk first, then level by level
    std::vector<TreeLeaf> leaves;
    Vec3 barkColor;               ///< this tree's shade of the bark
    float height = 1.0f;          ///< m, its trunk's length
};

/// The tree the settings grow from `base` up, `scale` times as big, of `seed`.
Tree growTree(const TreeSettings& s, const Vec3& base, float scale, uint64_t seed);

/// Its stems as tubes -- closed over their tips, the trunk over its foot
/// too; a branch's base inside its parent -- and its leaves as polygons,
/// added to `geo`: point Cd, N -- round the stems, the way each leaf faces
/// -- and flex -- how far along the wood from the
/// tree's foot, a share of its height: 0 at the ground, 1 about the crown's
/// top, what wind bends a tree by --; vertex uv -- round and up the bark,
/// a picture a metre of it, and each leaf in its quarter of the leaf picture
/// --; primitive level (-1 a leaf), stem, parent (the stem a stem grows
/// from, -1 the trunk; a leaf's, the stem it grows on), tree.
void meshTree(const Tree& tree, const TreeSettings& s, int treeIndex, Geometry& geo);

/// Its stems as open polylines -- point pscale their radius, flex;
/// primitive level, stem, parent, tree -- and its leaves as loose points --
/// N the way they face, pscale their size, Cd, flex, orient: what turns a
/// leaf modelled lying flat, facing +y, its stalk at the origin, pointing
/// along +z, onto it, as Copy to Points does -- added to `geo`.
void skeletonTree(const Tree& tree, int treeIndex, Geometry& geo);

}  // namespace pg
