#include "SimWorkspace.h"

#include "pg/gl/Png.h"

#include "misc/cpp/imgui_stdlib.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace pg::editor {
namespace {

using theme::Icon;

ImU32 categoryColor(const std::string& c) {
    if (c == "Objects") return IM_COL32(70, 98, 150, 255);
    if (c == "Sources") return IM_COL32(178, 86, 44, 255);
    if (c == "Forces") return IM_COL32(38, 124, 134, 255);
    if (c == "Simulation") return IM_COL32(112, 78, 160, 255);
    if (c == "Render") return IM_COL32(58, 128, 80, 255);
    return IM_COL32(110, 60, 60, 255);
}

Icon categoryIcon(const std::string& c) {
    if (c == "Objects") return Icon::Collider;
    if (c == "Sources") return Icon::Source;
    if (c == "Forces") return Icon::Force;
    if (c == "Simulation") return Icon::Solver;
    return Icon::Look;
}

Icon typeIcon(const sim::NodeType* t) {
    if (!t) return Icon::Error;
    const std::string name = t->name;
    if (name == "output") return Icon::Output;
    if (name == "water_source") return Icon::Drop;
    if (name == "rain") return Icon::Rain;
    if (name == "camera") return Icon::Camera;
    return categoryIcon(t->category);
}

/// Water sources are blue, among the orange of fire; the rain is grey-blue.
ImU32 typeColor(const sim::NodeType& t) {
    if (std::string(t.name) == "water_source") return IM_COL32(40, 108, 172, 255);
    if (std::string(t.name) == "rain") return IM_COL32(88, 106, 140, 255);
    if (std::string(t.name) == "camera") return IM_COL32(96, 98, 108, 255);
    return categoryColor(t.category);
}

ImU32 pinColor(sim::PinType t) {
    switch (t) {
        case sim::PinType::Source: return IM_COL32(240, 142, 60, 255);
        case sim::PinType::Force: return IM_COL32(70, 200, 215, 255);
        case sim::PinType::Collider: return IM_COL32(120, 155, 240, 255);
        case sim::PinType::Gas: return IM_COL32(200, 130, 235, 255);
        case sim::PinType::Look: return IM_COL32(110, 210, 135, 255);
        case sim::PinType::Water: return IM_COL32(64, 170, 250, 255);
        case sim::PinType::Liquid: return IM_COL32(40, 120, 230, 255);
        case sim::PinType::Camera: return IM_COL32(205, 208, 216, 255);
    }
    return IM_COL32_WHITE;
}

std::string number(float v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%g", static_cast<double>(v));
    return buf;
}

/// "fuel 14 · heat 1": what a node does, at a glance.
std::string summaryOf(const sim::Network& net, const sim::Node& n, const sim::Compiled& c) {
    auto v = [&](const char* p) { return net.value(n.id, p); };
    const std::string dot = " \xc2\xb7 ";
    const std::string& t = n.type;
    const std::string times = " \xc3\x97 ";
    auto shapeOf = [&]() { return std::string(sim::shapeName(static_cast<sim::Shape>(static_cast<int>(v("shape"))))); };
    if (t == "pyro_source") {
        std::string s = v("shape") != 0.0f ? shapeOf() : "";
        for (const char* p : {"fuel", "smoke", "heat"}) {
            if (v(p) > 0.0f) s += (s.empty() ? "" : dot) + std::string(p) + " " + number(v(p));
        }
        if (s.empty() || s == shapeOf()) s += (s.empty() ? "" : dot) + std::string("adds nothing");
        const int motion = static_cast<int>(v("motion"));
        if (motion == 1) s += dot + "circles";
        if (motion == 2) s += dot + "sways";
        if (v("end") > v("start")) s += dot + number(v("start")) + "\xe2\x80\x93" + number(v("end")) + " s";
        return s;
    }
    if (t == "turbulence") return "strength " + number(v("strength")) + dot + number(v("scale")) + " m";
    if (t == "wind") return number(v("speed")) + " m/s" + (v("gusts") > 0.0f ? dot + "gusts " + number(v("gusts")) : "");
    if (t == "vortex") {
        std::string s = number(v("speed")) + " m/s round";
        if (v("lift") != 0.0f) s += dot + "lift " + number(v("lift"));
        return s;
    }
    if (t == "attractor" || t == "drag") return "strength " + number(v("strength"));
    if (t == "object" && static_cast<sim::Shape>(static_cast<int>(v("shape"))) == sim::Shape::Mesh) {
        const std::string file = net.text(n.id, "file");
        return "mesh" + dot + (file.empty() ? std::string("no file") : fs::path(file).filename().string());
    }
    if (t == "object") {
        const sim::ParamValue s = net.param(n.id, "size");
        const bool even = s[0] == s[1] && s[1] == s[2];
        return shapeOf() + dot + (even ? number(s[0]) : number(s[0]) + times + number(s[1]) + times + number(s[2])) + " m";
    }
    if (t == "pyro_solver") {
        sim::Scene scene;
        const sim::ParamValue size = net.param(n.id, "size");
        scene.solver.size = Vec3(size[0], size[1], size[2]);
        scene.solver.resolution = static_cast<int>(v("resolution"));
        const sim::Domain d = scene.sanitized().solver.domain();
        return std::to_string(d.cells[0]) + " \xc3\x97 " + std::to_string(d.cells[1]) + " \xc3\x97 " +
               std::to_string(d.cells[2]) + " cells";
    }
    if (t == "water_source") {
        std::string s = shapeOf() + dot;
        if (static_cast<int>(v("mode")) == 1) {
            const sim::ParamValue jet = net.param(n.id, "velocity");
            s += "flow " + number(std::sqrt(jet[0] * jet[0] + jet[1] * jet[1] + jet[2] * jet[2])) + " m/s";
            if (v("end") > v("start")) s += dot + number(v("start")) + "\xe2\x80\x93" + number(v("end")) + " s";
        } else {
            s += "fill";
            if (v("start") > 0.0f) s += " at " + number(v("start")) + " s";
        }
        return s;
    }
    if (t == "liquid_solver") {
        const sim::ParamValue size = net.param(n.id, "size");
        const sim::Domain d = sim::Domain::ofBox(Vec3(size[0], size[1], size[2]),
                                                 std::clamp(static_cast<int>(v("resolution")), 16, 256));
        return std::to_string(d.cells[0]) + " \xc3\x97 " + std::to_string(d.cells[1]) + " \xc3\x97 " +
               std::to_string(d.cells[2]) + " cells" + dot + (v("closed_sides") != 0.0f ? "tank" : "open");
    }
    if (t == "water_look") return "clear to " + number(v("clarity")) + " m";
    if (t == "rain") {
        std::string s = number(v("rate")) + " /m\xc2\xb2s" + dot + number(v("speed")) + " m/s";
        if (v("end") > v("start")) s += dot + number(v("start")) + "\xe2\x80\x93" + number(v("end")) + " s";
        else if (v("start") > 0.0f) s += dot + "from " + number(v("start")) + " s";
        return s;
    }
    if (t == "output") return std::to_string(static_cast<int>(v("frames"))) + " frames" + dot + number(v("fps")) + " fps";
    if (t == "camera") {
        return number(v("focal")) + " mm" + dot + std::to_string(static_cast<int>(v("width"))) + times +
               std::to_string(static_cast<int>(v("height")));
    }
    (void)c;
    return {};
}

/// A slider's format: digits for the range, and the unit.
std::string formatFor(const sim::ParamDef& d) {
    const float span = d.max - d.min;
    std::string f = span <= 1.01f ? "%.3f" : span <= 20.0f ? "%.2f" : "%.1f";
    if (d.kind == sim::ParamKind::Int) f = "%d";
    if (d.unit && *d.unit) f += std::string(" ") + d.unit;
    return f;
}

std::string helpFor(const sim::ParamDef& d) {
    std::string h = d.help;
    h += "\n\n";
    h += d.name;
    if (d.kind == sim::ParamKind::Float || d.kind == sim::ParamKind::Int) {
        h += "   " + number(d.min) + " \xe2\x80\xa6 " + number(d.max);
        if (d.unit && *d.unit) h += std::string(" ") + d.unit;
    }
    return h;
}

bool readFile(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::stringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

}  // namespace

SimWorkspace::SimWorkspace(const gl::Api& gl, bool synchronous)
    : renderer_(gl), runner_(std::make_unique<SimRunner>(synchronous)), synchronous_(synchronous) {
    if (!renderer_.init(rendererLog_)) rendererLog_ = "The driver rejected the volume shader:\n" + rendererLog_;
    else rendererLog_.clear();
    if (!openExample("campfire")) newNetwork();
}

// --- files --------------------------------------------------------------------------------

std::string SimWorkspace::title() const {
    const std::string name = !path_.empty() ? fs::path(path_).filename().string()
                             : !example_.empty() ? example_ + " (example)"
                                                 : std::string("untitled.pgsim");
    return name + (modified() ? " *" : "");
}

bool SimWorkspace::modified() const { return net_.save() != savedText_; }

bool SimWorkspace::canOpen(const std::string& path) const { return fs::path(path).extension() == ".pgsim"; }

std::string SimWorkspace::folder() const {
#ifdef PG_SIM_EXAMPLES_DIR
    if (!example_.empty()) return PG_SIM_EXAMPLES_DIR;
#endif
    return path_.empty() ? std::string() : fs::path(path_).parent_path().string();
}

void SimWorkspace::load(const sim::Network& net, const std::string& path, const std::string& example) {
    net_ = net;
    path_ = path;
    example_ = example;
    savedText_ = net_.save();
    history_.reset(savedText_);
    canvas_.clearSelection();
    canvas_.frame();
    compiledRevision_ = ~0ull;
    recompile();
    current_ = 1;
    playing_ = true;
    throughCamera_ = false;
    framed_ = false;
    viewDirty_ = true;
}

bool SimWorkspace::open(const std::string& path) {
    std::string text, error;
    if (!readFile(path, text)) {
        setMessage(path + ": cannot read it", true);
        return false;
    }
    sim::Network net;
    std::vector<std::string> warnings;
    if (!sim::Network::load(text, net, error, &warnings)) {
        setMessage(path + ": " + error, true);
        return false;
    }
    load(net, path, "");
    if (!warnings.empty()) setMessage(path + ": " + warnings.front(), true);
    else setMessage("Opened " + path);
    return true;
}

bool SimWorkspace::openExample(const std::string& name) {
    sim::Network net;
    if (!sim::Network::example(name, net)) return false;
    load(net, "", name);
    setMessage("Example " + name + ": File > Save As keeps your changes");
    return true;
}

void SimWorkspace::newNetwork() {
    sim::Network net;
    const int source = net.add("pyro_source", 0, 0);
    net.setParam(source, "smoke", "4");
    net.setParam(source, "heat", "2");
    const int solver = net.add("pyro_solver", 300, 20);
    const int look = net.add("volume_look", 560, 20);
    const int out = net.add("output", 800, 20);
    net.connect(source, "source", solver, "sources");
    net.connect(solver, "gas", look, "gas");
    net.connect(look, "look", out, "look");
    load(net, "", "");
    setMessage("A new network: a source, the solver, a look, the output");
}

bool SimWorkspace::save(const std::string& path) {
    std::ofstream out(path, std::ios::binary);
    const std::string text = net_.save();
    if (!out || !(out << text)) {
        setMessage(path + ": cannot write it", true);
        return false;
    }
    path_ = path;
    example_.clear();
    savedText_ = text;
    setMessage("Saved " + path);
    return true;
}

void SimWorkspace::setMessage(std::string message, bool error) {
    message_ = std::move(message);
    messageError_ = error;
}

void SimWorkspace::restore(const std::string& state) {
    sim::Network net;
    std::string error;
    if (!sim::Network::load(state, net, error)) return;
    net_ = net;
    compiledRevision_ = ~0ull;
}

void SimWorkspace::undo() {
    if (!history_.canUndo()) return;
    restore(history_.undo());
    setMessage("Undone");
}

void SimWorkspace::redo() {
    if (!history_.canRedo()) return;
    restore(history_.redo());
    setMessage("Redone");
}

// --- each frame -----------------------------------------------------------------------------

void SimWorkspace::recompile() {
    if (net_.revision() == compiledRevision_) return;
    compiledRevision_ = net_.revision();
    const sim::Look before = compiled_.look;
    compiled_ = net_.compile(folder());
    if (compiled_.ok) runner_->set(compiled_.world, compiled_.frames);
    if (!(compiled_.look == before)) viewDirty_ = true;
    renderer_.look = compiled_.look;
    renderer_.setSolids(compiled_.solids);
    current_ = std::clamp(current_, 1, std::max(1, compiled_.frames));
}

std::shared_ptr<const sim::Frame> SimWorkspace::frameToShow() const {
    // The frame at the play head -- or, while the simulation has not got
    // there yet, the latest before it.
    const int cached = runner_->cached();
    if (cached == 0) return nullptr;
    return runner_->frame(std::min(current_, cached));
}

void SimWorkspace::update(float dt) {
    recompile();
    history_.track(net_.save(), settled());
    if (synchronous_) runner_->step();

    // Playback at the network's frame rate, never past what is simulated.
    const int cached = runner_->cached();
    if (playing_ && compiled_.ok) {
        const double frameTime = compiled_.world.timeStep;
        clock_ += synchronous_ ? frameTime : static_cast<double>(dt);
        while (clock_ >= frameTime) {
            clock_ -= frameTime;
            if (current_ < compiled_.frames && current_ < cached) {
                ++current_;
            } else if (current_ >= compiled_.frames && loop_ && cached >= compiled_.frames && !synchronous_) {
                current_ = 1;
            } else {
                clock_ = 0.0;  // waiting for the simulation, or at the end
                break;
            }
        }
    } else {
        clock_ = 0.0;
    }

    // The frame on screen. While a simulation that started again has no
    // frame yet, the last one stays: dragging a slider does not flicker.
    std::shared_ptr<const sim::Frame> f = frameToShow();
    if (!f && shown_ && (runner_->busy() || gizmo_.dragging())) f = shown_;
    if (f != shown_) {
        shown_ = f;
        if (f) renderer_.setFrame(*f);
        else renderer_.clearFrame();
        viewDirty_ = true;
    }
    if (!shown_) renderer_.setDomain(runner_->domain());
    updateGuides();
}

void SimWorkspace::shortcuts() {
    const ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput) return;
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Z)) undo();
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z) ||
        ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Y)) {
        redo();
    }
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S)) {
        if (path_.empty()) {
            files_.open("Save network", {".pgsim"}, true, (example_.empty() ? "untitled" : example_) + ".pgsim");
            fileAction_ = FileAction::SaveAs;
        } else {
            save(path_);
        }
    }
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_S)) {
        files_.open("Save network", {".pgsim"}, true, path_.empty() ? "untitled.pgsim" : path_);
        fileAction_ = FileAction::SaveAs;
    }
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_O)) {
        files_.open("Open network", {".pgsim"}, false, path_);
        fileAction_ = FileAction::Open;
    }
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_N)) newNetwork();
    if (io.KeyCtrl || io.KeyAlt) return;
    if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) playing_ = !playing_;
    if (ImGui::IsKeyPressed(ImGuiKey_Home, false)) current_ = 1;
    if (ImGui::IsKeyPressed(ImGuiKey_End, false)) current_ = std::max(1, runner_->cached());
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) {
        playing_ = false;
        current_ = std::max(1, current_ - 1);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) {
        playing_ = false;
        current_ = std::min(compiled_.frames, current_ + 1);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_G, false)) {
        guides_ = !guides_;
        guidesRevision_ = ~0ull;
    }
}

// --- the network ---------------------------------------------------------------------------

std::vector<CanvasNode> SimWorkspace::canvasNodes() const {
    std::vector<CanvasNode> out;
    std::map<std::pair<int, std::string>, int> inCount, outCount;
    for (const sim::Link& l : net_.links()) {
        ++outCount[{l.from, l.output}];
        ++inCount[{l.to, l.input}];
    }
    for (const sim::Node& n : net_.nodes()) {
        const sim::NodeType* t = sim::findNodeType(n.type);
        CanvasNode c;
        c.id = n.id;
        c.title = n.name;
        c.subtitle = t ? t->label : n.type;
        c.color = t ? typeColor(*t) : IM_COL32(120, 40, 40, 255);
        c.icon = typeIcon(t);
        c.x = n.x;
        c.y = n.y;
        if (t) {
            for (const sim::PinDef& p : t->inputs) {
                c.inputs.push_back({p.label, pinColor(p.type), p.many, inCount[{n.id, p.name}]});
            }
            for (const sim::PinDef& p : t->outputs) {
                c.outputs.push_back({p.label, pinColor(p.type), false, outCount[{n.id, p.name}]});
            }
        }
        c.bypassed = n.bypass;
        c.dimmed = !compiled_.isActive(n.id);
        for (const sim::Problem& p : compiled_.problems) {
            if (p.node != n.id) continue;
            c.problem = std::max(c.problem, p.level == sim::Problem::Level::Error ? 2 : 1);
            c.problemText += (c.problemText.empty() ? "" : "\n") + p.message;
        }
        c.summary = summaryOf(net_, n, compiled_);
        out.push_back(std::move(c));
    }
    return out;
}

std::vector<CanvasLink> SimWorkspace::canvasLinks() const {
    std::vector<CanvasLink> out;
    for (const sim::Link& l : net_.links()) {
        const sim::Node* a = net_.node(l.from);
        const sim::Node* b = net_.node(l.to);
        const sim::NodeType* ta = a ? sim::findNodeType(a->type) : nullptr;
        const sim::NodeType* tb = b ? sim::findNodeType(b->type) : nullptr;
        if (!ta || !tb) continue;
        int from = -1, to = -1;
        for (size_t i = 0; i < ta->outputs.size(); ++i) {
            if (l.output == ta->outputs[i].name) from = static_cast<int>(i);
        }
        for (size_t i = 0; i < tb->inputs.size(); ++i) {
            if (l.input == tb->inputs[i].name) to = static_cast<int>(i);
        }
        if (from < 0 || to < 0) continue;
        out.push_back({l.from, from, l.to, to, pinColor(ta->outputs[static_cast<size_t>(from)].type)});
    }
    return out;
}

CanvasModel SimWorkspace::canvasModel() {
    auto pinName = [this](const PinRef& p) -> std::string {
        const sim::Node* n = net_.node(p.node);
        const sim::NodeType* t = n ? sim::findNodeType(n->type) : nullptr;
        if (!t) return {};
        const auto& pins = p.output ? t->outputs : t->inputs;
        return p.pin >= 0 && static_cast<size_t>(p.pin) < pins.size() ? pins[static_cast<size_t>(p.pin)].name : "";
    };
    CanvasModel m;
    m.canConnect = [this, pinName](const PinRef& from, const PinRef& to, std::string* why) {
        return net_.canConnect(from.node, pinName(from), to.node, pinName(to), why);
    };
    m.connect = [this, pinName](const PinRef& from, const PinRef& to) {
        std::string why;
        if (!net_.connect(from.node, pinName(from), to.node, pinName(to), &why)) setMessage(why, true);
    };
    m.disconnect = [this, pinName](const CanvasLink& l) {
        net_.disconnect({l.from, pinName({l.from, l.fromPin, true}), l.to, pinName({l.to, l.toPin, false})});
    };
    m.move = [this](int node, float x, float y) {
        if (sim::Node* n = net_.node(node)) {
            n->x = x;
            n->y = y;
        }
    };
    m.remove = [this](const std::vector<int>& nodes) { removeNodes(nodes); };
    m.duplicate = [this](const std::vector<int>& nodes) { duplicate(nodes); };
    m.toggleBypass = [this](const std::vector<int>& nodes) { toggleBypass(nodes); };
    m.addMenu = [this](ImVec2 at, const PinRef* pending) { return addMenu(at, pending); };
    m.nodeMenu = [this](int node) { nodeMenu(node); };
    return m;
}

int SimWorkspace::addNode(const std::string& type, ImVec2 at, const PinRef* pending) {
    const int id = net_.add(type, std::round(at.x - 24.0f), std::round(at.y - 14.0f));
    if (!id) return 0;
    if (pending) {
        const sim::Node* other = net_.node(pending->node);
        const sim::NodeType* ot = other ? sim::findNodeType(other->type) : nullptr;
        const sim::NodeType* nt = sim::findNodeType(type);
        if (ot && nt) {
            const auto& pins = pending->output ? ot->outputs : ot->inputs;
            if (pending->pin >= 0 && static_cast<size_t>(pending->pin) < pins.size()) {
                const sim::PinDef& p = pins[static_cast<size_t>(pending->pin)];
                if (pending->output) {
                    for (const sim::PinDef& in : nt->inputs) {
                        if (net_.connect(other->id, p.name, id, in.name)) break;
                    }
                } else {
                    // Fed from the new node: it goes to the left of the pin.
                    if (sim::Node* n = net_.node(id)) n->x -= 220.0f;
                    for (const sim::PinDef& out : nt->outputs) {
                        if (net_.connect(id, out.name, other->id, p.name)) break;
                    }
                }
            }
        }
    }
    canvas_.select(id);
    return id;
}

bool SimWorkspace::addMenu(ImVec2 at, const PinRef* pending) {
    if (ImGui::IsWindowAppearing()) {
        search_.clear();
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::SetNextItemWidth(theme::px(250.0f));
    ImGui::InputTextWithHint("##search", "Search nodes\xe2\x80\xa6", &search_);
    std::string q = search_;
    for (char& ch : q) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));

    // Only the types that fit the pin a link was dragged from.
    std::optional<sim::PinType> want;
    bool wantOutput = false;
    if (pending) {
        const sim::Node* n = net_.node(pending->node);
        const sim::NodeType* t = n ? sim::findNodeType(n->type) : nullptr;
        const auto* pins = t ? (pending->output ? &t->outputs : &t->inputs) : nullptr;
        if (pins && pending->pin >= 0 && static_cast<size_t>(pending->pin) < pins->size()) {
            want = (*pins)[static_cast<size_t>(pending->pin)].type;
            wantOutput = !pending->output;  // the new node needs the other end
        }
    }
    auto fits = [&](const sim::NodeType& t) {
        if (want) {
            const auto& pins = wantOutput ? t.outputs : t.inputs;
            if (std::none_of(pins.begin(), pins.end(), [&](const sim::PinDef& p) { return p.type == *want; })) {
                return false;
            }
        }
        if (q.empty()) return true;
        std::string label = t.label, category = t.category;
        for (char& ch : label) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        for (char& ch : category) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        return label.find(q) != std::string::npos || category.find(q) != std::string::npos ||
               std::string(t.name).find(q) != std::string::npos;
    };
    const sim::NodeType* first = nullptr;
    const sim::NodeType* chosen = nullptr;
    ImGui::Dummy(ImVec2(0.0f, theme::px(2.0f)));
    for (const char* category : sim::nodeCategories()) {
        std::vector<const sim::NodeType*> types;
        for (const sim::NodeType& t : sim::nodeTypes()) {
            if (category == std::string(t.category) && fits(t)) types.push_back(&t);
        }
        if (types.empty()) continue;
        // The category, in its colour.
        const ImVec2 at0 = ImGui::GetCursorScreenPos();
        ImDrawList* d = ImGui::GetWindowDrawList();
        d->AddCircleFilled(ImVec2(at0.x + theme::px(5.0f), at0.y + ImGui::GetTextLineHeight() * 0.5f), theme::px(3.5f),
                           categoryColor(category));
        ImGui::SetCursorScreenPos(ImVec2(at0.x + theme::px(14.0f), at0.y));
        ImGui::TextDisabled("%s", category);
        for (const sim::NodeType* t : types) {
            if (!first) first = t;
            ImGui::PushID(t->name);
            const ImVec2 p = ImGui::GetCursorScreenPos();
            if (ImGui::Selectable("##t", false, 0, ImVec2(theme::px(250.0f), 0.0f))) chosen = t;
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                ImGui::BeginTooltip();
                ImGui::PushTextWrapPos(theme::px(320.0f));
                ImGui::TextUnformatted(t->help);
                ImGui::PopTextWrapPos();
                ImGui::EndTooltip();
            }
            const float h = ImGui::GetTextLineHeight();
            theme::drawIcon(d, typeIcon(t), ImVec2(p.x + theme::px(12.0f), p.y + h * 0.5f), h * 0.85f,
                            theme::shade(typeColor(*t), 0.35f));
            d->AddText(ImVec2(p.x + theme::px(26.0f), p.y), theme::kText, t->label);
            ImGui::PopID();
        }
        ImGui::Dummy(ImVec2(0.0f, theme::px(3.0f)));
    }
    if (!first) ImGui::TextDisabled("Nothing fits");
    if (first && ImGui::IsKeyPressed(ImGuiKey_Enter)) chosen = first;
    if (chosen) {
        addNode(chosen->name, at, pending);
        return true;
    }
    return false;
}

void SimWorkspace::nodeMenu(int id) {
    const sim::Node* n = net_.node(id);
    if (!n) return;
    const sim::NodeType* t = sim::findNodeType(n->type);
    ImGui::TextDisabled("%s  (%s)", n->name.c_str(), t ? t->label : n->type.c_str());
    ImGui::Separator();
    if (t && t->bypassable && ImGui::MenuItem("Bypass", "B", n->bypass)) toggleBypass({id});
    if (ImGui::MenuItem("Duplicate", "Ctrl+D")) duplicate(std::vector<int>(canvas_.selection().begin(), canvas_.selection().end()));
    if (ImGui::MenuItem("Delete", "Del")) removeNodes(std::vector<int>(canvas_.selection().begin(), canvas_.selection().end()));
    ImGui::Separator();
    if (ImGui::MenuItem("Frame", "F")) canvas_.frame(true);
    if (t) {
        ImGui::Separator();
        ImGui::PushTextWrapPos(theme::px(320.0f));
        ImGui::TextDisabled("%s", t->help);
        ImGui::PopTextWrapPos();
    }
}

void SimWorkspace::duplicate(const std::vector<int>& nodes) {
    std::map<int, int> copies;
    for (int id : nodes) {
        const sim::Node* n = net_.node(id);
        if (!n) continue;
        const sim::Node original = *n;
        const sim::NodeType* type = sim::findNodeType(original.type);
        if (!type) continue;
        // Objects, sources and forces go under the others of their kind; the
        // rest beside the original.
        const std::string category = type->category;
        const bool column = category == "Objects" || category == "Sources" || category == "Forces";
        const ImVec2 at = column ? freeSlot() : ImVec2(original.x + 40.0f, original.y + 40.0f);
        const int copy = net_.add(original.type, at.x, at.y);
        if (!copy) continue;
        net_.rename(copy, net_.uniqueName(original.name));
        for (const auto& [name, value] : original.params) net_.setParam(copy, name, value);
        for (const auto& [name, text] : original.texts) net_.setText(copy, name, text);
        net_.setBypass(copy, original.bypass);
        // In the world too, beside the original rather than inside it.
        const sim::Handles& h = sim::findNodeType(original.type)->handles;
        if (h.center) {
            const float step = h.size ? 1.2f * net_.param(copy, h.size)[0] : h.radius ? 2.2f * net_.value(copy, h.radius) : 0.3f;
            sim::ParamValue c = net_.param(copy, h.center);
            c[0] += step;
            net_.setParam(copy, h.center, c);
        }
        copies[id] = copy;
    }
    // Links among the copies, as among the originals -- and a copy feeds
    // what its original fed through an input that takes many (a solver's
    // colliders, sources, forces), so a copied object is in the way too.
    for (const sim::Link& l : std::vector<sim::Link>(net_.links())) {
        if (!copies.count(l.from)) continue;
        if (copies.count(l.to)) {
            net_.connect(copies[l.from], l.output, copies[l.to], l.input);
            continue;
        }
        const sim::Node* to = net_.node(l.to);
        const sim::NodeType* t = to ? sim::findNodeType(to->type) : nullptr;
        const sim::PinDef* in = t ? t->input(l.input) : nullptr;
        if (in && in->many) net_.connect(copies[l.from], l.output, l.to, l.input);
    }
    canvas_.clearSelection();
    for (const auto& [from, to] : copies) canvas_.select(to, true);
}

void SimWorkspace::removeNodes(const std::vector<int>& nodes) {
    for (int id : nodes) net_.remove(id);
}

void SimWorkspace::toggleBypass(const std::vector<int>& nodes) {
    // All on if any is off; else all off.
    bool any = false;
    for (int id : nodes) {
        const sim::Node* n = net_.node(id);
        const sim::NodeType* t = n ? sim::findNodeType(n->type) : nullptr;
        if (t && t->bypassable && !n->bypass) any = true;
    }
    for (int id : nodes) {
        const sim::Node* n = net_.node(id);
        const sim::NodeType* t = n ? sim::findNodeType(n->type) : nullptr;
        if (t && t->bypassable) net_.setBypass(id, any);
    }
}

void SimWorkspace::network(ImVec2 size) {
    (void)size;
    ui::PanelHeader h = ui::panelHeader(Icon::Network, "Network", "simulation");
    if (ui::headerButton(h, "frame", Icon::Search, "Frame the network (F)")) canvas_.frame();
    if (ui::headerButton(h, "add", Icon::Plus, "Add a node (Tab)")) ImGui::OpenPopup("add_from_header");
    if (ImGui::BeginPopup("add_from_header")) {
        if (addMenu(ImVec2(0.0f, 0.0f), nullptr)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    canvas_.draw("sim_canvas", canvasNodes(), canvasLinks(), canvasModel());
}

// --- parameters -------------------------------------------------------------------------------

void SimWorkspace::parameters(ImVec2 size) {
    (void)size;
    const int id = canvas_.current();
    const sim::Node* n = net_.node(id);
    const sim::NodeType* t = n ? sim::findNodeType(n->type) : nullptr;
    ui::PanelHeader h = ui::panelHeader(Icon::Parameters, "Parameters", t ? t->label : nullptr);
    if (n && t && t->bypassable) {
        if (ui::headerButton(h, "bypass", Icon::Bypass, "Bypass: leave the node out (B)", n->bypass)) toggleBypass({id});
    }
    ImGui::BeginChild("params", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
    if (n && t) nodeParameters(*n, *t);
    else networkOverview();
    ImGui::EndChild();
}

void SimWorkspace::nodeParameters(const sim::Node& node, const sim::NodeType& type) {
    const int id = node.id;
    // The name, and what the node is.
    ImDrawList* d = ImGui::GetWindowDrawList();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float side = ImGui::GetFrameHeight();
    d->AddRectFilled(at, ImVec2(at.x + side, at.y + side), typeColor(type), theme::px(5.0f));
    theme::drawIcon(d, typeIcon(&type), ImVec2(at.x + side * 0.5f, at.y + side * 0.5f), side * 0.62f, IM_COL32_WHITE);
    ImGui::Dummy(ImVec2(side, side));
    ImGui::SameLine();
    if (nameEditNode_ != id) {
        nameEdit_ = node.name;
        nameEditNode_ = id;
    }
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    ImGui::PushFont(theme::fonts().bold, 0.0f);
    if (ImGui::InputText("##name", &nameEdit_, ImGuiInputTextFlags_EnterReturnsTrue) || ImGui::IsItemDeactivatedAfterEdit()) {
        std::string why;
        if (!net_.rename(id, nameEdit_, &why)) {
            setMessage(why, true);
            nameEdit_ = node.name;
        }
    }
    ImGui::PopFont();
    ImGui::SetItemTooltip("The node's name: what --set NAME.param=value calls it");
    ui::note(type.help);

    // What is wrong with it.
    for (const sim::Problem& p : compiled_.problems) {
        if (p.node != id) continue;
        const bool error = p.level == sim::Problem::Level::Error;
        const ImVec2 q = ImGui::GetCursorScreenPos();
        const float h = ImGui::GetTextLineHeight();
        theme::drawIcon(ImGui::GetWindowDrawList(), error ? Icon::Error : Icon::Warning,
                        ImVec2(q.x + h * 0.5f, q.y + h * 0.5f), h * 0.9f, error ? theme::kRed : theme::kYellow);
        ImGui::SetCursorScreenPos(ImVec2(q.x + h * 1.4f, q.y));
        ImGui::PushStyleColor(ImGuiCol_Text, theme::vec(error ? theme::kRed : theme::kYellow));
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(p.message.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
    }
    if (node.bypass) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::vec(IM_COL32(230, 200, 90, 255)));
        ImGui::TextUnformatted("Bypassed: left out of the simulation.");
        ImGui::PopStyleColor();
    }
    if (!compiled_.isActive(id) && !node.bypass) {
        ui::note("Not linked to the Output: it takes no part.");
    }

    // The parameters, section by section, in the order of the table.
    std::vector<std::string> sections;
    for (const sim::ParamDef& p : type.params) {
        if (std::find(sections.begin(), sections.end(), p.section) == sections.end()) sections.push_back(p.section);
    }
    for (const std::string& section : sections) {
        ImGui::PushID(section.c_str());
        if (!ui::section(section.c_str())) {
            ImGui::PopID();
            continue;
        }
        for (const sim::ParamDef& p : type.params) {
            if (section != p.section) continue;
            ImGui::PushID(p.name);
            sim::ParamValue v = net_.param(id, p.name);
            const bool changed = !net_.isDefault(id, p.name);
            ui::rowLabel(p.label, changed, helpFor(p).c_str());
            if (ui::resetButton("reset", changed)) {
                net_.resetParam(id, p.name);
                v = net_.param(id, p.name);
            }
            bool edited = false;
            const std::string format = formatFor(p);
            switch (p.kind) {
                case sim::ParamKind::Float: edited = ui::sliderFloat("##v", v[0], p.min, p.max, format.c_str()); break;
                case sim::ParamKind::Int: {
                    int i = static_cast<int>(std::lround(v[0]));
                    edited = ImGui::SliderInt("##v", &i, static_cast<int>(p.min), static_cast<int>(p.max), format.c_str());
                    v[0] = static_cast<float>(i);
                    break;
                }
                case sim::ParamKind::Toggle: {
                    bool b = v[0] != 0.0f;
                    edited = ui::toggle("##v", b);
                    v[0] = b ? 1.0f : 0.0f;
                    break;
                }
                case sim::ParamKind::Vector: {
                    const float speed = std::max((p.max - p.min) / 400.0f, 0.001f);
                    edited = ui::dragVector("##v", v.data(), speed, "%.3g");
                    break;
                }
                case sim::ParamKind::Color: edited = ui::colorEdit("##v", v.data()); break;
                case sim::ParamKind::File: {
                    // The path, and a button that browses for one.
                    std::string value = net_.text(id, p.name);
                    const float button = ImGui::GetFrameHeight();
                    ImGui::SetNextItemWidth(std::max(10.0f, ImGui::GetContentRegionAvail().x - button - ImGui::GetStyle().ItemSpacing.x));
                    if (ImGui::InputTextWithHint("##v", "no file", &value, ImGuiInputTextFlags_EnterReturnsTrue) ||
                        ImGui::IsItemDeactivatedAfterEdit()) {
                        net_.setText(id, p.name, value);
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("\xe2\x80\xa6", ImVec2(button, button))) {
                        fs::path start(value);
                        if (!value.empty() && start.is_relative() && !folder().empty()) start = fs::path(folder()) / start;
                        std::vector<std::string> extensions(p.choices.begin(), p.choices.end());
                        files_.open("Choose a mesh", extensions, false, value.empty() ? folder() : start.string());
                        fileAction_ = FileAction::MeshFile;
                        fileNode_ = id;
                        fileParam_ = p.name;
                    }
                    ImGui::SetItemTooltip("Browse for an OBJ file");
                    break;
                }
                case sim::ParamKind::Choice: {
                    int i = static_cast<int>(v[0]);
                    const auto& labels = p.choiceLabels.empty() ? p.choices : p.choiceLabels;
                    if (labels.size() <= 3) {
                        edited = ui::segmented("##v", i, labels);
                    } else if (ImGui::BeginCombo("##v", labels[static_cast<size_t>(std::clamp(i, 0, static_cast<int>(labels.size()) - 1))])) {
                        for (size_t c = 0; c < labels.size(); ++c) {
                            if (ImGui::Selectable(labels[c], static_cast<int>(c) == i)) {
                                i = static_cast<int>(c);
                                edited = true;
                            }
                        }
                        ImGui::EndCombo();
                    }
                    v[0] = static_cast<float>(i);
                    break;
                }
            }
            if (edited) net_.setParam(id, p.name, v);
            ImGui::PopID();
        }
        ImGui::PopID();
    }
}

void SimWorkspace::networkOverview() {
    ImGui::PushFont(theme::fonts().bold, 0.0f);
    ImGui::TextUnformatted(title().c_str());
    ImGui::PopFont();
    ui::note("Nothing is selected. Click a node to see its parameters; Tab or a right click on the network "
             "adds one. Sources, forces and colliders feed the solvers -- the Pyro Solver for smoke and fire, the "
             "Liquid Solver for water, the Rain; what they simulate goes through a look to the Output.");
    ImGui::Spacing();
    if (ui::section("Simulation")) {
        if (compiled_.ok && compiled_.world.any()) {
            auto domainLines = [&](const char* what, const sim::Domain& dm) {
                const Vec3 sz = dm.size();
                ImGui::Text("%-11s %.2f \xc3\x97 %.2f \xc3\x97 %.2f m", what, static_cast<double>(sz.x),
                            static_cast<double>(sz.y), static_cast<double>(sz.z));
                ImGui::Text("Cells       %d \xc3\x97 %d \xc3\x97 %d  (%.2f million)", dm.cells[0], dm.cells[1], dm.cells[2],
                            static_cast<double>(dm.cellCount()) / 1e6);
            };
            if (compiled_.world.hasGas) {
                const sim::Scene& gas = compiled_.world.gas;
                domainLines("Gas", gas.sanitized().solver.domain());
                ImGui::Text("Sources %zu \xc2\xb7 forces %zu \xc2\xb7 colliders %zu", gas.emitters.size(),
                            gas.forces.size(), gas.colliders.size());
            }
            if (compiled_.world.hasWater) {
                const sim::LiquidScene& water = compiled_.world.water;
                domainLines("Water", water.sanitized().solver.domain());
                ImGui::Text("Sources %zu \xc2\xb7 forces %zu \xc2\xb7 colliders %zu", water.sources.size(),
                            water.forces.size(), water.colliders.size());
                const std::shared_ptr<const sim::Frame> f = frameToShow();
                if (f && !f->water.empty()) {
                    ImGui::Text("Particles   %zu  (%.0f litres)", f->water.particles, f->water.litres);
                }
            }
            if (compiled_.world.hasRain) {
                const sim::RainScene& rain = compiled_.world.rain;
                const Vec3& sz = rain.rain.size;
                ImGui::Text("Rain        %.2f \xc3\x97 %.2f m cloud, %.0f m up", static_cast<double>(sz.x),
                            static_cast<double>(sz.z), static_cast<double>(rain.rain.center.y));
                ImGui::Text("Forces %zu \xc2\xb7 colliders %zu", rain.forces.size(), rain.colliders.size());
                const std::shared_ptr<const sim::Frame> f = frameToShow();
                if (f && !f->rain.empty()) {
                    ImGui::Text("Drops       %zu  (%zu droplets)", f->rain.dropCount(), f->rain.dropletCount());
                }
            }
            if (compiled_.hasCamera) {
                const sim::Camera& cam = compiled_.camera;
                const sim::Node* n = net_.node(cam.node);
                ImGui::Text("Camera      %s  %.0f mm  %d \xc3\x97 %d", n ? n->name.c_str() : "", static_cast<double>(cam.focal),
                            cam.width, cam.height);
            }
            ImGui::Text("Frames      %d at %.0f fps  (%.1f s)", compiled_.frames,
                        1.0 / static_cast<double>(compiled_.world.timeStep),
                        compiled_.frames * static_cast<double>(compiled_.world.timeStep));
            ImGui::Text("Cache       %d frames, %.0f MB", runner_->cached(),
                        static_cast<double>(runner_->bytes()) / (1024.0 * 1024.0));
            if (runner_->stepMs() > 0.0) ImGui::Text("Step        %.0f ms", runner_->stepMs());
        } else {
            ImGui::TextColored(theme::vec(theme::kRed), "Nothing to simulate yet.");
        }
    }
    if (!compiled_.problems.empty() && ui::section("Problems")) {
        for (size_t i = 0; i < compiled_.problems.size(); ++i) {
            const sim::Problem& p = compiled_.problems[i];
            const sim::Node* n = net_.node(p.node);
            const std::string text = (n ? n->name + ": " : std::string()) + p.message;
            ImGui::PushID(static_cast<int>(i));
            ImGui::PushStyleColor(ImGuiCol_Text, theme::vec(p.level == sim::Problem::Level::Error ? theme::kRed : theme::kYellow));
            if (ImGui::Selectable(text.c_str()) && n) {
                canvas_.select(n->id);
                canvas_.reveal(n->id);
            }
            ImGui::PopStyleColor();
            ImGui::PopID();
        }
    }
}

// --- the viewport -------------------------------------------------------------------------------

void SimWorkspace::updateGuides() {
    const std::vector<int> chosen(canvas_.selection().begin(), canvas_.selection().end());
    if (guidesRevision_ == net_.revision() && guidesSelection_ == chosen) return;
    guidesRevision_ = net_.revision();
    guidesSelection_ = chosen;
    gl::Lines lines;
    if (guides_) {
        // Not the camera looked through: its lines would start at the eye.
        const sim::Camera* camera = compiled_.hasCamera && !throughCamera_ ? &compiled_.camera : nullptr;
        lines = gl::sceneGuides(compiled_.ok ? &compiled_.world : nullptr, compiled_.solids, chosen, compiled_.solver,
                                compiled_.liquidSolver, compiled_.rain, camera);
    }
    renderer_.setLines(lines);
    guideLines_ = std::move(lines);
    viewDirty_ = true;
}

void SimWorkspace::drawGnomon(ImDrawList* d, ImVec2 corner) const {
    float f[3], r[3], u[3];
    renderer_.orbit.axes(f, r, u);
    const Vec3 forward(f[0], f[1], f[2]), right(r[0], r[1], r[2]), up(u[0], u[1], u[2]);
    const float len = theme::px(22.0f);
    const ImVec2 c(corner.x + theme::px(34.0f), corner.y - theme::px(34.0f));
    struct Axis {
        Vec3 dir;
        ImU32 col;
        const char* label;
    } axes[3] = {{Vec3(1, 0, 0), IM_COL32(230, 86, 86, 255), "X"},
                 {Vec3(0, 1, 0), IM_COL32(120, 210, 96, 255), "Y"},
                 {Vec3(0, 0, 1), IM_COL32(90, 140, 240, 255), "Z"}};
    // Back to front, so the axis towards the eye is on top.
    std::sort(std::begin(axes), std::end(axes), [&](const Axis& a, const Axis& b) {
        return dot(a.dir, forward) > dot(b.dir, forward);
    });
    d->AddCircleFilled(c, len + theme::px(8.0f), IM_COL32(0, 0, 0, 60));
    for (const Axis& a : axes) {
        const ImVec2 tip(c.x + dot(a.dir, right) * len, c.y - dot(a.dir, up) * len);
        d->AddLine(c, tip, a.col, theme::px(2.0f));
        d->AddCircleFilled(tip, theme::px(7.0f), a.col);
        const ImVec2 t = ImGui::CalcTextSize(a.label);
        d->AddText(ImVec2(tip.x - t.x * 0.5f, tip.y - t.y * 0.5f), IM_COL32(20, 20, 24, 255), a.label);
    }
}

// --- the timeline ---------------------------------------------------------------------------------

float SimWorkspace::bottomHeight() const { return ImGui::GetFrameHeight() + theme::px(16.0f); }

void SimWorkspace::bottom(ImVec2 size) {
    (void)size;
    ui::TimelineState s;
    s.frames = std::max(1, compiled_.frames);
    s.current = current_;
    s.cached = runner_->cached();
    s.simulating = runner_->busy();
    s.playing = playing_;
    s.loop = loop_;
    s.fps = compiled_.ok ? 1.0f / compiled_.world.timeStep : 30.0f;
    const ui::TimelineActions a = ui::timeline("timeline", s);
    if (a.togglePlay) playing_ = !playing_;
    if (a.toStart) current_ = 1;
    if (a.toEnd) current_ = std::max(1, s.cached);
    if (a.back) {
        playing_ = false;
        current_ = std::max(1, current_ - 1);
    }
    if (a.forward) {
        playing_ = false;
        current_ = std::min(s.frames, current_ + 1);
    }
    if (a.toggleLoop) loop_ = !loop_;
    if (a.scrubTo > 0) {
        playing_ = false;
        current_ = a.scrubTo;
    }
}

// --- menus ----------------------------------------------------------------------------------------

void SimWorkspace::fileMenu() {
    if (ImGui::MenuItem("New", "Ctrl+N")) newNetwork();
    if (ImGui::MenuItem("Open\xe2\x80\xa6", "Ctrl+O")) {
        files_.open("Open network", {".pgsim"}, false, path_);
        fileAction_ = FileAction::Open;
    }
    if (ImGui::BeginMenu("Examples")) {
        for (const std::string& name : sim::Network::exampleNames()) {
            if (ImGui::MenuItem(name.c_str())) openExample(name);
        }
        ImGui::EndMenu();
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Save", "Ctrl+S")) {
        if (path_.empty()) {
            files_.open("Save network", {".pgsim"}, true, (example_.empty() ? "untitled" : example_) + ".pgsim");
            fileAction_ = FileAction::SaveAs;
        } else {
            save(path_);
        }
    }
    if (ImGui::MenuItem("Save As\xe2\x80\xa6", "Ctrl+Shift+S")) {
        files_.open("Save network", {".pgsim"}, true, path_.empty() ? (example_.empty() ? "untitled" : example_) + ".pgsim" : path_);
        fileAction_ = FileAction::SaveAs;
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Render Image\xe2\x80\xa6")) {
        files_.open("Render image", {".png"}, true, (example_.empty() ? std::string("frame") : example_) + ".png");
        fileAction_ = FileAction::Image;
    }
    if (ImGui::MenuItem("Render Frames\xe2\x80\xa6", nullptr, false, runner_->cached() > 0)) {
        files_.open("Render frames into a folder", {}, true, "");
        fileAction_ = FileAction::Frames;
    }
}

void SimWorkspace::editMenu() {
    if (ImGui::MenuItem("Undo", "Ctrl+Z", false, history_.canUndo())) undo();
    if (ImGui::MenuItem("Redo", "Ctrl+Shift+Z", false, history_.canRedo())) redo();
    ImGui::Separator();
    const std::vector<int> chosen(canvas_.selection().begin(), canvas_.selection().end());
    if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, !chosen.empty())) duplicate(chosen);
    if (ImGui::MenuItem("Delete", "Del", false, !chosen.empty())) removeNodes(chosen);
    if (ImGui::MenuItem("Bypass", "B", false, !chosen.empty())) toggleBypass(chosen);
    ImGui::Separator();
    if (ImGui::MenuItem("Arrange", "L")) canvas_.arrange();
    if (ImGui::MenuItem("Frame Network", "F")) canvas_.frame();
}

void SimWorkspace::menus() {
    if (ImGui::BeginMenu("Simulation")) {
        if (ImGui::MenuItem(playing_ ? "Pause" : "Play", "Space")) playing_ = !playing_;
        if (ImGui::MenuItem("To the Start", "Home")) current_ = 1;
        if (ImGui::MenuItem("Loop", nullptr, loop_)) loop_ = !loop_;
        ImGui::Separator();
        bool running = runner_->running();
        if (ImGui::MenuItem("Simulate Ahead", nullptr, &running)) runner_->setRunning(running);
        ImGui::SetItemTooltip("Simulate the frames before they are played; off, it waits.");
        if (ImGui::MenuItem("Simulate Again")) {
            runner_ = std::make_unique<SimRunner>(synchronous_);
            compiledRevision_ = ~0ull;
            recompile();
            shown_.reset();
        }
        ImGui::SetItemTooltip("Throws the cached frames away and simulates from frame 1.");
        if (ImGui::BeginMenu("Cache Size")) {
            for (int mb : {512, 1024, 2048, 4096}) {
                const std::string label = mb < 1024 ? std::to_string(mb) + " MB" : std::to_string(mb / 1024) + " GB";
                if (ImGui::MenuItem(label.c_str())) runner_->setBudget(static_cast<size_t>(mb) << 20);
            }
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Add")) {
        if (ImGui::IsWindowAppearing()) {
            addAt_ = floorPoint(camera_, ImVec2(camera_.lo.x + camera_.size.x * 0.5f, camera_.lo.y + camera_.size.y * 0.5f));
        }
        sceneMenu(addAt_);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        if (ImGui::MenuItem("Local Axes", nullptr, localAxes_)) localAxes_ = !localAxes_;
        if (ImGui::MenuItem("Snap", nullptr, snap_)) snap_ = !snap_;
        if (ImGui::MenuItem("Frame the Selection", "F")) frameSelection();
        ImGui::Separator();
        if (ImGui::MenuItem("Guides", "G", guides_)) {
            guides_ = !guides_;
            guidesRevision_ = ~0ull;
        }
        if (ImGui::MenuItem("Frame the Domain", "double click")) {
            setThroughCamera(false);
            framed_ = false;
        }
        if (ImGui::MenuItem("Frame the Network", "F")) canvas_.frame();
        ImGui::Separator();
        if (ImGui::MenuItem("Look Through the Camera", "0", throughCamera_, compiled_.hasCamera)) {
            setThroughCamera(!throughCamera_);
        }
        if (ImGui::MenuItem("Camera from View", "Ctrl+Alt+0")) cameraFromView();
        ImGui::EndMenu();
    }
}

void SimWorkspace::helpMenu() {
    ImGui::TextDisabled("Network");
    ImGui::TextUnformatted("Tab, right click          add a node");
    ImGui::TextUnformatted("Drag from a pin           link; into space: add a node, linked");
    ImGui::TextUnformatted("Drag a linked input       move the link, or drop it");
    ImGui::TextUnformatted("Wheel, middle drag        zoom, pan");
    ImGui::TextUnformatted("F / Del / Ctrl+D / B      frame, delete, duplicate, bypass");
    ImGui::Separator();
    ImGui::TextDisabled("Viewport");
    ImGui::TextUnformatted("Click                     select (Shift, Ctrl: add)");
    ImGui::TextUnformatted("Q  W  E  R                select, move, rotate, scale");
    ImGui::TextUnformatted("Drag a handle             move, turn, size (Ctrl: snap)");
    ImGui::TextUnformatted("Shift+A, right click      add an object, a source, a force");
    ImGui::TextUnformatted("Del, Ctrl+D, F, Esc       delete, duplicate, frame, cancel");
    ImGui::TextUnformatted("Left drag                 orbit");
    ImGui::TextUnformatted("Middle / Shift+left drag  pan");
    ImGui::TextUnformatted("Right drag, wheel         zoom");
    ImGui::TextUnformatted("Double click              frame what is clicked, or the domain");
    ImGui::TextUnformatted("0, Ctrl+Alt+0             look through the camera, camera from view");
    ImGui::Separator();
    ImGui::TextDisabled("Timeline");
    ImGui::TextUnformatted("Space  Home  End  Left  Right");
    ImGui::Separator();
    ImGui::TextDisabled("The same from the command line:");
    ImGui::TextUnformatted("  pgshader sim campfire fire.png --every 10");
    ImGui::TextUnformatted("  pgshader sim my.pgsim out.png --set fire.fuel=20");
}

void SimWorkspace::popups() {
    std::string chosen;
    if (!files_.draw(chosen)) return;
    switch (fileAction_) {
        case FileAction::Open: open(chosen); break;
        case FileAction::SaveAs: save(chosen); break;
        case FileAction::Image: renderImage(chosen); break;
        case FileAction::Frames: renderFrames(chosen); break;
        case FileAction::MeshFile:
            if (net_.setText(fileNode_, fileParam_, chosen)) {
                // An object that was no mesh becomes one.
                if (const sim::Node* n = net_.node(fileNode_); n && n->type == "object") {
                    net_.setParam(fileNode_, "shape", {static_cast<float>(sim::Shape::Mesh), 0.0f, 0.0f});
                }
            }
            break;
        case FileAction::ImportMesh: addMesh(chosen, addAt_); break;
        case FileAction::None: break;
    }
    fileAction_ = FileAction::None;
}

sim::Domain SimWorkspace::sceneBox() const {
    return compiled_.ok && compiled_.world.any() ? gl::sceneDomain(compiled_.world) : runner_->domain();
}

std::string SimWorkspace::gridsText() const {
    const std::string times = " \xc3\x97 ", dot = "  \xc2\xb7  ";
    auto cells = [&](const sim::Domain& d) {
        return std::to_string(d.cells[0]) + times + std::to_string(d.cells[1]) + times + std::to_string(d.cells[2]);
    };
    if (!compiled_.ok || !compiled_.world.any()) return cells(runner_->domain()) + " cells";
    const sim::World& w = compiled_.world;
    std::string text;
    if (w.hasGas) text = cells(w.gas.sanitized().solver.domain()) + " cells";
    if (w.hasWater) {
        if (!text.empty()) text = "gas " + text + dot;
        text += "water " + cells(w.water.sanitized().solver.domain());
        const std::shared_ptr<const sim::Frame> f = frameToShow();
        if (f && !f->water.empty()) {
            const double k = static_cast<double>(f->water.particles) / 1000.0;
            char buf[48];
            std::snprintf(buf, sizeof buf, "%s%.0f k particles", dot.c_str(), k);
            text += buf;
        }
    }
    if (w.hasRain) {
        const std::shared_ptr<const sim::Frame> f = frameToShow();
        if (!text.empty()) text += dot;
        char buf[48];
        std::snprintf(buf, sizeof buf, "rain %.0f k drops", f ? static_cast<double>(f->rain.dropCount()) / 1000.0 : 0.0);
        text += buf;
    }
    return text;
}

std::string SimWorkspace::status() const {
    char text[240];
    std::snprintf(text, sizeof text, "%zu nodes  \xc2\xb7  %s  \xc2\xb7  cache %d / %d (%.0f MB)%s  \xc2\xb7  %.0f ms a step",
                  net_.nodes().size(), gridsText().c_str(), runner_->cached(), compiled_.frames,
                  static_cast<double>(runner_->bytes()) / (1024.0 * 1024.0), runner_->full() ? " full" : "",
                  runner_->stepMs());
    return text;
}

void SimWorkspace::selectNode(const std::string& name) {
    if (const sim::Node* n = net_.named(name)) canvas_.select(n->id);
}

// --- images -----------------------------------------------------------------------------------------

void SimWorkspace::shotSize(int& width, int& height) const {
    width = compiled_.hasCamera ? compiled_.camera.width : std::max(viewWidth_, 64);
    height = compiled_.hasCamera ? compiled_.camera.height : std::max(viewHeight_, 64);
}

void SimWorkspace::renderShot(int width, int height) {
    // Through the camera, as it sees -- or as the viewport does -- without
    // the guides and the selection's highlight; twice the size, to be
    // averaged down.
    const gl::Orbit view = renderer_.orbit;
    if (compiled_.hasCamera) renderer_.orbit = gl::orbitThrough(compiled_.camera, focusOf(compiled_.camera));
    renderer_.setLines({});
    renderer_.setHighlight({}, 0);
    renderer_.render(width * 2, height * 2);
    renderer_.orbit = view;
    // Both back on the next frame.
    guidesRevision_ = ~0ull;
    highlightedHover_ = -1;
    viewDirty_ = true;
}

float SimWorkspace::focusOf(const sim::Camera& camera) const {
    const sim::Domain box = sceneBox();
    const Vec3 middle = box.origin() + box.size() * 0.5f;
    return std::max(dot(middle - camera.position, camera.forward()), 0.5f);
}

bool SimWorkspace::renderImage(const std::string& path) {
    int width = 0, height = 0;
    shotSize(width, height);
    renderShot(width, height);
    const std::vector<uint8_t> pixels = renderer_.readPixels(2);
    if (!gl::writePng(path, width, height, 3, pixels)) {
        setMessage(path + ": cannot write it", true);
        return false;
    }
    setMessage("Rendered " + path);
    return true;
}

bool SimWorkspace::renderFrames(const std::string& folder) {
    std::error_code ec;
    fs::create_directories(folder, ec);
    const int cached = runner_->cached();
    int width = 0, height = 0;
    shotSize(width, height);
    const std::string stem = example_.empty() ? (path_.empty() ? "frame" : fs::path(path_).stem().string()) : example_;
    int written = 0;
    for (int f = 1; f <= cached; ++f) {
        const auto frame = runner_->frame(f);
        if (!frame) break;
        renderer_.setFrame(*frame);
        renderShot(width, height);
        char name[64];
        std::snprintf(name, sizeof name, "_%04d.png", f);
        if (!gl::writePng((fs::path(folder) / (stem + name)).string(), width, height, 3, renderer_.readPixels(2))) break;
        ++written;
    }
    shown_.reset();  // put the frame at the play head back
    viewDirty_ = true;
    setMessage("Rendered " + std::to_string(written) + " frames into " + folder, written != cached);
    return written == cached;
}

}  // namespace pg::editor
