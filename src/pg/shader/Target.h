#pragma once
//
// Code generation targets: one class per shading language / graphics API.
//
// The generator works out WHAT the shader computes -- which nodes, in which
// order, with which types -- without committing to a language. A Target
// decides HOW it is written: type names, built-in function names, how uniforms
// and stage inputs are declared, what the entry points look like.
//
// Adding a language is one class: derive from Target (or from an existing one),
// override what differs, and register it:
//
//   TargetRegistry::instance().add(std::make_unique<MyMetalTarget>());
//
// Every node of every library then works with it, because node templates are
// written once, in the neutral dialect (GLSL syntax), and translate() renames
// what differs. A node that needs more than renaming gives the target its own
// template with `impl <target> <output> = ...` -- no C++ change.
//
#include "pg/shader/NodeLibrary.h"

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace pg::shader {

/// A uniform declared by a node -- a value the application sets.
struct UniformInfo {
    std::string name;
    Type type = Type::Float;
    Value defaultValue;      ///< numeric uniforms
    int binding = -1;        ///< samplers: 0, 1, ... in name order
};

/// One line of generated code: `type name = expression;`
struct Statement {
    Type type = Type::Float;
    std::string name;
    std::string expression;  ///< already in the target's language
};

/// What one stage computes, in dependency order, ready to be wrapped.
struct StageBody {
    std::vector<std::string> globals;    ///< globals the nodes read: "uv", "view", ...
    std::vector<std::string> uniforms;   ///< graph uniforms this stage reads
    std::vector<std::string> functions;  ///< helper functions, translated, callees first
    std::vector<Statement> statements;
    std::string result;  ///< fragment: the vec4 colour; vertex: the vec3 offset, or empty
};

/// Everything a target needs to write the final files.
struct Assembly {
    std::vector<UniformInfo> uniforms;  ///< the graph's uniforms, sorted by name
    StageBody vertex, fragment;
    /// Globals the fragment stage reads that must come from the vertex stage:
    /// a subset of "position", "normal", "uv", in that order.
    std::vector<std::string> varyings;

    bool fragmentUses(const std::string& global) const;
    bool vertexUses(const std::string& global) const;
    bool hasVarying(const std::string& v) const;
};

struct ShaderFile {
    std::string extension;  ///< ".vert", ".frag", ".hlsl" ...
    std::string text;
    std::vector<std::pair<Stage, std::string>> entryPoints;  ///< stage -> entry function
};

class Target {
public:
    virtual ~Target() = default;

    virtual std::string name() const = 0;         ///< "glsl330" -- used on the command line
    virtual std::string description() const = 0;  ///< "OpenGL 3.3 core (GLSL 330)"

    // --- expressions ---------------------------------------------------------

    virtual std::string typeName(Type t) const;
    /// Rewrites identifiers of neutral (GLSL) code into this language. The
    /// default leaves the code as it is.
    virtual std::string translate(const std::string& neutralCode) const;
    /// `vec3(a, b, c)`
    virtual std::string construct(Type t, const std::vector<std::string>& args) const;
    /// A scalar used as a vector: `vec3(x)`.
    virtual std::string splat(Type t, const std::string& scalar) const;

    /// A value converted between numeric types: a scalar is splatted, a vector
    /// truncated with a swizzle, missing components are 0 -- except w, which is 1.
    std::string convert(const std::string& expr, Type from, Type to) const;
    /// A constant of type `t`.
    std::string literal(const Value& v, Type t) const;

    // --- files ------------------------------------------------------------

    virtual std::vector<ShaderFile> assemble(const Assembly& a) const = 0;
};

/// Targets by name. The built-in ones register themselves; add your own.
class TargetRegistry {
public:
    static TargetRegistry& instance();

    /// Replaces a target of the same name.
    void add(std::unique_ptr<Target> target);
    const Target* find(const std::string& name) const;
    std::vector<const Target*> all() const;

private:
    TargetRegistry();
    std::vector<std::unique_ptr<Target>> targets_;
};

std::unique_ptr<Target> makeGlsl330Target();  ///< OpenGL 3.3 core
std::unique_ptr<Target> makeGles300Target();  ///< OpenGL ES 3.0 / WebGL 2
std::unique_ptr<Target> makeVulkanTarget();   ///< Vulkan: GLSL 450 for SPIR-V
std::unique_ptr<Target> makeHlslTarget();     ///< Direct3D 11/12: HLSL, shader model 5

/// Renames whole identifiers in `code`. Leaves numbers, comments and members
/// after '.' alone, so `v.mix` or `1e-5` are safe.
std::string renameIdentifiers(const std::string& code,
                              const std::map<std::string, std::string>& renames);

/// The uniforms every generated shader may use, set by the renderer:
/// u_model, u_viewProj (mat4), u_cameraPos, u_lightDir (vec3), u_time (float).
/// Graph uniforms must not reuse these names.
bool isBuiltinUniform(const std::string& name);

}  // namespace pg::shader
