#pragma once
//
// Compiles a shader graph into source code for one target.
//
//  1. Find the output node. Its inputs are the roots of the two stages: the
//     colour is computed per pixel, the offset per vertex.
//  2. Walk upstream from each root. Only nodes that reach the output are
//     compiled; anything else in the graph costs nothing.
//  3. Resolve types in dependency order: an `any` port becomes the widest type
//     connected to it, and every link converts to the type its input wants.
//  4. Expand each node's templates into statements, one variable per output:
//     `vec3 n4_color = mix(...);`. Names come from node ids, so the same graph
//     always produces the same text.
//  5. Hand the result to the Target, which writes declarations and entry points.
//
#include "pg/shader/ShaderGraph.h"
#include "pg/shader/Target.h"

#include <string>
#include <vector>

namespace pg::shader {

struct Diagnostic {
    int node = -1;  ///< graph node id, or -1 for the graph as a whole
    std::string message;
};

struct GeneratedShader {
    std::string target;
    std::vector<ShaderFile> files;      ///< empty when there are errors
    std::vector<UniformInfo> uniforms;  ///< the graph's own uniforms, sorted by name
    std::vector<Diagnostic> errors;

    bool ok() const { return errors.empty(); }
    /// The file holding the entry point of `stage`, or nullptr.
    const ShaderFile* fileFor(Stage stage) const;
    /// The entry point of `stage` ("main", "vs_main", ...).
    std::string entryPoint(Stage stage) const;
};

GeneratedShader generate(const ShaderGraph& graph, const NodeLibrary& library, const Target& target);

/// The name to save a generated file under: "marble.vulkan.frag", or just
/// "marble.hlsl" when the extension already names the target.
std::string outputFileName(const std::string& stem, const std::string& target, const ShaderFile& file);

}  // namespace pg::shader
