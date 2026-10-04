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
//   grit       small stones -- chips of glass -- a break throws out of the
//              crack, from the rim of the face that broke, and a knock where
//              it lands: they fly, the air holding the small back, tumbling;
//              out of the pieces they came from, they knock into the pieces,
//              the objects and the floor, bounce off -- taking on how those
//              move -- and come to rest, riding on a piece that moves until
//              it throws them off or tips. Pieces come loose flying fast
//              leave dust behind them (trail)
//   network    the glue as geometry (RigidGlue; RBD Constraints makes it,
//              RBD Pieces gives it back from a frame): a point a body, a
//              line a joint, its strength a share of Glue. Linked in, its
//              lines are the joints -- weakened, deleted, drawn where no
//              faces touch -- in place of those the solver finds
//   rebar      steel bars in the pieces (RigidRebar): where the glue has
//              broken, a bar still joins the pieces it runs through, one to
//              the next, as hard as it holds there -- the steel yields, or
//              the bond of the pieces that anchor it on one side of the
//              crack gives, when that holds less. Pulled harder, it gives:
//              the pieces part and the bar shows between them; bent, it
//              stays bent. Where the bond gives -- near the end of a bar,
//              or of a torn one -- the bar slides out of the pieces, one
//              after another, and the concrete falls off it; where the
//              steel does, it stretches, and tears a tenth longer (stretch)
//              over what of it yields
//   breaking   a piece knocked harder than its own section holds
//              (Fracture, pascals a square metre of it) breaks where it was
//              knocked: cut into fragments, most of them round the knock --
//              long along the fibres of wood (its point attribute grain), the
//              faces of the cracks rough (wood: splintered) -- each a body of
//              its own, flying apart as the piece moved, with dust and grit;
//              what knocked it goes on, slowed only by as much of a knock
//              as the piece could stand. A fragment can break again, down
//              to a size. The frames keep
//              where and how each piece broke (RigidShatter): the fragments
//              are made again from that when a frame is read back
//   guide      the pieces as they are to move: the same points, moved --
//              animated as the shot wants it. Each step a body is steered
//              to where the guide has it at the end of the step -- the
//              nearest a rigid body comes to its pieces' points there -- as
//              hard as the strength says; it still knocks into things. A
//              piece goes its own way after a time, when its glue breaks,
//              or when it is further from the guide than a reach
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
//   guide     f   how much the guide leads it, 0 to 1; 0: not at all
//   fracture  f   its section is this much as strong as the solver's
//                 Fracture says; 0: it never breaks
//   grain     v   the way its fibres run (Wood Fracture's): it breaks along them
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

#include <functional>
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
    /// Pascals the steel of the bars (RigidScene::rebar) yields at: a bar
    /// d across holds fy pi d^2 / 4 pulled, fy d^3 / 6 bent.
    float rebarStrength = 500e6f;
    /// Pascals its bond with the concrete holds along a bar's surface: a
    /// piece it runs l through anchors it with bond pi d l.
    float bond = 5e6f;
    /// How much longer a bar gets before it tears, as a share of what of
    /// it yields: its bare length and twenty times its diameter.
    float stretch = 0.1f;
    Vec3 gravity{0.0f, -9.81f, 0.0f};
    int substeps = 2;                 ///< steps of the solver a frame
    /// Bodies at rest -- gone nowhere for half a second, lying on the floor
    /// or on what lies still -- are frozen, nothing to step, until
    /// something comes at them, a flow pushes them or a charge goes off in
    /// them: a pile of rubble costs next to nothing. Off: every body is
    /// stepped to the end.
    bool rest = true;
    bool floor = true;                ///< a floor at y = 0
    float dust = 1.0f;                ///< smoke a broken joint gives off
    float impactDust = 1.0f;          ///< ... and a hard knock
    float dustSize = 0.3f;            ///< metres: how big a puff is
    float debris = 1.0f;              ///< grit a break or a knock throws out; 0: none
    /// Dust the pieces that came loose leave behind them as they fly fast,
    /// for a second and a half after -- the more the bigger and the faster
    /// they are; 0 none.
    float trail = 0.0f;
    /// The air the pieces squeeze out as they crush and knock -- it pushes
    /// the dust out along the ground: 1 as much as they would, 0 none.
    float air = 1.0f;
    /// How hard the guide steers the bodies: 1 onto where it has them at
    /// every step, less a softer pull that lags behind; 0 not at all.
    float guideStrength = 1.0f;
    /// Seconds after which the guide leads nothing; 0: all along.
    float guideUntil = 0.0f;
    /// Metres a body may be from where the guide has it -- stopped by
    /// something, say -- before it goes its own way; 0: however far.
    float guideReach = 0.0f;
    /// A piece whose glue breaks goes its own way.
    bool guideLetGo = true;
    /// How much the water holds the pieces up: 1 as much as the water they
    /// push aside weighs (Archimedes) -- lighter than water, they float --
    /// 0 not at all.
    float buoyancy = 1.0f;
    /// How hard the water carries the pieces and the grit along and slows
    /// them: 1 as it would, 0 not at all.
    float waterDrag = 1.0f;
    /// How much of the gas's flow carries the grit and the pieces -- the
    /// dust cloud's wind, a blast: 1 all of it, 0 none -- still air holds
    /// the grit back all the same.
    float airDrag = 1.0f;
    /// Pascals: how hard a knock a square metre of a piece's own section --
    /// its volume to the power of two thirds -- holds before the piece
    /// itself breaks where it is knocked. 0: the pieces never break, only
    /// the glue between them.
    float fracture = 0.0f;
    /// Fragments a piece breaks into -- up to twice as many in a knock
    /// four times as hard as it holds.
    int fracturePieces = 8;
    /// How many times over a piece can break: 1 only the pieces as they came
    /// in, 2 their fragments too.
    int fractureDepth = 2;
    float fractureMinSize = 0.1f;     ///< metres across: a piece smaller does not break
    /// Metres the faces of a new crack go in and out, each way: 0 flat cuts.
    /// Wood splinters five times as far along its fibres.
    float fractureRough = 0.01f;
    float timeStep = 1.0f / 30.0f;    ///< seconds a frame (the World's)

    bool operator==(const RigidSettings&) const = default;
};

struct RigidGuide;

struct RigidScene {
    RigidSettings solver;
    /// The pieces as they stand; compared by pointer -- another geometry,
    /// another simulation.
    std::shared_ptr<const Geometry> pieces;
    std::string attribute = "piece";   ///< what says which piece a primitive is of
    /// Steel bars in the pieces: polylines, their diameter the attribute
    /// width (on the primitives, else the points; 12 mm without it). Null:
    /// none. Compared by pointer, as the pieces are.
    std::shared_ptr<const Geometry> rebar;
    /// The glue as a network (rigidNetwork): its lines are the joints, in
    /// place of those found where the pieces touch. Null: those. Compared
    /// by pointer, as the pieces are.
    std::shared_ptr<const Geometry> constraints;
    /// Where the guide has the pieces at the end of the first step
    /// (rigidGuide). Null: nothing leads them. Compared by pointer.
    std::shared_ptr<const RigidGuide> guide;
    std::vector<Collider> colliders;   ///< the objects: still, or moving (velocity, spin)
    bool intoGas = false;              ///< the pieces are colliders of the gas
    bool intoWater = false;            ///< ... of the water
    bool intoRain = false;             ///< ... of the rain
    bool intoCloth = false;            ///< ... of the cloth
    bool intoGrains = false;           ///< ... of the grains
    /// The grit its breaks and knocks throw goes to the grains: as soon as
    /// a bit is out of the pieces it came from, it is a grain -- one that
    /// knocks into the others and piles up -- and no more the solver's
    /// (thrown()). Chips of glass stay its own.
    bool gritIntoGrains = false;
    Vec3 gritColor{0.4f, 0.39f, 0.37f};  ///< ... the colour those grains are
    bool dustIntoGas = false;          ///< broken glue and knocks puff smoke into the gas
    /// The faces a fracture cut -- the group the look paints as the broken
    /// inside: a piece that breaks puts the faces of its cracks in it.
    std::string insideGroup = "inside";
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

/// A joint of glue between two bodies of a layout.
struct RigidJoint {
    int a = 0, b = 0;       ///< the bodies, a < b
    Vec3 at;                ///< where their faces touch, at rest; halfway between them where none do
    Vec3 normal;            ///< across those faces, from a to b, at rest; from the one's middle to the other's where none touch
    float area = 0.0f;      ///< of those faces, square metres
    /// How strong it is, as a share of the solver's Glue: it holds Glue x
    /// area x strength newtons. 0 or less: nothing.
    float strength = 1.0f;
};

/// The glue of a geometry's bodies -- Houdini's constraint network: where
/// each body is and which piece it is, and the joints between them.
struct RigidGlue {
    std::vector<Vec3> centres;    ///< each body's at rest: the middle of its box, as the proxy has it
    std::vector<int32_t> piece;   ///< the piece each is: the value of the attribute, else its number 0, 1, ...
    std::vector<int32_t> part;    ///< which of its piece's bodies it is: 0, 1, ... in the order of the bodies
    std::vector<RigidJoint> joints;
    size_t skipped = 0;           ///< lines of the network that join no two bodies of these
};

/// The glue of `pieces`, laid out as `layout` has them. Without `network`:
/// a joint where two bodies touch face to face, its strength the weaker of
/// their attributes glue (1 without), times the weaker clusterglue where
/// both are of one cluster. With it: its lines (rigidNetwork), each from
/// its first point's body to its last's -- the body whose piece the point
/// names in `attribute` (and part, 0 without), or the nearest where the
/// points name none -- with its attributes strength (1 without) and area
/// (square metres; without, or 0, the faces the two share, else 0.01).
/// The same every time for the same geometry.
std::shared_ptr<const RigidGlue> rigidGlue(const Geometry& pieces, const RigidLayout& layout,
                                           const std::string& attribute, const Geometry* network = nullptr);

/// The glue as geometry, the network RBD Constraints makes: a point for each
/// body at its centre, with the piece it is in `attribute` (and part, where
/// a piece is several bodies); an open line for each joint, from the one's
/// point to the other's, with strength, area and Cd -- green as the Glue
/// holds, yellow weaker, blue stronger, grey none. Edited -- a line deleted,
/// weakened, drawn between two pieces -- it is linked into the solver's
/// Constraints.
std::shared_ptr<Geometry> rigidNetwork(const RigidGlue& glue, const std::string& attribute);

/// Steel bars in the pieces: where each runs through which body.
struct RigidRebar {
    struct Bar {
        std::vector<Vec3> points;   ///< where it runs, at rest
        std::vector<float> along;   ///< how far along it each point is, metres
        float width = 0.012f;       ///< its diameter, metres
        uint32_t first = 0, count = 0;  ///< its stations
    };
    /// A stretch of a bar inside one body, in the order along the bar;
    /// none where it runs outside them all.
    struct Station {
        int32_t body = 0;
        float in = 0.0f, out = 0.0f;  ///< how far along the bar it goes into the body, and out of it
    };
    std::vector<Bar> bars;
    std::vector<Station> stations;

    /// The point `s` metres along `bar`, and the way it runs there.
    static Vec3 at(const Bar& bar, float s);
    static Vec3 tangent(const Bar& bar, float s);
};

/// Where the bars `bars` -- polylines, a closed one round to its first
/// point again -- run through the bodies of `pieces` (laid out as `layout`
/// has them): through the convex parts the solver collides, as the proxy
/// has them. A stretch shorter than half the bar's diameter -- a corner
/// grazed -- is none.
std::shared_ptr<const RigidRebar> rigidRebar(const Geometry& pieces, const RigidLayout& layout, const Geometry& bars);

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

/// A piece that broke as the simulation ran: body `body` of the pieces as
/// they were then, cut where it was knocked. Where its fragments are comes
/// of these numbers alone (shatterPiece): a frame keeps them, not the
/// fragments' geometry.
struct RigidShatter {
    uint32_t body = 0;    ///< the body that broke
    Vec3 at;              ///< where it was knocked, as it rests
    uint32_t seed = 0;    ///< where its cells are
    uint32_t count = 0;   ///< how many fragments it was to break into
    float time = 0.0f;    ///< seconds

    bool operator==(const RigidShatter&) const = default;
};

/// Body `s.body` of `pieces` -- laid out as `layout` has them -- broken as
/// `s` says: cut into the Voronoi cells of s.count points, half of them
/// round s.at, the rest anywhere in it; measured, where the piece has a
/// point attribute grain, with its fibres six times shorter (grainCells);
/// the faces of the cracks rough as `settings` says, in `insideGroup`. The
/// fragments are appended to `pieces` -- their points and primitives after
/// all there are, each a new value of `attribute` -- and to `layout`, a
/// body each, numbered on from the last; no contacts. False, nothing
/// changed, when fewer than two fragments come of it. The same every time
/// for the same pieces and the same `s`.
bool shatterPiece(Geometry& pieces, RigidLayout& layout, const RigidShatter& s, const RigidSettings& settings,
                  const std::string& attribute, const std::string& insideGroup);

/// The pieces of `scene` after the breaks `shatters`, one after another;
/// `base`, when given, their layout as they came in (rigidLayout of
/// scene.pieces). Null where a break does not fit the pieces.
struct RigidBroken {
    std::shared_ptr<const Geometry> pieces;
    std::shared_ptr<const RigidLayout> layout;
    std::vector<RigidShatter> shatters;  ///< those it is of
};
/// `before`, when given and its breaks are the first of `shatters`, is gone on
/// from: only the breaks after those are made.
std::shared_ptr<const RigidBroken> rigidBroken(const RigidScene& scene, const std::vector<RigidShatter>& shatters,
                                               std::shared_ptr<const RigidLayout> base = nullptr,
                                               const RigidBroken* before = nullptr);

/// The rigid bodies of a frame.
struct RigidFrame {
    std::shared_ptr<const Geometry> pieces;        ///< at rest: the scene's
    std::shared_ptr<const RigidLayout> layout;     ///< their bodies; made from them when null
    std::string attribute;
    std::vector<RigidPose> poses;                  ///< body by body
    std::vector<uint32_t> vanished;                ///< the bodies blown to dust, in order
    /// The bodies a joint of which has broken -- come loose from one they
    /// were glued to, whether they have moved from it or not -- in order.
    std::vector<uint32_t> unglued;
    /// The grit in the air and on the ground: x, y, z and size (metres
    /// across), one after the other; how fast each bit goes (three floats
    /// a bit) and its own number -- the same from frame to frame, as long as
    /// it is there. Those two empty in a frame cached before they were kept.
    std::vector<float> debris;
    std::vector<float> debrisVelocity;
    std::vector<uint32_t> debrisIds;
    /// How each bit is turned -- a unit quaternion x, y, z, w a bit -- as it
    /// tumbles. Empty in a frame cached before it was kept (version 9).
    std::vector<float> debrisOrient;
    /// 1 where a bit of the grit is a chip of glass -- from a piece whose
    /// attribute glass is 1 or more. Empty: none is.
    std::vector<uint8_t> debrisGlass;
    size_t joints = 0, broken = 0;                 ///< the glue: how many joints, how many broken so far
    /// The joints of the glue, at rest -- the scene's -- null where not known:
    /// a frame read back, until adoptPieces gives it them.
    std::shared_ptr<const RigidGlue> glue;
    /// What became of each of them: kJointHolds; kJointBroken -- at
    /// jointTime, seconds; kJointNone -- it never held: no Glue, both pieces
    /// still, a piece with nothing to it, no strength. Empty: not known.
    std::vector<uint8_t> jointState;
    std::vector<float> jointTime;
    static constexpr uint8_t kJointHolds = 0, kJointBroken = 1, kJointNone = 2;
    /// The pieces that broke as it ran, in the order they did; their
    /// fragments are the bodies after those the pieces came in as.
    std::vector<RigidShatter> shatters;
    std::shared_ptr<const RigidRebar> rebar;       ///< the bars in the pieces, at rest; null: none
    /// What became of each station of a bar: kRebarLoose the bar slid out
    /// of its body, kRebarTorn the bar tore after it. Empty: all as built.
    std::vector<uint8_t> rebarState;
    static constexpr uint8_t kRebarLoose = 1, kRebarTorn = 2;

    bool empty() const { return poses.empty(); }
    size_t bytes() const {
        return poses.size() * sizeof(RigidPose) +
               (debris.size() + debrisVelocity.size() + debrisOrient.size() + jointTime.size()) * sizeof(float) +
               (debrisIds.size() + unglued.size()) * sizeof(uint32_t) + rebarState.size() + debrisGlass.size() +
               jointState.size() + shatters.size() * sizeof(RigidShatter);
    }
};

/// The glue of `f` as a network (as rigidNetwork(glue) has it) where the
/// bodies are: a point at each body's centre, moved and turned with it,
/// with its velocity v; a line for each joint that held -- broken 1 where it
/// has broken, at time (seconds; -1 where it holds), at where its faces
/// touched, gone with the first body -- red where broken. None where the
/// frame knows no glue.
std::shared_ptr<Geometry> rigidNetwork(const RigidFrame& f);

/// The attribute glass of primitive `p` (of an attribute glass, Int or
/// Float, or null): 1 a face of glass, 2 a face of a crack in it -- what
/// Glass Fracture gives them; 0 where it has none.
float glassOf(const AttributeArray* glass, size_t p);

/// Glass is whole until it breaks. For each body of `f` (its layout), 1
/// where it is of a pane of glass still whole: glass pieces -- primitives
/// whose glass is 1 or more -- that touch others at rest, none of whose
/// glue has broken (unglued), none of which has come away from the others
/// or is gone. Its cracks, the faces whose glass is 2, are not there yet.
/// Empty when there is no glass, or no pose: a frame of the pieces at rest
/// keeps all their faces.
std::vector<uint8_t> wholePanes(const RigidFrame& f);

/// The pieces of `f` where it puts them: points and normals moved and
/// turned, and each point's velocity in v; those blown to dust gone, and
/// the cracks of the panes of glass still whole (wholePanes).
std::shared_ptr<Geometry> posedPieces(const RigidFrame& f);

/// The grit of `f` added to `geo` as points: pscale half as wide as a bit
/// is, its velocity v, its number id and how it is turned, orient (a unit
/// quaternion: copied onto the points, a stone turns so) -- where the frame
/// has them (a frame cached before version 4 has neither of the first two,
/// before version 9 no orient) -- and glass 1 for a chip of glass, where
/// there are any. The first point added is returned.
size_t appendGrit(Geometry& geo, const RigidFrame& f);

/// How the solver's look draws the pieces: posed, their faces in the colour
/// Cd they have -- `color` where they have none -- and those of the group
/// `insideGroup`, the faces a fracture cut, in `inside`; the bars in them
/// (rebarBars) as tubes in `steel`; and the grit, as points of its size in
/// the colour of the inside, a shade darker.
std::shared_ptr<Geometry> drawnPieces(const RigidFrame& f, const Vec3& color, const Vec3& inside,
                                      const std::string& insideGroup, const Vec3& steel = Vec3(0.3f, 0.25f, 0.21f));

/// The bars of `f` where the pieces have taken them: a polyline for each
/// stretch of a bar that is in one piece -- torn off at a tear -- held by the
/// pieces it runs through and bent between them where they parted; the
/// point attributes width (its diameter) and v. None in a frame without bars.
std::shared_ptr<Geometry> rebarBars(const RigidFrame& f);

/// Where the solver has the points of `pieces`: their proxy -- the plain
/// cut, for rough concrete (Concrete Fracture) -- where they carry one, else
/// where they are. A point with a proxy of 0, merged from pieces that had
/// none, is where it is.
std::vector<Vec3> rigidPositions(const Geometry& pieces);

/// For each primitive of `pieces`, which piece it is of -- 0, 1, ... in the
/// order of the values of `attribute` (on the primitives, else the points),
/// or of what touches what when it has none. The count in `count`.
std::vector<int32_t> pieceOfPrimitives(const Geometry& pieces, const std::string& attribute, int& count);

/// Where a guide has the pieces: each piece -- numbered as
/// pieceOfPrimitives numbers them -- turned and moved from where it rests,
/// the nearest a rigid piece comes to its points in the guide. A few bytes
/// a piece, whatever the points: a frame keeps it, not the guide's geometry.
struct RigidGuide {
    std::vector<RigidPose> pieces;  ///< position and rotation of each; velocity and spin unused

    bool operator==(const RigidGuide&) const = default;
};

/// The guide the geometry `guide` gives the pieces: their points moved --
/// as many, in the same order. Null when it is not that.
std::shared_ptr<const RigidGuide> rigidGuide(const Geometry& pieces, const Geometry& guide,
                                             const std::string& attribute = "piece");

/// The pieces a guide reads -- what rigidGuide works out of them first, the
/// same at every frame: each point's piece (-1 for none), how many pieces,
/// and where each piece's points are on average at rest.
struct RigidGuideRest {
    std::vector<int32_t> pieceOfPoint;
    std::vector<Vec3> middle;
    size_t points = 0;
};
RigidGuideRest rigidGuideRest(const Geometry& pieces, const std::string& attribute = "piece");
/// ... and the guide from that, frame after frame. Null when `guide` does
/// not have as many points as the pieces.
std::shared_ptr<const RigidGuide> rigidGuide(const Geometry& pieces, const RigidGuideRest& rest, const Geometry& guide);

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

/// The colour a chip of glass is drawn in.
inline constexpr Vec3 kGlassChip(0.86f, 0.94f, 0.92f);

/// A bit of grit as it leaves the pieces: where, how fast, how big across.
struct RigidBit {
    Vec3 at, velocity;
    float size = 0.0f;
    bool operator==(const RigidBit&) const = default;
};

/// The water and the gas, as the pieces and their grit feel them: asked at
/// world points. Either may be missing.
struct RigidFluids {
    /// The height of the water's surface over x, z (WaterLevel); at or below
    /// -1e30 there is none. Null: no water.
    std::function<float(float x, float z)> waterLevel;
    /// How fast the water goes at a point in it.
    std::function<Vec3(const Vec3& p)> waterVelocity;
    /// How fast the gas goes at a point; 0 outside it. Null: no gas.
    std::function<Vec3(const Vec3& p)> airVelocity;
    /// Edges of the cells of the water's grid and of the gas's: how far
    /// round a piece the flow that goes past it is looked for.
    float waterCell = 0.1f, airCell = 0.1f;
};

/// What the water and the gas do to the pieces and the grit in the next step,
/// worked out before it from where everything is (RigidSolver::feel). The
/// same inputs, the same step: a checkpoint keeps them, and steps the pieces
/// again with them, the fluids gone.
struct RigidFlow {
    struct Push {
        uint32_t piece = 0;
        Vec3 force;   ///< newtons on the piece: the water holding it up, the flows dragging it
        Vec3 moment;  ///< newton metres: those forces' moment about the origin, sum of p x F
        /// What the cloth did to it (ClothSolver::reactions): moved its body
        /// by, sped it up by, and turned it faster by -- before the step.
        Vec3 shift, velocity, spin;
    };
    std::vector<Push> pushes;
    /// For each bit of grit there is: the velocity of what it is in -- the
    /// water's or the gas's -- to half a float's precision, x, y, z; and 1
    /// where that is the water.
    std::vector<uint16_t> gritFlow;
    std::vector<uint8_t> gritWet;

    bool empty() const { return pushes.empty() && gritFlow.empty(); }
    bool operator==(const RigidFlow& o) const {
        if (pushes.size() != o.pushes.size() || gritFlow != o.gritFlow || gritWet != o.gritWet) return false;
        for (size_t i = 0; i < pushes.size(); ++i) {
            const Push &a = pushes[i], &b = o.pushes[i];
            if (a.piece != b.piece || !(a.force == b.force) || !(a.moment == b.moment) || !(a.shift == b.shift) ||
                !(a.velocity == b.velocity) || !(a.spin == b.spin)) {
                return false;
            }
        }
        return true;
    }
};

class RigidSolver {
public:
    explicit RigidSolver(const RigidScene& scene);
    ~RigidSolver();
    RigidSolver(const RigidSolver&) = delete;
    RigidSolver& operator=(const RigidSolver&) = delete;

    /// Where the objects are to be at the end of the next step.
    void setColliders(const std::vector<Collider>& colliders);
    /// Where the guide has the pieces at the end of the next step, and how
    /// hard it steers them then (RigidSettings::guideStrength).
    void setGuide(std::shared_ptr<const RigidGuide> guide, float strength);
    /// What the water and the gas do to the pieces and the grit where they
    /// are now: the water holds up the part of each piece under its surface
    /// -- where it is, so a piece rights itself -- and it and the gas drag
    /// them their way (RigidSettings::buoyancy, waterDrag, airDrag).
    RigidFlow feel(const RigidFluids& fluids) const;
    /// ... for the next step to take; then it is spent. Without one, only
    /// gravity and what they knock into move them, and still air.
    void setFlow(RigidFlow flow);
    void step();

    /// Where the last step's time went, milliseconds, and what it stepped.
    struct Times {
        double jolt = 0.0;   ///< Jolt: the contacts and the constraints
        double glue = 0.0;   ///< the knocks breaking the glue, the bodies coming apart
        double grit = 0.0;   ///< the grit flying
        double rest = 0.0;   ///< the charges, the guide, the flows, the dust, the bars
        int bodies = 0;      ///< bodies of pieces that move
        int frozen = 0;      ///< ... of which frozen at rest: static until something comes at them
        int awake = 0;       ///< Jolt's active bodies after the step: those not asleep
        int knocks = 0;      ///< knocks the step found
        int contacts = 0;    ///< contacts Jolt found in the step, a pair of parts each
        int still = 0;       ///< bodies going nowhere lately, not frozen yet
        int waiting = 0;     ///< ... of which lying on what may still go
        int woken = 0;       ///< frozen bodies the step woke, before it and after
        int froze = 0;       ///< ... and froze
        double total() const { return jolt + glue + grit + rest; }
    };
    const Times& times() const;

    const RigidScene& scene() const { return scene_; }
    /// How many bodies it simulates: those of the pieces as they came in,
    /// and the fragments of those that broke.
    size_t pieceCount() const;
    /// The pieces as they are now: the scene's, with the fragments of those
    /// that broke after them.
    const std::shared_ptr<const Geometry>& pieces() const { return pieces_; }
    const std::shared_ptr<const RigidLayout>& layout() const { return layout_; }
    /// The pieces that broke so far.
    const std::vector<RigidShatter>& shatters() const;
    /// Its joints, at rest: those of the network linked in, else where the pieces touch.
    const std::shared_ptr<const RigidGlue>& glue() const { return glue_; }
    RigidFrame capture() const;
    /// The pieces as colliders of the water and the gas: meshes that move.
    std::vector<Collider> colliders() const;
    /// The dust of the last few steps: where glue broke and pieces knocked.
    std::vector<RigidDust> dust() const;
    /// The grit that came out of the pieces in the last step, for the grains
    /// (RigidScene::gritIntoGrains): theirs now, gone from the solver's.
    const std::vector<RigidBit>& thrown() const;
    /// Why nothing is simulated, if nothing is.
    const std::string& error() const { return error_; }

private:
    struct Impl;
    RigidScene scene_;
    std::shared_ptr<const Geometry> pieces_;
    std::shared_ptr<const RigidLayout> layout_;
    std::shared_ptr<const RigidGlue> glue_;
    std::unique_ptr<Impl> impl_;
    std::string error_;
};

}  // namespace pg::sim
