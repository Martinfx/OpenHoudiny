//
// The commands of pgshader, for the command line and for scripts: headless,
// like the rest of the core. See Commands.h; without a command pgshader opens
// the editor (main.cpp).
//
//   pgshader list   [--markdown] [--library FILE]...
//   pgshader gen    GRAPH.pgsg... [--target NAME|all] [-o DIR] [--library FILE]...
//   pgshader check  [GRAPH.pgsg...] [--nodes | --nodes-from FILE] [--glslang PATH]
//                   [--spirv-val PATH] [--library FILE]...
//   pgshader render GRAPH.pgsg OUT.png [--mesh sphere|torus|cube|plane|billboard] [--size N]
//                   [--time SECONDS] [--yaw DEG] [--pitch DEG] [--library FILE]...
//   pgshader sim    NETWORK.pgsim|EXAMPLE OUT.png [--frames N] [--every K] [--resolution 16..256]
//                   [--size WxH] [--yaw DEG] [--pitch DEG] [--distance D] [--guides]
//                   [--set NODE.PARAM=VALUE]...
//   pgshader sim --list
//   pgshader pyro   OUT.png [--preset EXAMPLE] ...      (sim with an example; fire is the campfire)
//
// `check` is the proof that the generated code is valid: it compiles every
// graph -- and with --nodes every output of every node in the library, in the
// fragment and in the vertex stage -- for every target with glslangValidator,
// and runs spirv-val on the SPIR-V. --nodes-from checks only the nodes one
// library file defines: what the author of a library wants to know.
// `render` draws a preview with OpenGL through EGL, with no window; it exists
// only when EGL was found at build time. So does `sim`: it compiles a network
// of simulation nodes (pg::sim::Network) -- a .pgsim file, or an example the
// program carries -- simulates it and renders the last frame, or with
// --every K frames K, 2K, 3K..., each file numbered by its frame, with the
// renderer of the editor's viewport. --set changes a parameter first:
// NODE.PARAM=VALUE, or PARAM=VALUE when a single node has that parameter.
//
#include "Commands.h"

#ifdef PG_HAVE_EGL
#include "pg/gl/HeadlessContext.h"
#include "pg/gl/Png.h"
#include "pg/gl/Preview.h"
#include "pg/gl/Volume.h"
#endif
#include "pg/sim/GeometryGraph.h"
#include "pg/sim/Network.h"
#include "pg/sim/World.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <random>
#include <sstream>
#include <thread>
#include <type_traits>

namespace fs = std::filesystem;
using namespace pg::shader;

namespace pg::cli {
namespace {

struct Options {
    std::string command;
    std::vector<std::string> positional;
    std::vector<std::string> libraries;
    std::string target = "all";
    std::string outDir = ".";
    bool nodes = false;
    std::string nodesFrom;  ///< only the nodes defined in this library file
    bool markdown = false;
    std::string glslang = "glslangValidator";
    std::string spirvVal;
    std::string mesh;  ///< empty: a billboard for graphs that blend, else a sphere
    int size = 512;
    std::string sizeText;  ///< --size as given: N, or WxH for pyro
    float time = 0.0f, yaw = 30.0f, pitch = 18.0f;
    bool yawSet = false, pitchSet = false;
    // pyro
    std::string preset = "fire";
    int frames = 0;      ///< 0: as many as the network's Output says
    int every = 0;       ///< write every k-th frame; 0: only the last
    int resolution = 0;  ///< 0: the network's
    float distance = 0.0f;
    std::vector<std::string> sets;  ///< NODE.PARAM=VALUE
    bool guides = false;             ///< sim: draw the domain and the sources
    bool listExamples = false;       ///< sim --list
};

int usage() {
    printUsage(stderr);
    return 2;
}

/// A whole number that is all of `text`.
bool parseInt(const std::string& text, long long& out) {
    if (text.empty()) return false;
    char* end = nullptr;
    errno = 0;
    out = std::strtoll(text.c_str(), &end, 10);
    return errno == 0 && end && *end == '\0';
}

bool parseArgs(int argc, char** argv, Options& o) {
    if (argc < 2) return false;
    o.command = argv[1];
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](std::string& out) {
            if (i + 1 >= argc) return false;
            out = argv[++i];
            return true;
        };
        std::string v;
        auto nextInt = [&](int& out) {
            long long n = 0;
            if (!next(v) || !parseInt(v, n) || n < std::numeric_limits<int>::min() ||
                n > std::numeric_limits<int>::max()) {
                return false;
            }
            out = static_cast<int>(n);
            return true;
        };
        if (a == "--library") { if (!next(v)) return false; o.libraries.push_back(v); }
        else if (a == "--target") { if (!next(o.target)) return false; }
        else if (a == "-o") { if (!next(o.outDir)) return false; }
        else if (a == "--nodes") o.nodes = true;
        else if (a == "--nodes-from") { if (!next(o.nodesFrom)) return false; o.nodes = true; }
        else if (a == "--markdown") o.markdown = true;
        else if (a == "--glslang") { if (!next(o.glslang)) return false; }
        else if (a == "--spirv-val") { if (!next(o.spirvVal)) return false; }
        else if (a == "--mesh") { if (!next(o.mesh)) return false; }
        else if (a == "--size") { if (!next(o.sizeText)) return false; o.size = std::atoi(o.sizeText.c_str()); }
        else if (a == "--time") { if (!next(v) || !parseFloat(v, o.time)) return false; }
        else if (a == "--yaw") { if (!next(v) || !parseFloat(v, o.yaw)) return false; o.yawSet = true; }
        else if (a == "--pitch") { if (!next(v) || !parseFloat(v, o.pitch)) return false; o.pitchSet = true; }
        else if (a == "--distance") { if (!next(v) || !parseFloat(v, o.distance)) return false; }
        else if (a == "--preset") { if (!next(o.preset)) return false; }
        else if (a == "--frames") { if (!nextInt(o.frames)) return false; }
        else if (a == "--every") { if (!nextInt(o.every)) return false; }
        else if (a == "--resolution") { if (!nextInt(o.resolution)) return false; }
        else if (a == "--set") { if (!next(v)) return false; o.sets.push_back(v); }
        else if (a == "--guides") o.guides = true;
        else if (a == "--list") o.listExamples = true;
        else if (!a.empty() && a[0] == '-') return false;
        else o.positional.push_back(a);
    }
    return true;
}

bool loadLibrary(const Options& o, NodeLibrary& lib) {
    lib = NodeLibrary::withBuiltins();
    for (const auto& path : o.libraries) {
        std::string error;
        if (!lib.loadFile(path, error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return false;
        }
    }
    return true;
}

bool loadGraph(const std::string& path, ShaderGraph& g) {
    std::ifstream in(path);
    if (!in) {
        std::fprintf(stderr, "%s: cannot open\n", path.c_str());
        return false;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    std::string error;
    if (!ShaderGraph::load(ss.str(), g, error)) {
        std::fprintf(stderr, "%s: %s\n", path.c_str(), error.c_str());
        return false;
    }
    return true;
}

/// "what [target]: node 3 (Mix): message", one line per error.
std::vector<std::string> errorLines(const std::string& what, const ShaderGraph& g, const NodeLibrary& lib,
                                    const GeneratedShader& s) {
    std::vector<std::string> lines;
    for (const auto& e : s.errors) {
        std::string where = "graph";
        if (const GraphNode* n = g.node(e.node)) {
            const NodeDef* def = lib.find(n->type);
            where = "node " + std::to_string(n->id) + " (" + (def ? def->label : n->type) + ")";
        }
        lines.push_back(what + " [" + s.target + "]: " + where + ": " + e.message);
    }
    return lines;
}

void printErrors(const std::string& what, const ShaderGraph& g, const NodeLibrary& lib,
                 const GeneratedShader& s) {
    for (const auto& line : errorLines(what, g, lib, s)) std::fprintf(stderr, "%s\n", line.c_str());
}

std::vector<const Target*> selectedTargets(const Options& o) {
    if (o.target == "all") return TargetRegistry::instance().all();
    if (const Target* t = TargetRegistry::instance().find(o.target)) return {t};
    std::fprintf(stderr, "unknown target '%s'; try 'pgshader list'\n", o.target.c_str());
    return {};
}

// --- list --------------------------------------------------------------------------

std::string portList(const NodeDef& d, bool inputs) {
    std::string s;
    if (inputs) {
        for (const auto& p : d.inputs) {
            if (!s.empty()) s += ", ";
            s += p.name + ": " + typeName(p.type);
            if (!p.defaultGlobal.empty()) s += " = $" + p.defaultGlobal;
        }
    } else {
        for (const auto& o : d.outputs) {
            if (!s.empty()) s += ", ";
            s += o.name + ": " + typeName(o.type);
        }
    }
    return s.empty() ? "-" : s;
}

int list(const Options& o, const NodeLibrary& lib) {
    if (o.markdown) {
        // Library text is plain text: in a table cell '|' ends the cell and
        // '<name>' would be taken for an HTML tag.
        auto cell = [](const std::string& text) {
            std::string out;
            for (char c : text) {
                if (c == '<') out += "&lt;";
                else if (c == '>') out += "&gt;";
                else if (c == '|') out += "\\|";
                else out += c;
            }
            return out;
        };
        std::printf("| Node | Category | Inputs | Outputs | What it does |\n|---|---|---|---|---|\n");
        for (const NodeDef* d : lib.nodes()) {
            std::printf("| **%s** `%s` | %s | %s | %s | %s |\n", cell(d->label).c_str(), d->name.c_str(),
                        cell(d->category).c_str(), cell(portList(*d, true)).c_str(),
                        cell(portList(*d, false)).c_str(), cell(d->description).c_str());
        }
        return 0;
    }
    std::printf("targets\n");
    for (const Target* t : TargetRegistry::instance().all()) {
        std::printf("  %-10s %s\n", t->name().c_str(), t->description().c_str());
    }
    std::string category;
    for (const NodeDef* d : lib.nodes()) {
        if (d->category != category) {
            category = d->category;
            std::printf("\n%s\n", category.c_str());
        }
        std::printf("  %-16s %s  ->  %s\n", d->name.c_str(), portList(*d, true).c_str(),
                    portList(*d, false).c_str());
    }
    return 0;
}

// --- gen ---------------------------------------------------------------------------

std::string stemOf(const std::string& path) { return fs::path(path).stem().string(); }

int gen(const Options& o, const NodeLibrary& lib) {
    if (o.positional.empty()) return usage();
    const auto targets = selectedTargets(o);
    if (targets.empty()) return 2;
    fs::create_directories(o.outDir);
    int failures = 0;
    for (const auto& path : o.positional) {
        ShaderGraph g;
        if (!loadGraph(path, g)) return 1;
        for (const Target* t : targets) {
            const GeneratedShader s = generate(g, lib, *t);
            if (!s.ok()) {
                printErrors(path, g, lib, s);
                ++failures;
                continue;
            }
            for (const auto& f : s.files) {
                const fs::path out = fs::path(o.outDir) / outputFileName(stemOf(path), t->name(), f);
                std::ofstream(out) << f.text;
                std::printf("wrote %s\n", out.string().c_str());
            }
        }
    }
    return failures ? 1 : 0;
}

// --- check -------------------------------------------------------------------------

struct Job {
    std::string label;    ///< what is being checked
    std::string command;  ///< shell command, output captured by the runner
    std::string source;   ///< the file compiled, shown on failure
};

std::string quote(const fs::path& p) { return "\"" + p.string() + "\""; }

/// How to validate each built-in target with glslang. Targets added by a user
/// have no entry and are reported as not checked.
std::vector<Job> jobsFor(const CheckTools& tools, const std::string& label, const GeneratedShader& s,
                         const fs::path& dir) {
    std::vector<Job> jobs;
    const std::string validate =
        tools.spirvVal.empty() ? "" : " && " + tools.spirvVal + " --target-env vulkan1.0 ";
    for (const auto& f : s.files) {
        const fs::path file = dir / (s.target + f.extension);
        std::ofstream(file) << f.text;
        if (s.target == "glsl330" || s.target == "gles300") {
            jobs.push_back({label + " " + file.filename().string(), tools.glslang + " " + quote(file),
                            file.string()});
        } else if (s.target == "vulkan") {
            const fs::path spv = file.string() + ".spv";
            jobs.push_back({label + " " + file.filename().string(),
                            tools.glslang + " -V --target-env vulkan1.0 " + quote(file) + " -o " + quote(spv) +
                                (validate.empty() ? "" : validate + quote(spv)),
                            file.string()});
        } else if (s.target == "hlsl") {
            for (const auto& [stage, entry] : f.entryPoints) {
                const fs::path spv = file.string() + "." + entry + ".spv";
                jobs.push_back({label + " " + file.filename().string() + ":" + entry,
                                tools.glslang + " -D -V -e " + entry + " -S " +
                                    (stage == Stage::Vertex ? "vert " : "frag ") + quote(file) + " -o " +
                                    quote(spv) + (validate.empty() ? "" : validate + quote(spv)),
                                file.string()});
            }
        }
    }
    return jobs;
}

/// One small graph per output of every node, wired into the output node --
/// once for the fragment stage and once for the vertex stage; nodes with
/// `any` inputs also once with vec3 values in them.
std::vector<std::pair<std::string, ShaderGraph>> nodeGraphs(const NodeLibrary& lib, const std::string& from) {
    const NodeDef* output = nullptr;
    for (const NodeDef* d : lib.nodes()) {
        if (d->isOutput) output = d;
    }
    std::vector<std::pair<std::string, ShaderGraph>> graphs;
    if (!output) return graphs;
    for (const NodeDef* d : lib.nodes()) {
        if (d->isOutput) continue;
        if (!from.empty() && d->origin.rfind(from + ":", 0) != 0) continue;  // origin is "file:line"
        const bool hasAny = std::any_of(d->inputs.begin(), d->inputs.end(),
                                        [](const PortDef& p) { return p.type == Type::Any; });
        for (const auto& out : d->outputs) {
            for (const auto& root : output->inputs) {
                for (int vec = 0; vec < (hasAny ? 2 : 1); ++vec) {
                    ShaderGraph g;
                    const int n = g.addNode(d->name, 0, 0, &lib);
                    const int o = g.addNode(output->name, 200, 0, &lib);
                    if (vec) {
                        for (const auto& p : d->inputs) {
                            if (p.type == Type::Any) g.setInput(n, p.name, Value::vector(0.5f, 0.25f, 2.0f));
                        }
                    }
                    g.connect(n, out.name, o, root.name, lib);
                    graphs.emplace_back(d->name + "." + out.name + "." + stageName(root.stage) + (vec ? ".vec3" : ""),
                                        std::move(g));
                }
            }
        }
    }
    return graphs;
}

int check(const Options& o, const NodeLibrary& lib) {
    std::vector<std::pair<std::string, ShaderGraph>> graphs;
    for (const auto& path : o.positional) {
        ShaderGraph g;
        if (!loadGraph(path, g)) return 1;
        graphs.emplace_back(stemOf(path), std::move(g));
    }
    if (o.nodes) {
        auto ng = nodeGraphs(lib, o.nodesFrom);
        if (ng.empty() && !o.nodesFrom.empty()) {
            std::fprintf(stderr, "pgshader: no nodes from '%s' -- load it with --library, spelled the same\n",
                         o.nodesFrom.c_str());
            return 1;
        }
        for (auto& g : ng) graphs.push_back(std::move(g));
    }
    if (graphs.empty()) return usage();
    for (const std::string& tool : {o.glslang, o.spirvVal}) {
        if (!tool.empty() && !toolAvailable(tool)) {
            std::fprintf(stderr, "pgshader: cannot run '%s' -- is it installed (glslang-tools, spirv-tools)?\n",
                         tool.c_str());
            return 1;
        }
    }

    const CheckReport r = checkGraphs(graphs, lib, CheckTools{o.glslang, o.spirvVal});
    for (const auto& e : r.generationErrors) std::fprintf(stderr, "%s\n", e.c_str());
    for (const auto& f : r.failures) std::fprintf(stderr, "FAIL %s\n", f.c_str());
    std::printf("%s\n", r.summary().c_str());
    for (const auto& t : r.unchecked) std::printf("  (no validator known for target '%s')\n", t.c_str());
    if (!r.keptDir.empty()) std::printf("generated files kept in %s\n", r.keptDir.c_str());
    return r.ok() ? 0 : 1;
}

// --- render ------------------------------------------------------------------------

int render(const Options& o, const NodeLibrary& lib) {
    if (o.positional.size() != 2) return usage();
#ifdef PG_HAVE_EGL
    ShaderGraph g;
    if (!loadGraph(o.positional[0], g)) return 1;
    const Target* t = TargetRegistry::instance().find("glsl330");
    const GeneratedShader s = generate(g, lib, *t);
    if (!s.ok()) {
        printErrors(o.positional[0], g, lib, s);
        return 1;
    }

    pg::gl::HeadlessContext context;
    std::string error;
    if (!context.create(error)) {
        std::fprintf(stderr, "render: %s\n", error.c_str());
        return 1;
    }
    pg::gl::Api gl;
    if (!gl.load(pg::gl::HeadlessContext::procAddress, error)) {
        std::fprintf(stderr, "render: OpenGL function %s is missing\n", error.c_str());
        return 1;
    }
    pg::gl::PreviewRenderer preview(gl);
    std::string log;
    if (!preview.setProgram(s.fileFor(Stage::Vertex)->text, s.fileFor(Stage::Fragment)->text, log)) {
        std::fprintf(stderr, "render: the driver rejected the shader:\n%s\n", log.c_str());
        return 1;
    }
    preview.setUniforms(s.uniforms);
    preview.setBlend(s.blend);
    const std::string mesh = !o.mesh.empty() ? o.mesh : s.blend != BlendMode::Opaque ? "billboard" : "sphere";
    bool found = false;
    for (pg::gl::MeshKind k : pg::gl::kMeshKinds) {
        if (mesh != pg::gl::meshName(k)) continue;
        preview.setMesh(k);
        found = true;
    }
    if (!found) {
        std::fprintf(stderr, "render: no mesh '%s' (sphere, torus, cube, plane, billboard)\n", mesh.c_str());
        return 1;
    }
    preview.orbit.yaw = o.yaw;
    preview.orbit.pitch = o.pitch;
    const int size = std::clamp(o.size, 16, 4096);
    preview.render(size * 2, size * 2, o.time);  // 2x, averaged down: anti-aliasing
    if (!pg::gl::writePng(o.positional[1], size, size, 3, preview.readPixels(2))) {
        std::fprintf(stderr, "render: cannot write %s\n", o.positional[1].c_str());
        return 1;
    }
    std::printf("wrote %s (%s, %s, %s)\n", o.positional[1].c_str(), pg::gl::meshName(preview.mesh()),
                blendModeName(s.blend), reinterpret_cast<const char*>(gl.GetString(pg::gl::RENDERER)));
    return 0;
#else
    (void)lib;
    std::fprintf(stderr, "render: this pgshader was built without EGL\n");
    return 1;
#endif
}

/// Where a network comes from: a file, or one of the examples compiled in.
/// `folder`: where its relative paths (meshes) are read from.
bool loadNetwork(const std::string& what, pg::sim::Network& net, std::string& error, std::string& folder) {
    std::error_code ec;
    if (fs::is_regular_file(what, ec)) {
        folder = fs::path(what).parent_path().string();
        std::ifstream in(what, std::ios::binary);
        std::stringstream text;
        text << in.rdbuf();
        std::vector<std::string> warnings;
        if (!pg::sim::Network::load(text.str(), net, error, &warnings)) {
            error = what + ": " + error;
            return false;
        }
        for (const std::string& w : warnings) std::fprintf(stderr, "sim: %s: %s\n", what.c_str(), w.c_str());
        return true;
    }
    if (pg::sim::Network::example(what, net)) {
        folder = PG_SIM_EXAMPLES_DIR;
        return true;
    }
    error = "no file or example '" + what + "'; the examples are:";
    for (const std::string& name : pg::sim::Network::exampleNames()) error += " " + name;
    return false;
}

/// NODE.PARAM=VALUE onto the network -- or PARAM=VALUE, when only one node
/// has a parameter of that name. False, with why, if it does not fit.
bool applySetting(const std::string& assignment, pg::sim::Network& net, std::string& error) {
    const size_t eq = assignment.find('=');
    if (eq == std::string::npos || eq == 0) {
        error = "--set wants NODE.PARAM=VALUE, not '" + assignment + "'";
        return false;
    }
    const std::string name = assignment.substr(0, eq), value = assignment.substr(eq + 1);
    const size_t dot = name.find('.');
    int node = 0;
    std::string param = name;
    if (dot != std::string::npos) {
        const pg::sim::Node* n = net.named(name.substr(0, dot));
        if (!n) {
            error = "no node '" + name.substr(0, dot) + "'; there are";
            for (const pg::sim::Node& m : net.nodes()) error += " " + m.name;
            return false;
        }
        node = n->id;
        param = name.substr(dot + 1);
    } else {
        std::vector<std::string> owners;
        for (const pg::sim::Node& n : net.nodes()) {
            const pg::sim::NodeType* t = pg::sim::findNodeType(n.type);
            if (t && t->param(param)) {
                node = n.id;
                owners.push_back(n.name);
            }
        }
        if (owners.size() != 1) {
            error = owners.empty() ? "no node has a parameter '" + param + "'"
                                   : "'" + param + "' is a parameter of several nodes; say which:";
            for (const std::string& o : owners) error += " " + o + "." + param;
            return false;
        }
    }
    std::string why;
    if (!net.setParam(node, param, value, &why)) {
        error = name + ": " + why;
        return false;
    }
    return true;
}

#ifdef PG_HAVE_EGL
/// fire.png, 30 -> fire_0030.png
std::string numbered(const std::string& path, int frame) {
    char digits[16];
    std::snprintf(digits, sizeof digits, "_%04d", frame);
    const fs::path p(path);
    return (p.parent_path() / (p.stem().string() + digits + p.extension().string())).string();
}
#endif

/// `sim NETWORK OUT.png`, and `pyro OUT.png --preset NAME`: the same, with an example.
int simulate(const Options& o, const std::string& network, const std::string& outPath) {
    const char* cmd = o.command.c_str();
    if (o.frames < 0 || o.every < 0) return usage();
    if (o.resolution != 0 && (o.resolution < 16 || o.resolution > 256)) {
        std::fprintf(stderr, "%s: --resolution wants 16 to 256 cells along the longest side, not %d\n", cmd,
                     o.resolution);
        return 1;
    }
    namespace sim = pg::sim;
    sim::Network net;
    std::string error, folder;
    if (!loadNetwork(network, net, error, folder)) {
        std::fprintf(stderr, "%s: %s\n", cmd, error.c_str());
        return 1;
    }
    for (const std::string& assignment : o.sets) {
        if (!applySetting(assignment, net, error)) {
            std::fprintf(stderr, "%s: %s\n", cmd, error.c_str());
            return 1;
        }
    }
    // The geometry nodes cook here: shapes for the simulations, and the
    // displayed node's geometry for the pictures -- from the frame just
    // simulated, for the nodes that bring a simulation back.
    sim::GeometryGraph geometry;
    std::shared_ptr<const sim::Frame> current;
    geometry.setFrames([&](int f) { return current && current->number == f ? current : nullptr; });
    sim::Compiled c = net.compile(folder, &geometry);
    geometry.sync(net, folder);
    // Geometry alone -- nothing simulated -- is drawn all the same.
    const bool geometryOnly = !c.ok && c.display != 0;
    for (const sim::Problem& p : c.problems) {
        const sim::Node* n = net.node(p.node);
        if (geometryOnly && p.level == sim::Problem::Level::Error) continue;
        std::fprintf(stderr, "%s: %s%s%s%s\n", cmd, p.level == sim::Problem::Level::Error ? "error: " : "warning: ",
                     n ? n->name.c_str() : "", n ? ": " : "", p.message.c_str());
    }
    if (!c.ok && !geometryOnly) return 1;
    if (geometryOnly) c.frames = 1;
    if (o.resolution > 0) {
        c.world.gas.solver.resolution = o.resolution;
        c.world.water.solver.resolution = o.resolution;
    }
    const int frames = o.frames > 0 ? o.frames : c.frames;
    if (o.every > frames) {
        std::fprintf(stderr, "%s: --every %d is more than the %d frames: no frame would be written\n", cmd, o.every,
                     frames);
        return 1;
    }
#ifdef PG_HAVE_EGL
    namespace gl = pg::gl;
    // Through the network's camera, at the size of its picture -- unless
    // the command line asks for a view round the scene. Without a camera:
    // tall for a plume, wide for a scene wider than it is high.
    const bool throughCamera = c.hasCamera && !(o.yawSet || o.pitchSet || o.distance > 0.0f);
    const Vec3 extent = gl::sceneDomain(c.world).size();
    int width = extent.y >= std::max(extent.x, extent.z) ? 400 : 640;
    int height = extent.y >= std::max(extent.x, extent.z) ? 600 : 400;
    if (throughCamera) {
        width = c.camera.width;
        height = c.camera.height;
    }
    if (!o.sizeText.empty()) {
        const size_t x = o.sizeText.find('x');
        width = std::atoi(o.sizeText.c_str());
        height = x == std::string::npos ? width * 3 / 2 : std::atoi(o.sizeText.c_str() + x + 1);
    }
    width = std::clamp(width, 16, 4096);
    height = std::clamp(height, 16, 4096);

    gl::HeadlessContext context;
    if (!context.create(error)) {
        std::fprintf(stderr, "%s: %s\n", cmd, error.c_str());
        return 1;
    }
    gl::Api api;
    if (!api.load(gl::HeadlessContext::procAddress, error)) {
        std::fprintf(stderr, "%s: OpenGL function %s is missing\n", cmd, error.c_str());
        return 1;
    }
    gl::VolumeRenderer volume(api);
    std::string log;
    if (!volume.init(log)) {
        std::fprintf(stderr, "%s: the driver rejected the volume shader:\n%s\n", cmd, log.c_str());
        return 1;
    }
    sim::WorldSolver solver(c.world);
    const sim::World& world = solver.world();
    const sim::Domain domain = world.hasGas ? world.gas.solver.domain() : world.water.solver.domain();
    volume.look = c.look;
    volume.setDomain(domain);
    volume.setSolids(c.solids);
    if (o.guides) {
        volume.setLines(gl::sceneGuides(&world, c.solids, {}, c.solver, c.liquidSolver, c.rain,
                                        c.hasCamera && !throughCamera ? &c.camera : nullptr));
    }
    sim::Domain box = gl::sceneDomain(world);
    // Geometry alone: the view frames it.
    bool framed = false;
    Vec3 middle;
    if (geometryOnly) {
        Vec3 lo, hi;
        volume.setGeometry(geometry.cook(c.display, 1, world.timeStep));
        if (volume.geometryBounds(lo, hi)) {
            const Vec3 size = hi - lo;
            box = sim::Domain::ofBox(Vec3(std::max(size.x, 0.1f), std::max(size.y, 0.1f), std::max(size.z, 0.1f)), 16);
            middle = (lo + hi) * 0.5f;
            framed = true;
        }
    }
    if (throughCamera) {
        const Vec3 middle = box.origin() + box.size() * 0.5f;
        volume.orbit = gl::orbitThrough(c.camera, std::max(dot(middle - c.camera.position, c.camera.forward()), 0.5f));
    } else {
        volume.orbit = gl::VolumeRenderer::viewOf(box);
        if (framed) {
            volume.orbit.target[0] = middle.x;
            volume.orbit.target[1] = middle.y;
            volume.orbit.target[2] = middle.z;
        }
        if (o.yawSet) volume.orbit.yaw = o.yaw;
        if (o.pitchSet) volume.orbit.pitch = o.pitch;
        if (o.distance > 0.0f) volume.orbit.distance = o.distance;
    }

    using Clock = std::chrono::steady_clock;
    auto ms = [](Clock::time_point since) {
        return std::chrono::duration<double, std::milli>(Clock::now() - since).count();
    };
    double simulating = 0.0, rendering = 0.0;
    int images = 0;
    std::string last;
    for (int f = 1; f <= frames; ++f) {
        auto t = Clock::now();
        solver.step();
        simulating += ms(t);
        if (o.every > 0 ? f % o.every != 0 : f != frames) continue;
        t = Clock::now();
        current = std::make_shared<const sim::Frame>(solver.capture());
        if (!geometryOnly) volume.setFrame(*current);
        // Animated: the look, the objects and the camera of this frame.
        if (!c.poses.empty()) {
            volume.look = c.lookAt(f);
            volume.setSolids(c.solidsAt(f));
            if (throughCamera) {
                const sim::Camera& cam = c.cameraAt(f);
                const Vec3 middle = box.origin() + box.size() * 0.5f;
                volume.orbit = gl::orbitThrough(cam, std::max(dot(middle - cam.position, cam.forward()), 0.5f));
            }
        }
        if (c.display) {
            volume.setGeometry(geometry.cook(c.display, f, world.timeStep));
            const std::string why = geometry.error(c.display);
            if (!why.empty()) std::fprintf(stderr, "%s: %s: %s\n", cmd, net.node(c.display)->name.c_str(), why.c_str());
        }
        volume.render(width * 2, height * 2);  // 2x, averaged down: anti-aliasing
        const std::vector<uint8_t> pixels = volume.readPixels(2);
        rendering += ms(t);
        last = o.every > 0 ? numbered(outPath, f) : outPath;
        if (!gl::writePng(last, width, height, 3, pixels)) {
            std::fprintf(stderr, "%s: cannot write %s\n", cmd, last.c_str());
            return 1;
        }
        ++images;
    }
    std::string what;
    if (world.hasGas) {
        const sim::Domain& d = solver.gas()->domain();
        what += ", gas " + std::to_string(d.cells[0]) + " x " + std::to_string(d.cells[1]) + " x " +
                std::to_string(d.cells[2]) + " cells";
    }
    if (world.hasWater) {
        const sim::Domain& d = solver.water()->domain();
        what += ", water " + std::to_string(d.cells[0]) + " x " + std::to_string(d.cells[1]) + " x " +
                std::to_string(d.cells[2]) + " cells, " + std::to_string(solver.water()->particleCount()) + " particles";
    }
    if (world.hasRain) {
        what += ", rain " + std::to_string(solver.rain()->drops().size()) + " drops, " +
                std::to_string(solver.rain()->droplets().size()) + " droplets";
    }
    if (throughCamera) {
        const sim::Node* n = net.node(c.camera.node);
        what += ", through " + (n ? n->name : std::string("the camera"));
    }
    std::printf("wrote %s%s: %s%s, %d frames (%.1f s); simulation %.1f ms/frame, rendering %.0f ms/image (%s)\n",
                last.c_str(), images > 1 ? (" and " + std::to_string(images - 1) + " before it").c_str() : "",
                network.c_str(), what.c_str(), frames, solver.time(), simulating / frames, rendering / std::max(images, 1),
                reinterpret_cast<const char*>(api.GetString(gl::RENDERER)));
    return 0;
#else
    (void)outPath;
    std::fprintf(stderr, "%s: this pgshader was built without EGL\n", cmd);
    return 1;
#endif
}

int simCommand(const Options& o) {
    if (o.listExamples) {
        for (const std::string& name : pg::sim::Network::exampleNames()) std::printf("%s\n", name.c_str());
        return 0;
    }
    if (o.positional.size() != 2) return usage();
    return simulate(o, o.positional[0], o.positional[1]);
}

/// The command of the earlier versions: an example by --preset.
int pyro(const Options& o) {
    if (o.positional.size() != 1) return usage();
    const std::string example = o.preset == "fire" ? "campfire" : o.preset;
    return simulate(o, example, o.positional[0]);
}

}  // namespace

bool isCommand(const std::string& word) {
    return word == "list" || word == "gen" || word == "check" || word == "render" || word == "sim" ||
           word == "pyro";
}

void printUsage(std::FILE* out) {
    std::fprintf(out,
                 "usage:\n"
#ifdef PG_HAVE_GUI
                 "  pgshader [NETWORK.pgsim | GRAPH.pgsg] [--example NAME] [--shaders] [--select NODE]\n"
                 "           [--library FILE]... [--target NAME] [--mesh NAME] [--size WxH]\n"
                 "           [--screenshot OUT.png [--frames N]] [--script FILE]\n"
                 "                  the node editor -- what runs without a command: smoke and fire\n"
                 "                  from nodes (the campfire example), shaders with --shaders\n"
#else
                 "  pgshader [NETWORK.pgsim | GRAPH.pgsg]   the node editor -- not in this build (PG_BUILD_GUI=OFF)\n"
#endif
                 "  pgshader list   [--markdown] [--library FILE]...\n"
                 "  pgshader gen    GRAPH.pgsg... [--target NAME|all] [-o DIR] [--library FILE]...\n"
                 "  pgshader check  [GRAPH.pgsg...] [--nodes | --nodes-from FILE] [--glslang PATH]\n"
                 "                  [--spirv-val PATH] [--library FILE]...\n"
                 "  pgshader render GRAPH.pgsg OUT.png [--mesh sphere|torus|cube|plane|billboard] [--size N]\n"
                 "                  [--time SECONDS] [--yaw DEG] [--pitch DEG] [--library FILE]...\n"
                 "  pgshader sim    NETWORK.pgsim|EXAMPLE OUT.png [--frames N] [--every K] [--resolution 16..256]\n"
                 "                  [--size WxH] [--yaw DEG] [--pitch DEG] [--distance D] [--guides]\n"
                 "                  [--set NODE.PARAM=VALUE]...\n"
                 "                  simulates a network of nodes and renders its last frame; --every K renders\n"
                 "                  frames K, 2K, 3K... as OUT_<frame>.png (K = 2: OUT_0002.png, OUT_0004.png...);\n"
                 "                  through the network's camera at its size, unless --yaw, --pitch or --distance\n"
                 "                  ask for a view round the scene\n"
                 "  pgshader sim --list    the examples it carries: campfire, smoke, ...\n"
                 "  pgshader pyro   OUT.png [--preset EXAMPLE] [...]   sim with an example (fire: campfire)\n"
                 "  pgshader help\n");
}

bool toolAvailable(const std::string& tool) {
#ifdef _WIN32
    const char* sink = " > NUL 2>&1";
#else
    const char* sink = " > /dev/null 2>&1";
#endif
    return std::system(("\"" + tool + "\" --version" + sink).c_str()) == 0;
}

std::string CheckReport::summary() const {
    return "checked " + std::to_string(graphs) + (graphs == 1 ? " graph" : " graphs") + " x " +
           std::to_string(targets) + " targets: " + std::to_string(compilerRuns) + " compiler runs, " +
           std::to_string(failures.size()) + " failed, " + std::to_string(generationFailures) +
           " did not generate";
}

CheckReport checkGraphs(const std::vector<std::pair<std::string, ShaderGraph>>& graphs,
                        const NodeLibrary& library, const CheckTools& tools) {
    CheckReport r;
    r.graphs = graphs.size();
    r.targets = TargetRegistry::instance().all().size();
    // A directory of its own: checks may run side by side (CTest -j, the editor).
    const fs::path root =
        fs::temp_directory_path() / ("pgshader-check-" + std::to_string(std::random_device{}()) + "-" +
                                     std::to_string(std::random_device{}()));
    std::vector<Job> jobs;
    for (const auto& [name, g] : graphs) {
        for (const Target* t : TargetRegistry::instance().all()) {
            const GeneratedShader s = generate(g, library, *t);
            if (!s.ok()) {
                for (auto& line : errorLines(name, g, library, s)) r.generationErrors.push_back(std::move(line));
                ++r.generationFailures;
                continue;
            }
            const fs::path dir = root / name;
            fs::create_directories(dir);
            auto js = jobsFor(tools, name, s, dir);
            if (js.empty() && std::find(r.unchecked.begin(), r.unchecked.end(), t->name()) == r.unchecked.end()) {
                r.unchecked.push_back(t->name());
            }
            jobs.insert(jobs.end(), js.begin(), js.end());
        }
    }
    r.compilerRuns = jobs.size();

    // Each job is its own process: run as many at once as there are cores.
    std::atomic<size_t> next{0};
    std::mutex mu;
    auto worker = [&](unsigned id) {
        const fs::path log = root / ("log" + std::to_string(id) + ".txt");
        for (size_t i = next++; i < jobs.size(); i = next++) {
            // Grouped, so the redirect covers every command of an `a && b` chain.
            const int rc = std::system(("(" + jobs[i].command + ") > " + quote(log) + " 2>&1").c_str());
            if (rc == 0) continue;
            std::ifstream in(log);
            std::stringstream ss;
            ss << in.rdbuf();
            std::lock_guard<std::mutex> lk(mu);
            r.failures.push_back(jobs[i].label + "\n  " + jobs[i].command + "\n" + ss.str() + "  source: " +
                                 jobs[i].source);
        }
    };
    fs::create_directories(root);
    const unsigned n = std::max(1u, std::thread::hardware_concurrency());
    std::vector<std::thread> threads;
    for (unsigned i = 0; i < n; ++i) threads.emplace_back(worker, i);
    for (auto& t : threads) t.join();
    std::sort(r.failures.begin(), r.failures.end());

    std::error_code ec;
    if (r.ok()) fs::remove_all(root, ec);
    else r.keptDir = root.string();
    return r;
}

int runCommand(int argc, char** argv) {
    Options o;
    if (!parseArgs(argc, argv, o) || !isCommand(o.command)) return usage();
    NodeLibrary lib;
    if (!loadLibrary(o, lib)) return 1;
    if (o.command == "list") return list(o, lib);
    if (o.command == "gen") return gen(o, lib);
    if (o.command == "check") return check(o, lib);
    if (o.command == "sim") return simCommand(o);
    if (o.command == "pyro") return pyro(o);
    return render(o, lib);
}

}  // namespace pg::cli
