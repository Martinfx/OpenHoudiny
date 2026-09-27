#pragma once
//
// A simulation built from nodes: the network the editor shows and a .pgsim
// file holds. Sources, forces and objects feed a Pyro Solver; its gas goes
// through a Volume Look to the Output.
//
//   [Pyro Source] --+
//   [Turbulence] ---+--> [Pyro Solver] --> [Volume Look] --> [Output]
//   [Object] -------+
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

/// What flows along a link. An output links only to an input of its type.
enum class PinType : uint8_t { Source, Force, Collider, Gas, Look };
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
};

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
    const char* category;  ///< "Objects", "Sources", "Forces", "Simulation", "Render"
    const char* help;
    std::vector<PinDef> inputs;
    std::vector<PinDef> outputs;
    std::vector<ParamDef> params;
    int version = 1;
    /// Objects, sources and forces can be bypassed: left out, kept in place.
    bool bypassable = false;
    Handles handles = {};

    const ParamDef* param(std::string_view name) const;
    const PinDef* input(std::string_view name) const;
    const PinDef* output(std::string_view name) const;
};

/// Every node type, in the order the editor lists them.
const std::vector<NodeType>& nodeTypes();
const NodeType* findNodeType(std::string_view name);

/// The categories, in the order the editor lists them.
const std::vector<const char*>& nodeCategories();

/// A value as files and the command line write it: "0.5", "0 1 0", "on", "circle".
std::string formatParam(const ParamDef& def, const ParamValue& value);
/// The other way round, kept within the parameter's limits. False, with why,
/// for text that is not a value of this parameter.
bool parseParam(const ParamDef& def, std::string_view text, ParamValue& out, std::string& error);

struct Node {
    int id = 0;
    std::string type;
    int version = 1;
    std::string name;     ///< unique in the network: what --set NAME.param=value names
    float x = 0.0f, y = 0.0f;  ///< where the editor shows it
    bool bypass = false;
    std::map<std::string, ParamValue> params;  ///< the values set; the rest are defaults
    std::map<std::string, std::string> texts;  ///< File parameters set; the rest are empty
};

struct Link {
    int from = 0;
    std::string output;
    int to = 0;
    std::string input;

    bool operator==(const Link&) const = default;
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
    /// What is simulated: the gas (world.gas, when world.hasGas), at the
    /// Output's frame rate.
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

    /// A parameter's value: the one set, else its default. The default of an
    /// unknown parameter is 0.
    ParamValue param(int id, std::string_view name) const;
    float value(int id, std::string_view name) const { return param(id, name)[0]; }
    /// Sets a parameter, kept within its limits. False for an unknown node or
    /// parameter.
    bool setParam(int id, std::string_view name, const ParamValue& value);
    /// The same from text ("0 1 0", "circle"). False, with why, if it is not.
    bool setParam(int id, std::string_view name, std::string_view text, std::string* error = nullptr);
    bool resetParam(int id, std::string_view name);
    bool isDefault(int id, std::string_view name) const;
    /// A File parameter: the path set, else empty.
    std::string text(int id, std::string_view name) const;
    /// False for an unknown node or a parameter that is not a File.
    bool setText(int id, std::string_view name, std::string_view value);
    bool setBypass(int id, bool on);

    std::string save() const;
    /// Replaces `out` with the network in `text`. Unknown node types are kept,
    /// and compile() reports them; unknown parameters and links that do not
    /// fit are dropped, each with a line in `warnings`.
    static bool load(std::string_view text, Network& out, std::string& error,
                     std::vector<std::string>* warnings = nullptr);

    /// `folder`: where the network's file is -- a relative path of a File
    /// parameter (a mesh) is read from there.
    Compiled compile(const std::string& folder = {}) const;

    /// Bumped by every edit -- all but moving a node, which goes through
    /// node() and changes nothing a simulation sees.
    uint64_t revision() const { return revision_; }

    /// The examples the program carries with it -- the same networks as the
    /// files in examples/sim, so that `pgshader pyro fire` needs no file.
    static const std::vector<std::string>& exampleNames();
    static bool example(std::string_view name, Network& out);
    static const char* exampleText(std::string_view name);

    static constexpr int kFormatVersion = 1;

private:
    int indexOf(int id) const;
    /// Turns the nodes of old types (Legacy in Network.cpp) into the types
    /// that replaced them.
    void upgrade(const std::vector<Link>& links);

    std::vector<Node> nodes_;
    std::vector<Link> links_;
    int nextId_ = 1;
    uint64_t revision_ = 0;
};

}  // namespace pg::sim
