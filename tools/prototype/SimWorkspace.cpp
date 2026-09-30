#include "SimWorkspace.h"

#include "pg/lang/Lang.h"

#include "pg/gl/Png.h"
#include "pg/io/Export.h"
#include "pg/sim/UsdExport.h"
#include "pg/io/Video.h"
#include "pg/sim/Asset.h"
#include "pg/sim/Cache.h"
#include "pg/sim/SparseGrid.h"

#include "misc/cpp/imgui_stdlib.h"

#ifndef _WIN32
#include <unistd.h>
#endif

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

/// Simulation > Preview Resolution: the grids of the gas and the water this
/// much as fine.
constexpr float kPreview = 0.5f;
/// A bake saves its state every this many frames.
constexpr int kCheckpointEvery = 10;

ImU32 categoryColor(const std::string& c) {
    if (c == "Geometry") return IM_COL32(148, 74, 110, 255);
    if (c == "Objects") return IM_COL32(70, 98, 150, 255);
    if (c == "Sources") return IM_COL32(178, 86, 44, 255);
    if (c == "Forces") return IM_COL32(38, 124, 134, 255);
    if (c == "Simulation") return IM_COL32(112, 78, 160, 255);
    if (c == "Render") return IM_COL32(58, 128, 80, 255);
    if (c == "Assets") return IM_COL32(150, 124, 48, 255);
    return IM_COL32(110, 60, 60, 255);
}

/// Whether a network has a node that simulates: a solver, the rain.
bool simulates(const sim::Network& net) {
    for (const sim::Node& n : net.nodes()) {
        if (n.type == "pyro_solver" || n.type == "liquid_solver" || n.type == "rain") return true;
    }
    return false;
}

/// Whether files can be made in `folder`.
bool writable(const fs::path& folder) {
    std::error_code ec;
    if (!fs::is_directory(folder, ec)) return false;
#ifdef _WIN32
    return true;
#else
    return access(folder.c_str(), W_OK) == 0;
#endif
}

/// `s` as one word of the shell's.
std::string shellWord(const std::string& s) {
    std::string q = "'";
    for (const char c : s) q += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return q + "'";
}

/// Opens a file or a folder in what the desktop opens it with; false when
/// that could not be started.
bool openExternally(const std::string& path) {
#if defined(_WIN32)
    const std::string command = "start \"\" \"" + path + "\"";
#elif defined(__APPLE__)
    const std::string command = "open " + shellWord(path) + " >/dev/null 2>&1 &";
#else
    const std::string command = "xdg-open " + shellWord(path) + " >/dev/null 2>&1 &";
#endif
    return std::system(command.c_str()) == 0;
}

/// A path as the messages show it: from the current folder when it is in
/// it, else whole.
std::string shownPath(const std::string& path) {
    std::error_code ec;
    const fs::path relative = fs::relative(path, fs::current_path(ec), ec);
    if (ec || relative.empty() || *relative.begin() == "..") return path;
    return relative.string();
}

Icon categoryIcon(const std::string& c) {
    if (c == "Geometry") return Icon::Geometry;
    if (c == "Objects") return Icon::Collider;
    if (c == "Sources") return Icon::Source;
    if (c == "Forces") return Icon::Force;
    if (c == "Simulation") return Icon::Solver;
    if (c == "Assets") return Icon::Asset;
    return Icon::Look;
}

Icon typeIcon(const sim::NodeType* t) {
    if (!t) return Icon::Error;
    const std::string name = t->name;
    if (name == "output") return Icon::Output;
    if (name == "water_source") return Icon::Drop;
    if (name == "rain") return Icon::Rain;
    if (name == "camera") return Icon::Camera;
    if (name == "sphere") return Icon::Sphere;
    if (name == "box") return Icon::Box;
    if (name == "tube") return Icon::Cylinder;
    if (name == "scatter" || name == "point_cloud" || name == "liquid_points" || name == "rain_points") return Icon::Points;
    if (name == "point_wrangle") return Icon::Code;
    if (name == "liquid_surface") return Icon::Drop;
    if (name == "file") return Icon::File;
    if (name == "asset_input") return Icon::Input;
    if (name == "foreach_begin" || name == "foreach_end") return Icon::Loop;
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
        case sim::PinType::Geometry: return IM_COL32(236, 150, 190, 255);
        case sim::PinType::Rain: return IM_COL32(150, 172, 210, 255);
        case sim::PinType::Rigid: return IM_COL32(214, 160, 96, 255);
        case sim::PinType::Cloth: return IM_COL32(200, 96, 150, 255);
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
    // The shape: the geometry linked into Shape, else its own.
    const std::vector<sim::Link> shaped = net.linksInto(n.id, "shape");
    const sim::Node* geometry = shaped.empty() ? nullptr : net.node(shaped.front().from);
    auto shapeOf = [&]() {
        if (geometry) return "shape of " + geometry->name;
        return std::string(sim::shapeName(static_cast<sim::Shape>(static_cast<int>(v("shape")))));
    };
    if (t == "pyro_source") {
        std::string s = v("shape") != 0.0f || geometry ? shapeOf() : "";
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
    if (t == "object" && geometry) return shapeOf();
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
    if (t == "scatter" || t == "point_cloud") return std::to_string(static_cast<int>(v("count"))) + " points";
    if (t == "box") {
        const sim::ParamValue s = net.param(n.id, "size");
        return number(s[0]) + times + number(s[1]) + times + number(s[2]) + " m";
    }
    if (t == "sphere") return "radius " + number(v("radius")) + " m";
    if (t == "tube") return "radius " + number(v("radius")) + dot + number(v("height")) + " m";
    if (t == "file") {
        const std::string file = net.text(n.id, "file");
        return file.empty() ? std::string("no file") : fs::path(file).filename().string();
    }
    if (t == "attribute_create") return "@" + net.text(n.id, "name");
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
    : gl_(gl), renderer_(gl), runner_(std::make_unique<SimRunner>(synchronous)), synchronous_(synchronous) {
    if (!renderer_.init(rendererLog_)) rendererLog_ = "The driver rejected the volume shader:\n" + rendererLog_;
    else rendererLog_.clear();
    // Liquid Points and the like read the frames the runner keeps.
    geometry_->setFrames([this](int frame) { return runner_ ? runner_->frame(frame) : nullptr; });
    cooker_ = std::make_unique<sim::Cooker>([this](int frame) { return runner_ ? runner_->frame(frame) : nullptr; });
    newNetwork();  // an empty scene; File > Examples has finished ones
}

// --- files --------------------------------------------------------------------------------

std::string SimWorkspace::title() const {
    if (editingAsset()) {
        const sim::AssetInfo& a = net_.asset();
        return (a.label.empty() ? a.name : a.label) + " (asset, version " + std::to_string(a.version) + ")" +
               (modified() ? " *" : "");
    }
    const std::string name = !path_.empty() ? fs::path(path_).filename().string()
                             : !example_.empty() ? example_ + " (example)"
                                                 : std::string("untitled.pgsim");
    return name + (modified() ? " *" : "");
}

bool SimWorkspace::modified() const { return net_.save() != savedText_; }

bool SimWorkspace::canOpen(const std::string& path) const {
    const fs::path ext = fs::path(path).extension();
    return ext == ".pgsim" || ext == ".pgasset";
}

std::string SimWorkspace::folder() const {
#ifdef PG_SIM_EXAMPLES_DIR
    if (!example_.empty()) return PG_SIM_EXAMPLES_DIR;
#endif
    return path_.empty() ? std::string() : fs::path(path_).parent_path().string();
}

void SimWorkspace::load(const sim::Network& net, const std::string& path, const std::string& example) {
    // Out of any asset gone into: the scene's graph is what cooks again.
    if (!levels_.empty()) {
        geometry_ = std::move(levels_.front().geometry);
        levels_.clear();
        ++levelsRevision_;
    }
    net_ = net;
    path_ = path;
    example_ = example;
    savedText_ = net_.save();
    history_.reset(savedText_);
    canvas_.clearSelection();
    canvas_.frame();
    // The frames of what was open before are not this network's.
    runner_->clear();
    shown_.reset();
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
    else if (editingAsset()) setMessage("Opened the asset " + path + ": Ctrl+S saves a new version, every instance following");
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
    load(sim::Network(), "", "");
    setMessage("");  // the status line says where to begin
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
    // Inside an asset, the scene's simulation stays as it was.
    if (!levels_.empty()) return;
    if (net_.revision() == compiledRevision_) return;
    compiledRevision_ = net_.revision();
    compiled_ = net_.compile(folder(), geometry_.get());
    if (compiled_.ok && preview_) compiled_.world = sim::preview(compiled_.world, kPreview);
    if (compiled_.ok) runner_->set(compiled_.world, compiled_.frames);
    else if (!simulates(net_)) runner_->clear();  // nothing left that simulates: its frames go too
    current_ = std::clamp(current_, 1, std::max(1, compiled_.frames));
    posedRevision_ = ~0ull;
    pose(current_);
}

void SimWorkspace::pose(int frame) {
    if (frame == posedFrame_ && posedRevision_ == compiledRevision_) return;
    const bool again = posedRevision_ != compiledRevision_;
    posedFrame_ = frame;
    posedRevision_ = compiledRevision_;
    const sim::Look& look = compiled_.lookAt(frame);
    if (!(renderer_.look == look)) {
        renderer_.look = look;
        viewDirty_ = true;
    }
    if (again || !compiled_.poses.empty()) {
        renderer_.setSolids(levels_.empty() ? compiled_.solidsAt(frame) : std::vector<sim::Solid>());
        viewDirty_ = true;
    }
}

void SimWorkspace::updatePieces() {
    const sim::Look& look = renderer_.look;
    // The pieces and the cloth, drawn with the displayed geometry.
    const bool bodies = shown_ && ((look.pieces && !shown_->rigid.empty()) || (look.cloth && !shown_->cloth.empty()));
    const std::shared_ptr<const sim::Frame> f = levels_.empty() && bodies ? shown_ : nullptr;
    char key[240];
    std::snprintf(key, sizeof key, "%g %g %g %g %g %g %s %d %g %g %g", look.piecesColor.x, look.piecesColor.y,
                  look.piecesColor.z, look.piecesInside.x, look.piecesInside.y, look.piecesInside.z,
                  look.insideGroup.c_str(), look.cloth ? 1 : 0, look.clothColor.x, look.clothColor.y, look.clothColor.z);
    if (f == piecesFrame_ && (!f || key == piecesKey_)) return;
    piecesFrame_ = f;
    piecesKey_ = key;
    renderer_.setPieces(f ? sim::drawnBodies(*f, look) : nullptr);
    viewDirty_ = true;
}

std::shared_ptr<const sim::Frame> SimWorkspace::frameToShow() const {
    // The frame at the play head -- or, while the simulation has not got
    // there yet, the latest before it.
    const int cached = runner_->cached();
    if (cached == 0) return nullptr;
    return runner_->frame(std::min(current_, cached));
}

void SimWorkspace::update(float dt) {
    if (enterRequest_) {
        enterAsset(enterRequest_);
        enterRequest_ = 0;
    }
    if (leaveRequest_) {
        leaveAsset();
        leaveRequest_ = false;
    }
    recompile();
    history_.track(net_.save(), settled());
    if (synchronous_) runner_->step();
    // A bake, and frames landing on disk: twice a second.
    if (ImGui::GetTime() - bakePolled_ >= 0.5) {
        bakePolled_ = ImGui::GetTime();
        pollBake();
    }

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
    std::shared_ptr<const sim::Frame> f = levels_.empty() ? frameToShow() : nullptr;  // inside an asset: its geometry alone
    if (!f && shown_ && levels_.empty() && (runner_->busy() || gizmo_.dragging())) f = shown_;
    if (f != shown_) {
        shown_ = f;
        if (f) renderer_.setFrame(*f);
        else renderer_.clearFrame();
        viewDirty_ = true;
    }
    if (!shown_) renderer_.setDomain(runner_->domain());
    pose(current_);
    updatePieces();
    updateGeometry();
    updateGuides();

    // Rendering frames or a video: a few a frame of the window.
    job_.step();
    std::string result, where;
    bool failed = false;
    if (job_.takeResult(result, failed, where)) {
        current_ = jobReturnFrame_;
        playing_ = jobWasPlaying_;
        shown_.reset();  // the frame at the play head, again
        viewDirty_ = true;
        setMessage(result, failed);
        notify(result, failed ? std::string() : where, failed, true);
        std::fprintf(stderr, "prototype: %s\n", result.c_str());
    }
}

void SimWorkspace::shortcuts() {
    const ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput) return;
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Z)) undo();
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z) ||
        ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Y)) {
        redo();
    }
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S) && editingAsset()) {
        commitAsset(path_);
    } else if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S)) {
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
        files_.open("Open network", {".pgsim", ".pgasset"}, false, path_);
        fileAction_ = FileAction::Open;
    }
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_N)) newNetwork();
    // Space plays and stops, let go -- unless it was held to turn the view.
    if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) spaceUsed_ = false;
    if (ImGui::IsKeyDown(ImGuiKey_Space) && (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Middle) ||
                                             ImGui::IsMouseClicked(ImGuiMouseButton_Right))) {
        spaceUsed_ = true;
    }
    if (io.KeyCtrl || io.KeyAlt) return;
    if (ImGui::IsKeyReleased(ImGuiKey_Space) && !spaceUsed_) playing_ = !playing_;
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
    if (ImGui::IsKeyPressed(ImGuiKey_U, false) && !levels_.empty()) leaveRequest_ = true;  // out of the asset
}

// --- the network ---------------------------------------------------------------------------

std::vector<CanvasNode> SimWorkspace::canvasNodes() const {
    static const sim::Compiled none;
    const sim::Compiled& compiled = levels_.empty() ? compiled_ : none;
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
        c.displayable = t && t->core;
        c.displayed = n.display;
        c.dimmed = levels_.empty() && !compiled.isActive(n.id);
        for (const sim::Problem& p : compiled.problems) {
            if (p.node != n.id) continue;
            c.problem = std::max(c.problem, p.level == sim::Problem::Level::Error ? 2 : 1);
            c.problemText += (c.problemText.empty() ? "" : "\n") + p.message;
        }
        if (const auto e = cookErrors_.find(n.id); e != cookErrors_.end()) {
            c.problem = 2;
            c.problemText += (c.problemText.empty() ? "" : "\n") + e->second;
        }
        c.summary = summaryOf(net_, n, compiled);
        if (t && std::string(t->core ? t->core : "") == "asset" && c.summary.empty()) {
            const auto def = sim::AssetLibrary::instance().find(n.type);
            c.summary = "asset, version " + std::to_string(def ? def->version : 0);
        }
        if (n.display) {
            if (const GeometryPtr& g = renderer_.geometry()) {
                const std::string dot = " \xc2\xb7 ";
                c.summary = std::to_string(g->pointCount()) + " points" +
                            (g->primitiveCount() ? dot + std::to_string(g->primitiveCount()) + " prims" : std::string()) +
                            (g->volumeCount() ? dot + std::to_string(g->volumeCount()) + " volumes" : std::string());
            }
        }
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
    m.toggleDisplay = [this](int node) { net_.setDisplay(net_.displayed() == node ? 0 : node); };
    m.addMenu = [this](ImVec2 at, const PinRef* pending) { return addMenu(at, pending); };
    m.nodeMenu = [this](int node) { nodeMenu(node); };
    m.open = [this](int node) {
        if (const sim::Node* n = net_.node(node); n && sim::AssetLibrary::instance().find(n->type)) enterRequest_ = node;
    };
    m.up = [this] { leaveRequest_ = !levels_.empty(); };
    return m;
}

int SimWorkspace::addNode(const std::string& type, ImVec2 at, const PinRef* pending) {
    const int id = net_.add(type, std::round(at.x - 24.0f), std::round(at.y - 14.0f));
    if (!id) return 0;
    // A geometry node shows when nothing does -- or when it goes on from the
    // one that does, as the next step of a chain.
    const sim::NodeType* added = sim::findNodeType(type);
    if (added && added->core && (!net_.displayed() || (pending && pending->output && pending->node == net_.displayed()))) {
        net_.setDisplay(id);
    }
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
    const std::vector<const sim::NodeType*> all = sim::allNodeTypes();
    for (const char* category : sim::nodeCategories()) {
        std::vector<const sim::NodeType*> types;
        for (const sim::NodeType* t : all) {
            if (category != std::string(t->category) || !fits(*t)) continue;
            // Inside an asset: geometry nodes -- not the asset itself.
            if (editingAsset() && (!t->core || net_.asset().name == t->name)) continue;
            // An Asset Input belongs inside an asset.
            if (!editingAsset() && std::string(t->name) == "asset_input") continue;
            types.push_back(t);
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

bool SimWorkspace::pickedMenu() {
    // Tab in the viewport: the geometry nodes, those that take a group --
    // what is picked goes into it -- first.
    if (ImGui::IsWindowAppearing()) {
        pickedSearch_.clear();
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::SetNextItemWidth(theme::px(250.0f));
    ImGui::InputTextWithHint("##search", "Search nodes\xe2\x80\xa6", &pickedSearch_);
    std::string q = pickedSearch_;
    for (char& ch : q) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    auto fits = [&](const sim::NodeType& t) {
        if (!t.core || t.inputs.empty() || t.inputs.front().type != sim::PinType::Geometry) return false;
        if (std::string(t.name) == "asset_input") return false;
        if (q.empty()) return true;
        std::string label = t.label;
        for (char& ch : label) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        return label.find(q) != std::string::npos || std::string(t.name).find(q) != std::string::npos;
    };
    const size_t picked = elementCount();
    const sim::NodeType* first = nullptr;
    const sim::NodeType* chosen = nullptr;
    auto list = [&](const char* heading, bool withGroup) {
        std::vector<const sim::NodeType*> types;
        for (const sim::NodeType* t : sim::allNodeTypes()) {
            if (fits(*t) && (t->param("group") != nullptr) == withGroup) types.push_back(t);
        }
        if (types.empty()) return;
        ImGui::TextDisabled("%s", heading);
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
            ImDrawList* d = ImGui::GetWindowDrawList();
            theme::drawIcon(d, typeIcon(t), ImVec2(p.x + theme::px(12.0f), p.y + h * 0.5f), h * 0.85f,
                            theme::shade(typeColor(*t), 0.35f));
            d->AddText(ImVec2(p.x + theme::px(26.0f), p.y), theme::kText, t->label);
            ImGui::PopID();
        }
        ImGui::Dummy(ImVec2(0.0f, theme::px(3.0f)));
    };
    ImGui::Dummy(ImVec2(0.0f, theme::px(2.0f)));
    list(picked ? "On what is picked" : "On all of it -- nothing is picked", true);
    list("After the shown node", false);
    if (!first) ImGui::TextDisabled("Nothing fits");
    if (first && ImGui::IsKeyPressed(ImGuiKey_Enter)) chosen = first;
    if (!chosen) return false;
    applyToPicked(chosen->name);
    return true;
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
    if (const auto def = sim::AssetLibrary::instance().find(n->type)) {
        ImGui::Separator();
        if (ImGui::MenuItem("Edit Contents", "I")) enterRequest_ = id;
        ImGui::SetItemTooltip("Go into the asset: what changes there changes every %s", t ? t->label : n->type.c_str());
    }
    {
        const std::vector<int> chosen(canvas_.selection().begin(), canvas_.selection().end());
        if (ImGui::MenuItem("Make Asset\xe2\x80\xa6")) {
            assetNodes_ = chosen;
            assetLabel_.clear();
            assetName_.clear();
            assetError_.clear();
            makeAssetOpen_ = true;
        }
        ImGui::SetItemTooltip("The selected geometry nodes as one asset of their own, a node of it in their place");
    }
    if (geometry_->contains(id)) {
        ImGui::Separator();
        if (ImGui::MenuItem("Display", "R", net_.displayed() == id)) net_.setDisplay(net_.displayed() == id ? 0 : id);
        if (ImGui::MenuItem("Export Geometry\xe2\x80\xa6")) chooseExport(id, false);
        ImGui::SetItemTooltip("Its geometry at the frame on screen: .ply points, .obj polygons, .vdb volumes");
        if (ImGui::MenuItem("Export Geometry Frames\xe2\x80\xa6", nullptr, false, runner_->cached() > 0)) chooseExport(id, true);
        ImGui::SetItemTooltip("Its geometry at every frame cached, a file a frame");
    }
    if (t) {
        ImGui::Separator();
        ImGui::PushTextWrapPos(theme::px(320.0f));
        ImGui::TextDisabled("%s", t->help);
        ImGui::PopTextWrapPos();
    }
}

void SimWorkspace::expressionFields(int id, const sim::ParamDef& p, const sim::ParamValue& now) {
    // One field a channel: the expression, or the value while there is none.
    const std::vector<std::string> chans = sim::Network::channels(p);
    const float x0 = ImGui::GetCursorScreenPos().x;  // what they give goes under them
    const float total = ImGui::GetContentRegionAvail().x;
    const float gap = theme::px(3.0f);
    const float w = (total - gap * static_cast<float>(chans.size() - 1)) / static_cast<float>(chans.size());
    std::string errors;
    for (size_t c = 0; c < chans.size(); ++c) {
        if (c) ImGui::SameLine(0.0f, gap);
        const std::string& ch = chans[c];
        const std::string key = std::to_string(id) + "." + ch;
        std::string expr = net_.expression(id, ch);
        char number[32];
        std::snprintf(number, sizeof number, "%g", static_cast<double>(now[c]));
        std::string value = editKey_ == key ? editText_ : expr.empty() ? std::string(number) : expr;
        ImGui::PushID(ch.c_str());
        ImGui::SetNextItemWidth(w);
        ImGui::PushFont(theme::fonts().mono, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, expr.empty() ? theme::vec(theme::kTextDim) : ImVec4(0.6f, 0.83f, 1.0f, 1.0f));
        ImGui::InputText("##e", &value);
        ImGui::PopStyleColor();
        ImGui::PopFont();
        if (ImGui::IsItemActive()) {
            editKey_ = key;
            editText_ = value;
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            // A plain number is a value; anything else an expression.
            if (value.empty() || lang::isNumber(value)) {
                net_.setExpression(id, ch, "");
                if (!value.empty()) {
                    sim::ParamValue v = net_.param(id, p.name);
                    v[c] = static_cast<float>(std::strtod(value.c_str(), nullptr));
                    net_.setParam(id, p.name, v);
                }
            } else {
                net_.setExpression(id, ch, value);
            }
            editKey_.clear();
        } else if (!ImGui::IsItemActive() && editKey_ == key) {
            editKey_.clear();
        }
        if (!expr.empty()) ImGui::SetItemTooltip("%s = %g", expr.c_str(), static_cast<double>(now[c]));
        ImGui::PopID();
        const std::string e = net_.expressionError(id, ch, static_cast<float>(current_));
        if (!e.empty()) errors += (errors.empty() ? "" : "\n") + ch + ": " + e;
    }
    ImGui::SetCursorScreenPos(ImVec2(x0, ImGui::GetCursorScreenPos().y));
    if (!errors.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::vec(theme::kRed));
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(errors.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
    } else if (net_.hasExpression(id, p.name)) {
        std::string shown = "=";
        for (size_t c = 0; c < chans.size(); ++c) {
            char buf[32];
            std::snprintf(buf, sizeof buf, " %g", static_cast<double>(now[c]));
            shown += buf;
        }
        ImGui::TextDisabled("%s at frame %d", shown.c_str(), current_);
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
        // Texts first: a snippet makes the parameters it asks for.
        for (const auto& [name, text] : original.texts) net_.setText(copy, name, text);
        for (const auto& [name, value] : original.params) net_.setParam(copy, name, value);
        for (const auto& [name, keys] : original.keys) {
            for (const sim::Key& k : keys) net_.setKey(copy, name, k.frame, k.value, k.interp);
        }
        for (const auto& [channel, text] : original.exprs) net_.setExpression(copy, channel, text);
        net_.setBypass(copy, original.bypass);
        // In the world too, beside the original rather than inside it -- an
        // object, a source; a geometry node is as it was.
        const sim::Handles& h = type->handles;
        if (h.center && category != "Geometry") {
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
    const std::string where = levelsText();
    ui::PanelHeader h = ui::panelHeader(Icon::Network, "Network",
                                        !where.empty() ? where.c_str() : editingAsset() ? "asset" : "simulation");
    if (ui::headerButton(h, "frame", Icon::Search, "Frame the network (F)")) canvas_.frame();
    if (!levels_.empty() && ui::headerButton(h, "up", Icon::Up, "Back up out of the asset, its new version saved (U)")) {
        leaveRequest_ = true;
    }
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
    ui::PanelHeader h = sheet_ ? ui::panelHeader(Icon::Table, "Spreadsheet", t && t->core ? t->label : nullptr)
                               : ui::panelHeader(Icon::Parameters, "Parameters", t ? t->label : nullptr);
    if (ui::headerButton(h, "sheet", Icon::Table, "The geometry spreadsheet: the points, vertices, primitives, detail "
                                                  "and volumes of the selected geometry node -- or of the displayed one",
                         sheet_)) {
        sheet_ = !sheet_;
    }
    if (n && t && t->bypassable && !sheet_) {
        if (ui::headerButton(h, "bypass", Icon::Bypass, "Bypass: leave the node out (B)", n->bypass)) toggleBypass({id});
    }
    if (n && t && t->core && !sheet_) {
        if (ui::headerButton(h, "display", Icon::Eye, "Display flag: show its geometry in the viewport (R)", n->display)) {
            net_.setDisplay(n->display ? 0 : id);
        }
    }
    if (sheet_) {
        ImGui::BeginChild("sheet", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
        spreadsheet();
        ImGui::EndChild();
        return;
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
    // Another node -- or this one renamed, undone, made again with its
    // number -- and the field shows its name, unless it is being typed in.
    if (nameEditNode_ != id || (!nameActive_ && nameEdit_ != node.name)) {
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
    nameActive_ = ImGui::IsItemActive();
    ImGui::PopFont();
    ImGui::SetItemTooltip("The node's name: what --set NAME.param=value calls it");
    ui::note(type.help);
    if (const auto def = sim::AssetLibrary::instance().find(node.type)) {
        // An asset's node: what it is, and the way in.
        const std::string file = def->file.empty() ? std::string("carried by the program or the network")
                                                   : fs::path(def->file).filename().string();
        ImGui::TextDisabled("Asset %s, version %d \xc2\xb7 %s", def->name.c_str(), def->version, file.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Edit Contents")) enterRequest_ = id;
        ImGui::SetItemTooltip("Go into the asset (I, or a double click on its node): what changes there changes every %s",
                              type.label);
    }

    // What is wrong with it.
    for (const sim::Problem& p : compiled_.problems) {
        if (p.node != id || !levels_.empty()) continue;
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
    if (const auto e = cookErrors_.find(id); e != cookErrors_.end()) {
        const ImVec2 q = ImGui::GetCursorScreenPos();
        const float h = ImGui::GetTextLineHeight();
        theme::drawIcon(ImGui::GetWindowDrawList(), Icon::Error, ImVec2(q.x + h * 0.5f, q.y + h * 0.5f), h * 0.9f, theme::kRed);
        ImGui::SetCursorScreenPos(ImVec2(q.x + h * 1.4f, q.y));
        ImGui::PushStyleColor(ImGuiCol_Text, theme::vec(theme::kRed));
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(e->second.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
    }
    if (const auto w = cookWarnings_.find(id); w != cookWarnings_.end()) {
        const ImVec2 q = ImGui::GetCursorScreenPos();
        const float h = ImGui::GetTextLineHeight();
        theme::drawIcon(ImGui::GetWindowDrawList(), Icon::Error, ImVec2(q.x + h * 0.5f, q.y + h * 0.5f), h * 0.9f, theme::kYellow);
        ImGui::SetCursorScreenPos(ImVec2(q.x + h * 1.4f, q.y));
        ImGui::PushStyleColor(ImGuiCol_Text, theme::vec(theme::kYellow));
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(w->second.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
    }
    if (const auto l = cookLogs_.find(id); l != cookLogs_.end()) {
        // What printf() said: the first lines of it.
        std::string shown = l->second;
        size_t lines = 0, cut = 0;
        for (; cut < shown.size() && lines < 12; ++cut) {
            if (shown[cut] == '\n') ++lines;
        }
        if (cut < shown.size()) shown = shown.substr(0, cut) + "\xe2\x80\xa6";
        ImGui::TextDisabled("printf():");
        ImGui::PushFont(theme::fonts().mono, 0.0f);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(shown.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopFont();
    }
    if (node.bypass) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::vec(IM_COL32(230, 200, 90, 255)));
        ImGui::TextUnformatted(type.core ? "Bypassed: what comes in goes on unchanged."
                                         : "Bypassed: left out of the simulation.");
        ImGui::PopStyleColor();
    }
    if (levels_.empty() && !editingAsset() && !compiled_.isActive(id) && !node.bypass) {
        ui::note(type.core ? "Not displayed, and not the shape of anything: it takes no part."
                           : "Not linked to the Output: it takes no part.");
    }
    if (type.input("shape")) {
        const std::vector<sim::Link> shaped = net_.linksInto(id, "shape");
        if (const sim::Node* g = shaped.empty() ? nullptr : net_.node(shaped.front().from)) {
            ui::note(("Its shape is the geometry of " + g->name +
                      ", as it is at frame 1. Its own shape, place and size below stand in only when that is empty.")
                         .c_str());
        }
    }

    // The parameters, section by section, in the order of the table --
    // and after them those the node's snippet asks for with ch().
    const std::vector<const sim::ParamDef*> defs = net_.params(id);
    std::vector<std::string> sections;
    for (const sim::ParamDef* p : defs) {
        if (std::find(sections.begin(), sections.end(), p->section) == sections.end()) sections.push_back(p->section);
    }
    for (const std::string& section : sections) {
        ImGui::PushID(section.c_str());
        if (!ui::section(section.c_str())) {
            ImGui::PopID();
            continue;
        }
        for (const sim::ParamDef* pp : defs) {
            const sim::ParamDef& p = *pp;
            if (section != p.section) continue;
            ImGui::PushID(p.name);
            // At the play head: an animated parameter's value there, and its key.
            const float frame = static_cast<float>(current_);
            sim::ParamValue v = net_.valueAt(id, p.name, frame);
            const bool changed = !net_.isDefault(id, p.name);
            const std::vector<sim::Key>* keys = net_.keys(id, p.name);
            const sim::Key* here = nullptr;
            if (keys) {
                for (const sim::Key& k : *keys) {
                    if (std::fabs(k.frame - frame) <= 1e-4f) here = &k;
                }
            }
            if (!sim::isText(p.kind)) {
                const int click = ui::keyButton("key", !keys ? 0 : here ? 2 : 1,
                                                here ? "A key at this frame: click to take it off, right click for more"
                                                : keys ? "Animated: click to add a key at this frame (the value there), "
                                                         "right click for more"
                                                       : "Click to animate: a key at this frame");
                if (click == 1) {
                    if (here) net_.removeKey(id, p.name, frame);
                    else net_.setKey(id, p.name, frame, v);
                }
                if (click == 2) ImGui::OpenPopup("keys");
                if (ImGui::BeginPopup("keys")) {
                    ImGui::TextDisabled("%s at frame %d", p.label, current_);
                    ImGui::Separator();
                    if (!here && ImGui::MenuItem("Set Key")) net_.setKey(id, p.name, frame, v);
                    if (here) {
                        for (const sim::Interp in : {sim::Interp::Smooth, sim::Interp::Linear, sim::Interp::Step}) {
                            const char* names[3] = {"Smooth to the next key", "Linear to the next key", "Step: hold to the next key"};
                            if (ImGui::MenuItem(names[static_cast<int>(in)], nullptr, here->interp == in)) {
                                net_.setKey(id, p.name, frame, here->value, in);
                            }
                        }
                        if (ImGui::MenuItem("Delete Key")) net_.removeKey(id, p.name, frame);
                    }
                    if (keys && ImGui::MenuItem("Delete All Keys", nullptr, false, true)) net_.clearKeys(id, p.name, frame);
                    ImGui::EndPopup();
                }
                keys = net_.keys(id, p.name);  // the button may have changed them
                here = nullptr;
                if (keys) {
                    for (const sim::Key& k : *keys) {
                        if (std::fabs(k.frame - frame) <= 1e-4f) here = &k;
                    }
                }
            }
            // fx: the value as an expression -- $F, ch("../node/param") ...
            const bool numeric = p.kind == sim::ParamKind::Float || p.kind == sim::ParamKind::Int ||
                                 p.kind == sim::ParamKind::Vector || p.kind == sim::ParamKind::Color;
            const std::string modeKey = std::to_string(id) + "." + p.name;
            bool exprMode = numeric && (net_.hasExpression(id, p.name) || exprMode_.count(modeKey));
            if (numeric) {
                const ImVec2 row = ImGui::GetCursorScreenPos();
                ImGui::SetCursorScreenPos(ImVec2(row.x + ImGui::GetFrameHeight() * 0.8f, row.y));
                if (ui::exprButton("fx", exprMode,
                                   exprMode ? "Driven by an expression: click to go back to a value (the one it has now)"
                                            : "Drive it by an expression: $F, $T, ch(\"../box1/sizex\") * 2 ...")) {
                    if (exprMode) {
                        const sim::ParamValue now = net_.valueAt(id, p.name, frame);
                        for (const std::string& ch : sim::Network::channels(p)) net_.setExpression(id, ch, "");
                        net_.setParam(id, p.name, now);
                        exprMode_.erase(modeKey);
                        exprMode = false;
                    } else {
                        exprMode_.insert(modeKey);
                        exprMode = true;
                    }
                }
                ImGui::SetCursorScreenPos(row);
            }
            const ImVec2 rowStart = ImGui::GetCursorScreenPos();
            ui::rowLabel(p.label, changed, helpFor(p).c_str());
            // Right click on its name: promote it, copy a reference to it.
            if (ImGui::IsItemHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) ImGui::OpenPopup("param");
            if (ImGui::BeginPopup("param")) {
                paramMenu(id, p);
                ImGui::EndPopup();
            }
            if (editingAsset() && net_.promotion(id, p.name)) {
                // Promoted: a mark at its row's start.
                const float fh = ImGui::GetFrameHeight();
                ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(rowStart.x - theme::px(7.0f), rowStart.y + fh * 0.2f),
                                                          ImVec2(rowStart.x - theme::px(4.0f), rowStart.y + fh * 0.8f),
                                                          theme::kAccent, theme::px(1.5f));
            }
            if (ui::resetButton("reset", changed)) {
                net_.resetParam(id, p.name);
                v = net_.param(id, p.name);
            }
            // An animated field is tinted: amber on a key, green between.
            const bool tinted = keys != nullptr;
            if (tinted) {
                const ImVec4 bg = here ? ImVec4(0.42f, 0.31f, 0.08f, 1.0f) : ImVec4(0.14f, 0.3f, 0.17f, 1.0f);
                ImGui::PushStyleColor(ImGuiCol_FrameBg, bg);
                ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(bg.x * 1.25f, bg.y * 1.25f, bg.z * 1.25f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(bg.x * 1.4f, bg.y * 1.4f, bg.z * 1.4f, 1.0f));
            }
            bool edited = false;
            const std::string format = formatFor(p);
            if (exprMode) {
                expressionFields(id, p, v);
                if (tinted) ImGui::PopStyleColor(3);
                ImGui::PopID();
                continue;
            }
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
                case sim::ParamKind::Text:
                case sim::ParamKind::Code: {
                    // Typed into a buffer of our own; applied when the field is let go.
                    const std::string key = std::to_string(id) + "." + p.name;
                    std::string value = editKey_ == key ? editText_ : net_.text(id, p.name);
                    if (p.kind == sim::ParamKind::Code) {
                        ImGui::PushFont(theme::fonts().mono, 0.0f);
                        const float lines = std::clamp(static_cast<float>(std::count(value.begin(), value.end(), '\n') + 2), 4.0f, 16.0f);
                        ImGui::InputTextMultiline("##v", &value, ImVec2(-1.0f, ImGui::GetTextLineHeight() * lines + theme::px(8.0f)),
                                                  ImGuiInputTextFlags_AllowTabInput);
                        ImGui::PopFont();
                    } else {
                        ImGui::SetNextItemWidth(-1.0f);
                        ImGui::InputText("##v", &value);
                    }
                    if (ImGui::IsItemActive()) {
                        editKey_ = key;
                        editText_ = value;
                    }
                    if (ImGui::IsItemDeactivatedAfterEdit()) {
                        net_.setText(id, p.name, value);
                        editKey_.clear();
                    } else if (!ImGui::IsItemActive() && editKey_ == key) {
                        editKey_.clear();
                    }
                    if (p.kind == sim::ParamKind::Code) ui::note("Applied when you click away.");
                    break;
                }
                case sim::ParamKind::Data: {
                    // What the viewport wrote -- the dabs of a brush: how
                    // many, and a way to start over.
                    const std::string& data = net_.text(id, p.name);
                    const size_t dabs = data.empty() ? 0 : static_cast<size_t>(std::count(data.begin(), data.end(), ';')) + 1;
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextUnformatted(dabs == 0 ? "none" : (std::to_string(dabs) + (dabs == 1 ? " dab" : " dabs")).c_str());
                    if (dabs > 0) {
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Clear")) net_.setText(id, p.name, "");
                    }
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
            if (tinted) ImGui::PopStyleColor(3);
            if (edited) net_.setParamAt(id, p.name, frame, v);
            ImGui::PopID();
        }
        ImGui::PopID();
    }
}

void SimWorkspace::networkOverview() {
    if (editingAsset()) {
        assetOverview();
        return;
    }
    ImGui::PushFont(theme::fonts().bold, 0.0f);
    ImGui::TextUnformatted(title().c_str());
    ImGui::PopFont();
    if (emptyScene()) {
        // Where to begin: what the Add menu adds, a click away.
        ui::note("An empty scene. Add what it should hold -- the solver, its look and the Output come with it -- "
                 "or open a finished one from File > Examples.");
        ImGui::Spacing();
        struct Start {
            const char* kind;
            const char* label;
        };
        static const Start starts[] = {{"fire", "Fire"},       {"smoke", "Smoke"},          {"water_block", "Water"},
                                       {"fountain", "Fountain"}, {"rain", "Rain"},           {"sphere", "Sphere"},
                                       {"box", "Box"}};
        for (size_t i = 0; i < std::size(starts); ++i) {
            if (i > 0) ImGui::SameLine();
            if (ImGui::Button(starts[i].label)) addToScene(starts[i].kind, Vec3(0.0f, 0.0f, 0.0f));
        }
        ImGui::Spacing();
        ImGui::TextDisabled("Shift+A or a right click in the viewport: the whole Add menu");
        ImGui::TextDisabled("Tab or a right click in the network: any node");
        return;
    }
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
                const std::shared_ptr<const sim::Frame> f = frameToShow();
                if (f && !f->gasTiles.empty()) {
                    const double held = static_cast<double>(f->gasTiles.size()) * sim::Tiles::kCells;
                    ImGui::Text("With gas    %.2f million  (%.1f %%)", held / 1e6,
                                100.0 * held / static_cast<double>(std::max<size_t>(f->domain.cellCount(), 1)));
                    ImGui::SetItemTooltip("Sparse: the cells of the tiles of 8 \xc3\x97 8 \xc3\x97 8 this frame has gas in -- "
                                          "all of the gas it keeps");
                }
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
            if (runner_->adopted()) {
                fs::path from(cacheFolder_);
                if (!from.has_filename()) from = from.parent_path();  // "cache/"
                ImGui::TextDisabled("            from %s", from.filename().string().c_str());
                ImGui::SetItemTooltip("Read from %s rather than simulated -- until what is simulated changes",
                                      cacheFolder_.c_str());
            } else if (runner_->stepMs() > 0.0) {
                ImGui::Text("Step        %.0f ms", runner_->stepMs());
            }
            if (preview_) {
                ImGui::TextColored(theme::vec(theme::kYellow), "Preview     grids half as fine");
                ImGui::SetItemTooltip("Simulation > Preview Resolution; a bake is at the full resolution");
            }
            if (bake_.running() || bake_.ended()) bakePanel();
            if (wedge_.any()) wedgePanel();
        } else {
            ImGui::TextColored(theme::vec(theme::kRed), "Nothing to simulate yet.");
        }
    }
    if (compiled_.ok) {
        const std::shared_ptr<const sim::Frame> f = frameToShow();
        if (f && f->profile.total() > 0.0f && ui::section("Profile", false)) profilePanel(*f);
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
    // Animated, the guides follow the frame shown.
    const int frame = compiled_.poses.empty() ? 1 : current_;
    if (guidesRevision_ == net_.revision() && guidesSelection_ == chosen && guidesFrame_ == frame) return;
    guidesRevision_ = net_.revision();
    guidesSelection_ = chosen;
    guidesFrame_ = frame;
    gl::Lines lines;
    if (guides_ && levels_.empty()) {  // inside an asset, the scene's guides wait with it
        // Not the camera looked through: its lines would start at the eye.
        const sim::Camera* camera = compiled_.hasCamera && !throughCamera_ ? &compiled_.cameraAt(frame) : nullptr;
        lines = gl::sceneGuides(compiled_.ok ? &compiled_.worldAt(frame) : nullptr, compiled_.solidsAt(frame), chosen,
                                compiled_.solver, compiled_.liquidSolver, compiled_.rain, camera);
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
    // The keys of the selected nodes, bright; the rest of the network's, dim.
    for (const int id : canvas_.selection()) {
        const std::vector<float> k = net_.keyFrames(id);
        s.keys.insert(s.keys.end(), k.begin(), k.end());
    }
    s.otherKeys = net_.keyFrames();
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
        files_.open("Open network", {".pgsim", ".pgasset"}, false, path_);
        fileAction_ = FileAction::Open;
    }
    if (ImGui::MenuItem("Open Asset\xe2\x80\xa6")) {
        files_.open("Open asset", {".pgasset"}, false, sim::AssetLibrary::userFolder() + "/");
        fileAction_ = FileAction::OpenAsset;
    }
    ImGui::SetItemTooltip("An asset's network (.pgasset) to edit: saved, it is its new version, every instance following");
    if (ImGui::BeginMenu("Examples")) {
        for (const std::string& name : sim::Network::exampleNames()) {
            if (ImGui::MenuItem(name.c_str())) openExample(name);
        }
        ImGui::EndMenu();
    }
    ImGui::Separator();
    if (editingAsset()) {
        if (ImGui::MenuItem("Save Asset", "Ctrl+S")) commitAsset(path_);
        ImGui::SetItemTooltip("A new version of the asset, written to its file: every instance follows");
        if (ImGui::MenuItem("Save Asset As\xe2\x80\xa6")) {
            files_.open("Save asset", {".pgasset"}, true,
                        path_.empty() ? sim::AssetLibrary::userFolder() + "/" + net_.asset().name + ".pgasset" : path_);
            fileAction_ = FileAction::SaveAsset;
        }
        if (!levels_.empty() && ImGui::MenuItem("Back Up", "U")) leaveRequest_ = true;
        ImGui::Separator();
    }
    if (!editingAsset() && ImGui::MenuItem("Save", "Ctrl+S")) {
        if (path_.empty()) {
            files_.open("Save network", {".pgsim"}, true, (example_.empty() ? "untitled" : example_) + ".pgsim");
            fileAction_ = FileAction::SaveAs;
        } else {
            save(path_);
        }
    }
    if (!editingAsset() && ImGui::MenuItem("Save As\xe2\x80\xa6", "Ctrl+Shift+S")) {
        files_.open("Save network", {".pgsim"}, true, path_.empty() ? (example_.empty() ? "untitled" : example_) + ".pgsim" : path_);
        fileAction_ = FileAction::SaveAs;
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Render Image\xe2\x80\xa6")) {
        files_.open("Render image", {".png", ".exr"}, true, (fs::path(renderFolder()) / (stem() + ".png")).string());
        fileAction_ = FileAction::Image;
    }
    ImGui::SetItemTooltip("The frame on screen as a PNG -- through the camera, if there is one; as an EXR, in linear "
                          "light with its passes for compositing: depth, motion vectors, masks");
    if (ImGui::MenuItem("Render Frames\xe2\x80\xa6", nullptr, false, compiled_.ok)) {
        files_.openFolder("Render frames into a folder", true, (fs::path(renderFolder()) / (stem() + "_frames")).string());
        fileAction_ = FileAction::Frames;
    }
    ImGui::SetItemTooltip("Every frame of the shot as a PNG, numbered -- as the simulation gets there");
    if (ImGui::MenuItem("Render Video\xe2\x80\xa6", nullptr, false, compiled_.ok)) chooseVideo();
    ImGui::SetItemTooltip(io::ffmpegAvailable() ? "Every frame of the shot into a video: .mp4 (H.264), .mov, .mkv, "
                                                  ".webm (VP9), .gif -- through ffmpeg -- or .avi (Motion JPEG)"
                                                : "Every frame of the shot into a video: .avi (Motion JPEG). With "
                                                  "ffmpeg installed also .mp4, .mov, .mkv, .webm and .gif");
    ImGui::Separator();
    const int shown = net_.displayed();
    if (ImGui::MenuItem("Export Geometry\xe2\x80\xa6", nullptr, false, shown != 0)) chooseExport(shown, false);
    ImGui::SetItemTooltip("The displayed node's geometry at the frame on screen: .ply points, .obj polygons, .vdb volumes");
    if (ImGui::MenuItem("Export Geometry Frames\xe2\x80\xa6", nullptr, false, shown != 0 && runner_->cached() > 0)) {
        chooseExport(shown, true);
    }
    ImGui::SetItemTooltip("The displayed node's geometry at every frame cached, a file a frame");
    if (ImGui::MenuItem("Export USD Scene\xe2\x80\xa6", nullptr, false, compiled_.ok && runner_->cached() > 0)) chooseUsd();
    ImGui::SetItemTooltip("The shot as one USD stage, for Houdini, Blender or a renderer: every frame cached -- the "
                          "displayed geometry, the pieces moving, the grit, the gas (VDB files beside it), the camera "
                          "and the light");
}

void SimWorkspace::editMenu() {
    if (ImGui::MenuItem("Undo", "Ctrl+Z", false, history_.canUndo())) undo();
    if (ImGui::MenuItem("Redo", "Ctrl+Shift+Z", false, history_.canRedo())) redo();
    ImGui::Separator();
    const std::vector<int> chosen(canvas_.selection().begin(), canvas_.selection().end());
    if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, !chosen.empty())) duplicate(chosen);
    if (ImGui::MenuItem("Delete", "Del", false, !chosen.empty())) removeNodes(chosen);
    if (ImGui::MenuItem("Bypass", "B", false, !chosen.empty())) toggleBypass(chosen);
    if (ImGui::MenuItem("Key Selection", "K", false, !chosen.empty())) keySelection();
    ImGui::SetItemTooltip("A key at the play head on where the selected nodes are: their place, turn and size");
    ImGui::Separator();
    if (ImGui::MenuItem("Make Asset\xe2\x80\xa6", nullptr, false, !chosen.empty())) {
        assetNodes_ = chosen;
        assetLabel_.clear();
        assetName_.clear();
        assetError_.clear();
        makeAssetOpen_ = true;
    }
    ImGui::SetItemTooltip("The selected geometry nodes as one asset of their own, a node of it in their place");
    const int current = canvas_.current();
    const sim::Node* cn = net_.node(current);
    if (ImGui::MenuItem("Edit Asset Contents", "I", false, cn && sim::AssetLibrary::instance().find(cn->type))) {
        enterRequest_ = current;
    }
    if (ImGui::MenuItem("Back Up", "U", false, !levels_.empty())) leaveRequest_ = true;
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
        ImGui::Separator();
        if (ImGui::MenuItem("Save Cache to Disk\xe2\x80\xa6", nullptr, false, runner_->cached() > 0)) {
            files_.openFolder("Save the cache into a folder", true, (fs::path(outputFolder()) / (stem() + "_cache")).string());
            fileAction_ = FileAction::SaveCache;
        }
        ImGui::SetItemTooltip("The frames simulated, a file each: to play, render and export again without simulating "
                              "-- here, or with prototype sim --from-cache.");
        if (ImGui::MenuItem("Load Cache from Disk\xe2\x80\xa6", nullptr, false, compiled_.ok)) {
            files_.openFolder("Load a cache: its folder", false,
                              cacheFolder_.empty() ? (fs::path(outputFolder()) / (stem() + "_cache")).string() : cacheFolder_);
            fileAction_ = FileAction::LoadCache;
        }
        ImGui::SetItemTooltip("Frames saved before, in place of simulating them -- until what is simulated changes.");
        ImGui::Separator();
        if (ImGui::MenuItem("Preview Resolution", nullptr, preview_)) {
            preview_ = !preview_;
            compiledRevision_ = ~0ull;  // the world again, at the other grids
            recompile();
            shown_.reset();
        }
        ImGui::SetItemTooltip("The gas and the water on grids half as fine: quick to work on. A bake is always at the "
                              "full resolution.");
        if (ImGui::MenuItem("Bake to Disk\xe2\x80\xa6", nullptr, false, compiled_.ok && !bake_.running() && !wedge_.running())) {
            files_.openFolder("Bake into a folder", true,
                              bakeFolder_.empty() ? (fs::path(outputFolder()) / (stem() + "_bake")).string() : bakeFolder_);
            fileAction_ = FileAction::Bake;
        }
        ImGui::SetItemTooltip("Simulates every frame at the full resolution in a process of its own, into a cache "
                              "folder: the editor stays free and plays the frames as they land. The state is saved "
                              "every %d frames: a bake cut short goes on from there (Resume Bake).", kCheckpointEvery);
        const bool resumable = !bake_.running() && !wedge_.running() && compiled_.ok && Bake::canResume(bakeFolder_, net_.save());
        if (ImGui::MenuItem("Resume Bake", nullptr, false, resumable)) startBake(bakeFolder_, true);
        ImGui::SetItemTooltip("Goes on with the bake cut short, from its last checkpoint.");
        if (ImGui::MenuItem("Cancel Bake", nullptr, false, bake_.running())) bake_.cancel();
        ImGui::SetItemTooltip("Stops the bake: the frames written stay, and its last checkpoint.");
        if (ImGui::MenuItem("Cancel Wedge", nullptr, false, wedge_.running())) wedge_.cancel();
        ImGui::SetItemTooltip("A wedge starts from a parameter: right click on its name, Wedge...");
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
    ImGui::TextDisabled("Editing the displayed geometry");
    ImGui::TextUnformatted("1  2  3  4                objects; points, edges, primitives");
    ImGui::TextUnformatted("Click, left drag          pick one, a box (Shift adds, Ctrl takes away)");
    ImGui::TextUnformatted("S, H                      box, lasso or brush; what is hidden too");
    ImGui::TextUnformatted("Alt / Space + left drag   orbit, while picking or painting");
    ImGui::TextUnformatted("W  E  R, drag a handle    move, turn, size what is picked (an Edit node)");
    ImGui::TextUnformatted("O, [ ], wheel in a drag   soft selection, its radius");
    ImGui::TextUnformatted("Ctrl+G, Del               a group of it, delete it (Group, Blast)");
    ImGui::TextUnformatted("Ctrl+A, Ctrl+I, Esc       pick all, the others, none");
    ImGui::TextUnformatted("P, [ ], Shift+wheel       paint an attribute (Ctrl: erase), brush size");
    ImGui::TextUnformatted("Tab, N                    a node on what is picked (PolyExtrude...), numbers");
    ImGui::Separator();
    ImGui::TextDisabled("Timeline");
    ImGui::TextUnformatted("Space  Home  End  Left  Right");
    ImGui::Separator();
    ImGui::TextDisabled("The same from the command line:");
    ImGui::TextUnformatted("  prototype sim campfire fire.png --every 10");
    ImGui::TextUnformatted("  prototype sim campfire fire.mp4            (every frame, a video)");
    ImGui::TextUnformatted("  prototype sim my.pgsim out.png --set fire.fuel=20");
    ImGui::TextUnformatted("  prototype sim my.pgsim - --cache my_cache");
    ImGui::TextUnformatted("  prototype sim campfire_vdb - --export-node volumes --export 'fire.$F4.vdb'");
}

void SimWorkspace::popups() {
    job_.draw();
    makeAssetDialog();
    wedgeDialog();
    std::string chosen;
    if (!files_.draw(chosen)) return;
    switch (fileAction_) {
        case FileAction::Open: open(chosen); break;
        case FileAction::SaveAs: save(chosen); break;
        case FileAction::Image: renderImage(chosen); break;
        case FileAction::Frames:
        case FileAction::Video: startRender(chosen); break;
        case FileAction::MeshFile:
            if (net_.setText(fileNode_, fileParam_, chosen)) {
                // An object that was no mesh becomes one.
                if (const sim::Node* n = net_.node(fileNode_); n && n->type == "object") {
                    net_.setParam(fileNode_, "shape", {static_cast<float>(sim::Shape::Mesh), 0.0f, 0.0f});
                }
            }
            break;
        case FileAction::ImportMesh: addMesh(chosen, addAt_); break;
        case FileAction::SaveCache: saveCache(chosen); break;
        case FileAction::LoadCache: loadCache(chosen); break;
        case FileAction::Bake: startBake(chosen, false); break;
        case FileAction::ExportGeometry: exportGeometry(fileNode_, chosen); break;
        case FileAction::ExportFrames: exportFrames(fileNode_, chosen); break;
        case FileAction::ExportUsd: exportUsd(chosen); break;
        case FileAction::OpenAsset: open(chosen); break;
        case FileAction::SaveAsset: commitAsset(chosen); break;
        case FileAction::None: break;
    }
    fileAction_ = FileAction::None;
}

sim::Domain SimWorkspace::sceneBox() const {
    return compiled_.ok && compiled_.world.any() ? gl::sceneDomain(compiled_.world) : runner_->domain();
}

std::string SimWorkspace::gridsText() const {
    const std::string times = " \xc3\x97 ", dot = "  \xc2\xb7  ";
    if (geometryOnly()) {
        // A model: what it shows, not what it would simulate.
        const GeometryPtr& g = renderer_.geometry();
        if (!g) return "no geometry shown";
        auto thousands = [](size_t n) {
            char buf[32];
            if (n >= 10000) std::snprintf(buf, sizeof buf, "%.0f k", static_cast<double>(n) / 1000.0);
            else std::snprintf(buf, sizeof buf, "%zu", n);
            return std::string(buf);
        };
        return thousands(g->pointCount()) + " points" + dot + thousands(g->primitiveCount()) + " primitives";
    }
    auto cells = [&](const sim::Domain& d) {
        return std::to_string(d.cells[0]) + times + std::to_string(d.cells[1]) + times + std::to_string(d.cells[2]);
    };
    if (emptyScene()) return "an empty scene";
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

bool SimWorkspace::geometryOnly() const {
    if (!levels_.empty() || editingAsset()) return true;
    if (!net_.displayed()) return false;
    return std::all_of(net_.nodes().begin(), net_.nodes().end(), [](const sim::Node& n) {
        const sim::NodeType* t = sim::findNodeType(n.type);
        return t && t->core;
    });
}

std::string SimWorkspace::status() const {
    if (editingAsset()) {
        const sim::AssetInfo& a = net_.asset();
        return std::to_string(net_.nodes().size()) + " nodes  \xc2\xb7  asset " + a.name + ", version " +
               std::to_string(a.version) + "  \xc2\xb7  " +
               (levels_.empty() ? std::string("Ctrl+S saves a new version") : levelsText() + "  \xc2\xb7  U goes back up");
    }
    if (emptyScene()) return "An empty scene  \xc2\xb7  Shift+A in the viewport, Tab in the network  \xc2\xb7  File > Examples";
    if (geometryOnly()) return std::to_string(net_.nodes().size()) + " nodes  \xc2\xb7  " + gridsText();
    char text[240], step[32];
    // Frames from disk were not simulated: no time a step.
    if (runner_->adopted()) std::snprintf(step, sizeof step, "from disk");
    else std::snprintf(step, sizeof step, "%.0f ms a step", runner_->stepMs());
    std::snprintf(text, sizeof text, "%zu nodes  \xc2\xb7  %s  \xc2\xb7  cache %d / %d (%.0f MB)%s  \xc2\xb7  %s",
                  net_.nodes().size(), gridsText().c_str(), runner_->cached(), compiled_.frames,
                  static_cast<double>(runner_->bytes()) / (1024.0 * 1024.0), runner_->full() ? " full" : "", step);
    std::string line = text;
    if (preview_) line += "  \xc2\xb7  preview";
    if (wedge_.running()) {
        line += "  \xc2\xb7  wedge " + std::to_string(wedge_.baking() + 1) + " / " + std::to_string(wedge_.variants().size());
    }
    if (bake_.running()) {
        line += "  \xc2\xb7  baking " + std::to_string(bake_.progress().frames) + " / " + std::to_string(bake_.frames());
        if (bake_.secondsLeft() > 0.0) line += ", " + Bake::duration(bake_.secondsLeft()) + " left";
    }
    return line;
}

void SimWorkspace::selectNode(const std::string& name) {
    if (const sim::Node* n = net_.named(name)) canvas_.select(n->id);
}

// --- images -----------------------------------------------------------------------------------------

void SimWorkspace::shotSize(int& width, int& height) const {
    width = compiled_.hasCamera ? compiled_.camera.width : std::max(viewWidth_, 64);
    height = compiled_.hasCamera ? compiled_.camera.height : std::max(viewHeight_, 64);
}

void SimWorkspace::renderShot(int width, int height, int frame) {
    // Through the camera, as it sees -- or as the viewport does -- without
    // the guides and the selection's highlight; twice the size, to be
    // averaged down.
    const gl::Orbit view = renderer_.orbit;
    if (compiled_.hasCamera) {
        const sim::Camera& camera = compiled_.cameraAt(frame);
        renderer_.orbit = gl::orbitThrough(camera, focusOf(camera));
        // The plate of that frame behind it.
        std::string why;
        if (!renderer_.setPlate(camera.plateFile(frame), camera, why)) setMessage(why, true);
    }
    renderer_.setLines({});
    renderer_.setHighlight({}, 0);
    for (int layer = 0; layer < gl::VolumeRenderer::kOverlayLayers; ++layer) {
        renderer_.setOverlay({}, layer);
        overlayKey_[layer].clear();  // back on the next frame
    }
    renderer_.render(width * 2, height * 2);
    renderer_.orbit = view;
    shownPlate_ = "\x01";  // the viewport sets its own plate again
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
    std::error_code ec;
    const fs::path parent = fs::path(path).parent_path();
    if (!parent.empty()) fs::create_directories(parent, ec);
    // Whatever went wrong before is not this render's.
    for (int i = 0; i < 16 && gl_.GetError() != 0;) ++i;
    // An EXR: the picture in linear light and its passes -- the motion to
    // the next frame's camera, when the camera moves.
    std::string ext = fs::path(path).extension().string();
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (ext == ".exr") {
        renderer_.passes.on = true;
        renderer_.passes.frameTime = compiled_.world.timeStep;
        renderer_.passes.moving = compiled_.hasCamera && !compiled_.poses.empty();
        if (renderer_.passes.moving) renderer_.passes.next = gl::orbitThrough(compiled_.cameraAt(current_ + 1), 1.0f);
        renderShot(width, height, current_);
        std::string error;
        const bool written = gl::writePassesExr(renderer_, path, "prototype " + stem() + ", frame " + std::to_string(current_), error);
        renderer_.passes.on = false;
        if (!written) {
            setMessage(error, true);
            notify(error, "", true);
            return false;
        }
        renderFolder_ = parent.string();
        const std::string done = "Rendered " + shownPath(path) + " with its passes (" + std::to_string(width) + " \xc3\x97 " +
                                 std::to_string(height) + ")";
        setMessage(done);
        notify(done, path, false);
        return true;
    }
    renderShot(width, height, current_);
    const std::vector<uint8_t> pixels = renderer_.readPixels(2);
    // What the driver says went wrong drawing it: written all the same, and said.
    std::string glError;
    if (const unsigned code = gl_.GetError()) {
        char hex[16];
        std::snprintf(hex, sizeof hex, "0x%04x", code);
        glError = std::string(" -- OpenGL reported error ") + hex + " while drawing it";
    }
    if (!gl::writePng(path, width, height, 3, pixels)) {
        const std::string error = path + ": cannot write it (" + std::strerror(errno) + ")";
        setMessage(error, true);
        notify(error, "", true);
        std::fprintf(stderr, "prototype: %s\n", error.c_str());
        return false;
    }
    renderFolder_ = parent.string();
    const std::string done = "Rendered " + shownPath(path) + " (" + std::to_string(width) + " \xc3\x97 " +
                             std::to_string(height) + ")" + glError;
    setMessage(done, !glError.empty());
    notify(done, path, false);
    std::fprintf(stderr, "prototype: rendered %s (%d x %d)%s\n", path.c_str(), width, height, glError.c_str());
    return true;
}

void SimWorkspace::startRender(const std::string& target) {
    if (!compiled_.ok) {
        setMessage("Nothing to render: the network does not compile", true);
        return;
    }
    int width = 0, height = 0;
    shotSize(width, height);
    jobWidth_ = width;
    jobHeight_ = height;
    jobReturnFrame_ = current_;
    jobWasPlaying_ = playing_;
    playing_ = false;
    runner_->setRunning(true);  // the shot is simulated as it is rendered
    std::string error;
    if (!job_.start(target, shownPath(target), stem(), width, height, 1.0 / static_cast<double>(compiled_.world.timeStep), 1,
                    std::max(1, compiled_.frames),
                    [this](int frame, std::vector<uint8_t>& rgb, std::string& why) { return drawShotFrame(frame, rgb, why); },
                    error)) {
        playing_ = jobWasPlaying_;
        setMessage(error, true);
        notify(error, "", true);
        return;
    }
    renderFolder_ = fs::path(target).parent_path().string();
}

bool SimWorkspace::drawShotFrame(int frame, std::vector<uint8_t>& rgb, std::string& error) {
    const std::shared_ptr<const sim::Frame> f = runner_->frame(frame);
    if (!f) {
        if (runner_->busy()) return false;  // on its way there
        error = runner_->full()      ? "the cache is full (Simulation > Cache Size)"
                : runner_->adopted() ? "the frames loaded from disk end"
                                     : "the simulation stopped";
        error += " at frame " + std::to_string(frame);
        return false;
    }
    // As the viewport shows it then: the frame, the objects and the look,
    // the displayed geometry.
    current_ = frame;
    if (f != shown_) {
        shown_ = f;
        renderer_.setFrame(*f);
    }
    pose(frame);
    updatePieces();
    updateGeometry();
    renderShot(jobWidth_, jobHeight_, frame);
    rgb = renderer_.readPixels(2);
    return true;
}

std::string SimWorkspace::renderFolder() const {
    std::error_code ec;
    if (!renderFolder_.empty() && fs::is_directory(renderFolder_, ec)) return renderFolder_;
    return outputFolder();
}

void SimWorkspace::chooseVideo() {
    const std::vector<std::string> kinds = io::videoExtensions();
    files_.open(io::ffmpegAvailable() ? "Render video" : "Render video (.avi -- with ffmpeg also .mp4, .webm, .gif)", kinds,
                true, (fs::path(renderFolder()) / (stem() + kinds.front())).string());
    fileAction_ = FileAction::Video;
}

void SimWorkspace::drawNotice(ImDrawList* d, ImVec2 lo, ImVec2 hi) {
    noticeLo_ = noticeHi_ = ImVec2(0.0f, 0.0f);
    if (notice_.text.empty()) return;
    if (notice_.until > 0.0 && ImGui::GetTime() > notice_.until) {
        notice_ = Notice();
        return;
    }
    // A card at the bottom: what happened, then Open and Show for what was
    // written, and a close.
    const ImGuiStyle& style = ImGui::GetStyle();
    const bool file = !notice_.path.empty();
    const float pad = theme::px(10.0f), gap = theme::px(6.0f), button = ImGui::GetFrameHeight();
    const float openW = ImGui::CalcTextSize("Open").x + 2.0f * style.FramePadding.x;
    const float showW = ImGui::CalcTextSize("Show").x + 2.0f * style.FramePadding.x;
    const float buttons = (file ? openW + showW + 2.0f * gap : 0.0f) + button;
    const float wrap = std::max(theme::px(120.0f), std::min(theme::px(560.0f), hi.x - lo.x - buttons - theme::px(80.0f)));
    const ImVec2 ts = ImGui::CalcTextSize(notice_.text.c_str(), nullptr, false, wrap);
    const float w = pad + ts.x + gap + buttons + pad, h = std::max(ts.y, button) + 2.0f * pad;
    const ImVec2 a(std::round((lo.x + hi.x - w) * 0.5f), std::round(hi.y - h - theme::px(44.0f)));
    const ImVec2 b(a.x + w, a.y + h);
    noticeLo_ = a;
    noticeHi_ = b;
    d->AddRectFilled(a, b, IM_COL32(24, 25, 29, 238), theme::px(7.0f));
    d->AddRect(a, b, notice_.error ? theme::kRed : theme::kAccentDim, theme::px(7.0f), 0, theme::px(1.0f));
    d->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(a.x + pad, a.y + (h - ts.y) * 0.5f),
               notice_.error ? theme::kRed : theme::kText, notice_.text.c_str(), nullptr, wrap);
    float x = a.x + pad + ts.x + gap;
    const float y = a.y + (h - button) * 0.5f;
    if (file) {
        std::error_code ec;
        const std::string folder = fs::is_directory(notice_.path, ec) ? notice_.path : fs::path(notice_.path).parent_path().string();
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        if (ImGui::Button("Open##notice")) openExternally(notice_.path);
        ImGui::SetItemTooltip("Open %s", notice_.path.c_str());
        x += openW + gap;
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        if (ImGui::Button("Show##notice")) openExternally(folder);
        ImGui::SetItemTooltip("The folder it is in: %s", folder.c_str());
        x += showW + gap;
    }
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    if (theme::iconButton("close_notice", Icon::Close, "Close", false, true, button)) notice_ = Notice();
}

void SimWorkspace::notify(std::string text, std::string path, bool error, bool sticky) {
    notice_.text = std::move(text);
    notice_.path = std::move(path);
    notice_.error = error;
    notice_.until = error || sticky ? 0.0 : ImGui::GetTime() + 12.0;
}

// --- the cache on disk, and export ------------------------------------------------------------------

std::string SimWorkspace::outputFolder() const {
    // The network's folder; else the current one -- unless files cannot be
    // made there (a program started from a menu may be in /) -- else home.
    std::error_code ec;
    const fs::path parent = fs::path(path_).parent_path();
    if (!parent.empty()) return parent.string();
    const fs::path here = fs::current_path(ec);
    if (!ec && writable(here)) return here.string();
    if (const char* home = std::getenv("HOME"); home && *home) return home;
    return here.string();
}

std::string SimWorkspace::stem() const {
    if (!path_.empty()) return fs::path(path_).stem().string();
    return example_.empty() ? std::string("untitled") : example_;
}

bool SimWorkspace::saveCache(const std::string& folder) {
    const int cached = runner_->cached();
    std::string error;
    uintmax_t bytes = 0;
    int written = 0;
    for (int f = 1; f <= cached; ++f) {
        const auto frame = runner_->frame(f);
        if (!frame) break;
        if (!sim::writeFrame(*frame, folder, error)) {
            setMessage(error, true);
            return false;
        }
        std::error_code ec;
        bytes += fs::file_size(sim::frameFile(folder, frame->number), ec);
        ++written;
    }
    // The frames of a longer cache saved there before are not this one's.
    std::error_code ec;
    for (int f = written + 1; fs::remove(sim::frameFile(folder, f), ec); ++f) {
    }
    sim::CacheInfo info;
    info.frames = written;
    info.fps = 1.0f / compiled_.world.timeStep;
    info.network = sim::networkHash(net_.save());
    if (written == 0 || !sim::writeCacheInfo(folder, info, error)) {
        setMessage(written == 0 ? std::string("No frame to save yet") : error, true);
        return false;
    }
    cacheFolder_ = folder;
    std::string text = "Saved " + std::to_string(written) + " frames into " + shownPath(folder) + " (" + ui::sizeText(bytes) + ")";
    if (written < compiled_.frames) text += " -- of " + std::to_string(compiled_.frames) + ": the rest are not simulated yet";
    setMessage(text);
    return true;
}

bool SimWorkspace::loadCache(const std::string& chosen) {
    // A file in the folder -- its cache.txt -- stands for the folder.
    std::error_code ec;
    const std::string folder = fs::is_directory(chosen, ec) ? chosen : fs::path(chosen).parent_path().string();
    sim::CacheInfo info;
    std::string error;
    if (!sim::readCacheInfo(folder, info, error)) {
        setMessage(error, true);
        return false;
    }
    if (!compiled_.ok) {
        setMessage("The network does not compile: its frames from disk need what it simulates", true);
        return false;
    }
    // No more than the timeline holds; read from disk as they are played.
    const int count = std::min(info.frames, std::max(1, compiled_.frames));
    std::string first;
    sim::Frame probe;
    if (!sim::readFrame(folder, 1, probe, first)) {
        setMessage(first, true);
        return false;
    }
    runner_->stream(compiled_.world, compiled_.frames, folder);
    cacheFolder_ = folder;
    shown_.reset();
    viewDirty_ = true;
    std::string text = "Loaded " + std::to_string(count) + " frames from " + shownPath(folder);
    if (info.frames > count) text += " (the network has " + std::to_string(compiled_.frames) + ")";
    if (info.network != sim::networkHash(net_.save())) {
        text += " -- they were written by another network, or another version of this one";
    }
    setMessage(text);
    return true;
}

bool SimWorkspace::startBake(const std::string& target, bool resume) {
    if (!compiled_.ok) {
        setMessage("The network does not compile: there is nothing to bake", true);
        return false;
    }
    std::string error;
    if (!bake_.start(net_.save(), folder(), target, compiled_.frames, kCheckpointEvery, resume, error)) {
        setMessage(error, true);
        return false;
    }
    bakeFolder_ = target;
    cacheFolder_ = target;
    // Its frames, as they land.
    runner_->stream(compiled_.world, compiled_.frames, target);
    shown_.reset();
    viewDirty_ = true;
    setMessage((resume ? "Resuming the bake of " : "Baking ") + std::to_string(compiled_.frames) + " frames into " +
               shownPath(target) + " in the background" + (preview_ ? ", at the full resolution" : ""));
    return true;
}

void SimWorkspace::pollBake() {
    if (wedge_.poll()) {
        const auto& variants = wedge_.variants();
        const int done = static_cast<int>(std::count_if(variants.begin(), variants.end(), [](const Wedge::Variant& v) {
            return v.state == Wedge::Variant::State::Done;
        }));
        if (!wedge_.running()) {
            const std::string text = "Wedge of " + wedge_.nodeName() + "." + wedge_.param() + ": " + std::to_string(done) +
                                     " of " + std::to_string(variants.size()) + " variants baked into " +
                                     shownPath(wedge_.root()) + " -- Show plays one";
            setMessage(text, done < static_cast<int>(variants.size()));
            notify(text, wedge_.root(), false, true);
        }
    }
    const bool was = bake_.running();
    bake_.poll();
    if (!runner_->folder().empty()) runner_->refresh();
    if (!was || bake_.running()) return;
    const std::string where = shownPath(bake_.folder());
    if (!bake_.failed()) {
        const std::string text = "Baked " + std::to_string(bake_.frames()) + " frames into " + where + " in " +
                                 Bake::duration(bake_.seconds());
        setMessage(text);
        notify(text, bake_.folder(), false, true);
    } else if (bake_.cancelled()) {
        setMessage("Bake cancelled at frame " + std::to_string(bake_.progress().frames) + " of " +
                   std::to_string(bake_.frames()) + (bake_.progress().checkpoint > 0 ? " -- Resume Bake goes on from frame " +
                                                     std::to_string(bake_.progress().checkpoint) : std::string()));
    } else {
        const std::string text = "The bake stopped at frame " + std::to_string(bake_.progress().frames) + ": " + bake_.why();
        setMessage(text, true);
        notify(text, std::string(), true, true);
    }
}

void SimWorkspace::bakePanel() {
    const sim::CacheInfo& p = bake_.progress();
    const int of = std::max(1, bake_.frames());
    char line[160];
    if (bake_.running()) {
        std::snprintf(line, sizeof line, "%d / %d", p.frames, of);
        ImGui::TextUnformatted("Bake       ");
        ImGui::SameLine();
        ImGui::ProgressBar(static_cast<float>(p.frames) / static_cast<float>(of), ImVec2(-1.0f, 0.0f), line);
        std::string when = p.stepMs > 0.0 ? Bake::duration(p.stepMs / 1000.0) + " a frame" : std::string("starting");
        if (bake_.secondsLeft() > 0.0) when += ", " + Bake::duration(bake_.secondsLeft()) + " left";
        ImGui::TextDisabled("            %s", when.c_str());
        if (p.checkpoint > 0) ImGui::TextDisabled("            checkpoint at frame %d", p.checkpoint);
        if (ImGui::SmallButton("Cancel Bake")) bake_.cancel();
        ImGui::SetItemTooltip("Stops the bake: the frames written stay, and its last checkpoint.");
    } else if (bake_.failed()) {
        ImGui::TextColored(theme::vec(bake_.cancelled() ? theme::kYellow : theme::kRed), "Bake        %s at %d / %d",
                           bake_.cancelled() ? "cancelled" : "stopped", p.frames, of);
        if (!bake_.why().empty()) ImGui::SetItemTooltip("%s", bake_.why().c_str());
    } else {
        ImGui::Text("Bake        %d frames in %s", of, Bake::duration(bake_.seconds()).c_str());
    }
}

void SimWorkspace::profilePanel(const sim::Frame& f) {
    const sim::Frame::Profile& p = f.profile;
    const float total = std::max(p.total(), 1e-3f);
    ImGui::TextDisabled("The step to frame %d: %.0f ms", f.number, static_cast<double>(p.total()));
    auto bar = [&](const char* name, float ms, float of, bool inner) {
        if (ms <= 0.0f && inner) return;
        char text[64];
        std::snprintf(text, sizeof text, "%.0f ms  %.0f %%", static_cast<double>(ms), 100.0 * ms / of);
        ImGui::TextUnformatted(inner ? "   " : "");
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::TextUnformatted(name);
        ImGui::SameLine(theme::px(110.0f));
        ImGui::ProgressBar(std::clamp(ms / of, 0.0f, 1.0f), ImVec2(-1.0f, 0.0f), text);
    };
    if (compiled_.world.hasRigid) {
        bar("Pieces", p.rigid, total, false);
        bar("Into scenes", p.scenes, total, false);
    }
    if (compiled_.world.hasGas) {
        bar("Gas", p.gas, total, false);
        static const char* stages[8] = {"solids", "tiles", "emit", "advect", "combust", "forces", "project", "dissipate"};
        for (int s = 0; s < 8; ++s) bar(stages[s], p.gasStages[s], total, true);
    }
    if (compiled_.world.hasWater) bar("Water", p.water, total, false);
    if (compiled_.world.hasRain) bar("Rain", p.rain, total, false);
    if (compiled_.world.hasCloth) bar("Cloth", p.cloth, total, false);
    ui::note("Of the whole step. Frames read from disk say nothing: a bake's time is in its bake.log.");
}

void SimWorkspace::wedgeDialog() {
    if (wedgeOpen_) {
        ImGui::OpenPopup("Wedge");
        wedgeOpen_ = false;
    }
    if (!ImGui::BeginPopupModal("Wedge", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    const sim::Node* n = net_.node(wedgeNode_);
    const sim::NodeType* t = n ? sim::findNodeType(n->type) : nullptr;
    const sim::ParamDef* def = nullptr;
    for (size_t i = 0; t && i < t->params.size(); ++i) {
        if (t->params[i].name == wedgeParam_) def = &t->params[i];
    }
    if (!def) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    const bool whole = def->kind == sim::ParamKind::Int;
    ImGui::Text("%s \xc2\xb7 %s", n->name.c_str(), def->label);
    ImGui::TextDisabled("A bake a value, one after another, at the full resolution: %d frames each.", compiled_.frames);
    ImGui::Spacing();
    const char* format = whole ? "%.0f" : "%.3g";
    // The names before the fields.
    auto label = [](const char* text) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(text);
        ImGui::SameLine();
    };
    label("From");
    ImGui::SetNextItemWidth(theme::px(100.0f));
    ImGui::InputFloat("##from", &wedgeFrom_, 0.0f, 0.0f, format);
    ImGui::SameLine();
    label("to");
    ImGui::SetNextItemWidth(theme::px(100.0f));
    ImGui::InputFloat("##to", &wedgeTo_, 0.0f, 0.0f, format);
    ImGui::SameLine();
    label("in");
    ImGui::SetNextItemWidth(theme::px(100.0f));
    ImGui::InputInt("##count", &wedgeCount_);
    ImGui::SameLine();
    ImGui::TextUnformatted("variants");
    wedgeCount_ = std::clamp(wedgeCount_, 2, 16);
    wedgeFrom_ = std::clamp(wedgeFrom_, def->lo, def->hi);
    wedgeTo_ = std::clamp(wedgeTo_, def->lo, def->hi);
    const std::vector<float> values = Wedge::values(wedgeFrom_, wedgeTo_, wedgeCount_, whole);
    std::string list;
    for (const float v : values) {
        char text[32];
        std::snprintf(text, sizeof text, whole ? "%.0f" : "%.4g", static_cast<double>(v));
        list += (list.empty() ? "" : ", ") + std::string(text);
    }
    ImGui::TextDisabled("Values: %s", list.c_str());
    label("Folder");
    ImGui::SetNextItemWidth(theme::px(460.0f));
    ImGui::InputText("##folder", &wedgeFolder_);
    ImGui::SetItemTooltip("A folder a variant goes into under it, and wedge.txt saying which value each holds");
    if (!wedgeError_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::vec(theme::kRed));
        ImGui::TextUnformatted(wedgeError_.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::Spacing();
    if (ImGui::Button("Bake the Wedge", ImVec2(theme::px(140.0f), 0.0f))) {
        std::string error;
        if (wedge_.start(net_, wedgeNode_, wedgeParam_, values, wedgeFolder_, folder(), compiled_.frames, error)) {
            wedgeShown_ = -1;
            setMessage("Baking " + std::to_string(values.size()) + " variants of " + n->name + "." + wedgeParam_ +
                       " into " + shownPath(wedgeFolder_) + ", one after another");
            ImGui::CloseCurrentPopup();
        } else {
            wedgeError_ = error;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(theme::px(120.0f), 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void SimWorkspace::wedgePanel() {
    const auto& variants = wedge_.variants();
    ImGui::Text("Wedge       %s.%s", wedge_.nodeName().c_str(), wedge_.param().c_str());
    ImGui::SetItemTooltip("%s", wedge_.root().c_str());
    for (size_t i = 0; i < variants.size(); ++i) {
        const Wedge::Variant& v = variants[i];
        ImGui::PushID(static_cast<int>(i));
        char value[48];
        std::snprintf(value, sizeof value, "  %s %-8.4g", static_cast<int>(i) == wedgeShown_ ? "\xe2\x96\xb6" : " ",
                      static_cast<double>(v.value));
        ImGui::TextUnformatted(value);
        ImGui::SameLine(theme::px(110.0f));
        using S = Wedge::Variant::State;
        const bool baking = v.state == S::Baking;
        if (baking) {
            const sim::CacheInfo& p = wedge_.bake().progress();
            char line[64];
            std::string left = wedge_.bake().secondsLeft() > 0.0 ? ", " + Bake::duration(wedge_.bake().secondsLeft()) + " left"
                                                                   : std::string();
            std::snprintf(line, sizeof line, "%d / %d%s", p.frames, wedge_.frames(), left.c_str());
            ImGui::ProgressBar(static_cast<float>(p.frames) / static_cast<float>(std::max(1, wedge_.frames())),
                               ImVec2(theme::px(200.0f), 0.0f), line);
        } else if (v.state == S::Done) {
            ImGui::TextDisabled("%-26s", ("baked in " + Bake::duration(v.seconds)).c_str());
        } else if (v.state == S::Failed) {
            ImGui::TextColored(theme::vec(theme::kRed), "%-26s", "failed");
            ImGui::SetItemTooltip("%s", v.why.c_str());
        } else {
            ImGui::TextDisabled("%-26s", v.state == S::Waiting ? "waiting" : "cancelled");
        }
        if (v.state == S::Done || baking) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Show")) showVariant(i);
            ImGui::SetItemTooltip("Its value into the parameter, its frames played from its folder");
        }
        ImGui::PopID();
    }
    if (wedge_.running()) {
        if (ImGui::SmallButton("Cancel Wedge")) wedge_.cancel();
        ImGui::SetItemTooltip("Stops the variant baking and those waiting; what is baked stays");
    }
}

void SimWorkspace::showVariant(size_t i) {
    const auto& variants = wedge_.variants();
    if (i >= variants.size() || !net_.node(wedge_.node())) return;
    const Wedge::Variant& v = variants[i];
    net_.setParam(wedge_.node(), wedge_.param(), sim::ParamValue{v.value, 0.0f, 0.0f});
    recompile();
    if (!compiled_.ok) return;
    runner_->stream(compiled_.world, compiled_.frames, v.folder);
    cacheFolder_ = v.folder;
    wedgeShown_ = static_cast<int>(i);
    shown_.reset();
    viewDirty_ = true;
    char value[32];
    std::snprintf(value, sizeof value, "%g", static_cast<double>(v.value));
    setMessage("Variant " + std::to_string(i + 1) + ": " + wedge_.nodeName() + "." + wedge_.param() + " = " + value +
               ", its frames from " + shownPath(v.folder));
}

void SimWorkspace::chooseExport(int id, bool frames) {
    const sim::Node* n = net_.node(id);
    if (!n || !geometry_->contains(id)) return;
    // What the node makes suggests the file: volumes alone go to OpenVDB,
    // the rest to PLY.
    const GeometryPtr geo = geometryOf(id);
    const bool volumes = geo && geo->volumeCount() > 0 && geo->pointCount() == 0;
    std::vector<std::string> kinds;
    for (const char* const* e = io::geometryExtensions(); *e; ++e) kinds.emplace_back(*e);
    std::stable_partition(kinds.begin(), kinds.end(), [&](const std::string& e) { return e == (volumes ? ".vdb" : ".ply"); });
    const std::string name = n->name + (frames ? ".$F4" : "") + kinds.front();
    files_.open(frames ? "Export geometry frames ($F4: the frame)" : "Export geometry", kinds, true,
                (fs::path(outputFolder()) / name).string());
    fileNode_ = id;
    fileAction_ = frames ? FileAction::ExportFrames : FileAction::ExportGeometry;
}

bool SimWorkspace::exportGeometry(int id, const std::string& path) {
    const sim::Node* n = net_.node(id);
    const GeometryPtr geo = n ? geometryOf(id) : nullptr;
    if (!geo) {
        setMessage("No geometry to export", true);
        return false;
    }
    const std::string why = geometry_->error(id);
    if (!why.empty()) {
        setMessage(n->name + ": " + why, true);
        return false;
    }
    std::error_code ec;
    if (fs::path(path).has_parent_path()) fs::create_directories(fs::path(path).parent_path(), ec);
    std::string error;
    if (!io::writeGeometry(*geo, path, error)) {
        setMessage(error, true);
        return false;
    }
    setMessage("Exported " + n->name + " at frame " + std::to_string(shownFrame()) + " to " + shownPath(path));
    return true;
}

bool SimWorkspace::exportFrames(int id, const std::string& pattern) {
    const sim::Node* n = net_.node(id);
    if (!n || !geometry_->contains(id)) return false;
    const int cached = runner_->cached();
    std::string error, last;
    int written = 0;
    for (int f = 1; f <= cached; ++f) {
        const GeometryPtr geo = geometry_->cook(id, f, compiled_.world.timeStep);
        const std::string why = geometry_->error(id);
        if (!geo || !why.empty()) {
            setMessage(n->name + " at frame " + std::to_string(f) + ": " + (why.empty() ? "no geometry" : why), true);
            return false;
        }
        last = io::framePath(pattern, f);
        std::error_code ec;
        if (fs::path(last).has_parent_path()) fs::create_directories(fs::path(last).parent_path(), ec);
        if (!io::writeGeometry(*geo, last, error)) {
            setMessage(error, true);
            return false;
        }
        ++written;
    }
    setMessage("Exported " + std::to_string(written) + " frames of " + n->name + ", the last " + shownPath(last));
    return written > 0;
}

void SimWorkspace::chooseUsd() {
    files_.open("Export USD scene", {".usda"}, true, (fs::path(outputFolder()) / (stem() + ".usda")).string());
    fileAction_ = FileAction::ExportUsd;
}

bool SimWorkspace::exportUsd(const std::string& path) {
    const int shown = net_.displayed();
    const sim::Node* n = shown ? net_.node(shown) : nullptr;
    const bool withGeometry = n && geometry_->contains(shown);
    sim::UsdExport usd(path, withGeometry ? n->name : std::string("geometry"), 1.0f / compiled_.world.timeStep);
    const int cached = runner_->cached();
    std::string error;
    for (int f = 1; f <= cached; ++f) {
        const std::shared_ptr<const sim::Frame> frame = runner_->frame(f);
        if (!frame) continue;
        const GeometryPtr geo = withGeometry ? geometry_->cook(shown, f, compiled_.world.timeStep) : nullptr;
        if (!usd.add(*frame, geo, compiled_.hasCamera ? &compiled_.cameraAt(f) : nullptr, compiled_.lookAt(f), error)) {
            setMessage(error, true);
            return false;
        }
    }
    if (!usd.finish(error)) {
        setMessage(error, true);
        return false;
    }
    std::string text = "Exported " + std::to_string(usd.frames()) + " frames as USD to " + shownPath(path);
    if (usd.bodies() > 0) text += ", " + std::to_string(usd.bodies()) + " bodies";
    if (usd.frameFiles() > 0) text += ", what changes every frame in " + std::to_string(usd.frameFiles()) + " layers beside it";
    if (usd.gasFiles() > 0) text += ", the gas in " + std::to_string(usd.gasFiles()) + " VDB files";
    setMessage(text);
    return true;
}

}  // namespace pg::editor
