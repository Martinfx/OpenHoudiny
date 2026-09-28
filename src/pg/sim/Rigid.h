#pragma once
//
// Rigid bodies: the pieces of something broken -- a Voronoi Fracture's --
// that fall, knock into each other and into what is in the scene, glued to
// the pieces they touch until a knock harder than the glue holds breaks
// them apart. Jolt Physics (MIT) steps them, on one thread: the same on
// every machine and on any number of threads.
//
//   pieces     the geometry, its primitives numbered by an attribute
//              (piece). A piece is a body -- or several, when it is made of
//              parts that do not touch -- each part colliding as its convex
//              hull, so a piece of many parts (a cluster of Voronoi cells, a
//              corner of wall and floor) keeps its shape; as heavy as its
//              volume at its density. Pieces with a point attribute proxy
//              (Concrete Fracture's: the plain cut under the rough one) are
//              simulated as the proxy has them and drawn as they are
//   glue       where two bodies touch face to face, a joint as strong as the
//              faces are big: Glue a square metre. Glued, pieces are one
//              body, as rigid as a single piece -- as built, it stands;
//              glued to a still piece, it is held where it is. A knock
//              breaks the joints of the piece it lands on that hold less
//              than it -- how hard: what it took to change how the body
//              moved, or to stop the two things -- and some of it (spread:
//              half) goes on through to the pieces beyond, breaking what
//              holds less there, and so on -- no further than rings of
//              pieces round where it landed, when that is set. What is
//              still glued goes on as a body of its own, as it moved. A
//              break puffs dust and throws out grit
//   colliders  the floor, and the objects linked in: still, or keyed --
//              those push what is in their way
//
// Attributes of the pieces -- on the primitives, else the points; a body
// takes those of its first primitive -- set them apart:
//
//   density   f   kg/m^3, in place of the solver's
//   v, w      v   the velocity and the spin (radians a second) it starts with
//   active    i   0: it does not move -- a foundation -- though it is glued
//                 and in the way
//   glue      f   its joints are this much as strong (the weaker of two); 0:
//                 none
//   release   f   seconds, above 0: then its joints break -- a charge goes
//                 off --
//   kick      v   ... and this velocity is added to it
//   vanish    i   1: at release it is gone, blown to dust -- a burst of it,
//                 and grit -- as explosives pulverise a column
//   crush     f   a knock more than this many times as hard as its glue
//                 holds crushes it to dust: the walls a falling floor lands
//                 on. 0: never
//   cluster   i   the chunk it is of (RBD Cluster), 0 none: its joints to
//                 pieces of the same chunk are ...
//   clusterglue f ... this much as strong (the weaker of two) -- a chunk
//                 comes off whole and breaks up where it lands hard
//
// As in Houdini, a merge fills an attribute a geometry lacks with 0: set
// active and glue on all the pieces, not on some.
//
// A frame keeps where each body is, how it is turned and how it moves
// (RigidFrame), and the grit in the air; the RBD Pieces node puts the
// pieces there, and the solver's look draws them -- their cut faces in a
// colour of their own. The water, the gas and the rain can take the pieces
// as colliders that move with them, and the gas the dust -- from the glue
// that breaks and from the knocks -- as smoke.
//
#include "pg/core/Geometry.h"
#include "pg/sim/Scene.h"

#include <memory>
#include <string>
#include <vector>

namespace pg::sim {

struct RigidSettings {
    float density = 2000.0f;          ///< kg per cubic metre
    float friction = 0.6f;
    float bounce = 0.1f;              ///< how much of its speed a piece keeps in a knock
    /// Pascals: how hard a knock a square metre of glued face holds.
    /// 0: no glue.
    float glue = 500000.0f;
    /// How much of a knock goes on through a joint to the pieces beyond:
    /// 0.5 half -- a hard knock breaks the glue far round where it lands --
    /// 0 none: only the pieces it lands on come loose.
    float spread = 0.5f;
    /// How many rings of pieces round those a knock lands on it can break
    /// loose, however hard it is: 1 the pieces next to them. 0: as far as
    /// spread carries it.
    int rings = 0;
    Vec3 gravity{0.0f, -9.81f, 0.0f};
    int substeps = 2;                 ///< steps of the solver a frame
    bool floor = true;                ///< a floor at y = 0
    float dust = 1.0f;                ///< smoke a broken joint gives off
    float impactDust = 1.0f;          ///< ... and a hard knock
    float dustSize = 0.3f;            ///< metres: how big a puff is
    float debris = 1.0f;              ///< grit a break or a knock throws out; 0: none
    /// The air the pieces squeeze out as they crush and knock -- it pushes
    /// the dust out along the ground: 1 as much as they would, 0 none.
    float air = 1.0f;
    float timeStep = 1.0f / 30.0f;    ///< seconds a frame (the World's)

    bool operator==(const RigidSettings&) const = default;
};

struct RigidScene {
    RigidSettings solver;
    /// The pieces as they stand; compared by pointer -- another geometry,
    /// another simulation.
    std::shared_ptr<const Geometry> pieces;
    std::string attribute = "piece";   ///< what says which piece a primitive is of
    std::vector<Collider> colliders;   ///< the objects: still, or moving (velocity, spin)
    bool intoGas = false;              ///< the pieces are colliders of the gas
    bool intoWater = false;            ///< ... of the water
    bool intoRain = false;             ///< ... of the rain
    bool dustIntoGas = false;          ///< broken glue and knocks puff smoke into the gas
    int node = 0;

    /// The settings kept to what the solver can do: no negative density,
    /// friction or glue, a bounce of 0 to 1, 1 to 16 substeps.
    RigidScene sanitized() const;

    bool operator==(const RigidScene&) const = default;
};

/// What a geometry is as rigid bodies: its pieces, each a body -- or
/// several, when it is made of parts that do not touch -- and where the
/// bodies touch face to face, which the glue holds.
struct RigidLayout {
    std::vector<int32_t> bodyOf;  ///< the body of each primitive
    int bodies = 0;
    /// Each body's parts: the points of each closed piece of surface it is
    /// made of -- what collides, as its convex hull.
    std::vector<std::vector<std::vector<uint32_t>>> parts;
    std::vector<std::vector<uint32_t>> prims;  ///< each body's primitives
    struct Contact {
        int a = 0, b = 0;   ///< the bodies, a < b
        float area = 0.0f;  ///< of the faces they share, world units squared
        Vec3 at;            ///< the middle of those
        Vec3 normal;        ///< across them, from a to b
    };
    std::vector<Contact> contacts;
};

/// The bodies of `pieces`, numbered in the order of their first primitives.
/// Faces touch where they lie in one plane, facing each other, and overlap.
std::shared_ptr<const RigidLayout> rigidLayout(const Geometry& pieces, const std::string& attribute);

/// A body where it is: its rest points p go to position + rotate(p).
struct RigidPose {
    Vec3 position;
    Vec4 rotation{0.0f, 0.0f, 0.0f, 1.0f};  ///< a unit quaternion: x, y, z, w
    Vec3 velocity;                          ///< of the point at `position`, world units a second
    Vec3 spin;                              ///< the axis it turns about, as long as radians a second

    Vec3 apply(const Vec3& p) const;
    /// How fast the point that rests at `p` goes.
    Vec3 velocityAt(const Vec3& p) const { return velocity + cross(spin, apply(p) - position); }
    bool operator==(const RigidPose& o) const {
        return position == o.position && rotation.x == o.rotation.x && rotation.y == o.rotation.y &&
               rotation.z == o.rotation.z && rotation.w == o.rotation.w && velocity == o.velocity && spin == o.spin;
    }
};

/// The rigid bodies of a frame.
struct RigidFrame {
    std::shared_ptr<const Geometry> pieces;        ///< at rest: the scene's
    std::shared_ptr<const RigidLayout> layout;     ///< their bodies; made from them when null
    std::string attribute;
    std::vector<RigidPose> poses;                  ///< body by body
    std::vector<uint32_t> vanished;                ///< the bodies blown to dust, in order
    /// The grit in the air and on the ground: x, y, z and size (metres
    /// across), one after the other; how fast each bit goes (three floats
    /// a bit) and its own number -- the same from frame to frame, as long as
    /// it is there. Those two empty in a frame cached before they were kept.
    std::vector<float> debris;
    std::vector<float> debrisVelocity;
    std::vector<uint32_t> debrisIds;
    size_t joints = 0, broken = 0;                 ///< the glue: how many joints, how many broken so far

    bool empty() const { return poses.empty(); }
    size_t bytes() const {
        return poses.size() * sizeof(RigidPose) + (debris.size() + debrisVelocity.size()) * sizeof(float) +
               debrisIds.size() * sizeof(uint32_t);
    }
};

/// The pieces of `f` where it puts them: points and normals moved and
/// turned, and each point's velocity in v; those blown to dust gone.
std::shared_ptr<Geometry> posedPieces(const RigidFrame& f);

/// The grit of `f` added to `geo` as points: pscale half as wide as a bit
/// is, its velocity v and its number id -- where the frame has them (a frame
/// cached before version 4 has neither). The first point added is returned.
size_t appendGrit(Geometry& geo, const RigidFrame& f);

/// How the solver's look draws the pieces: posed, their faces in the colour
/// Cd they have -- `color` where they have none -- and those of the group
/// `insideGroup`, the faces a fracture cut, in `inside`; and the grit, as
/// points of its size in the colour of the inside, a shade darker.
std::shared_ptr<Geometry> drawnPieces(const RigidFrame& f, const Vec3& color, const Vec3& inside,
                                      const std::string& insideGroup);

/// Where the solver has the points of `pieces`: their proxy -- the plain
/// cut, for rough concrete (Concrete Fracture) -- where they carry one, else
/// where they are. A point with a proxy of 0, merged from pieces that had
/// none, is where it is.
std::vector<Vec3> rigidPositions(const Geometry& pieces);

/// For each primitive of `pieces`, which piece it is of -- 0, 1, ... in the
/// order of the values of `attribute` (on the primitives, else the points),
/// or of what touches what when it has none. The count in `count`.
std::vector<int32_t> pieceOfPrimitives(const Geometry& pieces, const std::string& attribute, int& count);

/// Whether this build has rigid bodies (it was built with Jolt).
bool rigidAvailable();

/// A puff of dust: where, how big, how much -- fading as it ages -- and
/// how fast it goes, the gas taking that on; and how fast it swells, with
/// the air a crush or a knock squeezed out.
struct RigidDust {
    Vec3 at, velocity;
    float size = 0.3f;
    float amount = 1.0f;
    float expansion = 0.0f;  ///< 1/s, as a source's (Emitter)
};

class RigidSolver {
public:
    explicit RigidSolver(const RigidScene& scene);
    ~RigidSolver();
    RigidSolver(const RigidSolver&) = delete;
    RigidSolver& operator=(const RigidSolver&) = delete;

    /// Where the objects are to be at the end of the next step.
    void setColliders(const std::vector<Collider>& colliders);
    void step();

    const RigidScene& scene() const { return scene_; }
    /// How many bodies it simulates.
    size_t pieceCount() const;
    const std::shared_ptr<const RigidLayout>& layout() const { return layout_; }
    RigidFrame capture() const;
    /// The pieces as colliders of the water and the gas: meshes that move.
    std::vector<Collider> colliders() const;
    /// The dust of the last few steps: where glue broke and pieces knocked.
    std::vector<RigidDust> dust() const;
    /// Why nothing is simulated, if nothing is.
    const std::string& error() const { return error_; }

private:
    struct Impl;
    RigidScene scene_;
    std::shared_ptr<const RigidLayout> layout_;
    std::unique_ptr<Impl> impl_;
    std::string error_;
};

}  // namespace pg::sim
