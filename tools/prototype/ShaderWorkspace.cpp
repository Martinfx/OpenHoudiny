#include "ShaderWorkspace.h"

#include "pg/gl/Png.h"
#include "pg/io/Video.h"
#include "pg/sim/Cache.h"

#include "misc/cpp/imgui_stdlib.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

namespace fs = std::filesystem;
using namespace pg::shader;

namespace pg::editor {
namespace {

using theme::Icon;

ImU32 typeColor(Type t) {
    switch (t) {
        case Type::Float: return IM_COL32(165, 168, 176, 255);
        case Type::Vec2: return IM_COL32(110, 200, 130, 255);
        case Type::Vec3: return IM_COL32(235, 200, 80, 255);
        case Type::Vec4: return IM_COL32(200, 130, 240, 255);
        case Type::Sampler2D: return IM_COL32(90, 150, 240, 255);
        case Type::Any: return IM_COL32(236, 236, 240, 255);
    }
    return IM_COL32_WHITE;
}

ImU32 categoryColor(const std::string& category) {
    if (category == "Input") return IM_COL32(52, 101, 164, 255);
    if (category == "Math") return IM_COL32(86, 90, 104, 255);
    if (category == "Vector") return IM_COL32(58, 118, 100, 255);
    if (category == "Texture") return IM_COL32(150, 96, 52, 255);
    if (category == "Pattern") return IM_COL32(118, 74, 140, 255);
    if (category == "Lighting") return IM_COL32(156, 124, 36, 255);
    if (category == "Color") return IM_COL32(150, 70, 110, 255);
    if (category == "Effect") return IM_COL32(170, 80, 40, 255);
    if (category == "Output") return IM_COL32(160, 54, 54, 255);
    return IM_COL32(62, 90, 64, 255);  // anything a user library adds
}

Icon categoryIcon(const std::string& category) {
    if (category == "Input") return Icon::Input;
    if (category == "Math" || category == "Vector") return Icon::Math;
    if (category == "Texture") return Icon::Texture;
    if (category == "Pattern") return Icon::Pattern;
    if (category == "Lighting") return Icon::Look;
    if (category == "Effect") return Icon::Source;
    if (category == "Output") return Icon::Output;
    return Icon::Shader;
}

bool isIdentifier(const std::string& s) {
    if (s.empty() || !(std::isalpha(static_cast<unsigned char>(s[0])) || s[0] == '_')) return false;
    return std::all_of(s.begin(), s.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; });
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string numbersText(const Value& v, Type t) {
    std::string s;
    for (int i = 0; i < std::max(1, componentCount(t)); ++i) {
        if (i) s += ' ';
        s += formatFloat(v.v[static_cast<size_t>(i)]);
    }
    return s;
}

Value parseNumbers(const std::string& text, Type t, const Value& fallback) {
    std::istringstream in(text);
    Value v;
    int n = 0;
    for (std::string w; in >> w && n < 4; ++n) {
        if (!parseFloat(w, v.v[static_cast<size_t>(n)])) return fallback;
    }
    if (n == 0) return fallback;
    v.type = vectorType(n);
    return v.as(t);
}

/// A widget for a number or a vector, as wide as the next item. True when edited.
bool valueWidget(const char* id, Value& v, Type type, bool color) {
    ImGui::PushID(id);
    bool changed = false;
    const int n = componentCount(type);
    if (color && n == 3) {
        changed = ImGui::ColorEdit3("##v", v.v.data(), ImGuiColorEditFlags_Float);
    } else if (color && n == 4) {
        changed = ImGui::ColorEdit4("##v", v.v.data(), ImGuiColorEditFlags_Float | ImGuiColorEditFlags_AlphaBar);
    } else if (n == 1) {
        changed = ImGui::DragFloat("##v", v.v.data(), 0.01f, 0.0f, 0.0f, "%.3g");
    } else if (n == 2) {
        changed = ImGui::DragFloat2("##v", v.v.data(), 0.01f, 0.0f, 0.0f, "%.3g");
    } else if (n == 3) {
        changed = ui::dragVector("##v", v.v.data(), 0.01f, "%.3g");
    } else if (n == 4) {
        changed = ImGui::DragFloat4("##v", v.v.data(), 0.01f, 0.0f, 0.0f, "%.3g");
    }
    ImGui::PopID();
    return changed;
}

bool readFile(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::stringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

const char* fileLabel(const ShaderFile& f) {
    if (f.extension == ".vert") return "vertex";
    if (f.extension == ".frag") return "fragment";
    return f.extension.c_str() + (f.extension.empty() ? 0 : 1);
}

}  // namespace

ShaderWorkspace::ShaderWorkspace(const gl::Api& gl, std::vector<std::string> libraryFiles, std::string examplesDir)
    : libraryFiles_(std::move(libraryFiles)), examplesDir_(std::move(examplesDir)), preview_(gl), gl_(gl) {
    reloadLibrary();
    newGraph();
}

// --- model ---------------------------------------------------------------------------------

void ShaderWorkspace::setMessage(std::string message, bool error) {
    message_ = std::move(message);
    messageError_ = error;
}

void ShaderWorkspace::reloadLibrary() {
    NodeLibrary lib = NodeLibrary::withBuiltins();
    std::string problems;
    for (const auto& file : libraryFiles_) {
        std::string error;
        if (!lib.loadFile(file, error)) problems += (problems.empty() ? "" : "; ") + error;
    }
    library_ = std::move(lib);
    compiledRevision_ = ~0ull;
    swatchRevision_ = ~0ull;
    swatchFailed_.clear();
    if (!problems.empty()) setMessage(problems, true);
    else setMessage("Library: " + std::to_string(library_.size()) + " nodes");
}

void ShaderWorkspace::newGraph() {
    graph_ = ShaderGraph();
    const int color = graph_.addNode("color", 0, 40, &library_);
    const int lit = graph_.addNode("lambert", 260, 0, &library_);
    const int out = graph_.addNode("surface_output", 520, 20, &library_);
    graph_.connect(color, "color", lit, "color", library_);
    graph_.connect(lit, "result", out, "color", library_);
    path_.clear();
    markSaved(graph_.save());
    history_.reset(savedText_, graphKey());
    canvas_.setView({});  // no selection, no room made for the last graph's thumbnails
    canvas_.frame();
    clearSwatches();
    uniformValues_.clear();
    preview_.resetUniformValues();
    compiledRevision_ = ~0ull;
    pickMesh_ = true;
}

bool ShaderWorkspace::canOpen(const std::string& path) const { return fs::path(path).extension() == ".pgsg"; }

bool ShaderWorkspace::open(const std::string& path) {
    std::string text, error;
    ShaderGraph g;
    if (!readFile(path, text)) {
        setMessage(path + ": cannot read it", true);
        return false;
    }
    if (!ShaderGraph::load(text, g, error)) {
        setMessage(path + ": " + error, true);
        return false;
    }
    show(std::move(g), path);
    setMessage("Opened " + path);
    return true;
}

void ShaderWorkspace::show(ShaderGraph g, const std::string& path) {
    graph_ = std::move(g);
    path_ = path;
    markSaved(graph_.save());
    history_.reset(savedText_, graphKey());
    canvas_.setView({});
    canvas_.frame();
    clearSwatches();
    uniformValues_.clear();
    preview_.resetUniformValues();
    compiledRevision_ = ~0ull;
    pickMesh_ = true;
    pickFragmentTab_ = true;
}

bool ShaderWorkspace::recover(const std::string& text, const std::string& of, const std::string&) {
    ShaderGraph g;
    std::string error;
    if (!ShaderGraph::load(text, g, error)) {
        setMessage("Cannot recover the graph: " + error, true);
        return false;
    }
    show(std::move(g), of);
    // Not saved: unchanged is what its file holds, if it is there.
    std::string file;
    ShaderGraph saved;
    markSaved(!of.empty() && readFile(of, file) && ShaderGraph::load(file, saved, error) ? saved.save() : std::string());
    setMessage("Recovered " + (of.empty() ? std::string("a graph not saved") : fs::path(of).filename().string()) +
               " as it was autosaved: Ctrl+S keeps it");
    return true;
}

bool ShaderWorkspace::save(const std::string& path) {
    // Written whole beside it, then put in its place: a crash or a full disk
    // halfway leaves the file as it was.
    const std::string text = stateText();
    std::string error;
    if (!sim::writeWhole(path, text, error)) {
        setMessage(error, true);
        return false;
    }
    markSaved(text);
    path_ = path;
    setMessage("Saved " + path);
    return true;
}

void ShaderWorkspace::saveThen(std::function<void()> then) {
    if (path_.empty()) {
        saveAsDialog();
        afterSave_ = std::move(then);
        return;
    }
    if (save(path_) && then) then();
}

void ShaderWorkspace::saveAsDialog() {
    files_.open("Save shader graph", {".pgsg"}, true, path_.empty() ? "untitled.pgsg" : path_);
    fileAction_ = FileAction::SaveAs;
}

void ShaderWorkspace::undo() {
    if (!history_.canUndo()) return;
    restore(history_.undo([this] { return stateText(); }));
    history_.adopt(graphKey());
}

void ShaderWorkspace::redo() {
    if (!history_.canRedo()) return;
    restore(history_.redo([this] { return stateText(); }));
    history_.adopt(graphKey());
}

void ShaderWorkspace::restore(const std::string& state) {
    ShaderGraph g;
    std::string error;
    if (!ShaderGraph::load(state, g, error)) return;
    graph_ = std::move(g);
    compiledRevision_ = ~0ull;
    swatchRevision_ = ~0ull;  // a graph loaded counts its revisions anew
}

void ShaderWorkspace::exportShaders(const std::string& dir) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    const std::string stem = path_.empty() ? "shader" : fs::path(path_).stem().string();
    int written = 0;
    for (const Target* t : TargetRegistry::instance().all()) {
        const GeneratedShader s = generate(graph_, library_, *t);
        if (!s.ok()) {
            setMessage("Export: the graph has errors", true);
            return;
        }
        for (const auto& f : s.files) {
            std::string error;
            if (!sim::writeWhole((fs::path(dir) / outputFileName(stem, t->name(), f)).string(), f.text, error)) {
                setMessage("Export: " + error, true);
                return;
            }
            ++written;
        }
    }
    setMessage("Exported " + std::to_string(written) + " files to " + dir);
}

void ShaderWorkspace::setCodeTarget(const std::string& name) {
    if (!TargetRegistry::instance().find(name)) {
        setMessage("No target '" + name + "'", true);
        return;
    }
    codeTarget_ = name;
    compiledRevision_ = ~0ull;
}

void ShaderWorkspace::savePreviewImage(const std::string& path) {
    if (!preview_.hasProgram()) {
        setMessage("Nothing to save: the graph does not compile", true);
        return;
    }
    const int size = 1024;
    preview_.render(size * 2, size * 2, time_);
    if (gl::writePng(path, size, size, 3, preview_.readPixels(2))) setMessage("Saved " + path);
    else setMessage(path + ": cannot write it", true);
}

void ShaderWorkspace::startValidation() {
    if (validation_.valid()) return;
    cli::CheckTools tools;
    if (!cli::toolAvailable(tools.glslang)) {
        setMessage("Validating needs glslangValidator on the PATH (package glslang-tools)", true);
        return;
    }
    if (cli::toolAvailable("spirv-val")) tools.spirvVal = "spirv-val";
    const std::string name = path_.empty() ? "untitled" : fs::path(path_).stem().string();
    validatedRevision_ = graph_.revision();
    validation_ = std::async(std::launch::async, [graph = graph_, library = library_, tools, name] {
        return cli::checkGraphs({{name, graph}}, library, tools);
    });
    setMessage("Validating with glslangValidator\xe2\x80\xa6");
}

void ShaderWorkspace::pollValidation() {
    if (!validation_.valid() || validation_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    validationReport_ = validation_.get();
    hasValidation_ = true;
    setMessage((validationReport_.ok() ? "Valid: " : "Not valid: ") + validationReport_.summary(), !validationReport_.ok());
}

void ShaderWorkspace::recompile() {
    if (graph_.revision() == compiledRevision_) return;
    compiledRevision_ = graph_.revision();
    const Target* gl330 = TargetRegistry::instance().find("glsl330");
    previewShader_ = generate(graph_, library_, *gl330);
    const Target* view = TargetRegistry::instance().find(codeTarget_);
    codeShader_ = view && view != gl330 ? generate(graph_, library_, *view) : previewShader_;
    driverLog_.clear();
    if (previewShader_.ok()) {
        std::string log;
        if (preview_.setProgram(previewShader_.fileFor(Stage::Vertex)->text, previewShader_.fileFor(Stage::Fragment)->text,
                                log)) {
            preview_.setUniforms(previewShader_.uniforms);
            preview_.setBlend(previewShader_.blend);
        } else {
            driverLog_ = log;
        }
        if (pickMesh_) {
            const bool blends = previewShader_.blend != BlendMode::Opaque;
            if (blends) preview_.setMesh(gl::MeshKind::Billboard);
            else if (preview_.mesh() == gl::MeshKind::Billboard) preview_.setMesh(gl::MeshKind::Sphere);
        }
    }
    pickMesh_ = false;
}

std::string ShaderWorkspace::title() const {
    return (path_.empty() ? std::string("untitled.pgsg") : fs::path(path_).filename().string()) + (modified() ? " *" : "");
}

const std::string& ShaderWorkspace::stateText() const {
    const uint64_t key = graphKey();
    if (!stateTextValid_ || key != stateTextKey_) {
        stateText_ = graph_.save();
        stateTextKey_ = key;
        stateTextValid_ = true;
    }
    return stateText_;
}

void ShaderWorkspace::markSaved(std::string text) {
    savedText_ = std::move(text);
    ++savedGeneration_;
}

bool ShaderWorkspace::modified() const {
    const uint64_t key = graphKey();
    if (key != modifiedKey_ || savedGeneration_ != modifiedGeneration_) {
        modified_ = stateText() != savedText_;
        modifiedKey_ = key;
        modifiedGeneration_ = savedGeneration_;
    }
    return modified_;
}

void ShaderWorkspace::update(float dt) {
    if (animate_) time_ += dt;
    recompile();
    pollValidation();
    history_.track(graphKey(), settled(), ImGui::GetTime(), [this] { return stateText(); });
    updateSwatches();
    job_.step();
    std::string result, where;
    bool failed = false;
    if (job_.takeResult(result, failed, where)) setMessage(result, failed);
}

void ShaderWorkspace::savePreviewVideo(const std::string& path) {
    if (!preview_.hasProgram()) {
        setMessage("Nothing to save: the graph does not compile", true);
        return;
    }
    constexpr int size = 720;
    std::string error;
    const bool started = job_.start(
        path, path, "preview", size, size, 30.0, 0, 149,
        [this](int frame, std::vector<uint8_t>& rgb, std::string&) {
            preview_.render(size * 2, size * 2, static_cast<float>(frame) / 30.0f);
            rgb = preview_.readPixels(2);
            return true;
        },
        error);
    if (!started) setMessage(error, true);
}

void ShaderWorkspace::shortcuts() {
    if (ImGui::GetIO().WantTextInput) return;
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Z)) undo();
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z) ||
        ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Y)) {
        redo();
    }
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S)) {
        if (path_.empty()) saveAsDialog();
        else save(path_);
    }
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_S)) saveAsDialog();
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_O)) {
        unlessUnsaved("opening another graph", [this] {
            files_.open("Open shader graph", {".pgsg"}, false, path_.empty() ? examplesDir_ : path_);
            fileAction_ = FileAction::Open;
        });
    }
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_N)) unlessUnsaved("starting a new graph", [this] { newGraph(); });
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_R)) reloadLibrary();
    if (ImGui::IsKeyPressed(ImGuiKey_F5, false)) startValidation();
}

// --- the network -----------------------------------------------------------------------------

std::vector<CanvasNode> ShaderWorkspace::canvasNodes() const {
    std::map<int, std::string> broken;
    for (const auto& e : previewShader_.errors) {
        if (e.node >= 0) broken[e.node] += (broken[e.node].empty() ? "" : "\n") + e.message;
    }
    std::vector<CanvasNode> out;
    for (const GraphNode& n : graph_.nodes()) {
        const NodeDef* def = library_.find(n.type);
        CanvasNode c;
        c.id = n.id;
        c.title = def ? def->label : "? " + n.type;
        c.color = def ? categoryColor(def->category) : IM_COL32(120, 40, 40, 255);
        c.icon = def ? categoryIcon(def->category) : Icon::Error;
        c.x = n.x;
        c.y = n.y;
        if (def) {
            for (const PortDef& p : def->inputs) {
                c.inputs.push_back({p.name, typeColor(p.type), false, graph_.linkInto(n.id, p.name) ? 1 : 0});
            }
            for (const OutputDef& o : def->outputs) {
                int links = 0;
                for (const Link& l : graph_.links()) links += l.fromNode == n.id && l.fromPort == o.name;
                c.outputs.push_back({o.name, typeColor(o.type), false, links});
            }
            // A name parameter is what the node is about: show it.
            for (const ParamDef& p : def->params) {
                if (!p.isString || p.isEnum()) continue;
                const auto it = n.params.find(p.name);
                c.summary = p.name + ": " + (it != n.params.end() ? it->second : p.text);
                break;
            }
        } else {
            c.problem = 2;
            c.problemText = "Not in any loaded library";
        }
        if (auto it = broken.find(n.id); it != broken.end()) {
            c.problem = 2;
            c.problemText = it->second;
        }
        c.thumbnail = showsThumbnail(n);
        c.thumbnailTexture = c.thumbnail && swatches_ ? static_cast<ImTextureID>(swatches_->texture(n.id)) : 0;
        out.push_back(std::move(c));
    }
    return out;
}

std::vector<CanvasLink> ShaderWorkspace::canvasLinks() const {
    std::vector<CanvasLink> out;
    for (const Link& l : graph_.links()) {
        const GraphNode* from = graph_.node(l.fromNode);
        const GraphNode* to = graph_.node(l.toNode);
        const NodeDef* fd = from ? library_.find(from->type) : nullptr;
        const NodeDef* td = to ? library_.find(to->type) : nullptr;
        if (!fd || !td || fd->outputIndex(l.fromPort) < 0 || td->inputIndex(l.toPort) < 0) continue;
        const int o = fd->outputIndex(l.fromPort);
        out.push_back({l.fromNode, o, l.toNode, td->inputIndex(l.toPort), typeColor(fd->outputs[static_cast<size_t>(o)].type)});
    }
    return out;
}

CanvasModel ShaderWorkspace::canvasModel() {
    auto portName = [this](const PinRef& p) -> std::string {
        const GraphNode* n = graph_.node(p.node);
        const NodeDef* def = n ? library_.find(n->type) : nullptr;
        if (!def || p.pin < 0) return {};
        const size_t i = static_cast<size_t>(p.pin);
        if (p.output) return i < def->outputs.size() ? def->outputs[i].name : "";
        return i < def->inputs.size() ? def->inputs[i].name : "";
    };
    CanvasModel m;
    m.canConnect = [this, portName](const PinRef& from, const PinRef& to, std::string* why) {
        ShaderGraph trial = graph_;
        return trial.connect(from.node, portName(from), to.node, portName(to), library_, why);
    };
    m.connect = [this, portName](const PinRef& from, const PinRef& to) {
        std::string why;
        if (!graph_.connect(from.node, portName(from), to.node, portName(to), library_, &why)) setMessage(why, true);
    };
    m.disconnect = [this, portName](const CanvasLink& l) { graph_.disconnect(l.to, portName({l.to, l.toPin, false})); };
    m.move = [this](int node, float x, float y) {
        if (GraphNode* n = graph_.node(node)) {
            n->x = x;
            n->y = y;
        }
    };
    m.remove = [this](const std::vector<int>& nodes) {
        for (int id : nodes) graph_.removeNode(id);
    };
    m.duplicate = [this](const std::vector<int>& nodes) { duplicate(nodes); };
    m.addMenu = [this](ImVec2 at, const PinRef* pending) { return addMenu(at, pending); };
    m.nodeMenu = [this](int node) { nodeMenu(node); };
    return m;
}

bool ShaderWorkspace::addMenu(ImVec2 at, const PinRef* pending) {
    if (ImGui::IsWindowAppearing()) {
        search_.clear();
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::SetNextItemWidth(theme::px(250.0f));
    ImGui::InputTextWithHint("##search", "Search nodes\xe2\x80\xa6", &search_);
    const NodeDef* chosen = nullptr;
    const NodeDef* first = nullptr;
    const std::string q = lower(search_);
    for (const auto& category : library_.categories()) {
        std::vector<const NodeDef*> defs;
        for (const NodeDef* d : library_.nodes()) {
            if (d->category != category) continue;
            if (!q.empty() && lower(d->label).find(q) == std::string::npos && d->name.find(q) == std::string::npos &&
                lower(d->category).find(q) == std::string::npos) {
                continue;
            }
            defs.push_back(d);
        }
        if (defs.empty()) continue;
        if (q.empty()) {
            // Categories as submenus while nothing is searched: the library is long.
            if (ui::beginIconMenu(categoryIcon(category), theme::shade(categoryColor(category), 0.4f), category.c_str())) {
                for (const NodeDef* d : defs) {
                    if (ImGui::MenuItem(d->label.c_str())) chosen = d;
                    if (!d->description.empty()) ImGui::SetItemTooltip("%s", d->description.c_str());
                }
                ImGui::EndMenu();
            }
        } else {
            ImGui::TextDisabled("%s", category.c_str());
            for (const NodeDef* d : defs) {
                if (!first) first = d;
                if (ImGui::MenuItem(("  " + d->label).c_str())) chosen = d;
                if (!d->description.empty()) ImGui::SetItemTooltip("%s", d->description.c_str());
            }
        }
    }
    if (first && ImGui::IsKeyPressed(ImGuiKey_Enter)) chosen = first;
    if (!chosen) return false;
    const int id = graph_.addNode(chosen->name, std::round(at.x - 24.0f), std::round(at.y - 14.0f), &library_);
    // Dropped from a pin: wire the new node to it, the first port that takes it.
    if (pending) {
        const GraphNode* other = graph_.node(pending->node);
        const NodeDef* od = other ? library_.find(other->type) : nullptr;
        const size_t index = static_cast<size_t>(std::max(pending->pin, 0));
        if (od && pending->output && index < od->outputs.size()) {
            for (const auto& in : chosen->inputs) {
                if (graph_.connect(other->id, od->outputs[index].name, id, in.name, library_)) break;
            }
        } else if (od && !pending->output && index < od->inputs.size()) {
            if (GraphNode* n = graph_.node(id)) n->x -= 220.0f;
            for (const auto& out : chosen->outputs) {
                if (graph_.connect(id, out.name, other->id, od->inputs[index].name, library_)) break;
            }
        }
    }
    canvas_.select(id);
    return true;
}

void ShaderWorkspace::nodeMenu(int id) {
    const GraphNode* n = graph_.node(id);
    if (!n) return;
    const NodeDef* def = library_.find(n->type);
    ImGui::TextDisabled("%s", def ? def->label.c_str() : n->type.c_str());
    ImGui::Separator();
    const std::vector<int> chosen(canvas_.selection().begin(), canvas_.selection().end());
    if (ImGui::MenuItem("Duplicate", "Ctrl+D")) duplicate(chosen);
    if (ImGui::MenuItem("Delete", "Del")) {
        for (int i : chosen) graph_.removeNode(i);
    }
    if (ImGui::MenuItem("Frame", "F")) canvas_.frame(true);
    if (def && (def->isOutput || !def->outputs.empty())) {
        const bool on = !thumbnailsHidden_.count(id);
        if (ImGui::MenuItem("Thumbnail", nullptr, on, thumbnails_)) {
            for (const int i : chosen) {
                if (on) thumbnailsHidden_.insert(i);
                else thumbnailsHidden_.erase(i);
            }
        }
        ImGui::SetItemTooltip(thumbnails_ ? "A swatch in the node: what its first output is, on the preview's mesh"
                                          : "Thumbnails are off: View > Node Thumbnails");
    }
    if (def && !def->description.empty()) {
        ImGui::Separator();
        ImGui::PushTextWrapPos(theme::px(320.0f));
        ImGui::TextDisabled("%s", def->description.c_str());
        ImGui::PopTextWrapPos();
    }
}

void ShaderWorkspace::duplicate(const std::vector<int>& nodes) {
    std::map<int, int> copies;
    for (int id : nodes) {
        const GraphNode* n = graph_.node(id);
        if (!n) continue;
        const GraphNode original = *n;
        const int copy = graph_.addNode(original.type, original.x + 40.0f, original.y + 40.0f, &library_);
        for (const auto& [port, value] : original.inputs) graph_.setInput(copy, port, value);
        for (const auto& [param, text] : original.params) graph_.setParam(copy, param, text);
        copies[id] = copy;
    }
    for (const Link& l : std::vector<Link>(graph_.links())) {
        if (copies.count(l.fromNode) && copies.count(l.toNode)) {
            graph_.connect(copies[l.fromNode], l.fromPort, copies[l.toNode], l.toPort, library_);
        }
    }
    canvas_.clearSelection();
    for (const auto& [from, to] : copies) canvas_.select(to, true);
}

void ShaderWorkspace::network(ImVec2 size) {
    (void)size;
    ui::PanelHeader h = ui::panelHeader(Icon::Network, "Network", "shader");
    if (ui::headerButton(h, "frame", Icon::Search, "Frame the network (F)")) canvas_.frame();
    canvas_.draw("shader_canvas", canvasNodes(), canvasLinks(), canvasModel());
}

// --- parameters --------------------------------------------------------------------------------

void ShaderWorkspace::parameters(ImVec2 size) {
    (void)size;
    const GraphNode* n = graph_.node(canvas_.current());
    const NodeDef* def = n ? library_.find(n->type) : nullptr;
    ui::panelHeader(Icon::Parameters, "Parameters", def ? def->label.c_str() : nullptr);
    ImGui::BeginChild("params", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
    if (n && def) {
        nodeParameters(*n, *def);
    } else {
        ImGui::PushFont(theme::fonts().bold, 0.0f);
        ImGui::TextUnformatted(title().c_str());
        ImGui::PopFont();
        ui::note("Nothing is selected. Click a node to edit its inputs and parameters; Tab or a right click on "
                 "the network adds one. The Surface Output's inputs are what the shader returns.");
        ImGui::Spacing();
        ImGui::Text("%zu nodes, %zu links, library of %zu nodes", graph_.nodes().size(), graph_.links().size(),
                    library_.size());
        uniformsPanel();
    }
    ImGui::EndChild();
}

void ShaderWorkspace::nodeParameters(const GraphNode& node, const NodeDef& def) {
    ImDrawList* d = ImGui::GetWindowDrawList();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float side = ImGui::GetFrameHeight();
    d->AddRectFilled(at, ImVec2(at.x + side, at.y + side), categoryColor(def.category), theme::px(5.0f));
    theme::drawIcon(d, categoryIcon(def.category), ImVec2(at.x + side * 0.5f, at.y + side * 0.5f), side * 0.62f,
                    IM_COL32_WHITE);
    ImGui::Dummy(ImVec2(side, side));
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::PushFont(theme::fonts().bold, 0.0f);
    ImGui::Text("%s", def.label.c_str());
    ImGui::PopFont();
    ImGui::SameLine();
    ImGui::TextDisabled("#%d  %s", node.id, def.name.c_str());
    if (!def.description.empty()) ui::note(def.description.c_str());
    for (const auto& e : previewShader_.errors) {
        if (e.node == node.id) ImGui::TextColored(theme::vec(theme::kRed), "%s", e.message.c_str());
    }

    if (!def.inputs.empty() && ui::section("Inputs")) {
        for (const PortDef& p : def.inputs) {
            ImGui::PushID(p.name.c_str());
            const Link* link = graph_.linkInto(node.id, p.name);
            const auto it = node.inputs.find(p.name);
            ui::rowLabel(p.name.c_str(), it != node.inputs.end() || link, typeName(p.type));
            if (link) {
                const GraphNode* from = graph_.node(link->fromNode);
                const NodeDef* fd = from ? library_.find(from->type) : nullptr;
                const std::string text = "\xe2\x86\x90 " + (fd ? fd->label : std::string("?")) + "." + link->fromPort;
                if (ImGui::Button(text.c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 0.0f)) && from) {
                    canvas_.select(from->id);
                    canvas_.reveal(from->id);
                }
                ImGui::SetItemTooltip("Linked: click to go to the node it comes from");
            } else if (it == node.inputs.end() && !p.defaultGlobal.empty()) {
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("$%s", p.defaultGlobal.c_str());
                ImGui::SameLine();
                if (ImGui::SmallButton("set a value")) graph_.setInput(node.id, p.name, p.defaultValue);
            } else {
                const bool changed = it != node.inputs.end();
                if (ui::resetButton("reset", changed)) {
                    graph_.resetInput(node.id, p.name);
                    ImGui::PopID();
                    continue;
                }
                Value v = changed ? it->second : p.defaultValue;
                const Type shown = p.type == Type::Any ? v.type : p.type;
                v = v.as(shown);
                if (valueWidget("v", v, shown, p.color)) graph_.setInput(node.id, p.name, v);
            }
            ImGui::PopID();
        }
    }
    if (!def.params.empty() && ui::section("Parameters")) {
        for (const ParamDef& p : def.params) {
            ImGui::PushID(p.name.c_str());
            const auto it = node.params.find(p.name);
            ui::rowLabel(p.name.c_str(), it != node.params.end(), nullptr);
            if (ui::resetButton("reset", it != node.params.end())) {
                graph_.resetParam(node.id, p.name);
                ImGui::PopID();
                continue;
            }
            if (p.isEnum()) {
                const std::string current = it != node.params.end() ? it->second : p.text;
                int index = 0;
                std::vector<const char*> labels;
                for (size_t c = 0; c < p.choices.size(); ++c) {
                    labels.push_back(p.choices[c].c_str());
                    if (p.choices[c] == current) index = static_cast<int>(c);
                }
                if (labels.size() <= 3) {
                    if (ui::segmented("e", index, labels)) graph_.setParam(node.id, p.name, p.choices[static_cast<size_t>(index)]);
                } else if (ImGui::BeginCombo("##e", current.c_str())) {
                    for (const auto& choice : p.choices) {
                        if (ImGui::Selectable(choice.c_str(), choice == current)) graph_.setParam(node.id, p.name, choice);
                    }
                    ImGui::EndCombo();
                }
            } else if (p.isString) {
                std::string text = it != node.params.end() ? it->second : p.text;
                if (ImGui::InputText("##s", &text, ImGuiInputTextFlags_EnterReturnsTrue) || ImGui::IsItemDeactivatedAfterEdit()) {
                    if (isIdentifier(text)) graph_.setParam(node.id, p.name, text);
                    else setMessage("'" + text + "' is not a valid name", true);
                }
            } else {
                Value v = it != node.params.end() ? parseNumbers(it->second, p.type, p.value) : p.value;
                if (valueWidget("n", v, p.type, p.color)) graph_.setParam(node.id, p.name, numbersText(v, p.type));
            }
            ImGui::PopID();
        }
    }
}

void ShaderWorkspace::uniformsPanel() {
    std::vector<const UniformInfo*> numeric;
    for (const auto& u : previewShader_.uniforms) {
        if (u.type != Type::Sampler2D) numeric.push_back(&u);
    }
    if (numeric.empty() || !ui::section("Uniforms")) return;
    ui::note("The shader's uniforms, changed live -- nothing is compiled again.");
    for (const UniformInfo* u : numeric) {
        ImGui::PushID(u->name.c_str());
        auto it = uniformValues_.find(u->name);
        Value v = it != uniformValues_.end() ? it->second.as(u->type) : u->defaultValue;
        ui::rowLabel(u->name.c_str(), it != uniformValues_.end(), typeName(u->type));
        if (ui::resetButton("reset", it != uniformValues_.end())) {
            uniformValues_.erase(u->name);
            preview_.setUniformValue(u->name, u->defaultValue);
            v = u->defaultValue;
        }
        const ImGuiColorEditFlags flags = ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR;
        bool changed = false;
        if (u->type == Type::Vec3) changed = ImGui::ColorEdit3("##u", v.v.data(), flags);
        else if (u->type == Type::Vec4) changed = ImGui::ColorEdit4("##u", v.v.data(), flags);
        else changed = valueWidget("u", v, u->type, false);
        if (changed) {
            uniformValues_[u->name] = v;
            preview_.setUniformValue(u->name, v);
        }
        ImGui::PopID();
    }
}

// --- viewport and code ---------------------------------------------------------------------------

void ShaderWorkspace::viewport(ImVec2 size) {
    (void)size;
    ui::PanelHeader h = ui::panelHeader(Icon::Viewport, "Preview", gl::meshName(preview_.mesh()));
    if (ui::headerButton(h, "image", Icon::Camera, "Save the preview as a PNG\xe2\x80\xa6")) {
        files_.open("Save preview image", {".png"}, true,
                    path_.empty() ? "preview.png" : fs::path(path_).replace_extension(".png").string());
        fileAction_ = FileAction::Image;
    }
    if (ui::headerButton(h, "reset", Icon::Viewport, "The camera back where it started")) preview_.orbit = gl::Orbit{};
    if (ui::headerButton(h, "animate", animate_ ? Icon::Pause : Icon::Play, "Animate: $time runs", animate_)) {
        animate_ = !animate_;
    }
    if (ui::headerButton(h, "mesh", Icon::Shader, "The mesh to preview on")) ImGui::OpenPopup("mesh");
    if (ImGui::BeginPopup("mesh")) {
        for (gl::MeshKind k : gl::kMeshKinds) {
            if (ImGui::MenuItem(gl::meshName(k), nullptr, k == preview_.mesh())) preview_.setMesh(k);
        }
        ImGui::EndPopup();
    }
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const int w = std::max(16, static_cast<int>(avail.x)), hh = std::max(16, static_cast<int>(avail.y));
    // Twice the size, shown scaled down: cheap anti-aliasing.
    preview_.render(w * 2, hh * 2, time_);
    const ImVec2 lo = ImGui::GetCursorScreenPos();
    ImGui::Image(ImTextureRef(static_cast<ImTextureID>(preview_.colorTexture())),
                 ImVec2(static_cast<float>(w), static_cast<float>(hh)), ImVec2(0, 1), ImVec2(1, 0));
    ImGui::SetCursorScreenPos(lo);
    ImGui::InvisibleButton("view", ImVec2(static_cast<float>(w), static_cast<float>(hh)),
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsItemActive()) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
            preview_.orbit.distance = std::clamp(preview_.orbit.distance * std::exp(io.MouseDelta.y * 0.006f), 1.5f, 12.0f);
        } else {
            preview_.orbit.yaw -= io.MouseDelta.x * 0.4f;
            preview_.orbit.pitch = std::clamp(preview_.orbit.pitch + io.MouseDelta.y * 0.4f, -85.0f, 85.0f);
        }
    }
    if (ImGui::IsItemHovered() && io.MouseWheel != 0.0f) {
        preview_.orbit.distance = std::clamp(preview_.orbit.distance * (1.0f - io.MouseWheel * 0.08f), 1.5f, 12.0f);
    }
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) preview_.orbit = gl::Orbit{};
    ImDrawList* d = ImGui::GetWindowDrawList();
    const float pad = theme::px(10.0f);
    char text[64];
    std::snprintf(text, sizeof text, "t = %.1f s", static_cast<double>(time_));
    d->AddText(ImVec2(lo.x + pad, lo.y + pad), IM_COL32(235, 236, 240, 200), text);
    if (!previewShader_.ok()) {
        const char* why = "The graph has errors: see Problems";
        const ImVec2 t = ImGui::CalcTextSize(why);
        const ImVec2 c(lo.x + static_cast<float>(w) * 0.5f, lo.y + static_cast<float>(hh) * 0.5f);
        d->AddRectFilled(ImVec2(c.x - t.x * 0.5f - pad, c.y - t.y * 0.5f - pad), ImVec2(c.x + t.x * 0.5f + pad, c.y + t.y * 0.5f + pad),
                         IM_COL32(20, 20, 24, 220), theme::px(6.0f));
        d->AddText(ImVec2(c.x - t.x * 0.5f, c.y - t.y * 0.5f), theme::kRed, why);
    }
}

float ShaderWorkspace::bottomHeight() const { return bottomHeight_ > 0.0f ? bottomHeight_ : theme::px(300.0f); }

void ShaderWorkspace::bottom(ImVec2 size) {
    (void)size;
    const size_t problems = previewShader_.errors.size() + (driverLog_.empty() ? 0 : 1) +
                            (hasValidation_ && !validationReport_.ok() ? 1 : 0);
    const std::string label = problems ? "Problems (" + std::to_string(problems) + ")" : std::string("Problems");
    ui::PanelHeader h = ui::panelHeader(Icon::Code, bottomTab_ == 0 ? "Generated code" : "Problems");
    if (ui::headerButton(h, "problems", problems ? Icon::Warning : Icon::Info, label.c_str(), bottomTab_ == 1)) bottomTab_ = 1;
    if (ui::headerButton(h, "code", Icon::Code, "Generated code", bottomTab_ == 0)) bottomTab_ = 0;
    ImGui::BeginChild("bottom", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
    if (bottomTab_ == 0) codePanel();
    else problemsPanel();
    ImGui::EndChild();
}

void ShaderWorkspace::codePanel() {
    const Target* current = TargetRegistry::instance().find(codeTarget_);
    ImGui::SetNextItemWidth(theme::px(260.0f));
    if (ImGui::BeginCombo("##target", current ? current->description().c_str() : codeTarget_.c_str())) {
        for (const Target* t : TargetRegistry::instance().all()) {
            if (ImGui::Selectable(t->description().c_str(), t->name() == codeTarget_)) {
                codeTarget_ = t->name();
                compiledRevision_ = ~0ull;
            }
        }
        ImGui::EndCombo();
    }
    if (!codeShader_.ok()) {
        ImGui::SameLine();
        ImGui::TextColored(theme::vec(theme::kRed), "The graph has errors -- see Problems.");
        return;
    }
    ImGui::SameLine();
    if (!ImGui::BeginTabBar("files")) return;
    for (const auto& f : codeShader_.files) {
        const bool pick = pickFragmentTab_ && f.extension == ".frag";
        if (!ImGui::BeginTabItem(fileLabel(f), nullptr, pick ? ImGuiTabItemFlags_SetSelected : 0)) continue;
        if (ImGui::SmallButton("Copy")) ImGui::SetClipboardText(f.text.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("%zu lines", static_cast<size_t>(std::count(f.text.begin(), f.text.end(), '\n')));
        ImGui::BeginChild("code", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::PushFont(theme::fonts().mono, 0.0f);
        ImGui::TextUnformatted(f.text.c_str());
        ImGui::PopFont();
        ImGui::EndChild();
        ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
    pickFragmentTab_ = false;
}

void ShaderWorkspace::problemsPanel() {
    const auto& errors = previewShader_.errors;
    if (errors.empty() && driverLog_.empty() && !hasValidation_ && !validation_.valid()) {
        ImGui::TextDisabled("No problems. Tools > Validate (F5) compiles the graph for every target.");
    }
    for (size_t i = 0; i < errors.size(); ++i) {
        const auto& e = errors[i];
        const GraphNode* n = graph_.node(e.node);
        const NodeDef* def = n ? library_.find(n->type) : nullptr;
        const std::string where = n ? (def ? def->label : n->type) + " #" + std::to_string(n->id) : "graph";
        ImGui::PushID(static_cast<int>(i));
        ImGui::PushStyleColor(ImGuiCol_Text, theme::vec(theme::kRed));
        if (ImGui::Selectable((where + ": " + e.message).c_str()) && n) {
            canvas_.select(n->id);
            canvas_.reveal(n->id);
        }
        ImGui::PopStyleColor();
        ImGui::PopID();
    }
    if (!driverLog_.empty()) ImGui::TextColored(theme::vec(theme::kRed), "driver: %s", driverLog_.c_str());
    if (validation_.valid()) {
        ImGui::TextDisabled("validating with glslangValidator\xe2\x80\xa6");
    } else if (hasValidation_) {
        const cli::CheckReport& r = validationReport_;
        ImGui::TextColored(theme::vec(r.ok() ? theme::kGreen : theme::kRed), "%s %s", r.ok() ? "valid:" : "not valid:",
                           r.summary().c_str());
        if (validatedRevision_ != graph_.revision()) ImGui::TextDisabled("(for an earlier version of the graph -- F5)");
        for (const auto& e : r.generationErrors) ImGui::TextWrapped("%s", e.c_str());
        for (const auto& f : r.failures) ImGui::TextWrapped("%s", f.c_str());
        for (const auto& t : r.unchecked) ImGui::TextDisabled("no validator for target '%s'", t.c_str());
    }
}

// --- menus --------------------------------------------------------------------------------------

void ShaderWorkspace::examplesMenu(const std::string& dir) {
    // Graphs, then a submenu per folder. A folder may carry the node libraries
    // its graphs need (*.pgnodes); opening one of its graphs loads them.
    std::vector<fs::path> graphs, folders, libraries;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (e.is_directory()) folders.push_back(e.path());
        else if (e.path().extension() == ".pgsg") graphs.push_back(e.path());
        else if (e.path().extension() == ".pgnodes") libraries.push_back(e.path());
    }
    std::sort(graphs.begin(), graphs.end());
    std::sort(folders.begin(), folders.end());
    std::sort(libraries.begin(), libraries.end());
    if (graphs.empty() && folders.empty()) ImGui::TextDisabled("no examples in %s", dir.c_str());
    for (const auto& g : graphs) {
        if (!ImGui::MenuItem(g.stem().string().c_str())) continue;
        unlessUnsaved("opening the example " + g.stem().string(), [this, g, libraries] {
            bool added = false;
            for (const auto& l : libraries) {
                if (std::find(libraryFiles_.begin(), libraryFiles_.end(), l.string()) != libraryFiles_.end()) continue;
                libraryFiles_.push_back(l.string());
                added = true;
            }
            if (added) reloadLibrary();
            open(g.string());
        });
    }
    for (const auto& f : folders) {
        if (!ImGui::BeginMenu(f.filename().string().c_str())) continue;
        examplesMenu(f.string());
        ImGui::EndMenu();
    }
}

void ShaderWorkspace::fileMenu() {
    if (ImGui::MenuItem("New", "Ctrl+N")) unlessUnsaved("starting a new graph", [this] { newGraph(); });
    if (ImGui::MenuItem("Open\xe2\x80\xa6", "Ctrl+O")) {
        unlessUnsaved("opening another graph", [this] {
            files_.open("Open shader graph", {".pgsg"}, false, path_.empty() ? examplesDir_ : path_);
            fileAction_ = FileAction::Open;
        });
    }
    if (ImGui::BeginMenu("Examples")) {
        examplesMenu(examplesDir_);
        ImGui::EndMenu();
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Save", "Ctrl+S")) {
        if (path_.empty()) saveAsDialog();
        else save(path_);
    }
    if (ImGui::MenuItem("Save As\xe2\x80\xa6", "Ctrl+Shift+S")) saveAsDialog();
    ImGui::Separator();
    if (ImGui::MenuItem("Export Shaders\xe2\x80\xa6")) {
        files_.open("Export shaders into a folder", {}, true, "shaders");
        fileAction_ = FileAction::Export;
    }
    if (ImGui::MenuItem("Save Preview Image\xe2\x80\xa6")) {
        files_.open("Save preview image", {".png"}, true, "preview.png");
        fileAction_ = FileAction::Image;
    }
    if (ImGui::MenuItem("Save Preview Video\xe2\x80\xa6")) {
        const std::vector<std::string> kinds = io::videoExtensions();
        files_.open("Save preview video: five seconds of $time", kinds, true, "preview" + kinds.front());
        fileAction_ = FileAction::Video;
    }
    ImGui::SetItemTooltip("The preview animated, $time from 0 to 5 s, 720 x 720 at 30 fps");
}

void ShaderWorkspace::editMenu() {
    if (ImGui::MenuItem("Undo", "Ctrl+Z", false, history_.canUndo())) undo();
    if (ImGui::MenuItem("Redo", "Ctrl+Shift+Z", false, history_.canRedo())) redo();
    ImGui::Separator();
    const std::vector<int> chosen(canvas_.selection().begin(), canvas_.selection().end());
    if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, !chosen.empty())) duplicate(chosen);
    if (ImGui::MenuItem("Delete", "Del", false, !chosen.empty())) {
        for (int id : chosen) graph_.removeNode(id);
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Arrange", "L")) canvas_.arrange();
    if (ImGui::MenuItem("Frame Network", "F")) canvas_.frame();
}

void ShaderWorkspace::menus() {
    if (ImGui::BeginMenu("View")) {
        if (ImGui::MenuItem("Frame the Network", "F")) canvas_.frame();
        if (ImGui::MenuItem("Node Thumbnails", nullptr, thumbnails_)) thumbnails_ = !thumbnails_;
        ImGui::SetItemTooltip("A swatch in each node of what its first output is, on the preview's mesh. A "
                              "node's menu hides its own.");
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Library")) {
        if (ImGui::MenuItem("Reload", "Ctrl+R")) reloadLibrary();
        if (ImGui::MenuItem("Add Library File\xe2\x80\xa6")) {
            files_.open("Add a node library", {".pgnodes"}, false, examplesDir_);
            fileAction_ = FileAction::Library;
        }
        ImGui::Separator();
        ImGui::TextDisabled("%zu nodes: built-in%s", library_.size(), libraryFiles_.empty() ? "" : " +");
        for (const auto& f : libraryFiles_) ImGui::TextDisabled("  %s", f.c_str());
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Tools")) {
        const bool running = validation_.valid();
        if (ImGui::MenuItem(running ? "Validating\xe2\x80\xa6" : "Validate with glslangValidator", "F5", false, !running)) {
            startValidation();
            bottomTab_ = 1;
        }
        ImGui::SetItemTooltip("Compiles the graph for every target, like `prototype check`.");
        ImGui::EndMenu();
    }
}

void ShaderWorkspace::helpMenu() {
    ui::keysHelp({{"", "Network"},
                  {"Tab, right click", "add a node"},
                  {"Drag from a pin", "link; into space: add a node, linked"},
                  {"Drag a linked input", "move the link, or drop it"},
                  {"Wheel, middle drag", "zoom, pan"},
                  {"F  Del  Ctrl+D", "frame, delete, duplicate"},
                  {"", "Preview and code"},
                  {"Drag, wheel on the preview", "orbit, zoom"},
                  {"F5", "validate with glslangValidator"},
                  {"Ctrl+R", "reload the node libraries"}},
                 {"prototype list | gen | check | render ...", "prototype help"});
}

void ShaderWorkspace::popups() {
    job_.draw();
    std::string chosen;
    if (!files_.draw(chosen)) {
        if (!files_.isOpen()) afterSave_ = nullptr;  // no file picked: what waited for it does not happen
        return;
    }
    switch (fileAction_) {
        case FileAction::Open: open(chosen); break;
        case FileAction::SaveAs:
            if (save(chosen) && afterSave_) {
                const std::function<void()> then = std::move(afterSave_);
                afterSave_ = nullptr;
                fileAction_ = FileAction::None;
                then();
                return;
            }
            break;
        case FileAction::Export: exportShaders(chosen); break;
        case FileAction::Image: savePreviewImage(chosen); break;
        case FileAction::Video: savePreviewVideo(chosen); break;
        case FileAction::Library:
            libraryFiles_.push_back(chosen);
            reloadLibrary();
            break;
        case FileAction::None: break;
    }
    fileAction_ = FileAction::None;
}

// --- thumbnails: the swatches -------------------------------------------------------------

bool ShaderWorkspace::showsThumbnail(const GraphNode& n) const {
    if (!thumbnails_ || thumbnailsHidden_.count(n.id)) return false;
    const NodeDef* def = library_.find(n.type);
    return def && (def->isOutput || !def->outputs.empty());
}

GeneratedShader ShaderWorkspace::swatchShader(int id) const {
    const GraphNode* n = graph_.node(id);
    const NodeDef* def = n ? library_.find(n->type) : nullptr;
    if (!def) return {};
    if (def->isOutput) return previewShader_;  // the graph's own: what the preview shows
    // The graph with the node's first output -- the first an output can take
    // -- as the colour, nothing moving the vertices, drawn opaque.
    ShaderGraph g = graph_;
    int out = 0;
    for (const GraphNode& m : g.nodes()) {
        const NodeDef* d = library_.find(m.type);
        if (d && d->isOutput) out = m.id;
    }
    if (!out) out = g.addNode("surface_output", 0.0f, 0.0f, &library_);
    const NodeDef* outDef = g.node(out) ? library_.find(g.node(out)->type) : nullptr;
    if (!outDef) return {};
    for (const PortDef& p : outDef->inputs) g.disconnect(out, p.name);
    if (outDef->param("blend")) g.setParam(out, "blend", "opaque");
    const PortDef* colour = nullptr;
    for (const PortDef& p : outDef->inputs) {
        if (p.stage == Stage::Fragment) colour = &p;
    }
    if (!colour) return {};
    for (const OutputDef& o : def->outputs) {
        if (g.connect(id, o.name, out, colour->name, library_)) {
            return generate(g, library_, *TargetRegistry::instance().find("glsl330"));
        }
    }
    return {};
}

void ShaderWorkspace::clearSwatches() {
    if (swatches_) swatches_->clear();
    thumbnailsHidden_.clear();
    swatchFailed_.clear();
    swatchRevision_ = ~0ull;
}

void ShaderWorkspace::updateSwatches() {
    std::set<int> alive;
    for (const GraphNode& n : graph_.nodes()) alive.insert(n.id);
    if (swatches_) swatches_->keep(alive);
    for (auto it = swatchFailed_.begin(); it != swatchFailed_.end();) {
        it = alive.count(it->first) ? std::next(it) : swatchFailed_.erase(it);
    }
    const std::vector<int> shown = canvas_.thumbnailsShown();
    if (!thumbnails_ || shown.empty()) return;
    if (!swatchRenderer_) {
        swatchRenderer_ = std::make_unique<gl::PreviewRenderer>(gl_);
        swatches_ = std::make_unique<Thumbnails>(gl_);
    }
    // What every swatch shows: the graph, as its text says -- a graph
    // loaded counts its revisions anew -- and the preview's mesh. The
    // sliders' values change them as often as the swatches may be drawn.
    if (graph_.revision() != swatchRevision_) {
        swatchRevision_ = graph_.revision();
        const std::string text = graph_.save();
        swatchKey_ = mixKey(1469598103934665603ull, text.data(), text.size());
    }
    const uint64_t key = mixKey(swatchKey_, preview_.mesh());
    uint64_t live = 1469598103934665603ull;
    for (const auto& [name, value] : uniformValues_) {
        live = mixKey(live, name.data(), name.size());
        live = mixKey(live, value.type);
        live = mixKey(live, value.v);
    }
    const double now = ImGui::GetTime();
    struct Due {
        int urgency;
        double drawn;
        size_t order;
        int node;
    };
    std::vector<Due> due;
    for (size_t i = 0; i < shown.size(); ++i) {
        const GraphNode* n = graph_.node(shown[i]);
        if (!n || !showsThumbnail(*n)) continue;
        if (const auto f = swatchFailed_.find(n->id); f != swatchFailed_.end() && f->second == key) continue;
        if (const int urgency = swatches_->stale(n->id, key, live, now)) {
            due.push_back({urgency, swatches_->drawnAt(n->id), i, n->id});
        }
    }
    std::sort(due.begin(), due.end(), [](const Due& a, const Due& b) {
        if (a.urgency != b.urgency) return a.urgency < b.urgency;
        if (a.drawn != b.drawn) return a.drawn < b.drawn;
        return a.order < b.order;
    });
    const auto start = std::chrono::steady_clock::now();
    auto ms = [](std::chrono::steady_clock::time_point t) {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
    };
    int drawn = 0;
    for (const Due& d : due) {
        if (drawn >= 3 || (drawn > 0 && ms(start) > 8.0)) break;
        const auto t0 = std::chrono::steady_clock::now();
        const GeneratedShader shader = swatchShader(d.node);
        std::string log;
        gl::PreviewRenderer& r = *swatchRenderer_;
        if (!shader.ok() || !shader.fileFor(Stage::Vertex) || !shader.fileFor(Stage::Fragment) ||
            !r.setProgram(shader.fileFor(Stage::Vertex)->text, shader.fileFor(Stage::Fragment)->text, log)) {
            swatchFailed_[d.node] = key;  // not again until the graph changes
            continue;
        }
        r.setUniforms(shader.uniforms);
        r.resetUniformValues();
        for (const auto& [name, value] : uniformValues_) r.setUniformValue(name, value);
        r.setBlend(shader.blend);
        r.setMesh(preview_.mesh());
        // The mesh filling the picture's height: a billboard -- upright,
        // turned to the eye -- seen straight on, a plane from above.
        r.orbit = gl::Orbit{};
        r.orbit.distance = 3.7f;  // the ball, of radius 1, whole
        if (preview_.mesh() == gl::MeshKind::Billboard) {
            r.orbit.pitch = 0.0f;
            r.orbit.distance = 3.2f;
        } else if (preview_.mesh() == gl::MeshKind::Plane) {
            r.orbit.pitch = 55.0f;
        }
        r.render(2 * Thumbnails::kWidth, 2 * Thumbnails::kHeight, time_);
        swatches_->take(d.node, r.colorTexture(), key, live, now, ms(t0));
        ++drawn;
    }
}

std::string ShaderWorkspace::status() const {
    return std::to_string(graph_.nodes().size()) + " nodes  \xc2\xb7  " + std::to_string(graph_.links().size()) +
           " links  \xc2\xb7  " + std::to_string(library_.size()) + " in the library  \xc2\xb7  code: " + codeTarget_;
}

}  // namespace pg::editor
