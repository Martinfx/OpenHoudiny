#include "Editor.h"

#include "imgui.h"
#include "imnodes.h"
#include "misc/cpp/imgui_stdlib.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

namespace fs = std::filesystem;
using namespace pg::shader;

namespace pg::editor {
namespace {

// imnodes identifies pins by int: node id in the high bits, then an output
// flag and the port index. A link is identified by the input it feeds -- an
// input has at most one link.
constexpr int kParamBase = 64;
int pinId(int node, int index, bool output) { return node * 256 + (output ? 128 : 0) + index; }
int pinNode(int pin) { return pin / 256; }
bool pinIsOutput(int pin) { return pin % 256 >= 128; }
int pinIndex(int pin) { return pin % 128; }

ImU32 typeColor(Type t) {
    switch (t) {
        case Type::Float:     return IM_COL32(165, 165, 165, 255);
        case Type::Vec2:      return IM_COL32(110, 200, 130, 255);
        case Type::Vec3:      return IM_COL32(235, 200, 80, 255);
        case Type::Vec4:      return IM_COL32(200, 130, 240, 255);
        case Type::Sampler2D: return IM_COL32(90, 150, 240, 255);
        case Type::Any:       return IM_COL32(240, 240, 240, 255);
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
    if (category == "Output") return IM_COL32(160, 54, 54, 255);
    return IM_COL32(62, 90, 64, 255);  // anything a user library adds
}

ImU32 lighter(ImU32 c) {
    auto ch = [&](int shift) { return std::min(255u, ((c >> shift) & 0xFFu) + 28u); };
    return IM_COL32(ch(IM_COL32_R_SHIFT), ch(IM_COL32_G_SHIFT), ch(IM_COL32_B_SHIFT), 255);
}

bool isIdentifier(const std::string& s) {
    if (s.empty() || !(std::isalpha(static_cast<unsigned char>(s[0])) || s[0] == '_')) return false;
    return std::all_of(s.begin(), s.end(),
                       [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; });
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

/// A compact widget for a number or vector. True when edited.
bool valueWidget(const char* id, Value& v, Type type, bool color, float width) {
    ImGui::PushID(id);
    ImGui::SetNextItemWidth(width);
    bool changed = false;
    const int n = componentCount(type);
    if (color && n == 3) {
        changed = ImGui::ColorEdit3("##v", v.v.data(),
                                    ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_Float);
    } else if (color && n == 4) {
        changed = ImGui::ColorEdit4("##v", v.v.data(),
                                    ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_Float |
                                        ImGuiColorEditFlags_AlphaBar);
    } else if (n == 1) {
        changed = ImGui::DragFloat("##v", v.v.data(), 0.01f, 0.0f, 0.0f, "%.3g");
    } else if (n == 2) {
        changed = ImGui::DragFloat2("##v", v.v.data(), 0.01f, 0.0f, 0.0f, "%.2g");
    } else if (n == 3) {
        changed = ImGui::DragFloat3("##v", v.v.data(), 0.01f, 0.0f, 0.0f, "%.2g");
    } else if (n == 4) {
        changed = ImGui::DragFloat4("##v", v.v.data(), 0.01f, 0.0f, 0.0f, "%.2g");
    }
    ImGui::PopID();
    return changed;
}

bool readFile(const std::string& path, std::string& out) {
    std::ifstream in(path);
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

Editor::Editor(const gl::Api& gl, std::vector<std::string> libraryFiles, std::string examplesDir)
    : libraryFiles_(std::move(libraryFiles)), examplesDir_(std::move(examplesDir)), preview_(gl) {
    reloadLibrary();
    newGraph();
}

// --- model ---------------------------------------------------------------------

void Editor::reloadLibrary() {
    NodeLibrary lib = NodeLibrary::withBuiltins();
    std::string problems;
    for (const auto& file : libraryFiles_) {
        std::string error;
        if (!lib.loadFile(file, error)) problems += (problems.empty() ? "" : "; ") + error;
    }
    library_ = std::move(lib);
    compiledRevision_ = ~0ull;
    if (!problems.empty()) setStatus(problems, true);
    else setStatus("library: " + std::to_string(library_.size()) + " nodes");
}

void Editor::newGraph() {
    graph_ = ShaderGraph();
    const int color = graph_.addNode("color", 40, 60, &library_);
    const int lit = graph_.addNode("lambert", 260, 60, &library_);
    const int out = graph_.addNode("surface_output", 500, 60, &library_);
    graph_.connect(color, "color", lit, "color", library_);
    graph_.connect(lit, "result", out, "color", library_);
    path_.clear();
    savedText_ = graph_.save();
    gridPositions_.clear();
    for (const auto& n : graph_.nodes()) gridPositions_[n.id] = {n.x, n.y};
    ImNodes::ClearNodeSelection();
    ImNodes::ClearLinkSelection();
    uniformValues_.clear();
    preview_.resetUniformValues();
    compiledRevision_ = ~0ull;
    frameAll_ = true;
}

bool Editor::open(const std::string& path) {
    std::string text, error;
    ShaderGraph g;
    if (!readFile(path, text)) {
        setStatus(path + ": cannot open", true);
        return false;
    }
    if (!ShaderGraph::load(text, g, error)) {
        setStatus(path + ": " + error, true);
        return false;
    }
    graph_ = std::move(g);
    path_ = path;
    savedText_ = graph_.save();
    gridPositions_.clear();
    for (const auto& n : graph_.nodes()) gridPositions_[n.id] = {n.x, n.y};
    ImNodes::ClearNodeSelection();
    ImNodes::ClearLinkSelection();
    uniformValues_.clear();
    preview_.resetUniformValues();
    compiledRevision_ = ~0ull;
    frameAll_ = true;
    setStatus("opened " + path);
    return true;
}

bool Editor::save(const std::string& path) {
    std::ofstream out(path);
    if (!out) {
        setStatus(path + ": cannot write", true);
        return false;
    }
    savedText_ = graph_.save();
    out << savedText_;
    path_ = path;
    setStatus("saved " + path);
    return true;
}

void Editor::exportShaders(const std::string& dir) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    const std::string stem = path_.empty() ? "shader" : fs::path(path_).stem().string();
    int written = 0;
    for (const Target* t : TargetRegistry::instance().all()) {
        const GeneratedShader s = generate(graph_, library_, *t);
        if (!s.ok()) {
            setStatus("export: the graph has errors", true);
            return;
        }
        for (const auto& f : s.files) {
            std::ofstream((fs::path(dir) / outputFileName(stem, t->name(), f)).string()) << f.text;
            ++written;
        }
    }
    setStatus("exported " + std::to_string(written) + " files to " + dir);
}

void Editor::setCodeTarget(const std::string& name) {
    if (!TargetRegistry::instance().find(name)) {
        setStatus("no target '" + name + "'", true);
        return;
    }
    codeTarget_ = name;
    compiledRevision_ = ~0ull;
}

void Editor::setStatus(std::string message, bool error) {
    status_ = std::move(message);
    statusIsError_ = error;
}

void Editor::recompile() {
    if (graph_.revision() == compiledRevision_) return;
    compiledRevision_ = graph_.revision();

    // The preview always runs GLSL 330; the code view shows any target.
    const Target* gl330 = TargetRegistry::instance().find("glsl330");
    previewShader_ = generate(graph_, library_, *gl330);
    const Target* view = TargetRegistry::instance().find(codeTarget_);
    codeShader_ = view && view != gl330 ? generate(graph_, library_, *view) : previewShader_;

    driverLog_.clear();
    if (previewShader_.ok()) {
        std::string log;
        if (preview_.setProgram(previewShader_.fileFor(Stage::Vertex)->text,
                                previewShader_.fileFor(Stage::Fragment)->text, log)) {
            preview_.setUniforms(previewShader_.uniforms);
        } else {
            driverLog_ = log;  // the generator's fault, not the user's: worth seeing
        }
    }
}

// --- frame -----------------------------------------------------------------------

std::string Editor::title() const {
    return (path_.empty() ? std::string("untitled") : fs::path(path_).filename().string()) +
           (modified_ ? " *" : "") + " - pgshadered";
}

void Editor::frame(float seconds) {
    recompile();
    modified_ = graph_.save() != savedText_;

    ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S)) {
        if (path_.empty()) openPopup_ = "Save graph as";
        else save(path_);
    }
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_O)) openPopup_ = "Open graph";
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_R)) reloadLibrary();

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::Begin("pgshader", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_MenuBar |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar();

    menuBar();
    popups();

    const float statusHeight = ImGui::GetFrameHeightWithSpacing();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float height = avail.y - statusHeight;
    sidePanelWidth_ = std::clamp(sidePanelWidth_, 320.0f, std::max(320.0f, avail.x - 300.0f));
    const float splitter = 6.0f;

    ImGui::BeginChild("canvas_host", ImVec2(avail.x - sidePanelWidth_ - splitter, height),
                      ImGuiChildFlags_Borders);
    canvas();
    ImGui::EndChild();

    ImGui::SameLine(0.0f, 0.0f);
    ImGui::InvisibleButton("splitter", ImVec2(splitter, height));
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if (ImGui::IsItemActive()) sidePanelWidth_ -= io.MouseDelta.x;
    ImGui::SameLine(0.0f, 0.0f);

    ImGui::BeginChild("side", ImVec2(sidePanelWidth_, height));
    sidePanel(seconds);
    ImGui::EndChild();

    statusBar();
    ImGui::End();
}

// --- menus and popups -----------------------------------------------------------

void Editor::menuBar() {
    if (!ImGui::BeginMenuBar()) return;
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("New")) newGraph();
        if (ImGui::MenuItem("Open...", "Ctrl+O")) openPopup_ = "Open graph";
        if (ImGui::MenuItem("Save", "Ctrl+S")) {
            if (path_.empty()) openPopup_ = "Save graph as";
            else save(path_);
        }
        if (ImGui::MenuItem("Save as...")) openPopup_ = "Save graph as";
        ImGui::Separator();
        if (ImGui::MenuItem("Export shaders...")) openPopup_ = "Export shaders";
        ImGui::Separator();
        if (ImGui::MenuItem("Quit")) quit_ = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Examples")) {
        examplesMenu(examplesDir_);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Library")) {
        if (ImGui::MenuItem("Reload", "Ctrl+R")) reloadLibrary();
        if (ImGui::MenuItem("Add library file...")) openPopup_ = "Add library file";
        ImGui::Separator();
        ImGui::TextDisabled("%zu nodes: built-in%s", library_.size(), libraryFiles_.empty() ? "" : " +");
        for (const auto& f : libraryFiles_) ImGui::TextDisabled("  %s", f.c_str());
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help")) {
        ImGui::TextUnformatted("Right click on the canvas      add a node");
        ImGui::TextUnformatted("Drag from a pin into space     add a node, connected");
        ImGui::TextUnformatted("Drag between pins              connect");
        ImGui::TextUnformatted("Ctrl + click a link end        detach");
        ImGui::TextUnformatted("Delete                         remove selected nodes and links");
        ImGui::TextUnformatted("Middle drag, or Alt + drag     pan");
        ImGui::TextUnformatted("F or Home                      frame the whole graph");
        ImGui::TextUnformatted("Drag / wheel on the preview    orbit / zoom");
        ImGui::TextUnformatted("Ctrl+R                         reload the node libraries");
        ImGui::EndMenu();
    }
    ImGui::EndMenuBar();
}

void Editor::examplesMenu(const std::string& dir) {
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
        bool added = false;
        for (const auto& l : libraries) {
            const bool loaded =
                std::find(libraryFiles_.begin(), libraryFiles_.end(), l.string()) != libraryFiles_.end();
            if (loaded) continue;
            libraryFiles_.push_back(l.string());
            added = true;
        }
        if (added) reloadLibrary();
        open(g.string());
    }
    for (const auto& f : folders) {
        if (!ImGui::BeginMenu(f.filename().string().c_str())) continue;
        examplesMenu(f.string());
        ImGui::EndMenu();
    }
}

void Editor::popups() {
    if (openPopup_) {
        pathInput_ = std::string(openPopup_) == "Export shaders"
                         ? (path_.empty() ? "shaders" : (fs::path(path_).parent_path() / "shaders").string())
                         : path_;
        ImGui::OpenPopup(openPopup_);
        openPopup_ = nullptr;
    }
    auto pathPopup = [&](const char* title, const char* button, auto&& action) {
        if (!ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(520.0f);
        const bool enter = ImGui::InputText("##path", &pathInput_, ImGuiInputTextFlags_EnterReturnsTrue);
        if (ImGui::Button(button) || enter) {
            if (!pathInput_.empty()) action(pathInput_);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    };
    pathPopup("Open graph", "Open", [&](const std::string& p) { open(p); });
    pathPopup("Save graph as", "Save", [&](const std::string& p) { save(p); });
    pathPopup("Export shaders", "Export", [&](const std::string& p) { exportShaders(p); });
    pathPopup("Add library file", "Load", [&](const std::string& p) {
        libraryFiles_.push_back(p);
        reloadLibrary();
    });
}

// --- the canvas ------------------------------------------------------------------

void Editor::canvas() {
    const ImVec2 canvasSize = ImGui::GetContentRegionAvail();
    std::set<int> broken;
    for (const auto& e : previewShader_.errors) broken.insert(e.node);

    ImNodes::BeginNodeEditor();
    for (const GraphNode& node : graph_.nodes()) {
        if (auto it = gridPositions_.find(node.id); it != gridPositions_.end()) {
            ImNodes::SetNodeGridSpacePos(node.id, ImVec2(it->second.first, it->second.second));
            gridPositions_.erase(it);
        }
        if (auto it = screenPositions_.find(node.id); it != screenPositions_.end()) {
            ImNodes::SetNodeScreenSpacePos(node.id, ImVec2(it->second.first, it->second.second));
            screenPositions_.erase(it);
        }
        const bool isBroken = broken.count(node.id) > 0;
        if (isBroken) ImNodes::PushColorStyle(ImNodesCol_NodeOutline, IM_COL32(235, 70, 70, 255));
        nodeWidget(node, library_.find(node.type));
        if (isBroken) ImNodes::PopColorStyle();
    }
    for (const Link& l : graph_.links()) {
        const GraphNode* from = graph_.node(l.fromNode);
        const GraphNode* to = graph_.node(l.toNode);
        const NodeDef* fd = from ? library_.find(from->type) : nullptr;
        const NodeDef* td = to ? library_.find(to->type) : nullptr;
        if (!fd || !td || fd->outputIndex(l.fromPort) < 0 || td->inputIndex(l.toPort) < 0) continue;
        const int in = pinId(l.toNode, td->inputIndex(l.toPort), false);
        const Type type = fd->outputs[static_cast<size_t>(fd->outputIndex(l.fromPort))].type;
        ImNodes::PushColorStyle(ImNodesCol_Link, typeColor(type));
        ImNodes::Link(in, pinId(l.fromNode, fd->outputIndex(l.fromPort), true), in);
        ImNodes::PopColorStyle();
    }
    ImNodes::MiniMap(0.14f, ImNodesMiniMapLocation_BottomRight);
    const bool canvasHovered = ImNodes::IsEditorHovered();  // only answers inside the editor
    ImNodes::EndNodeEditor();

    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && !ImGui::GetIO().WantTextInput &&
        (ImGui::IsKeyPressed(ImGuiKey_F, false) || ImGui::IsKeyPressed(ImGuiKey_Home, false))) {
        frameAll_ = true;
    }
    if (frameAll_) {
        frameAll(canvasSize);
        frameAll_ = false;
    }

    // Remember where the nodes are, for saving -- not an edit of the code, so
    // no new revision. Before the events: a node added below is not drawn yet.
    for (const GraphNode& node : graph_.nodes()) {
        const ImVec2 p = ImNodes::GetNodeGridSpacePos(node.id);
        if (GraphNode* n = graph_.node(node.id)) {
            n->x = std::round(p.x);
            n->y = std::round(p.y);
        }
    }

    handleCanvasEvents(canvasHovered);
}

void Editor::frameAll(const ImVec2& canvas) {
    // imnodes has no zoom, so this only pans: a graph that fits is centred, a
    // bigger one shows its top-left corner -- where the inputs are.
    if (graph_.nodes().empty()) return;
    ImVec2 lo(1e9f, 1e9f), hi(-1e9f, -1e9f);
    for (const GraphNode& n : graph_.nodes()) {
        const ImVec2 p = ImNodes::GetNodeGridSpacePos(n.id);
        const ImVec2 size = ImNodes::GetNodeDimensions(n.id);
        lo = ImVec2(std::min(lo.x, p.x), std::min(lo.y, p.y));
        hi = ImVec2(std::max(hi.x, p.x + size.x), std::max(hi.y, p.y + size.y));
    }
    const float margin = 20.0f;
    auto axis = [&](float min, float max, float extent) {
        return max - min <= extent - 2.0f * margin ? (extent - (max - min)) * 0.5f - min : margin - min;
    };
    ImNodes::EditorContextResetPanning(ImVec2(axis(lo.x, hi.x, canvas.x), axis(lo.y, hi.y, canvas.y)));
}

void Editor::nodeWidget(const GraphNode& node, const NodeDef* def) {
    const ImU32 title = def ? categoryColor(def->category) : IM_COL32(120, 40, 40, 255);
    ImNodes::PushColorStyle(ImNodesCol_TitleBar, title);
    ImNodes::PushColorStyle(ImNodesCol_TitleBarHovered, lighter(title));
    ImNodes::PushColorStyle(ImNodesCol_TitleBarSelected, lighter(lighter(title)));
    ImNodes::BeginNode(node.id);

    ImNodes::BeginNodeTitleBar();
    ImGui::TextUnformatted(def ? def->label.c_str() : ("? " + node.type).c_str());
    if (def && !def->description.empty()) ImGui::SetItemTooltip("%s", def->description.c_str());
    ImNodes::EndNodeTitleBar();

    ImGui::PushID(node.id);
    if (!def) {
        ImGui::TextDisabled("not in any loaded library");
    } else {
        // Inputs and params: a label, then a widget filling the column.
        // Outputs are right-aligned to the same edge.
        const float column = 120.0f;
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        for (size_t i = 0; i < def->inputs.size(); ++i) {
            const PortDef& p = def->inputs[i];
            const bool linked = graph_.linkInto(node.id, p.name) != nullptr;
            ImNodes::PushColorStyle(ImNodesCol_Pin, typeColor(p.type));
            ImNodes::BeginInputAttribute(pinId(node.id, static_cast<int>(i), false),
                                         linked ? ImNodesPinShape_CircleFilled : ImNodesPinShape_Circle);
            ImGui::TextUnformatted(p.name.c_str());
            if (!linked) {
                ImGui::SameLine();
                auto it = node.inputs.find(p.name);
                if (it == node.inputs.end() && !p.defaultGlobal.empty()) {
                    ImGui::TextDisabled("$%s", p.defaultGlobal.c_str());
                } else {
                    Value v = it != node.inputs.end() ? it->second : p.defaultValue;
                    const Type shown = p.type == Type::Any ? v.type : p.type;
                    v = v.as(shown);
                    // Room for three or four numbers, unless it is a colour swatch.
                    const float least = componentCount(shown) > 1 && !p.color ? 110.0f : 60.0f;
                    const float width = std::max(least, column - ImGui::CalcTextSize(p.name.c_str()).x);
                    if (valueWidget(p.name.c_str(), v, shown, p.color, width)) {
                        graph_.setInput(node.id, p.name, v);
                    }
                }
            }
            ImNodes::EndInputAttribute();
            ImNodes::PopColorStyle();
        }

        for (size_t j = 0; j < def->params.size(); ++j) {
            const ParamDef& p = def->params[j];
            ImNodes::BeginStaticAttribute(pinId(node.id, kParamBase + static_cast<int>(j), false));
            ImGui::TextDisabled("%s", p.name.c_str());
            ImGui::SameLine();
            auto it = node.params.find(p.name);
            if (p.isString) {
                std::string text = it != node.params.end() ? it->second : p.text;
                ImGui::SetNextItemWidth(std::max(60.0f, column - ImGui::CalcTextSize(p.name.c_str()).x));
                ImGui::PushID(p.name.c_str());
                if (ImGui::InputText("##s", &text, ImGuiInputTextFlags_EnterReturnsTrue) ||
                    ImGui::IsItemDeactivatedAfterEdit()) {
                    if (isIdentifier(text)) graph_.setParam(node.id, p.name, text);
                    else setStatus("'" + text + "' is not a valid name", true);
                }
                ImGui::PopID();
            } else {
                Value v = it != node.params.end() ? parseNumbers(it->second, p.type, p.value) : p.value;
                const float width = std::max(60.0f, column - ImGui::CalcTextSize(p.name.c_str()).x);
                if (valueWidget(p.name.c_str(), v, p.type, p.color, width)) {
                    graph_.setParam(node.id, p.name, numbersText(v, p.type));
                }
            }
            ImNodes::EndStaticAttribute();
        }

        for (size_t o = 0; o < def->outputs.size(); ++o) {
            const OutputDef& out = def->outputs[o];
            ImNodes::PushColorStyle(ImNodesCol_Pin, typeColor(out.type));
            ImNodes::BeginOutputAttribute(pinId(node.id, static_cast<int>(o), true));
            ImGui::Indent(std::max(0.0f, column + spacing - ImGui::CalcTextSize(out.name.c_str()).x));
            ImGui::TextUnformatted(out.name.c_str());
            ImNodes::EndOutputAttribute();
            ImNodes::PopColorStyle();
        }
    }
    ImGui::PopID();

    ImNodes::EndNode();
    ImNodes::PopColorStyle();
    ImNodes::PopColorStyle();
    ImNodes::PopColorStyle();
}

void Editor::handleCanvasEvents(bool canvasHovered) {
    auto portOf = [&](int pin, std::string& port) -> const GraphNode* {
        const GraphNode* n = graph_.node(pinNode(pin));
        const NodeDef* def = n ? library_.find(n->type) : nullptr;
        if (!def) return nullptr;
        const size_t index = static_cast<size_t>(pinIndex(pin));
        if (pinIsOutput(pin)) {
            if (index >= def->outputs.size()) return nullptr;
            port = def->outputs[index].name;
        } else {
            if (index >= def->inputs.size()) return nullptr;
            port = def->inputs[index].name;
        }
        return n;
    };

    int start = 0, end = 0;
    if (ImNodes::IsLinkCreated(&start, &end)) {
        const int out = pinIsOutput(start) ? start : end;
        const int in = pinIsOutput(start) ? end : start;
        std::string outPort, inPort, error;
        const GraphNode* from = portOf(out, outPort);
        const GraphNode* to = portOf(in, inPort);
        if (!pinIsOutput(out) || pinIsOutput(in) || !from || !to) {
            setStatus("connect an output to an input", true);
        } else if (!graph_.connect(from->id, outPort, to->id, inPort, library_, &error)) {
            setStatus(error, true);
        }
    }

    int link = 0;
    if (ImNodes::IsLinkDestroyed(&link)) {
        std::string inPort;
        if (const GraphNode* to = portOf(link, inPort)) graph_.disconnect(to->id, inPort);
    }

    int dropped = 0;
    const ImVec2 mouse = ImGui::GetMousePos();
    if (ImNodes::IsLinkDropped(&dropped, false)) {
        pendingPin_ = dropped;
        addX_ = mouse.x;
        addY_ = mouse.y;
        ImGui::OpenPopup("add_node");
    }
    int hovered = 0;
    if (canvasHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && !ImNodes::IsNodeHovered(&hovered)) {
        pendingPin_ = -1;
        addX_ = mouse.x;
        addY_ = mouse.y;
        ImGui::OpenPopup("add_node");
    }

    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) && !ImGui::GetIO().WantTextInput &&
        ImGui::IsKeyPressed(ImGuiKey_Delete)) {
        if (const int n = ImNodes::NumSelectedLinks(); n > 0) {
            std::vector<int> ids(static_cast<size_t>(n));
            ImNodes::GetSelectedLinks(ids.data());
            for (int id : ids) {
                std::string inPort;
                if (const GraphNode* to = portOf(id, inPort)) graph_.disconnect(to->id, inPort);
            }
        }
        if (const int n = ImNodes::NumSelectedNodes(); n > 0) {
            std::vector<int> ids(static_cast<size_t>(n));
            ImNodes::GetSelectedNodes(ids.data());
            for (int id : ids) graph_.removeNode(id);
        }
        ImNodes::ClearNodeSelection();
        ImNodes::ClearLinkSelection();
    }

    addNodePopup();
}

void Editor::addNodePopup() {
    if (!ImGui::BeginPopup("add_node")) return;
    if (ImGui::IsWindowAppearing()) {
        search_.clear();
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::SetNextItemWidth(220.0f);
    ImGui::InputTextWithHint("##search", "search nodes", &search_);

    const NodeDef* chosen = nullptr;
    if (!search_.empty()) {
        const std::string q = lower(search_);
        const NodeDef* first = nullptr;
        for (const NodeDef* d : library_.nodes()) {
            if (lower(d->label).find(q) == std::string::npos && d->name.find(q) == std::string::npos &&
                lower(d->category).find(q) == std::string::npos) {
                continue;
            }
            if (!first) first = d;
            if (ImGui::MenuItem(d->label.c_str(), d->category.c_str())) chosen = d;
            if (!d->description.empty()) ImGui::SetItemTooltip("%s", d->description.c_str());
        }
        if (first && ImGui::IsKeyPressed(ImGuiKey_Enter)) chosen = first;
    } else {
        for (const auto& category : library_.categories()) {
            if (!ImGui::BeginMenu(category.c_str())) continue;
            for (const NodeDef* d : library_.nodes()) {
                if (d->category != category) continue;
                if (ImGui::MenuItem(d->label.c_str())) chosen = d;
                if (!d->description.empty()) ImGui::SetItemTooltip("%s", d->description.c_str());
            }
            ImGui::EndMenu();
        }
    }

    if (chosen) {
        const int id = graph_.addNode(chosen->name, 0.0f, 0.0f, &library_);
        screenPositions_[id] = {addX_, addY_};
        // Dropped from a pin: wire the new node to it, first compatible port.
        if (pendingPin_ >= 0) {
            const GraphNode* other = graph_.node(pinNode(pendingPin_));
            const NodeDef* od = other ? library_.find(other->type) : nullptr;
            const size_t index = static_cast<size_t>(pinIndex(pendingPin_));
            if (od && pinIsOutput(pendingPin_) && index < od->outputs.size()) {
                for (const auto& in : chosen->inputs) {
                    if (graph_.connect(other->id, od->outputs[index].name, id, in.name, library_)) break;
                }
            } else if (od && !pinIsOutput(pendingPin_) && index < od->inputs.size()) {
                for (const auto& out : chosen->outputs) {
                    if (graph_.connect(id, out.name, other->id, od->inputs[index].name, library_)) break;
                }
            }
        }
        pendingPin_ = -1;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// --- side panel --------------------------------------------------------------------

void Editor::sidePanel(float seconds) {
    previewPanel(seconds);
    uniformsPanel();
    codePanel();
    problemsPanel();
}

void Editor::previewPanel(float seconds) {
    if (!ImGui::CollapsingHeader("Preview", ImGuiTreeNodeFlags_DefaultOpen)) return;
    const float width = ImGui::GetContentRegionAvail().x;
    const float height = std::min(width, 420.0f);
    const float time = animate_ ? seconds : pausedTime_;
    if (width < 16.0f) return;

    // Rendered at twice the size and shown scaled down: cheap anti-aliasing.
    preview_.render(static_cast<int>(width * 2.0f), static_cast<int>(height * 2.0f), time);
    ImGui::Image(ImTextureRef(static_cast<ImTextureID>(preview_.colorTexture())), ImVec2(width, height),
                 ImVec2(0, 1), ImVec2(1, 0));
    if (ImGui::IsItemHovered()) {
        const ImGuiIO& io = ImGui::GetIO();
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            preview_.orbit.yaw -= io.MouseDelta.x * 0.4f;
            preview_.orbit.pitch = std::clamp(preview_.orbit.pitch + io.MouseDelta.y * 0.4f, -85.0f, 85.0f);
        }
        if (io.MouseWheel != 0.0f) {
            const float zoomed = preview_.orbit.distance * (1.0f - io.MouseWheel * 0.08f);
            preview_.orbit.distance = std::clamp(zoomed, 1.5f, 12.0f);
        }
    }

    ImGui::SetNextItemWidth(110.0f);
    if (ImGui::BeginCombo("##mesh", gl::meshName(preview_.mesh()))) {
        for (gl::MeshKind k : gl::kMeshKinds) {
            if (ImGui::Selectable(gl::meshName(k), k == preview_.mesh())) preview_.setMesh(k);
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Checkbox("Animate", &animate_) && !animate_) pausedTime_ = seconds;
    ImGui::SameLine();
    ImGui::TextDisabled("t = %.1f s", time);
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset view")) preview_.orbit = gl::Orbit{};
}

void Editor::uniformsPanel() {
    std::vector<const UniformInfo*> numeric;
    for (const auto& u : previewShader_.uniforms) {
        if (u.type != Type::Sampler2D) numeric.push_back(&u);
    }
    if (numeric.empty()) return;
    if (!ImGui::CollapsingHeader("Uniforms -- change without recompiling", ImGuiTreeNodeFlags_DefaultOpen)) {
        return;
    }
    for (const UniformInfo* u : numeric) {
        auto it = uniformValues_.find(u->name);
        Value v = it != uniformValues_.end() ? it->second.as(u->type) : u->defaultValue;
        ImGui::TextUnformatted(u->name.c_str());
        ImGui::SameLine(150.0f);
        // A vector uniform is usually a colour, but not always: numbers and a
        // swatch, unclamped.
        const ImGuiColorEditFlags flags = ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR;
        bool changed = false;
        ImGui::PushID(u->name.c_str());
        ImGui::SetNextItemWidth(240.0f);
        if (u->type == Type::Vec3) changed = ImGui::ColorEdit3("##u", v.v.data(), flags);
        else if (u->type == Type::Vec4) changed = ImGui::ColorEdit4("##u", v.v.data(), flags);
        else changed = valueWidget("##u", v, u->type, false, 240.0f);
        ImGui::PopID();
        if (changed) {
            uniformValues_[u->name] = v;
            preview_.setUniformValue(u->name, v);
        }
        ImGui::SameLine();
        ImGui::PushID(u->name.c_str());
        if (ImGui::SmallButton("default")) {
            uniformValues_.erase(u->name);
            preview_.setUniformValue(u->name, u->defaultValue);
        }
        ImGui::PopID();
    }
}

void Editor::codePanel() {
    if (!ImGui::CollapsingHeader("Generated code", ImGuiTreeNodeFlags_DefaultOpen)) return;
    const Target* current = TargetRegistry::instance().find(codeTarget_);
    ImGui::SetNextItemWidth(-1.0f);
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
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "The graph has errors -- see Problems.");
        return;
    }
    if (!ImGui::BeginTabBar("files")) return;
    for (const auto& f : codeShader_.files) {
        // The fragment stage is where a graph's work usually is: open on it.
        const bool pick = pickFragmentTab_ && f.extension == ".frag";
        if (!ImGui::BeginTabItem(fileLabel(f), nullptr, pick ? ImGuiTabItemFlags_SetSelected : 0)) continue;
        if (ImGui::SmallButton("Copy")) ImGui::SetClipboardText(f.text.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("%zu lines", static_cast<size_t>(std::count(f.text.begin(), f.text.end(), '\n')));
        ImGui::BeginChild("code", ImVec2(0, std::max(160.0f, ImGui::GetContentRegionAvail().y - 110.0f)),
                          ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
        if (codeFont_) ImGui::PushFont(codeFont_, 0.0f);
        ImGui::TextUnformatted(f.text.c_str());
        if (codeFont_) ImGui::PopFont();
        ImGui::EndChild();
        ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
    pickFragmentTab_ = false;
}

void Editor::problemsPanel() {
    const auto& errors = previewShader_.errors;
    const size_t count = errors.size() + (driverLog_.empty() ? 0 : 1);
    const std::string header =
        (count ? "Problems (" + std::to_string(count) + ")" : std::string("Problems (none)")) + "###problems";
    if (!ImGui::CollapsingHeader(header.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) return;
    for (size_t i = 0; i < errors.size(); ++i) {
        const auto& e = errors[i];
        const GraphNode* n = graph_.node(e.node);
        const NodeDef* def = n ? library_.find(n->type) : nullptr;
        const std::string where = n ? (def ? def->label : n->type) + " #" + std::to_string(n->id) : "graph";
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::Selectable((where + ": " + e.message).c_str()) && n) {
            ImNodes::ClearNodeSelection();
            ImNodes::SelectNode(n->id);
            ImNodes::EditorContextMoveToNode(n->id);
        }
        ImGui::PopID();
    }
    if (!driverLog_.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "driver: %s", driverLog_.c_str());
    }
}

void Editor::statusBar() {
    ImGui::Separator();
    ImGui::TextDisabled("%s%s  |  %zu nodes, %zu links", path_.empty() ? "untitled" : path_.c_str(),
                        modified_ ? " *" : "", graph_.nodes().size(), graph_.links().size());
    if (!status_.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("  |  ");
        ImGui::SameLine();
        if (statusIsError_) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", status_.c_str());
        else ImGui::TextUnformatted(status_.c_str());
    }
}

}  // namespace pg::editor
