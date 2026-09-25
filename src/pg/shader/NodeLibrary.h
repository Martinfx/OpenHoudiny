#pragma once
//
// Node definitions, and the text format they are written in.
//
// No node is hard-coded. The C++ side knows types, templates and stages; what
// `mix` or `lambert` means is written in a .pgnodes file -- the built-in nodes
// included (src/pg/shader/builtin.pgnodes). Adding a node means adding a few
// lines of text; a user library loaded later can add nodes or replace built-in
// ones without recompiling anything.
//
//   # comments start with '#'
//   node mix
//       label    Mix
//       category Math
//       description Blends a and b by t.
//       in  a any = 0.0
//       in  b any = 1.0
//       in  t float = 0.5
//       out result any = mix({a}, {b}, {t})
//
//   node texture
//       category Texture
//       param name string = albedo              # not connectable; editable
//       uniform u_{name} sampler2D              # declared in the generated shader
//       in  uv vec2 = $uv                        # unconnected: the mesh UVs
//       out rgba vec4 = texture(u_{name}, {uv})
//       impl hlsl rgba = u_{name}.Sample(u_{name}_sampler, {uv})
//       out rgb vec3 = {rgba}.rgb                # outputs can use earlier outputs
//
//   function pg_hash                              # helper code, emitted once if used
//   float pg_hash(vec2 p) { ... }
//   end
//
// Line syntax inside a node:
//   label <text> | category <text> | description <text> | version <int>
//   kind output                                   the graph's root; its inputs
//                                                 are the stage outputs
//   in    <name> <type> [= <numbers> | = $<global>] [color] [stage vertex|fragment]
//   param <name> <float|vec2|vec3|vec4|string> [= <value>] [color]
//   uniform <name-template> <type> [= <default-template>]
//   uses  <function>...
//   out   <name> <type> = <template>
//   impl  <target> <output> = <template>          per-target override of an output
//
// Templates are code in the neutral dialect (GLSL syntax); each target
// translates identifiers such as vec3 or mix into its own language.
// Placeholders: {input}, {param}, {earlier-output}, and globals $position,
// $normal, $uv, $view, $light, $time.
//
#include "pg/shader/Types.h"

#include <map>
#include <string>
#include <vector>

namespace pg::shader {

enum class Stage : uint8_t { Vertex, Fragment };
const char* stageName(Stage s);

struct PortDef {
    std::string name;
    Type type = Type::Float;
    Value defaultValue;          ///< used when unconnected and there is no global
    std::string defaultGlobal;   ///< "uv" for `= $uv`: unconnected means the global
    Stage stage = Stage::Fragment;  ///< only meaningful on an output node
    bool color = false;          ///< UI hint: edit as a colour
};

struct ParamDef {
    std::string name;
    bool isString = false;
    Type type = Type::Float;     ///< numeric params
    Value value;
    std::string text;            ///< string params
    bool color = false;
};

struct OutputDef {
    std::string name;
    Type type = Type::Float;
    std::string code;                             ///< neutral template
    std::map<std::string, std::string> perTarget; ///< target name -> template
};

struct UniformDef {
    std::string nameTemplate;     ///< e.g. "u_{name}"
    Type type = Type::Float;
    std::string defaultTemplate;  ///< e.g. "{value}"; empty for samplers
};

struct NodeDef {
    std::string name;
    std::string label;
    std::string category = "Other";
    std::string description;
    int version = 1;
    bool isOutput = false;
    std::vector<PortDef> inputs;
    std::vector<ParamDef> params;
    std::vector<OutputDef> outputs;
    std::vector<UniformDef> uniforms;
    std::vector<std::string> functions;  ///< helper functions the templates call
    std::string origin;                  ///< "file:line", for messages

    const PortDef* input(const std::string& n) const;
    const OutputDef* output(const std::string& n) const;
    const ParamDef* param(const std::string& n) const;
    int inputIndex(const std::string& n) const;
    int outputIndex(const std::string& n) const;
};

struct FunctionDef {
    std::string name;
    std::string code;                             ///< neutral
    std::map<std::string, std::string> perTarget; ///< target name -> code
    std::vector<std::string> uses;                ///< functions this one calls
    std::string origin;
};

/// The global inputs templates can read as $name.
struct GlobalDef {
    const char* name;
    Type type;
    const char* meaning;
};
const std::vector<GlobalDef>& globals();
const GlobalDef* findGlobal(const std::string& name);

/// A template split into literal code, {placeholders} and $globals.
struct TemplatePiece {
    enum class Kind : uint8_t { Code, Placeholder, Global } kind = Kind::Code;
    std::string text;  ///< the code, or the name without braces / dollar sign
};

/// False, with `error` set, for a malformed template (an unmatched '{', or
/// '{' / '$' not followed by a name).
bool parseTemplate(const std::string& tmpl, std::vector<TemplatePiece>& out, std::string& error);

class NodeLibrary {
public:
    /// A library holding the built-in nodes.
    static NodeLibrary withBuiltins();

    /// Adds every node and function in `text`. A definition replaces an earlier
    /// one of the same name -- that is how a user library overrides a built-in.
    /// On error nothing is added and `error` reads "origin:line: message".
    bool load(const std::string& text, const std::string& origin, std::string& error);
    bool loadFile(const std::string& path, std::string& error);

    const NodeDef* find(const std::string& name) const;
    const FunctionDef* findFunction(const std::string& name) const;

    /// All nodes, sorted by category and label -- the order menus show them in.
    std::vector<const NodeDef*> nodes() const;
    std::vector<std::string> categories() const;
    size_t size() const { return nodes_.size(); }

private:
    std::map<std::string, NodeDef> nodes_;
    std::map<std::string, FunctionDef> functions_;
};

/// The text of src/pg/shader/builtin.pgnodes, compiled into the library.
const char* builtinLibrarySource();

}  // namespace pg::shader
