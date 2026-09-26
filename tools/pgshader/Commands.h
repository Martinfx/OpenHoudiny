#pragma once
//
// The commands of pgshader: list, gen, check, render, pyro. Without a command,
// pgshader opens the editor instead (main.cpp); the editor calls checkGraphs()
// for its Validate action, so both check a graph the same way.
//
#include "pg/shader/Generator.h"

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace pg::cli {

/// list, gen, check, render, pyro.
bool isCommand(const std::string& word);
/// Runs `pgshader <command> ...`; argv[1] is the command.
int runCommand(int argc, char** argv);
/// How to use pgshader: the editor and the commands.
void printUsage(std::FILE* out);

/// The compilers `check` runs.
struct CheckTools {
    std::string glslang = "glslangValidator";
    std::string spirvVal;  ///< empty: no SPIR-V validation
};

/// True if `tool --version` runs -- the tool is installed and on the PATH.
bool toolAvailable(const std::string& tool);

struct CheckReport {
    size_t graphs = 0;
    size_t targets = 0;
    size_t compilerRuns = 0;
    size_t generationFailures = 0;              ///< (graph, target) pairs that did not generate
    std::vector<std::string> generationErrors;  ///< "graph [target]: node 3 (Mix): ..."
    std::vector<std::string> failures;          ///< a failed compiler run: what, how, the output
    std::vector<std::string> unchecked;         ///< targets without a known validator
    std::string keptDir;                        ///< the generated files, kept when something failed

    bool ok() const { return generationFailures == 0 && failures.empty(); }
    /// "checked 3 graphs x 4 targets: 24 compiler runs, 0 failed, 0 did not generate"
    std::string summary() const;
};

/// Generates every graph for every registered target and compiles the files
/// with glslangValidator -- and the SPIR-V with spirv-val -- several processes
/// at a time.
CheckReport checkGraphs(const std::vector<std::pair<std::string, shader::ShaderGraph>>& graphs,
                        const shader::NodeLibrary& library, const CheckTools& tools);

}  // namespace pg::cli
