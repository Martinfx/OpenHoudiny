//
// pgshader: the shader graph from the command line. Headless, like the rest
// of the core -- the editor is one client of the same library, this is another.
//
//   pgshader list   [--markdown] [--library FILE]...
//   pgshader gen    GRAPH.pgsg... [--target NAME|all] [-o DIR] [--library FILE]...
//   pgshader check  [GRAPH.pgsg...] [--nodes | --nodes-from FILE] [--glslang PATH]
//                   [--spirv-val PATH] [--library FILE]...
//   pgshader render GRAPH.pgsg OUT.png [--mesh sphere|torus|cube|plane|billboard] [--size N]
//                   [--time SECONDS] [--yaw DEG] [--pitch DEG] [--library FILE]...
//
// `check` is the proof that the generated code is valid: it compiles every
// graph -- and with --nodes every output of every node in the library, in the
// fragment and in the vertex stage -- for every target with glslangValidator,
// and runs spirv-val on the SPIR-V. --nodes-from checks only the nodes one
// library file defines: what the author of a library wants to know.
// `render` draws a preview with OpenGL through EGL, with no window; it exists
// only when EGL was found at build time.
//
#include "pg/shader/Generator.h"

#ifdef PG_HAVE_EGL
#include "pg/gl/HeadlessContext.h"
#include "pg/gl/Png.h"
#include "pg/gl/Preview.h"
#endif

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;
using namespace pg::shader;

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
    float time = 0.0f, yaw = 30.0f, pitch = 18.0f;
};

int usage() {
    std::fprintf(stderr,
                 "usage:\n"
                 "  pgshader list   [--markdown] [--library FILE]...\n"
                 "  pgshader gen    GRAPH.pgsg... [--target NAME|all] [-o DIR] [--library FILE]...\n"
                 "  pgshader check  [GRAPH.pgsg...] [--nodes | --nodes-from FILE] [--glslang PATH]\n"
                 "                  [--spirv-val PATH] [--library FILE]...\n"
                 "  pgshader render GRAPH.pgsg OUT.png [--mesh sphere|torus|cube|plane|billboard] [--size N]\n"
                 "                  [--time SECONDS] [--yaw DEG] [--pitch DEG] [--library FILE]...\n");
    return 2;
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
        if (a == "--library") { if (!next(v)) return false; o.libraries.push_back(v); }
        else if (a == "--target") { if (!next(o.target)) return false; }
        else if (a == "-o") { if (!next(o.outDir)) return false; }
        else if (a == "--nodes") o.nodes = true;
        else if (a == "--nodes-from") { if (!next(o.nodesFrom)) return false; o.nodes = true; }
        else if (a == "--markdown") o.markdown = true;
        else if (a == "--glslang") { if (!next(o.glslang)) return false; }
        else if (a == "--spirv-val") { if (!next(o.spirvVal)) return false; }
        else if (a == "--mesh") { if (!next(o.mesh)) return false; }
        else if (a == "--size") { if (!next(v)) return false; o.size = std::atoi(v.c_str()); }
        else if (a == "--time") { if (!next(v) || !parseFloat(v, o.time)) return false; }
        else if (a == "--yaw") { if (!next(v) || !parseFloat(v, o.yaw)) return false; }
        else if (a == "--pitch") { if (!next(v) || !parseFloat(v, o.pitch)) return false; }
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

void printErrors(const std::string& what, const ShaderGraph& g, const NodeLibrary& lib,
                 const GeneratedShader& s) {
    for (const auto& e : s.errors) {
        std::string where = "graph";
        if (const GraphNode* n = g.node(e.node)) {
            const NodeDef* def = lib.find(n->type);
            where = "node " + std::to_string(n->id) + " (" + (def ? def->label : n->type) + ")";
        }
        std::fprintf(stderr, "%s [%s]: %s: %s\n", what.c_str(), s.target.c_str(), where.c_str(),
                     e.message.c_str());
    }
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
std::vector<Job> jobsFor(const Options& o, const std::string& label, const GeneratedShader& s,
                         const fs::path& dir) {
    std::vector<Job> jobs;
    const std::string validate = o.spirvVal.empty() ? "" : " && " + o.spirvVal + " --target-env vulkan1.0 ";
    for (const auto& f : s.files) {
        const fs::path file = dir / (s.target + f.extension);
        std::ofstream(file) << f.text;
        if (s.target == "glsl330" || s.target == "gles300") {
            jobs.push_back({label + " " + file.filename().string(), o.glslang + " " + quote(file), file.string()});
        } else if (s.target == "vulkan") {
            const fs::path spv = file.string() + ".spv";
            jobs.push_back({label + " " + file.filename().string(),
                            o.glslang + " -V --target-env vulkan1.0 " + quote(file) + " -o " + quote(spv) +
                                (validate.empty() ? "" : validate + quote(spv)),
                            file.string()});
        } else if (s.target == "hlsl") {
            for (const auto& [stage, entry] : f.entryPoints) {
                const fs::path spv = file.string() + "." + entry + ".spv";
                jobs.push_back({label + " " + file.filename().string() + ":" + entry,
                                o.glslang + " -D -V -e " + entry + " -S " +
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

    const fs::path root = fs::temp_directory_path() / ("pgshader-check-" + std::to_string(std::rand()));
    std::vector<Job> jobs;
    int generationFailures = 0;
    std::vector<std::string> unchecked;
    for (const auto& [name, g] : graphs) {
        for (const Target* t : TargetRegistry::instance().all()) {
            const GeneratedShader s = generate(g, lib, *t);
            if (!s.ok()) {
                printErrors(name, g, lib, s);
                ++generationFailures;
                continue;
            }
            const fs::path dir = root / name;
            fs::create_directories(dir);
            auto js = jobsFor(o, name, s, dir);
            if (js.empty() && std::find(unchecked.begin(), unchecked.end(), t->name()) == unchecked.end()) {
                unchecked.push_back(t->name());
            }
            jobs.insert(jobs.end(), js.begin(), js.end());
        }
    }

    // Each job is its own process: run as many at once as there are cores.
    std::atomic<size_t> next{0};
    std::mutex mu;
    std::vector<std::string> failures;
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
            failures.push_back(jobs[i].label + "\n  " + jobs[i].command + "\n" + ss.str() + "  source: " +
                               jobs[i].source);
        }
    };
    const unsigned n = std::max(1u, std::thread::hardware_concurrency());
    std::vector<std::thread> threads;
    for (unsigned i = 0; i < n; ++i) threads.emplace_back(worker, i);
    for (auto& t : threads) t.join();

    std::sort(failures.begin(), failures.end());
    for (const auto& f : failures) std::fprintf(stderr, "FAIL %s\n", f.c_str());
    std::printf("checked %zu graphs x %zu targets: %zu compiler runs, %zu failed, %d did not generate\n",
                graphs.size(), TargetRegistry::instance().all().size(), jobs.size(), failures.size(),
                generationFailures);
    for (const auto& t : unchecked) std::printf("  (no validator known for target '%s')\n", t.c_str());
    if (failures.empty() && generationFailures == 0) {
        fs::remove_all(root);
        return 0;
    }
    std::printf("generated files kept in %s\n", root.string().c_str());
    return 1;
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

}  // namespace

int main(int argc, char** argv) {
    Options o;
    if (!parseArgs(argc, argv, o)) return usage();
    NodeLibrary lib;
    if (!loadLibrary(o, lib)) return 1;
    if (o.command == "list") return list(o, lib);
    if (o.command == "gen") return gen(o, lib);
    if (o.command == "check") return check(o, lib);
    if (o.command == "render") return render(o, lib);
    return usage();
}
