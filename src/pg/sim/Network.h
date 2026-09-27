#pragma once
//
// A simulation built from nodes: the network the editor shows and a .pgsim
// file holds. Sources, forces and objects feed the solvers -- a Pyro Solver
// for smoke and fire, a Liquid Solver for water -- and what each simulates
// goes through its look to the Output, which draws them all in one scene.
//
//   [Pyro Source] --+
//   [Turbulence] ---+--> [Pyro Solver] ---> [Volume Look] --+
//   [Object] -------+                                       +--> [Output]
//   [Water Source] -+--> [Liquid Solver] -> [Water Look] ---+
//   [Wind] ----------------> [Rain] ------------------------+
//
// Objects are the solids of the scene: every one is drawn, and those linked
// into a solver's Colliders are in the way of what it simulates. Objects and
// sources have a shape, a position, a rotation and a size (Shape.h) -- what
// the viewport's gizmo moves, turns and sizes (NodeType::handles).
//
// compile() turns the network into what runs: a Scene (Scene.h) for the
// solver, a Look (Look.h) for the renderer, and the problems it found, each
// tied to the node it is about, so that the editor can mark it.
//
// Files from earlier versions still load: a node type that changed carries
// its version, and load() turns an old node into its successor (a Sphere
// Source becomes a Pyro Source with the shape sphere, its radius a size).
//
// The node types are a table (nodeTypes()): the type of a pin decides what may
// be linked to it; a parameter carries its section, range, unit and help. The
// editor draws nodes and their parameters from the table alone -- a node type
// added there shows up in the editor, the files and the command line.
//
// File format (.pgsim) -- plain text, one fact per line, in a stable order:
//
//   pgsim 1
//   node 1 pyro_source 2 fire 40 80     # id, type, type version, name, editor x y
//     param center 0 0.12 0             # the parameters that differ from the default
//     param fuel 14
//     param motion circle
//     bypass                            # left out of the simulation
//   node 2 pyro_solver 1 solver 320 80
//   link 1.source -> 2.sources          # an output to an input
//
#include "pg/sim/Camera.h"
#include "pg/sim/Look.h"
#include "pg/sim/Scene.h"
#include "pg/sim/World.h"

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace pg::sim {

class GeometryGraph;

/// What flows along a link. An output links only to an input of its type.
enum class PinType : uint8_t { Source, Force, Collider, Gas, Look, Water, Liquid, Camera, Geometry, Rain };
const char* pinTypeName(PinType type);

struct PinDef {
    const char* name;
    const char* label;
    PinType type;
    bool many = false;  ///< an input that takes any number of links
};

enum class ParamKind : uint8_t {
    Float,
    Int,
    Toggle,  ///< 0 or 1
    Vector,  ///< three numbers: a position, a size, a direction
    Color,   ///< three numbers, 0 to 1
    Choice,  ///< one of `choices`; the value is its index
    File,    ///< a path, text (Node::texts); files write it in quotes; `choices` are the extensions
    Text,    ///< a line of text (Node::texts): a name, a group
    Code,    ///< lines of text (Node::texts): a snippet of the per-element language
};

/// File, Text and Code: parameters whose value is text, kept in Node::texts.
inline bool isText(ParamKind kind) { return kind == ParamKind::File || kind == ParamKind::Text || kind == ParamKind::Code; }

/// A parameter's value: one number, or three for vectors and colours.
using ParamValue = std::array<float, 3>;

struct ParamDef {
    const char* name;
    const char* label;
    const char* section;
    ParamKind kind;
    ParamValue value;       ///< the default
    float min, max;         ///< where a slider runs
    float lo, hi;           ///< what the value is kept within -- typed, it may pass the slider
    const char* unit;       ///< "m", "m/s", "s", "1/s", "deg" or ""
    const char* help;
    std::vector<const char*> choices = {};  ///< Choice: the names, as files and the command line write them
    std::vector<const char*> choiceLabels = {};  ///< ... and as the editor shows them
    const char* text = "";  ///< File, Text, Code: the default
};

/// The parameters of a node that the viewport's gizmo moves, turns and
/// sizes; null where the node has none.
struct Handles {
    const char* center = nullptr;    ///< Vector, m: where it is
    const char* rotation = nullptr;  ///< Vector, degrees about x, y, z
    const char* axis = nullptr;      ///< Vector: a direction, turned by the gizmo
    const char* size = nullptr;      ///< Vector, m: its extent along its own axes
    const char* radius = nullptr;    ///< Float, m: a radius, across its axis
    const char* height = nullptr;    ///< Float, m: a length along its axis

    bool any() const { return center || rotation || axis || size || radius || height; }
};

struct NodeType {
    const char* name;      ///< "pyro_source"
    const char* label;     ///< "Pyro Source"
    const char* category;  ///< "Geometry", "Objects", "Sources", "Forces", "Simulation", "Render"
    const char* help;
    std::vector<PinDef> inputs;
    std::vector<PinDef> outputs;
    std::vector<ParamDef> params;
    int version = 1;
    /// Objects, sources, forces and geometry nodes can be bypassed: left out
    /// -- a geometry node passes its first input on -- kept in place.
    bool bypassable = false;
    Handles handles = {};
    /// A geometry node: the type of the core's cook engine (pg/nodes) that
    /// does its work, or of a node that brings a simulation back as geometry
    /// (GeometryGraph.h). Null for the rest.
    const char* core = nullptr;

    const ParamDef* param(std::string_view name) const;
    const PinDef* input(std::string_view name) const;
    const PinDef* output(std::string_view name) const;
};

/// Every node type, in the order the editor lists them.
const std::vector<NodeType>& nodeTypes();
/// Those and the digital assets of the library (Asset.h).
std::vector<const NodeType*> allNodeTypes();
/// Letters, digits and '_', not starting with a digit.
bool isValidName(std::string_view name);
/// Text that lives as long as the program: what a NodeType made at run
/// time -- an asset's -- points its names at.
const char* internText(const std::string& text);
const NodeType* findNodeType(std::string_view name);

/// The categories, in the order the editor lists them.
const std::vector<const char*>& nodeCategories();

/// A value as files and the command line write it: "0.5", "0 1 0", "on", "circle".
std::string formatParam(const ParamDef& def, const ParamValue& value);
/// The other way round, kept within the parameter's limits. False, with why,
/// for text that is not a value of this parameter.
bool parseParam(const ParamDef& def, std::string_view text, ParamValue& out, std::string& error);

/// How an animated parameter goes from a key to the next.
enum class Interp : uint8_t {
    Smooth,  ///< eases through the keys: a cubic that flattens at the first and last key and where the value turns
    Linear,  ///< straight from one to the next
    Step,    ///< holds the key's value up to the next key
};
/// "smooth", "linear", "step": as files write it.
const char* interpName(Interp interp);

/// A keyframe: the value a parameter has at a frame, and how it goes on to
/// the next key.
struct Key {
    float frame = 1.0f;
    ParamValue value{};
    Interp interp = Interp::Smooth;

    bool operator==(const Key&) const = default;
};

/// The value of a parameter of `kind` animated by `keys` (in the order of
/// their frames) at `frame`: before the first key its value, after the last
/// the last's. Ints come out whole; toggles and choices step.
ParamValue evaluate(const std::vector<Key>& keys, float frame, ParamKind kind);

struct Node {
    int id = 0;
    std::string type;
    int version = 1;
    std::string name;     ///< unique in the network: what --set NAME.param=value names
    float x = 0.0f, y = 0.0f;  ///< where the editor shows it
    bool bypass = false;
    /// A geometry node whose geometry the viewport shows, and renders draw:
    /// at most one in a network.
    bool display = false;
    std::map<std::string, ParamValue> params;  ///< the values set; the rest are defaults
    std::map<std::string, std::string> texts;  ///< File, Text and Code parameters set; the rest are defaults
    /// The animated parameters: their keys, in the order of their frames.
    /// An animated parameter's value is its keys'; `params` holds it for
    /// when the keys are taken off.
    std::map<std::string, std::vector<Key>> keys;
    /// The parameters a wrangle's snippet asks for -- ch("height"),
    /// chv("dir") -- besides those of its type: made from the snippet each
    /// time it changes, not written to files (their values are).
    std::vector<ParamDef> spares;
    /// Parameters driven by an expression -- "$F * 0.1",
    /// "ch(\"../box1/sizex\") * 2" -- by channel: "sizex" for a number,
    /// "center.y" for a component of a vector. An expression wins over the
    /// value and the keys.
    std::map<std::string, std::string> exprs;
};

struct Link {
    int from = 0;
    std::string output;
    int to = 0;
    std::string input;

    bool operator==(const Link&) const = default;
};

/// A parameter of a node inside a digital asset, shown as the asset's own
/// (Asset.h).
struct Promotion {
    std::string node;   ///< the node inside, by name
    std::string param;  ///< its parameter
    std::string name;   ///< the asset's parameter
    std::string label;  ///< as the editor shows it; empty: the parameter's

    bool operator==(const Promotion&) const = default;
};

/// What makes a network a digital asset's definition: its name -- the type
/// of its instances -- its version, and what it shows of the nodes inside.
struct AssetInfo {
    std::string name;   ///< "building"; empty: the network is no asset
    std::string label;  ///< "Building"
    int version = 1;
    std::string help;
    std::vector<Promotion> promoted;

    bool operator==(const AssetInfo&) const = default;
};

struct Problem {
    enum class Level : uint8_t { Error, Warning };
    Level level = Level::Error;
    int node = 0;  ///< the node it is about; 0 for the network as a whole
    std::string message;
};

/// What a network compiles to.
struct Compiled {
    /// True when the Output is reached from a solver: there is something to
    /// simulate. False with the errors in `problems` that say why not.
    bool ok = false;
    /// What is simulated: the gas (world.gas, when world.hasGas) and the
    /// water (world.water, when world.hasWater), at the Output's frame rate.
    World world;
    Look look;
    /// Every object of the network, bypassed ones aside, as it is drawn --
    /// whether a solver collides with it or not.
    std::vector<Solid> solids;
    int frames = 150;  ///< how long the simulation runs, from the Output
    std::vector<Problem> problems;
    /// The nodes that take part in what is simulated, by id, sorted: the
    /// editor dims the others.
    std::vector<int> active;
    int output = 0, lookNode = 0, solver = 0;  ///< the Output, the Volume Look, the Pyro Solver; 0 if none
    int waterLook = 0, liquidSolver = 0;       ///< the Water Look, the Liquid Solver; 0 if none
    int rain = 0;                              ///< the Rain; 0 if none
    /// The camera the Output renders through (its node in camera.node), if
    /// one is linked into it: otherwise the renders frame the scene.
    bool hasCamera = false;
    Camera camera;
    /// The geometry node whose geometry is shown (Network::displayed()); 0 if none.
    int display = 0;

    /// When something is animated, what is drawn at each frame from 1 on:
    /// the look, the objects where they are, the camera. (What is simulated
    /// frame by frame is world.animation.) Empty when nothing is animated.
    struct Pose {
        Look look;
        std::vector<Solid> solids;
        Camera camera;
    };
    std::vector<Pose> poses;
    const Look& lookAt(int frame) const;
    const std::vector<Solid>& solidsAt(int frame) const;
    const Camera& cameraAt(int frame) const;
    /// The world at `frame`: world.at(frame).
    const World& worldAt(int frame) const { return world.at(frame); }

    bool errors() const;
    bool isActive(int node) const;
};

class Network {
public:
    /// Adds a node of `type` with a name of its own ("turbulence1"), its
    /// parameters at their defaults. Returns its id, or 0 for an unknown type.
    int add(std::string_view type, float x = 0.0f, float y = 0.0f);
    /// Removes the node and its links.
    bool remove(int id);

    Node* node(int id);
    const Node* node(int id) const;
    const Node* named(std::string_view name) const;
    const std::vector<Node>& nodes() const { return nodes_; }
    /// "turbulence" -> "turbulence1", or the first free number after it.
    std::string uniqueName(std::string_view base) const;
    /// Letters, digits and '_', not starting with a digit, unique. False, with
    /// why, if not.
    bool rename(int id, std::string_view name, std::string* error = nullptr);

    /// Links an output to an input. An input that takes one link gives up the
    /// one it had. False, with why, for unknown pins or different types.
    bool connect(int from, std::string_view output, int to, std::string_view input, std::string* error = nullptr);
    bool canConnect(int from, std::string_view output, int to, std::string_view input,
                    std::string* error = nullptr) const;
    bool disconnect(const Link& link);
    const std::vector<Link>& links() const { return links_; }
    /// The links into an input, in the order they were made -- the order the
    /// solver applies its forces in.
    std::vector<Link> linksInto(int to, std::string_view input) const;

    /// The parameters of node `id` in the order the editor shows them: its
    /// type's, then those its snippet asks for (Node::spares).
    std::vector<const ParamDef*> params(int id) const;
    /// A parameter of node `id`, of its type or its snippet; null if none.
    const ParamDef* paramDef(int id, std::string_view name) const;

    /// A parameter's value: the one set, else its default. The default of an
    /// unknown parameter is 0.
    ParamValue param(int id, std::string_view name) const;
    float value(int id, std::string_view name) const { return param(id, name)[0]; }
    /// Sets a parameter, kept within its limits. False for an unknown node or
    /// parameter.
    bool setParam(int id, std::string_view name, const ParamValue& value);
    /// The same from text ("0 1 0", "circle"). False, with why, if it is not.
    bool setParam(int id, std::string_view name, std::string_view text, std::string* error = nullptr);
    /// Back to the default, and the keys taken off.
    bool resetParam(int id, std::string_view name);
    /// True for a parameter left at its default -- not set, not animated.
    bool isDefault(int id, std::string_view name) const;

    // --- animation ---------------------------------------------------------------------
    /// A parameter at `frame`: from its keys when it is animated, else param().
    ParamValue valueAt(int id, std::string_view name, float frame) const;
    /// Adds a key at `frame` -- or replaces the one there -- its value kept
    /// within the parameter's limits; the parameter is animated from then
    /// on. Text parameters are not animated. False for an unknown node or
    /// parameter, or a text one.
    bool setKey(int id, std::string_view name, float frame, const ParamValue& value, Interp interp = Interp::Smooth);
    bool removeKey(int id, std::string_view name, float frame);
    /// Takes the keys off; the parameter keeps the value it had at `frame`.
    bool clearKeys(int id, std::string_view name, float frame = 1.0f);
    /// The keys of a parameter, null if it is not animated.
    const std::vector<Key>* keys(int id, std::string_view name) const;
    bool animated(int id, std::string_view name) const { return keys(id, name) != nullptr; }
    /// True when any parameter of any node is animated.
    bool anyAnimated() const;
    /// What editing a parameter at `frame` does: a key there when it is
    /// animated, else its value.
    bool setParamAt(int id, std::string_view name, float frame, const ParamValue& value);
    /// The frames that have a key of any parameter of node `id` -- of every
    /// node, for 0 -- in order, each once.
    std::vector<float> keyFrames(int id = 0) const;
    // --- expressions ----------------------------------------------------------------------
    /// The channels of a parameter: {"sizex"} for a number, {"center.x",
    /// "center.y", "center.z"} for a vector or a colour; none for text.
    static std::vector<std::string> channels(const ParamDef& d);
    /// The expression on `channel` of node `id`; empty if none.
    std::string expression(int id, std::string_view channel) const;
    /// Drives `channel` by `text` -- $F, $T, $FPS, ch("../node/param"), and
    /// everything else a wrangle's expressions have; an empty text takes it
    /// off. An expression that does not parse is kept: expressionError()
    /// says what is wrong, and the parameter keeps its value meanwhile.
    /// False for an unknown node or channel.
    bool setExpression(int id, std::string_view channel, std::string_view text);
    /// True if a channel of parameter `name` has an expression.
    bool hasExpression(int id, std::string_view name) const;
    /// What is wrong with the expression on `channel` at `frame`; empty if
    /// nothing (or no expression).
    std::string expressionError(int id, std::string_view channel, float frame = 1.0f) const;
    /// True when the parameter changes from frame to frame: it has keys, or
    /// an expression that reads the time or a parameter that changes.
    bool varies(int id, std::string_view name) const;

    /// A File, Text or Code parameter: the text set, else its default.
    std::string text(int id, std::string_view name) const;
    /// False for an unknown node or a parameter that is not text.
    bool setText(int id, std::string_view name, std::string_view value);
    bool setBypass(int id, bool on);
    /// Shows the geometry of node `id` -- 0: none -- in the viewport and the
    /// renders. False for a node that is not a geometry node.
    bool setDisplay(int id);
    /// The node whose geometry is shown; 0 if none.
    int displayed() const;

    std::string save() const;
    /// Replaces `out` with the network in `text`. Unknown node types are kept,
    /// and compile() reports them; unknown parameters and links that do not
    /// fit are dropped, each with a line in `warnings`.
    static bool load(std::string_view text, Network& out, std::string& error,
                     std::vector<std::string>* warnings = nullptr);

    /// `folder`: where the network's file is -- a relative path of a File
    /// parameter (a mesh) is read from there. `geometry`: where the geometry
    /// nodes cook -- the editor keeps one, so that only what changed cooks
    /// again; without one they cook afresh. Geometry linked into a Shape is
    /// taken at frame 1.
    ///
    /// Animated parameters are taken frame by frame: what is simulated at
    /// each (world.animation) and what is drawn (Compiled::poses), with the
    /// velocity of what moves. The grids and the frame rate are frame 1's.
    Compiled compile(const std::string& folder = {}, GeometryGraph* geometry = nullptr) const;

    // --- digital assets ----------------------------------------------------------------
    /// What makes this network an asset's definition; its name is empty when
    /// it is none.
    const AssetInfo& asset() const { return asset_; }
    void setAsset(AssetInfo info);
    /// Shows parameter `param` of node `id` as the asset's own -- `on` false:
    /// no longer. Its name there is the parameter's, made unique. False for
    /// an unknown node or parameter.
    bool promote(int id, std::string_view param, bool on);
    /// How parameter `param` of node `id` is promoted; null if it is not.
    const Promotion* promotion(int id, std::string_view param) const;

    /// Bumped by every edit -- all but moving a node, which goes through
    /// node() and changes nothing a simulation sees.
    uint64_t revision() const { return revision_; }

    /// The examples the program carries with it -- the same networks as the
    /// files in examples/sim, so that `prototype pyro fire` needs no file.
    static const std::vector<std::string>& exampleNames();
    static bool example(std::string_view name, Network& out);
    static const char* exampleText(std::string_view name);

    static constexpr int kFormatVersion = 1;

private:
    struct CompileMemo;
    /// The network at `frame`; `quiet`: without problems -- the first frame said them.
    Compiled compileFrame(const std::string& folder, GeometryGraph* geometry, float frame, CompileMemo& memo,
                          bool quiet) const;
    int indexOf(int id) const;
    const ParamDef* def(const Node& n, std::string_view name) const;
    /// valueAt() at `depth` expressions down; what went wrong with them in
    /// `error`, if one is given.
    ParamValue valueAtDepth(int id, std::string_view name, float frame, int depth, std::string* error = nullptr) const;
    bool variesDepth(int id, std::string_view name, int depth) const;
    friend class ExpressionHost;
    /// Makes Node::spares what the node's snippet asks for. True if they changed.
    static bool syncSpares(Node& n);
    /// Turns the nodes of old types (Legacy in Network.cpp) into the types
    /// that replaced them.
    void upgrade(const std::vector<Link>& links);

    std::vector<Node> nodes_;
    std::vector<Link> links_;
    int nextId_ = 1;
    uint64_t revision_ = 0;
    AssetInfo asset_;
};

}  // namespace pg::sim
