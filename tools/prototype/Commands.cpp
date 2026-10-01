//
// The commands of prototype, for the command line and for scripts: headless,
// like the rest of the core. See Commands.h; without a command prototype opens
// the editor (main.cpp).
//
//   prototype list   [--markdown] [--library FILE]...
//   prototype gen    GRAPH.pgsg... [--target NAME|all] [-o DIR] [--library FILE]...
//   prototype check  [GRAPH.pgsg...] [--nodes | --nodes-from FILE] [--glslang PATH]
//                    [--spirv-val PATH] [--library FILE]...
//   prototype render GRAPH.pgsg OUT.png|OUT.mp4 [--mesh sphere|torus|cube|plane|billboard] [--size N]
//                    [--time SECONDS] [--frames N] [--yaw DEG] [--pitch DEG] [--library FILE]...
//   prototype sim    NETWORK.pgsim|EXAMPLE OUT.png|OUT.exr|OUT.mp4|- [--frames N] [--start N] [--every K] [--resolution 16..1024]
//                    [--size WxH] [--yaw DEG] [--pitch DEG] [--distance D] [--guides]
//                    [--set NODE.PARAM=VALUE]... [--cache DIR [--checkpoint K] [--resume]] [--from-cache DIR]
//                    [--export PATH] [--export-node NODE] [--preview F] [--renderer gl|path [--samples N]]
//   prototype sim --list
//   prototype pyro   OUT.png [--preset EXAMPLE] ...      (sim with an example; fire is the campfire)
//   prototype cook   NETWORK.pgsim|EXAMPLE OUT.obj|OUT.ply|OUT.vdb|- [--node NODE] [--set NODE.PARAM=VALUE]...
//                    [--frame N] [--frames N [--start N]] [--threads N] [--hash]
//
// `check` is the proof that the generated code is valid: it compiles every
// graph -- and with --nodes every output of every node in the library, in the
// fragment and in the vertex stage -- for every target with glslangValidator,
// and runs spirv-val on the SPIR-V. --nodes-from checks only the nodes one
// library file defines: what the author of a library wants to know.
// `render` draws a preview with OpenGL and no window (Offscreen.h: EGL, or a
// hidden window in the builds with the editor); to a video (.mp4, .avi...:
// pg/io/Video.h), --frames of it animated. So do the pictures of `sim`: it
// compiles a network of simulation nodes (pg::sim::Network) -- a .pgsim file,
// or an example the program carries -- simulates it and renders the last
// frame, or with --every K frames K, 2K, 3K..., each file numbered by its
// frame, or every frame into a video, with the renderer of the editor's
// viewport; --start S draws and exports from frame S on -- a farm machine's
// share of a shot read from a cache. OUT.exr: the pictures in linear light
// with their passes for compositing -- depth, motion vectors, masks
// (gl::writePassesExr). --renderer path renders with the path tracer
// instead (pg/render): light followed as it bounces, on the processor, no
// OpenGL wanted -- as the Output's Render section sets it, --samples N a
// pixel; its EXR: the light, Z, albedo.* and N.*. --set changes a
// parameter first: NODE.PARAM=VALUE, or PARAM=VALUE when a single node has
// that parameter; a VALUE that is not a value is an expression ($F, ch()),
// and NODE.PARAM.x=... sets one component of a vector. --cache DIR writes every frame to a folder, and
// --from-cache DIR reads them from one instead of simulating (pg/sim/Cache.h);
// its cache.txt says how far it has got after every frame. --checkpoint K
// saves the whole state of the simulation there every K frames, and
// --resume goes on from it -- a bake cut short does not start again (the
// editor's Bake to Disk runs this, in a process of its own). --preview F
// simulates the gas and the water on grids F as fine (sim::preview);
// --export PATH writes the displayed geometry of every frame -- or that of
// --export-node -- to .ply, .obj, .vdb or .usda files, $F4 in PATH the
// frame (pg/io/Export.h); a .usda without $F is the whole shot as one USD
// stage -- the geometry, the pieces, the grit, the water's surface, the
// rain, the gas (VDB files beside it), the camera and the light; what
// changes every frame in a layer a frame beside it (pg/sim/UsdExport.h).
// '-' for OUT.png draws nothing: the cache and the export alone, which need
// no OpenGL.
//
// `cook` cooks a network's geometry and nothing else -- no simulation, no
// OpenGL: the displayed node, or --node, at frame 1, --frame N, or frames 1
// (--start S: S) to --frames N; written to OUT by its extension ($F4 in OUT
// for the frame),
// or '-' for nothing. It says what it made, how long it took and, with
// --hash, the geometry's content hash -- the same on any number of threads
// (--threads N), which is how the determinism of a network is checked.
//
#include "Commands.h"

#include "pg/lang/Lang.h"
#include "Offscreen.h"

#ifdef PG_CAN_RENDER
#include "pg/gl/Png.h"
#include "pg/gl/Preview.h"
#include "pg/gl/Volume.h"
#endif
#include "pg/core/Instances.h"
#include "pg/core/Parallel.h"
#include "pg/io/Exr.h"
#include "pg/io/Export.h"
#include "pg/io/Picture.h"
#include "pg/io/Video.h"
#include "pg/render/PathTracer.h"
#include "pg/render/Save.h"
#include "pg/sim/Cache.h"
#include "pg/sim/GeometryGraph.h"
#include "pg/sim/Network.h"
#include "pg/sim/UsdExport.h"
#include "pg/sim/WaterMesh.h"
#include "pg/sim/World.h"
#include "pg/usd/Geom.h"
#include "pg/usd/Stage.h"

#include <algorithm>
#include <atomic>
#include <cctype>
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
    int frames = 0;      ///< 0: as many as the network's Output says; --end is the same
    int start = 0;       ///< --start: the first frame written; 0: frame 1
    int every = 0;       ///< write every k-th frame; 0: only the last
    int resolution = 0;  ///< 0: the network's
    float distance = 0.0f;
    std::vector<std::string> sets;  ///< NODE.PARAM=VALUE
    bool guides = false;             ///< sim: draw the domain and the sources
    bool listExamples = false;       ///< sim --list
    std::string cacheDir;            ///< sim --cache: write every frame there
    int checkpoint = 0;              ///< sim --checkpoint K: the state into the cache every K frames; 0: never
    bool resume = false;             ///< sim --resume: go on from the cache's checkpoint
    float preview = 1.0f;            ///< sim --preview F: the grids F as fine
    std::string fromCache;           ///< sim --from-cache: read the frames from there
    std::string exportPattern;       ///< sim --export: the displayed geometry of every frame, to files
    std::string exportNode;          ///< sim --export-node: that node's rather than the displayed one's
    std::string renderer = "gl";     ///< sim --renderer: gl, the viewport's; path, the path tracer
    int samples = 0;                 ///< sim --samples: a pixel, for the path tracer; 0: the Output's
    // cook
    std::string node;   ///< --node: which node's geometry, rather than the displayed one's
    int frame = 0;      ///< --frame: the one frame cooked; 0: frame 1
    int threads = 0;    ///< --threads: how many; 0: all there are
    bool hash = false;  ///< --hash: print the geometry's content hash
    /// --folder: where the network's relative paths (meshes) are read from,
    /// rather than beside its file -- for a network saved elsewhere (Python).
    std::string folder;
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
        else if (a == "--frames" || a == "--end") { if (!nextInt(o.frames)) return false; }
        else if (a == "--start") { if (!nextInt(o.start)) return false; }
        else if (a == "--every") { if (!nextInt(o.every)) return false; }
        else if (a == "--resolution") { if (!nextInt(o.resolution)) return false; }
        else if (a == "--set") { if (!next(v)) return false; o.sets.push_back(v); }
        else if (a == "--guides") o.guides = true;
        else if (a == "--cache") { if (!next(o.cacheDir)) return false; }
        else if (a == "--checkpoint") { if (!nextInt(o.checkpoint)) return false; }
        else if (a == "--resume") o.resume = true;
        else if (a == "--preview") { if (!next(v) || !parseFloat(v, o.preview)) return false; }
        else if (a == "--from-cache") { if (!next(o.fromCache)) return false; }
        else if (a == "--export") { if (!next(o.exportPattern)) return false; }
        else if (a == "--export-node") { if (!next(o.exportNode)) return false; }
        else if (a == "--renderer") { if (!next(o.renderer)) return false; }
        else if (a == "--samples") { if (!nextInt(o.samples)) return false; }
        else if (a == "--list") o.listExamples = true;
        else if (a == "--node") { if (!next(o.node)) return false; }
        else if (a == "--frame") { if (!nextInt(o.frame)) return false; }
        else if (a == "--threads") { if (!nextInt(o.threads)) return false; }
        else if (a == "--hash") o.hash = true;
        else if (a == "--folder") { if (!next(o.folder)) return false; }
        else if (a.size() > 1 && a[0] == '-') return false;  // "-" alone: sim with no pictures
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
    std::fprintf(stderr, "unknown target '%s'; try 'prototype list'\n", o.target.c_str());
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
            std::fprintf(stderr, "prototype: no nodes from '%s' -- load it with --library, spelled the same\n",
                         o.nodesFrom.c_str());
            return 1;
        }
        for (auto& g : ng) graphs.push_back(std::move(g));
    }
    if (graphs.empty()) return usage();
    for (const std::string& tool : {o.glslang, o.spirvVal}) {
        if (!tool.empty() && !toolAvailable(tool)) {
            std::fprintf(stderr, "prototype: cannot run '%s' -- is it installed (glslang-tools, spirv-tools)?\n",
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
#ifdef PG_CAN_RENDER
    ShaderGraph g;
    if (!loadGraph(o.positional[0], g)) return 1;
    const Target* t = TargetRegistry::instance().find("glsl330");
    const GeneratedShader s = generate(g, lib, *t);
    if (!s.ok()) {
        printErrors(o.positional[0], g, lib, s);
        return 1;
    }

    Offscreen context;
    std::string error;
    if (!context.create(error)) {
        std::fprintf(stderr, "render: %s\n", error.c_str());
        return 1;
    }
    pg::gl::Api gl;
    if (!gl.load(context.procAddress(), error)) {
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
    const std::string& out = o.positional[1];
    const char* renderer = reinterpret_cast<const char*>(gl.GetString(pg::gl::RENDERER));
    if (pg::io::isVideoPath(out)) {
        // The preview animated: --frames of it, 30 a second, from --time on.
        const int frames = o.frames > 0 ? o.frames : 90;
        auto video = pg::io::openVideo(out, size, size, 30.0, error);
        if (!video) {
            std::fprintf(stderr, "render: %s\n", error.c_str());
            return 1;
        }
        for (int f = 0; f < frames; ++f) {
            preview.render(size * 2, size * 2, o.time + static_cast<float>(f) / 30.0f);
            const std::vector<uint8_t> pixels = preview.readPixels(2);
            if (!video->add(pixels.data(), error)) {
                std::fprintf(stderr, "render: %s\n", error.c_str());
                return 1;
            }
        }
        if (!video->finish(error)) {
            std::fprintf(stderr, "render: %s\n", error.c_str());
            return 1;
        }
        std::printf("wrote %s (%d frames at 30 fps, %s; %s, %s, %s)\n", out.c_str(), frames, video->codec().c_str(),
                    pg::gl::meshName(preview.mesh()), blendModeName(s.blend), renderer);
        return 0;
    }
    preview.render(size * 2, size * 2, o.time);  // 2x, averaged down: anti-aliasing
    if (!pg::gl::writePng(out, size, size, 3, preview.readPixels(2))) {
        std::fprintf(stderr, "render: cannot write %s\n", out.c_str());
        return 1;
    }
    std::printf("wrote %s (%s, %s, %s)\n", out.c_str(), pg::gl::meshName(preview.mesh()), blendModeName(s.blend), renderer);
    return 0;
#else
    (void)lib;
    std::fprintf(stderr, "render: this prototype was built without EGL and without the editor: it draws no picture\n");
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
    // center.y=... : a component, as a number or an expression.
    if (param.size() > 2 && param[param.size() - 2] == '.' && !net.paramDef(node, param)) {
        const std::string base = param.substr(0, param.size() - 2);
        const pg::sim::ParamDef* d = net.paramDef(node, base);
        const int c = param.back() - 'x';
        if (!d || c < 0 || c > 2 || (d->kind != pg::sim::ParamKind::Vector && d->kind != pg::sim::ParamKind::Color)) {
            error = name + ": no such parameter";
            return false;
        }
        if (pg::lang::isNumber(value)) {
            pg::sim::ParamValue v = net.param(node, base);
            v[static_cast<size_t>(c)] = static_cast<float>(std::strtod(value.c_str(), nullptr));
            net.setExpression(node, param, "");
            return net.setParam(node, base, v);
        }
        net.setExpression(node, param, value);
        why = net.expressionError(node, param);
        if (!why.empty()) {
            net.setExpression(node, param, "");
            error = name + ": " + why;
            return false;
        }
        return true;
    }
    if (!net.setParam(node, param, value, &why)) {
        // Not a value: an expression, for a number or a vector.
        const pg::sim::ParamDef* d = net.paramDef(node, param);
        const bool numeric = d && (d->kind == pg::sim::ParamKind::Float || d->kind == pg::sim::ParamKind::Int ||
                                   d->kind == pg::sim::ParamKind::Vector || d->kind == pg::sim::ParamKind::Color);
        if (numeric) {
            const std::vector<std::string> chans = pg::sim::Network::channels(*d);
            for (const std::string& ch : chans) net.setExpression(node, ch, value);
            const std::string e = net.expressionError(node, chans.front());
            if (e.empty()) return true;
            for (const std::string& ch : chans) net.setExpression(node, ch, "");
            why += " -- and as an expression: " + e;
        }
        error = name + ": " + why;
        return false;
    }
    return true;
}

/// fire.png, 30 -> fire_0030.png
std::string numbered(const std::string& path, int frame) {
    char digits[16];
    std::snprintf(digits, sizeof digits, "_%04d", frame);
    const fs::path p(path);
    return (p.parent_path() / (p.stem().string() + digits + p.extension().string())).string();
}

/// The camera of the viewport's orbit round `target`: `yaw` degrees round
/// the vertical, `pitch` above the horizon, `distance` away, 35 degrees
/// from the top of the picture to its bottom.
pg::sim::Camera orbitCamera(const Vec3& target, float yaw, float pitch, float distance, int width, int height) {
    const float y = yaw * 0.01745329252f, p = pitch * 0.01745329252f;
    const Vec3 towards(-std::cos(p) * std::sin(y), -std::sin(p), -std::cos(p) * std::cos(y));
    pg::sim::Camera c;
    c.position = target - towards * distance;
    c.rotation = pg::sim::Camera::rotationFor(towards, Vec3(0.0f, 1.0f, 0.0f));
    c.focal = 12.0f / std::tan(17.5f * 0.01745329252f);
    c.width = width;
    c.height = height;
    return c;
}

/// The path tracer's picture of a frame, `samples` passes, into `path`
/// (none: into `rgb` alone, for a video).
bool pathTraced(pg::render::PathTracer& tracer, const std::string& path, std::vector<uint8_t>& rgb,
                const std::string& comment, std::string& error) {
    const pg::render::Settings& s = tracer.settings();
    const int every = std::max(1, s.samples / 8);
    for (int i = tracer.samples(); i < s.samples; ++i) {
        tracer.pass();
        if ((i + 1) % every == 0 && s.samples >= 32) {
            std::fprintf(stderr, "  %d/%d samples (%.1f s)\r", i + 1, s.samples, tracer.seconds());
            std::fflush(stderr);
        }
    }
    if (s.samples >= 32) std::fprintf(stderr, "\n");
    if (path.empty()) {
        rgb = pg::render::displayRgb(tracer, s.denoise);
        return true;
    }
    return pg::render::savePicture(tracer, path, s.denoise, comment, error);
}


/// `sim NETWORK OUT.png`, and `pyro OUT.png --preset NAME`: the same, with an example.
int simulate(const Options& o, const std::string& network, const std::string& outPath) {
    const char* cmd = o.command.c_str();
    if (o.frames < 0 || o.every < 0 || o.start < 0) return usage();
    if (o.resolution != 0 && (o.resolution < 16 || o.resolution > 1024)) {
        std::fprintf(stderr, "%s: --resolution wants 16 to 1024 cells along the longest side, not %d\n", cmd,
                     o.resolution);
        return 1;
    }
    if (o.checkpoint < 0 || !(o.preview > 0.0f && o.preview <= 1.0f)) {
        std::fprintf(stderr, "%s: --checkpoint wants a number of frames, --preview a fraction above 0, at most 1\n", cmd);
        return 1;
    }
    if ((o.checkpoint > 0 || o.resume) && (o.cacheDir.empty() || !o.fromCache.empty())) {
        std::fprintf(stderr, "%s: --checkpoint and --resume keep the state in the cache: give --cache DIR, without "
                             "--from-cache\n", cmd);
        return 1;
    }
    // "-": no pictures -- the cache or the export alone.
    const bool pictures = outPath != "-";
    if (!pictures && o.cacheDir.empty() && o.exportPattern.empty()) {
        std::fprintf(stderr, "%s: '-' draws no picture: give --cache DIR or --export PATH as well\n", cmd);
        return 1;
    }
    namespace sim = pg::sim;
    sim::Network net;
    std::string error, folder;
    if (!loadNetwork(network, net, error, folder)) {
        std::fprintf(stderr, "%s: %s\n", cmd, error.c_str());
        return 1;
    }
    if (!o.folder.empty()) folder = o.folder;
    for (const std::string& assignment : o.sets) {
        if (!applySetting(assignment, net, error)) {
            std::fprintf(stderr, "%s: %s\n", cmd, error.c_str());
            return 1;
        }
    }
    // The geometry nodes cook here: shapes for the simulations, and the
    // displayed node's geometry for the pictures and the export -- from the
    // frame just simulated, for the nodes that bring a simulation back.
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
    // Through the Output's camera, over the Output's frames -- a layout, a
    // plate filmed of a set; else a still.
    if (geometryOnly && !c.hasCamera) c.frames = 1;
    if (o.resolution > 0) {
        c.world.gas.solver.resolution = o.resolution;
        c.world.water.solver.resolution = o.resolution;
    }
    c.world = sim::preview(c.world, o.preview);
    int frames = o.frames > 0 ? o.frames : c.frames;
    // What is exported: the displayed node's geometry, or the one named.
    int exported = c.display;
    if (!o.exportNode.empty()) {
        const sim::Node* n = net.named(o.exportNode);
        const sim::NodeType* t = n ? sim::findNodeType(n->type) : nullptr;
        if (!t || !t->core) {
            std::fprintf(stderr, "%s: --export-node %s: no geometry node of that name\n", cmd, o.exportNode.c_str());
            return 1;
        }
        exported = n->id;
    }
    // A .usda: the whole shot, as one USD stage -- with or without geometry.
    std::unique_ptr<sim::UsdExport> usd;
    if (sim::isUsdPath(o.exportPattern) && o.exportPattern.find("$F") == std::string::npos) {
        const sim::Node* n = exported ? net.node(exported) : nullptr;
        usd = std::make_unique<sim::UsdExport>(o.exportPattern, n ? n->name : std::string("geometry"),
                                               1.0f / c.world.sanitized().timeStep);
    }
    if (!o.exportPattern.empty() && !exported && !usd) {
        std::fprintf(stderr, "%s: --export writes the displayed geometry, and no node is displayed: give one the "
                             "display flag, or name it with --export-node\n", cmd);
        return 1;
    }
    // The frames from a cache on disk, rather than simulated.
    sim::CacheInfo from;
    if (!o.fromCache.empty()) {
        if (!sim::readCacheInfo(o.fromCache, from, error)) {
            std::fprintf(stderr, "%s: %s\n", cmd, error.c_str());
            return 1;
        }
        if (from.network != sim::networkHash(net.save())) {
            std::fprintf(stderr, "%s: warning: the cache in %s was written by another network (or another version "
                                 "of it); its frames are used as they are\n", cmd, o.fromCache.c_str());
        }
        frames = o.frames > 0 ? std::min(o.frames, from.frames) : from.frames;
    }
    // The frames written: --start to the last. A simulation gets there from
    // frame 1 all the same; a cache is read from --start.
    const int first = std::max(o.start, 1);
    if (first > frames) {
        std::fprintf(stderr, "%s: --start %d is after the last frame, %d\n", cmd, first, frames);
        return 1;
    }
    if (pictures && o.every > frames) {
        std::fprintf(stderr, "%s: --every %d is more than the %d frames: no frame would be written\n", cmd, o.every,
                     frames);
        return 1;
    }
    if (o.renderer != "gl" && o.renderer != "path") {
        std::fprintf(stderr, "%s: --renderer wants gl (the viewport's) or path (the path tracer), not %s\n", cmd,
                     o.renderer.c_str());
        return 1;
    }
    if (o.samples < 0 || o.samples > 1 << 16) {
        std::fprintf(stderr, "%s: --samples wants 1 to 65536 a pixel, not %d\n", cmd, o.samples);
        return 1;
    }
    // The path tracer: light followed as it goes, on the processor -- no
    // OpenGL wanted.
    const bool pathTrace = pictures && o.renderer == "path";
    // Through the network's camera, at the size of its picture -- unless
    // the command line asks for a view round the scene. Without a camera:
    // tall for a plume, wide for a scene wider than it is high.
    const bool throughCamera = c.hasCamera && !(o.yawSet || o.pitchSet || o.distance > 0.0f);
    const Vec3 extent = sim::sceneDomain(c.world).size();
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

    // A video: every frame -- or every K-th -- into one file.
    const bool video = pictures && pg::io::isVideoPath(outPath);
    // An EXR: the picture in linear light and its passes, a file a frame.
    std::string outExt = fs::path(outPath).extension().string();
    for (char& ch : outExt) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    const bool exr = pictures && outExt == ".exr";
    std::unique_ptr<pg::io::VideoWriter> movie;
    const double videoFps = 1.0 / (static_cast<double>(c.world.timeStep) * std::max(o.every, 1));
    // Opened before anything is simulated: a video that cannot be written
    // says so at once.
    if (video && !(movie = pg::io::openVideo(outPath, width, height, videoFps, error))) {
        std::fprintf(stderr, "%s: %s\n", cmd, error.c_str());
        return 1;
    }
#ifdef PG_CAN_RENDER
    namespace gl = pg::gl;
    Offscreen context;
    gl::Api api;
    std::unique_ptr<gl::VolumeRenderer> volume;
    if (pictures && !pathTrace) {
        if (!context.create(error)) {
            std::fprintf(stderr, "%s: %s\n", cmd, error.c_str());
            return 1;
        }
        if (!api.load(context.procAddress(), error)) {
            std::fprintf(stderr, "%s: OpenGL function %s is missing\n", cmd, error.c_str());
            return 1;
        }
        volume = std::make_unique<gl::VolumeRenderer>(api);
        std::string log;
        if (!volume->init(log)) {
            std::fprintf(stderr, "%s: the driver rejected the volume shader:\n%s\n", cmd, log.c_str());
            return 1;
        }
    }
#else
    if (pictures && !pathTrace) {
        std::fprintf(stderr, "%s: this prototype was built without EGL and without the editor: it draws no picture "
                             "with OpenGL -- --renderer path renders on the processor; '-' in place of OUT.png "
                             "simulates, caches and exports all the same\n", cmd);
        return 1;
    }
#endif
    // Simulated only when the frames do not come from a cache.
    std::unique_ptr<sim::WorldSolver> solver;
    std::shared_ptr<const sim::RigidLayout> adoptedLayout;  // the pieces' bodies, for frames read back
    std::shared_ptr<const sim::RigidRebar> adoptedRebar;    // ... and the bars in them
    std::shared_ptr<const sim::RigidGlue> adoptedGlue;      // ... and the joints of their glue
    if (o.fromCache.empty()) solver = std::make_unique<sim::WorldSolver>(c.world);
    const sim::World world = c.world.sanitized();
    // What the cache says of itself as it is written -- how far it has got,
    // how long a frame takes, the frame of its checkpoint -- for whoever
    // watches it: the editor, playing a bake as it runs.
    const uint64_t networkHash = sim::networkHash(net.save());
    sim::CacheInfo progress;
    progress.fps = 1.0f / world.timeStep;
    progress.network = networkHash;
    progress.of = frames;
    // Going on from the cache's checkpoint: the solver taken to its frame.
    int resumed = 0;
    if (o.resume) {
        sim::CacheInfo before;
        std::string state;
        if (!sim::readCacheInfo(o.cacheDir, before, error) || !sim::readCheckpoint(o.cacheDir, state, error)) {
            std::fprintf(stderr, "%s: --resume: %s\n", cmd, error.c_str());
            return 1;
        }
        if (before.network != networkHash) {
            std::fprintf(stderr, "%s: --resume: the checkpoint in %s is of another network (or another version of "
                                 "it)\n", cmd, o.cacheDir.c_str());
            return 1;
        }
        const auto t = std::chrono::steady_clock::now();
        if (!solver->loadState(state, error)) {
            std::fprintf(stderr, "%s: --resume: %s\n", cmd, error.c_str());
            return 1;
        }
        resumed = solver->frame();
        if (resumed > frames) {
            std::fprintf(stderr, "%s: --resume: the checkpoint is of frame %d, after the last, %d\n", cmd, resumed,
                         frames);
            return 1;
        }
        progress.checkpoint = resumed;
        progress.stepMs = before.stepMs;
        // Frames after the checkpoint are made again: the note says so.
        progress.frames = resumed;
        if (resumed > 0 && !sim::writeCacheInfo(o.cacheDir, progress, error)) {
            std::fprintf(stderr, "%s: %s\n", cmd, error.c_str());
            return 1;
        }
        std::printf("resumed at frame %d from %s (%.1f s)\n", resumed, sim::checkpointFile(o.cacheDir).c_str(),
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count());
        std::fflush(stdout);
    }
#ifdef PG_CAN_RENDER
    sim::Domain box = gl::sceneDomain(world);
    if (volume) {
        const sim::Domain domain = world.hasGas ? world.gas.solver.domain() : world.water.solver.domain();
        volume->look = c.look;
        volume->setDomain(domain);
        volume->setSolids(c.solids);
        if (o.guides) {
            volume->setLines(gl::sceneGuides(&world, c.solids, {}, c.solver, c.liquidSolver, c.rain,
                                             c.hasCamera && !throughCamera ? &c.camera : nullptr));
        }
        // Geometry alone: the view frames it.
        bool framed = false;
        Vec3 middle;
        if (geometryOnly) {
            Vec3 lo, hi;
            volume->setGeometry(geometry.cook(c.display, 1, world.timeStep));
            if (volume->geometryBounds(lo, hi)) {
                const Vec3 size = hi - lo;
                box = sim::Domain::ofBox(Vec3(std::max(size.x, 0.1f), std::max(size.y, 0.1f), std::max(size.z, 0.1f)), 16);
                middle = (lo + hi) * 0.5f;
                framed = true;
            }
        }
        if (throughCamera) {
            const Vec3 centre = box.origin() + box.size() * 0.5f;
            volume->orbit = gl::orbitThrough(c.camera, std::max(dot(centre - c.camera.position, c.camera.forward()), 0.5f));
        } else {
            volume->orbit = gl::VolumeRenderer::viewOf(box);
            if (framed) {
                volume->orbit.target[0] = middle.x;
                volume->orbit.target[1] = middle.y;
                volume->orbit.target[2] = middle.z;
            }
            if (o.yawSet) volume->orbit.yaw = o.yaw;
            if (o.pitchSet) volume->orbit.pitch = o.pitch;
            if (o.distance > 0.0f) volume->orbit.distance = o.distance;
        }
    }
#endif
    // The path tracer's view, without the camera: the viewport's -- round
    // the simulations' box, or framing the geometry alone.
    pg::render::SceneBuilder builder;
    pg::render::PathTracer tracer;
    pg::render::Settings settings = c.render;
    settings.width = width;
    settings.height = height;
    if (o.samples > 0) settings.samples = o.samples;
    const sim::Domain domain = sim::sceneDomain(world);
    pg::render::Box domainBox;
    domainBox.lo = domain.origin();
    domainBox.hi = domain.origin() + domain.size();
    Vec3 viewTarget(0.0f, 0.46f * domain.size().y, 0.0f);
    float viewYaw = 35.0f, viewPitch = 12.0f;
    float viewDistance = 0.92f * 0.5f * length(domain.size()) / std::sin(17.5f * 0.01745329252f);
    if (pathTrace && geometryOnly && c.display) {
        const GeometryPtr geo = geometry.cook(c.display, first, world.timeStep);
        Vec3 lo, hi;
        if (geo) drawnBox(*geo, lo, hi);
        if (geo && lo.x <= hi.x) {
            const Vec3 size(std::max(hi.x - lo.x, 0.1f), std::max(hi.y - lo.y, 0.1f), std::max(hi.z - lo.z, 0.1f));
            viewTarget = (lo + hi) * 0.5f;
            viewDistance = 0.92f * 0.5f * length(size) / std::sin(17.5f * 0.01745329252f);
        }
    }
    if (o.yawSet) viewYaw = o.yaw;
    if (o.pitchSet) viewPitch = o.pitch;
    if (o.distance > 0.0f) viewDistance = o.distance;

    using Clock = std::chrono::steady_clock;
    auto ms = [](Clock::time_point since) {
        return std::chrono::duration<double, std::milli>(Clock::now() - since).count();
    };
    double simulating = 0.0, rendering = 0.0;
    int images = 0, cachedFrames = 0, exports = 0, passed = 0;  // passed: frames from --start on
    int plateErrors = 0;
    std::string last, lastExport;
    int simulated = 0;  // frames stepped here -- after the checkpoint, when resumed
    sim::Frame::Profile spent;  // where the time of the steps went, summed
    for (int f = solver ? resumed + 1 : first; f <= frames; ++f) {
        const bool inRange = f >= first;  // before --start: simulated, cached, not drawn or exported
        bool draws = false;
#ifdef PG_CAN_RENDER
        draws = volume && inRange && (o.every > 0 ? f % o.every == 0 : video || f == frames);
#endif
        if (pathTrace) draws = inRange && (o.every > 0 ? f % o.every == 0 : video || f == frames);
        auto t = Clock::now();
        // The frame: simulated -- and taken when something wants it -- or read.
        if (solver) {
            solver->step();
            const sim::Frame::Profile& p = solver->profile();
            spent.rigid += p.rigid;
            spent.scenes += p.scenes;
            spent.gas += p.gas;
            spent.water += p.water;
            spent.rain += p.rain;
            spent.cloth += p.cloth;
            for (int s = 0; s < 8; ++s) spent.gasStages[s] += p.gasStages[s];
            if (draws || !o.cacheDir.empty() || (inRange && !o.exportPattern.empty())) {
                current = std::make_shared<const sim::Frame>(solver->capture());
            }
        } else {
            auto read = std::make_shared<sim::Frame>();
            if (!sim::readFrame(o.fromCache, f, *read, error)) {
                std::fprintf(stderr, "%s: %s\n", cmd, error.c_str());
                return 1;
            }
            read->number = f;  // the file's name says which it is
            sim::adoptPieces(*read, world.rigid, &adoptedLayout, &adoptedRebar, &adoptedGlue);
            sim::adoptCloth(*read, world.cloth);
            current = std::move(read);
        }
        simulating += ms(t);
        ++simulated;
        if (!o.cacheDir.empty()) {
            if (!sim::writeFrame(*current, o.cacheDir, error)) {
                std::fprintf(stderr, "%s: %s\n", cmd, error.c_str());
                return 1;
            }
            ++cachedFrames;
            // Every K frames, the state -- not after the last: there is
            // nothing to go on to.
            if (solver && o.checkpoint > 0 && f % o.checkpoint == 0 && f < frames) {
                if (!sim::writeCheckpoint(o.cacheDir, solver->saveState(), error)) {
                    std::fprintf(stderr, "%s: %s\n", cmd, error.c_str());
                    return 1;
                }
                progress.checkpoint = f;
            }
            progress.frames = f;
            progress.stepMs = simulating / simulated;
            if (f < frames && !sim::writeCacheInfo(o.cacheDir, progress, error)) {
                std::fprintf(stderr, "%s: %s\n", cmd, error.c_str());
                return 1;
            }
        }
        if (!inRange) continue;
        ++passed;
        if (usd) {
            const GeometryPtr geo = exported ? geometry.cook(exported, f, world.timeStep) : nullptr;
            if (!usd->add(*current, geo, c.hasCamera ? &c.cameraAt(f) : nullptr, c.lookAt(f), error)) {
                std::fprintf(stderr, "%s: %s\n", cmd, error.c_str());
                return 1;
            }
        } else if (!o.exportPattern.empty()) {
            const GeometryPtr geo = geometry.cook(exported, f, world.timeStep);
            lastExport = pg::io::framePath(o.exportPattern, f);
            std::error_code ec;
            if (!std::filesystem::path(lastExport).parent_path().empty()) {
                std::filesystem::create_directories(std::filesystem::path(lastExport).parent_path(), ec);
            }
            if (!geo || !pg::io::writeGeometry(*geo, lastExport, error)) {
                std::fprintf(stderr, "%s: %s\n", cmd, geo ? error.c_str() : "the exported node gave no geometry");
                return 1;
            }
            ++exports;
        }
        if (pathTrace && draws) {
            t = Clock::now();
            pg::render::SceneInput in;
            if (c.display) {
                in.geometry = geometry.cook(c.display, f, world.timeStep);
                const std::string why = geometry.error(c.display);
                if (!why.empty()) std::fprintf(stderr, "%s: %s: %s\n", cmd, net.node(c.display)->name.c_str(), why.c_str());
            }
            in.look = c.lookAt(f);
            if (!geometryOnly) {
                in.bodies = sim::drawnBodies(*current, in.look);
                if (!current->water.empty() && in.look.waterSurface) in.water = sim::waterMesh(current->water, &current->rain);
            }
            in.solids = c.solidsAt(f);
            in.camera = throughCamera ? c.cameraAt(f)
                                      : orbitCamera(viewTarget, viewYaw, viewPitch, viewDistance, width, height);
            in.camera.width = width;
            in.camera.height = height;
            in.sunAngle = settings.sunAngle;
            in.domain = domainBox;
            tracer.setSettings(settings);
            tracer.setScene(builder.build(in));
            const std::string file = movie ? std::string() : o.every > 0 ? numbered(outPath, f) : outPath;
            std::vector<uint8_t> rgb;
            if (!pathTraced(tracer, file, rgb, "prototype sim " + network + ", frame " + std::to_string(f), error)) {
                std::fprintf(stderr, "%s: %s\n", cmd, error.c_str());
                return 1;
            }
            if (movie && !movie->add(rgb.data(), error)) {
                std::fprintf(stderr, "%s: %s\n", cmd, error.c_str());
                return 1;
            }
            last = movie ? outPath : file;
            rendering += ms(t);
            ++images;
            continue;
        }
#ifdef PG_CAN_RENDER
        if (!draws) continue;
        t = Clock::now();
        if (!geometryOnly) {
            volume->setFrame(*current);
            const sim::Look& k = volume->look;
            volume->setPieces(sim::drawnBodies(*current, k));
        }
        // Animated: the look, the objects and the camera of this frame.
        if (!c.poses.empty()) {
            volume->look = c.lookAt(f);
            volume->setSolids(c.solidsAt(f));
            if (throughCamera) {
                const sim::Camera& cam = c.cameraAt(f);
                const Vec3 centre = box.origin() + box.size() * 0.5f;
                volume->orbit = gl::orbitThrough(cam, std::max(dot(centre - cam.position, cam.forward()), 0.5f));
            }
        }
        if (c.display) {
            volume->setGeometry(geometry.cook(c.display, f, world.timeStep));
            const std::string why = geometry.error(c.display);
            if (!why.empty()) std::fprintf(stderr, "%s: %s: %s\n", cmd, net.node(c.display)->name.c_str(), why.c_str());
        }
        if (exr) {
            // The passes: motion to the next frame's camera, when it moves.
            volume->passes.on = true;
            volume->passes.frameTime = world.timeStep;
            volume->passes.moving = throughCamera && !c.poses.empty();
            if (volume->passes.moving) {
                const sim::Camera& next = c.cameraAt(f + 1);
                volume->passes.next = gl::orbitThrough(next, 1.0f);
            }
        }
        // The plate behind it all, through the shot's camera.
        if (throughCamera && !c.cameraAt(f).plate.empty()) {
            const sim::Camera& cam = c.cameraAt(f);
            std::string why;
            if (!volume->setPlate(cam.plateFile(f), cam, why) && plateErrors++ < 3) {
                std::fprintf(stderr, "%s: %s\n", cmd, why.c_str());
            }
        }
        volume->render(width * 2, height * 2);  // 2x, averaged down: anti-aliasing
        if (exr) {
            last = o.every > 0 ? numbered(outPath, f) : outPath;
            if (!gl::writePassesExr(*volume, last, "prototype sim " + network + ", frame " + std::to_string(f), error)) {
                std::fprintf(stderr, "%s: %s\n", cmd, error.c_str());
                return 1;
            }
            rendering += ms(t);
            ++images;
            continue;
        }
        const std::vector<uint8_t> pixels = volume->readPixels(2);
        if (movie) {
            if (!movie->add(pixels.data(), error)) {
                std::fprintf(stderr, "%s: %s\n", cmd, error.c_str());
                return 1;
            }
            last = outPath;
        } else {
            last = o.every > 0 ? numbered(outPath, f) : outPath;
            if (!gl::writePng(last, width, height, 3, pixels)) {
                std::fprintf(stderr, "%s: cannot write %s\n", cmd, last.c_str());
                return 1;
            }
        }
        rendering += ms(t);
        ++images;
#endif
    }
    if (movie && !movie->finish(error)) {
        std::fprintf(stderr, "%s: %s\n", cmd, error.c_str());
        return 1;
    }
    if (usd && !usd->finish(error)) {
        std::fprintf(stderr, "%s: %s\n", cmd, error.c_str());
        return 1;
    }
    if (!o.cacheDir.empty()) {
        // Done: the note says so, and the checkpoint has served.
        sim::CacheInfo info;
        info.frames = frames;
        info.fps = 1.0f / world.timeStep;
        info.network = networkHash;
        if (solver && simulated > 0) info.stepMs = simulating / simulated;
        if (!sim::writeCacheInfo(o.cacheDir, info, error)) {
            std::fprintf(stderr, "%s: %s\n", cmd, error.c_str());
            return 1;
        }
        std::error_code ec;
        fs::remove(sim::checkpointFile(o.cacheDir), ec);
    }
    std::string what;
    if (solver && world.hasGas) {
        const sim::Domain& d = solver->gas()->domain();
        what += ", gas " + std::to_string(d.cells[0]) + " x " + std::to_string(d.cells[1]) + " x " +
                std::to_string(d.cells[2]) + " cells";
    }
    if (solver && world.hasWater) {
        const sim::Domain& d = solver->water()->domain();
        what += ", water " + std::to_string(d.cells[0]) + " x " + std::to_string(d.cells[1]) + " x " +
                std::to_string(d.cells[2]) + " cells, " + std::to_string(solver->water()->particleCount()) + " particles";
    }
    if (solver && world.hasRain) {
        what += ", rain " + std::to_string(solver->rain()->drops().size()) + " drops, " +
                std::to_string(solver->rain()->droplets().size()) + " droplets";
    }
    if (!o.fromCache.empty()) what += ", read from " + o.fromCache;
    if (images > 0 && throughCamera) {
        const sim::Node* n = net.node(c.camera.node);
        what += ", through " + (n ? n->name : std::string("the camera"));
    }
    std::string written = last;
    if (movie) {
        char rate[32];
        std::snprintf(rate, sizeof rate, "%g", videoFps);
        written += " (" + std::to_string(movie->frames()) + " frames at " + rate + " fps, " + movie->codec() + ")";
    } else if (images > 1) {
        written += " and " + std::to_string(images - 1) + " before it";
    }
    std::string through;
    if (images > 0 && pathTrace) {
        char text[160];
        std::snprintf(text, sizeof text, " through the path tracer (%s), %d samples a pixel%s",
                      pg::render::rayEngineName(builder.engine()).c_str(), settings.samples,
                      settings.denoise ? ", denoised" : "");
        through = text;
    }
#ifdef PG_CAN_RENDER
    if (images > 0 && !pathTrace) through = std::string(" through ") + context.kind();
#endif
    // Simulated: every frame to the last; read: those from --start.
    const int stepped = solver ? simulated : passed;
    const std::string range = first > 1 ? " " + std::to_string(first) + "-" + std::to_string(frames) : std::string();
    if (images > 0) {
        std::printf("wrote %s: %s%s, %d frames%s (%.1f s); %s %.1f ms/frame, rendering %.0f ms/image%s\n",
                    written.c_str(), network.c_str(), what.c_str(), passed, range.c_str(),
                    static_cast<double>(passed) * static_cast<double>(world.timeStep), solver ? "simulation" : "reading",
                    simulating / std::max(stepped, 1), rendering / std::max(images, 1), through.c_str());
    } else if (solver) {
        const std::string from = resumed > 0 ? ", from frame " + std::to_string(resumed + 1) : std::string();
        std::printf("%s: simulated%s, %d frames%s; simulation %.1f ms/frame\n", network.c_str(), what.c_str(), frames,
                    from.c_str(), simulating / std::max(stepped, 1));
    } else {
        std::printf("%s: %d frames%s read from %s; reading %.1f ms/frame\n", network.c_str(), passed, range.c_str(),
                    o.fromCache.c_str(), simulating / std::max(stepped, 1));
    }
    // Where the time went -- what to make faster, or coarser.
    if (solver && simulated > 0 && spent.total() > 0.0f) {
        const double all = spent.total();
        auto share = [&](float ms) { return 100.0 * ms / all; };
        std::string line = "time:";
        auto add = [&](const char* name, float ms, bool there) {
            if (!there) return;
            char text[64];
            std::snprintf(text, sizeof text, " %s %.0f%%", name, share(ms));
            line += text;
        };
        add("pieces", spent.rigid, world.hasRigid);
        add("into scenes", spent.scenes, world.hasRigid);
        add("gas", spent.gas, world.hasGas);
        if (world.hasGas) {
            static const char* stages[8] = {"solids", "tiles", "emit", "advect", "combust", "forces", "project", "dissipate"};
            line += " (";
            for (int s = 0; s < 8; ++s) {
                char text[48];
                std::snprintf(text, sizeof text, "%s%s %.0f%%", s ? ", " : "", stages[s], share(spent.gasStages[s]));
                line += text;
            }
            line += ")";
        }
        add("water", spent.water, world.hasWater);
        add("rain", spent.rain, world.hasRain);
        add("cloth", spent.cloth, world.hasCloth);
        std::printf("%s\n", line.c_str());
    }
    if (cachedFrames > 0) std::printf("cached %d frames in %s\n", cachedFrames, o.cacheDir.c_str());
    if (exports > 0) std::printf("exported %d frames of geometry, the last %s\n", exports, lastExport.c_str());
    if (usd) {
        std::string beside;
        if (usd->frameFiles() > 0) {
            beside += ", what changes every frame in " + std::to_string(usd->frameFiles()) + " layers beside it";
        }
        if (usd->gasFiles() > 0) beside += ", the gas in " + std::to_string(usd->gasFiles()) + " VDB files";
        std::printf("exported %d frames as USD to %s: %d bodies%s\n", usd->frames(), usd->path().c_str(), usd->bodies(),
                    beside.c_str());
    }
    return 0;
}

int simCommand(const Options& o) {
    if (o.listExamples) {
        for (const std::string& name : pg::sim::Network::exampleNames()) std::printf("%s\n", name.c_str());
        return 0;
    }
    if (o.positional.size() != 2 || o.threads < 0) return usage();
    if (o.threads > 0) pg::TaskPool::instance().setThreadCount(static_cast<unsigned>(o.threads));
    return simulate(o, o.positional[0], o.positional[1]);
}

/// `cook NETWORK OUT`: a network's geometry, cooked and written.
int cook(const Options& o) {
    if (o.positional.size() != 2 || o.frames < 0 || o.frame < 0 || o.threads < 0 || o.start < 0) return usage();
    pg::sim::Network net;
    std::string error, folder;
    if (!loadNetwork(o.positional[0], net, error, folder)) {
        std::fprintf(stderr, "cook: %s\n", error.c_str());
        return 1;
    }
    if (!o.folder.empty()) folder = o.folder;
    for (const std::string& s : o.sets) {
        if (!applySetting(s, net, error)) {
            std::fprintf(stderr, "cook: %s\n", error.c_str());
            return 1;
        }
    }
    int id = net.displayed();
    if (!o.node.empty()) {
        const pg::sim::Node* n = net.named(o.node);
        if (!n) {
            std::fprintf(stderr, "cook: no node '%s'\n", o.node.c_str());
            return 1;
        }
        id = n->id;
    }
    if (!id) {
        std::fprintf(stderr, "cook: no node is displayed -- --node NODE says which to cook\n");
        return 1;
    }
    if (o.threads > 0) pg::TaskPool::instance().setThreadCount(static_cast<unsigned>(o.threads));
    pg::sim::GeometryGraph geo;
    geo.sync(net, folder);
    if (!geo.contains(id)) {
        std::fprintf(stderr, "cook: %s is not a geometry node\n", net.node(id)->name.c_str());
        return 1;
    }
    const int first = o.frames > 0 ? std::max(o.start, 1) : std::max(o.frame, 1);
    const int last = o.frames > 0 ? o.frames : first;
    if (first > last) {
        std::fprintf(stderr, "cook: --start %d is after the last frame, %d\n", first, last);
        return 1;
    }
    const std::string& out = o.positional[1];
    const bool numbered = last > first || out.find("$F") != std::string::npos;
    for (int f = first; f <= last; ++f) {
        const auto start = std::chrono::steady_clock::now();
        const pg::GeometryPtr g = geo.cook(id, f, 1.0f / 30.0f);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        bool failed = false;
        for (const pg::sim::Node& n : net.nodes()) {
            const std::string e = geo.error(n.id);
            if (e.empty()) continue;
            std::fprintf(stderr, "cook: %s: %s\n", n.name.c_str(), e.c_str());
            failed |= n.id == id;
        }
        if (failed || !g) return 1;
        std::printf("frame %d: %zu points, %zu primitives, %zu volumes, %.1f ms", f, g->pointCount(), g->primitiveCount(),
                    g->volumeCount(), ms);
        if (o.hash) std::printf(", hash %016llx", static_cast<unsigned long long>(g->hash()));
        std::printf("\n");
        if (out == "-") continue;
        const std::string path = numbered ? pg::io::framePath(out, f) : out;
        if (!pg::io::writeGeometry(*g, path, error)) {
            std::fprintf(stderr, "cook: %s\n", error.c_str());
            return 1;
        }
        std::printf("wrote %s\n", path.c_str());
    }
    return 0;
}

// --- usd ---------------------------------------------------------------------------

/// What a USD file composes to, as the program reads it: the stage's units,
/// axes and time, its layers, its prims as a tree, its cameras, and what
/// USD Import makes of it at a frame.
int usdInfo(const Options& o) {
    if (o.positional.size() != 1) return usage();
    const std::string& path = o.positional[0];
    std::string error;
    const auto stage = usd::Stage::open(path, error);
    if (!stage) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    std::printf("%s: %s up, %g m a unit", path.c_str(), stage->zUp() ? "Z" : "Y", stage->metersPerUnit());
    if (stage->hasTimeRange()) std::printf(", time codes %g to %g", stage->startTimeCode(), stage->endTimeCode());
    std::printf(" at %g a second", stage->timeCodesPerSecond());
    if (!stage->defaultPrim().empty()) std::printf("; default prim %s", stage->defaultPrim().c_str());
    std::printf("\n");
    const std::vector<std::string> files = stage->files();
    std::printf("%zu %s:", files.size(), files.size() == 1 ? "layer" : "layers");
    for (const std::string& f : files) std::printf(" %s", fs::path(f).filename().string().c_str());
    std::printf("\n");
    for (const std::string& w : stage->warnings()) std::printf("warning: %s\n", w.c_str());
    size_t shown = 0;
    for (const auto& owned : stage->prims()) {
        const usd::Stage::Prim& p = *owned;
        if (p.path == "/") continue;
        int depth = 0;
        for (const usd::Stage::Prim* a = p.parent; a && a->parent; a = a->parent) ++depth;
        if (++shown > 400) {
            std::printf("  ... %zu prims in all\n", stage->prims().size() - 1);
            break;
        }
        std::string flags;
        if (p.specifier == usd::Specifier::Class) flags += " class";
        else if (p.specifier == usd::Specifier::Over) flags += " over";
        if (!p.active) flags += " inactive";
        for (const usd::Stage::Opinion& op : p.opinions) {
            for (const auto& [set, variant] : op.spec->variantSelections) {
                if (flags.find("{" + set + "=") == std::string::npos) flags += " {" + set + "=" + variant + "}";
            }
        }
        std::printf("  %s%s%s%s%s\n", std::string(static_cast<size_t>(depth) * 2, ' ').c_str(), p.name.c_str(),
                    p.type.empty() ? "" : ("  " + p.type).c_str(), flags.empty() ? "" : "  [", flags.empty() ? "" : (flags.substr(1) + "]").c_str());
    }
    const double time = usd::timeCodeAt(*stage, o.frame > 0 ? o.frame : 1, stage->timeCodesPerSecond());
    for (const usd::Stage::Prim* c : usd::cameras(*stage)) {
        usd::CameraSample s;
        usd::cameraAt(*stage, *c, time, true, s);
        std::printf("camera %s: %g mm lens, film back %g x %g, at (%.3f, %.3f, %.3f) m%s\n", c->path.c_str(),
                    s.focalLength, s.horizontalAperture, s.verticalAperture, s.world.at(3, 0), s.world.at(3, 1),
                    s.world.at(3, 2), usd::cameraVaries(*stage, *c) ? ", moving" : "");
    }
    std::vector<std::string> notes;
    const auto geo = usd::importGeometry(*stage, time, usd::ImportOptions{}, &notes);
    std::printf("time code %g: %zu points, %zu primitives from %zu prims%s\n", time, geo->pointCount(),
                geo->primitiveCount(), usd::geometryPrims(*stage, usd::ImportOptions{}).size(),
                usd::geometryVaries(*stage, usd::ImportOptions{}) ? ", changing in time" : "");
    for (size_t i = 0; i < notes.size() && i < 10; ++i) std::printf("  %s\n", notes[i].c_str());
    return 0;
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
           word == "pyro" || word == "cook" || word == "usd";
}

void printUsage(std::FILE* out) {
    std::fprintf(out,
                 "usage:\n"
#ifdef PG_HAVE_GUI
                 "  prototype [NETWORK.pgsim | GRAPH.pgsg] [--example NAME] [--shaders] [--select NODE]\n"
                 "            [--library FILE]... [--target NAME] [--mesh NAME] [--size WxH]\n"
                 "            [--screenshot OUT.png [--frames N]] [--script FILE]\n"
                 "                   the node editor -- what runs without a command: an empty scene,\n"
                 "                   an example with --example, shaders with --shaders\n"
#else
                 "  prototype [NETWORK.pgsim | GRAPH.pgsg]   the node editor -- not in this build (PG_BUILD_GUI=OFF)\n"
#endif
                 "  prototype list   [--markdown] [--library FILE]...\n"
                 "  prototype gen    GRAPH.pgsg... [--target NAME|all] [-o DIR] [--library FILE]...\n"
                 "  prototype check  [GRAPH.pgsg...] [--nodes | --nodes-from FILE] [--glslang PATH]\n"
                 "                   [--spirv-val PATH] [--library FILE]...\n"
                 "  prototype render GRAPH.pgsg OUT.png|OUT.mp4 [--mesh sphere|torus|cube|plane|billboard] [--size N]\n"
                 "                   [--time SECONDS] [--frames N] [--yaw DEG] [--pitch DEG] [--library FILE]...\n"
                 "                   a video: --frames of the preview animated, 30 a second (90)\n"
                 "  prototype sim    NETWORK.pgsim|EXAMPLE OUT.png|OUT.exr|OUT.mp4|- [--frames N] [--start N] [--every K]\n"
                 "                   [--resolution 16..1024]\n"
                 "                   [--size WxH] [--yaw DEG] [--pitch DEG] [--distance D] [--guides]\n"
                 "                   [--set NODE.PARAM=VALUE]... [--cache DIR [--checkpoint K] [--resume]]\n"
                 "                   [--from-cache DIR] [--export PATH] [--export-node NODE] [--threads N]\n"
                 "                   [--preview F] [--renderer gl|path [--samples N]]\n"
                 "                   simulates a network of nodes and renders its last frame; --every K renders\n"
                 "                   frames K, 2K, 3K... as OUT_<frame>.png (K = 2: OUT_0002.png, OUT_0004.png...);\n"
                 "                   OUT.exr: linear light and passes for compositing (Z, forward.u/v, mask.*);\n"
                 "                   --renderer path: the path tracer, on the processor (the Output's Render\n"
                 "                   settings; --samples N a pixel); its EXR: light, Z, albedo.*, N.*;\n"
                 "                   a video gets every frame (every K-th): .avi always, .mp4 .mov .mkv .webm .gif\n"
                 "                   when ffmpeg is installed;\n"
                 "                   through the network's camera at its size, unless --yaw, --pitch or --distance\n"
                 "                   ask for a view round the scene. --cache writes every frame to DIR, and\n"
                 "                   how far it has got to DIR/cache.txt; --checkpoint K the state every K\n"
                 "                   frames, which --resume goes on from; --preview F: the grids F as fine;\n"
                 "                   --from-cache reads them from there instead of simulating; --export writes\n"
                 "                   the displayed geometry of every frame, PATH with $F4 for the frame:\n"
                 "                   .ply points, .obj polygons, .vdb volumes, .usda; a .usda without $F: the\n"
                 "                   whole shot as one USD stage -- geometry, pieces, grit, water, rain, gas (VDB\n"
                 "                   beside it), camera, light; what changes every frame in a layer a frame\n"
                 "                   beside it (NAME_frames/).\n"
                 "                   '-' for OUT.png: no pictures. --threads N: on N threads (all there are\n"
                 "                   by default) -- the same frames on any number.\n"
                 "                   --start N: pictures and export from frame N on (a farm's share of a shot);\n"
                 "                   --end N is --frames N. A simulation still starts at frame 1; a cache is\n"
                 "                   read from N.\n"
                 "                   --folder DIR: the network's relative paths (meshes) are read from DIR.\n"
                 "                   --set takes an expression too: 'fire.center.x=sin($T*6)*0.3',\n"
                 "                   'box1.sizex=ch(\"../base/sizex\")*2', 'fire.center={0, $F*0.01, 0}'\n"
                 "  prototype sim --list    the examples it carries: campfire, smoke, ...\n"
                 "  prototype pyro   OUT.png [--preset EXAMPLE] [...]   sim with an example (fire: campfire)\n"
                 "  prototype cook   NETWORK.pgsim|EXAMPLE OUT.obj|OUT.ply|OUT.vdb|- [--node NODE]\n"
                 "                   [--set NODE.PARAM=VALUE]... [--frame N] [--frames N [--start N]] [--threads N] [--hash]\n"
                 "                   cooks the geometry of the displayed node (or --node) -- no simulation --\n"
                 "                   and writes it by OUT's extension, $F4 in OUT for the frame; '-' writes nothing.\n"
                 "                   Says what it made and how long it took; --hash its content hash, the same\n"
                 "                   on any number of --threads\n"
                 "  prototype usd    FILE.usd|.usda|.usdc|.usdz [--frame N]\n"
                 "                   what a USD file composes to, read by the program's own reader: units, up\n"
                 "                   axis, time codes, layers, the prims as a tree, cameras, and what USD Import\n"
                 "                   makes of it at frame N (1: the stage's first time code)\n"
                 "  prototype help\n");
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
        fs::temp_directory_path() / ("prototype-check-" + std::to_string(std::random_device{}()) + "-" +
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
    if (o.command == "cook") return cook(o);
    if (o.command == "pyro") return pyro(o);
    if (o.command == "usd") return usdInfo(o);
    return render(o, lib);
}

}  // namespace pg::cli
