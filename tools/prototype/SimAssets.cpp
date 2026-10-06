// Digital assets in the Simulation network of the editor: going into an
// asset's inside and back up, an asset made of chosen nodes, parameters
// promoted, a new version saved -- the library (pg/sim/Asset.h) holds them.
#include "SimWorkspace.h"

#include "pg/sim/Asset.h"

#include "misc/cpp/imgui_stdlib.h"

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace fs = std::filesystem;

namespace pg::editor {
namespace {

std::string labelOf(const sim::AssetInfo& info) { return info.label.empty() ? info.name : info.label; }

/// "Rock Pile" -> "rock_pile": a name made of a label.
std::string nameFromLabel(const std::string& label) {
    std::string out;
    for (const char c : label) {
        if (std::isalnum(static_cast<unsigned char>(c))) out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        else if (!out.empty() && out.back() != '_') out += '_';
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    if (!out.empty() && std::isdigit(static_cast<unsigned char>(out.front()))) out = "a_" + out;
    return out;
}

}  // namespace

std::string SimWorkspace::levelsText() const {
    if (levels_.empty()) return {};
    const std::string arrow = " \xe2\x80\xba ";
    std::string text = levels_.front().path.empty() ? std::string("scene") : fs::path(levels_.front().path).stem().string();
    for (const Level& l : levels_) {
        const sim::Node* n = l.net.node(l.instance);
        text += arrow + (n ? n->name : std::string("?"));
    }
    return text;
}

bool SimWorkspace::enterAsset(int id) {
    const sim::Node* n = net_.node(id);
    const auto def = n ? sim::AssetLibrary::instance().find(n->type) : nullptr;
    if (!def) return false;
    Level up;
    up.net = net_;
    up.snapshot = std::make_shared<const sim::Network>(net_);
    up.folder = folder();
    up.path = path_;
    up.example = example_;
    up.savedText = savedText_;
    up.history = std::move(history_);
    up.geometry = std::move(geometry_);
    up.view = canvas_.view();
    up.thumbnailsHidden = std::move(thumbnailsHidden_);
    up.instance = id;
    levels_.push_back(std::move(up));
    thumbnailsHidden_.clear();
    clearThumbnails();  // the inside's nodes are others
    ++levelsRevision_;

    net_ = *def->net;
    path_ = def->file;
    example_.clear();
    markSaved(net_.save());
    history_ = History();
    history_.reset(savedText_, networkKey());
    geometry_ = std::make_unique<sim::GeometryGraph>();
    geometry_->setFrames([this](int frame) { return runner_ ? runner_->frame(frame) : nullptr; });
    canvas_.setView({});
    canvas_.frame();
    exprMode_.clear();
    editKey_.clear();
    nameEditNode_ = 0;
    shown_.reset();
    renderer_.clearFrame();  // the scene's simulation waits; its asset alone shows
    posedRevision_ = ~0ull;
    guidesRevision_ = ~0ull;
    viewDirty_ = true;
    const std::string label = labelOf(net_.asset());
    setMessage("Inside " + label + ": what changes here changes every " + label + " -- U goes back up");
    return true;
}

bool SimWorkspace::leaveAsset() {
    if (levels_.empty()) return false;
    if (!commitAsset()) return false;
    Level up = std::move(levels_.back());
    levels_.pop_back();
    ++levelsRevision_;
    net_ = std::move(up.net);
    path_ = std::move(up.path);
    example_ = std::move(up.example);
    markSaved(std::move(up.savedText));
    history_ = std::move(up.history);
    geometry_ = std::move(up.geometry);
    canvas_.setView(up.view);
    thumbnailsHidden_ = std::move(up.thumbnailsHidden);
    clearThumbnails();
    exprMode_.clear();
    editKey_.clear();
    nameEditNode_ = 0;
    // Compiled again: the asset's geometry may be the shape of what is simulated.
    compiledRevision_ = ~0ull;
    shown_.reset();
    posedRevision_ = ~0ull;
    guidesRevision_ = ~0ull;
    viewDirty_ = true;
    return true;
}

bool SimWorkspace::commitAsset(const std::string& chosen) {
    if (!editingAsset()) return true;
    if (net_.save() == savedText_ && chosen.empty()) return true;
    sim::AssetInfo info = net_.asset();
    const auto old = sim::AssetLibrary::instance().find(info.name);
    info.version = std::max(info.version, old ? old->version : 0) + 1;
    sim::Network def = net_;
    def.setAsset(info);
    const std::string file = !chosen.empty() ? chosen
                             : !path_.empty() ? path_
                                              : (fs::path(sim::AssetLibrary::userFolder()) / (info.name + ".pgasset")).string();
    std::string error;
    // Into the library first: what cannot be an asset is not written.
    if (!sim::AssetLibrary::instance().add(def, file, error) || !sim::writeAsset(def, file, error)) {
        setMessage(error, true);
        return false;
    }
    net_.setAsset(info);
    path_ = file;
    markSaved(net_.save());
    setMessage(labelOf(info) + " version " + std::to_string(info.version) + " saved to " + file +
               ": every instance follows");
    return true;
}

std::vector<GeometryPtr> SimWorkspace::assetInputs() {
    std::vector<GeometryPtr> inputs;
    if (!levels_.empty()) {
        Level& up = levels_.back();
        const sim::Node* inst = up.net.node(up.instance);
        const sim::NodeType* t = inst ? sim::findNodeType(inst->type) : nullptr;
        for (size_t i = 0; t && i < t->inputs.size(); ++i) {
            const std::vector<sim::Link> in = up.net.linksInto(up.instance, t->inputs[i].name);
            inputs.push_back(in.empty() ? nullptr
                                        : up.geometry->cook(in.front().from, shownFrame(), compiled_.world.timeStep));
        }
    }
    return inputs;
}

void SimWorkspace::feedAssetInputs() { geometry_->setInputs(assetInputs()); }

void SimWorkspace::makeAssetDialog() {
    if (makeAssetOpen_) {
        ImGui::OpenPopup("Make Asset");
        makeAssetOpen_ = false;
    }
    if (!ImGui::BeginPopupModal("Make Asset", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::TextUnformatted(assetNodes_.size() == 1 ? "The selected node, as one asset of its own:"
                                                   : ("The " + std::to_string(assetNodes_.size()) +
                                                      " selected nodes, as one asset of their own:")
                                                         .c_str());
    ImGui::TextDisabled("in their place a node of it; what comes into them comes into it.");
    ImGui::Spacing();
    // The names before the fields, as in a parameter pane.
    auto labelled = [](const char* text) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(text);
        ImGui::SameLine(theme::px(60.0f));
        ImGui::SetNextItemWidth(theme::px(320.0f));
    };
    labelled("Label");
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    const std::string before = nameFromLabel(assetLabel_);
    if (ImGui::InputText("##label", &assetLabel_) && (assetName_.empty() || assetName_ == before)) {
        assetName_ = nameFromLabel(assetLabel_);
    }
    labelled("Name");
    ImGui::InputText("##name", &assetName_);
    ImGui::SetItemTooltip("Its node type: letters, digits and _ -- what a network's file calls it");
    const std::string file = (fs::path(sim::AssetLibrary::userFolder()) / (assetName_ + ".pgasset")).string();
    ImGui::TextDisabled("Saved to %s", file.c_str());
    if (!assetError_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::vec(theme::kRed));
        ImGui::PushTextWrapPos(theme::px(420.0f));
        ImGui::TextUnformatted(assetError_.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
    }
    const int button = ui::dialogButtons({"Make", "Cancel"});
    const bool make = button == 0 || (ImGui::IsKeyPressed(ImGuiKey_Enter) && !ImGui::IsAnyItemActive());
    if (button == 1 || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        ImGui::CloseCurrentPopup();
    } else if (make) {
        sim::AssetInfo info;
        info.name = assetName_;
        info.label = assetLabel_.empty() ? assetName_ : assetLabel_;
        std::string error;
        sim::Network def;
        if (!sim::isValidName(info.name)) {
            assetError_ = "'" + info.name + "' is not a name: letters, digits and _";
        } else if (sim::AssetLibrary::instance().find(info.name)) {
            assetError_ = "There is an asset " + info.name + " already: another name";
        } else if (const int inst = sim::collapseToAsset(net_, assetNodes_, info, file, &def, error); !inst) {
            assetError_ = error;
        } else {
            if (!sim::writeAsset(def, file, error)) setMessage(error, true);
            else setMessage(info.label + " is an asset, saved to " + file + ": double click it to go inside");
            canvas_.select(inst);
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::EndPopup();
}

void SimWorkspace::paramMenu(int id, const sim::ParamDef& p) {
    const sim::Node* n = net_.node(id);
    if (!n) return;
    ImGui::TextDisabled("%s \xc2\xb7 %s", n->name.c_str(), p.label);
    ImGui::Separator();
    if (editingAsset()) {
        const bool promoted = net_.promotion(id, p.name) != nullptr;
        if (ImGui::MenuItem(promoted ? "Unpromote" : "Promote to the Asset")) net_.promote(id, p.name, !promoted);
        ImGui::SetItemTooltip(promoted ? "No longer a parameter of the asset's node: the value here is what it has"
                                       : "A parameter of the asset's node: each of them sets it; the value here is "
                                         "the default");
    }
    if (ImGui::MenuItem("Copy Reference")) {
        ImGui::SetClipboardText(("ch(\"../" + n->name + "/" + p.name + "\")").c_str());
        setMessage("Copied ch(\"../" + n->name + "/" + p.name + "\"): paste it into an expression (fx)");
    }
    ImGui::SetItemTooltip("ch(\"../node/param\"), for an expression of another parameter");
    if ((p.kind == sim::ParamKind::Float || p.kind == sim::ParamKind::Int) && !editingAsset()) {
        const bool busy = wedge_.running() || bake_.running();
        if (ImGui::MenuItem("Wedge\xe2\x80\xa6", nullptr, false, compiled_.ok && !busy)) {
            const float now = net_.param(id, p.name)[0];
            wedgeNode_ = id;
            wedgeParam_ = p.name;
            // Round what it is: half of it to half as much again, or the
            // slider's run when it is 0.
            wedgeFrom_ = now != 0.0f ? std::clamp(now * 0.5f, p.lo, p.hi) : p.min;
            wedgeTo_ = now != 0.0f ? std::clamp(now * 1.5f, p.lo, p.hi) : p.max;
            wedgeCount_ = 4;
            wedgeFolder_ = (fs::path(outputFolder()) / (stem() + "_wedge_" + n->name + "_" + p.name)).string();
            wedgeError_.clear();
            wedgeOpen_ = true;
        }
        ImGui::SetItemTooltip(busy ? "A bake or a wedge is running"
                                   : "Bakes the simulation with this parameter at several values, one after another, "
                                     "at the full resolution -- to choose between them by looking");
    }
}

void SimWorkspace::assetOverview() {
    sim::AssetInfo info = net_.asset();
    ImGui::PushFont(theme::fonts().bold, 0.0f);
    ImGui::Text("%s", labelOf(info).c_str());
    ImGui::PopFont();
    ImGui::SameLine();
    ImGui::TextDisabled("asset %s, version %d", info.name.c_str(), info.version);
    if (!levels_.empty()) ImGui::TextDisabled("%s", levelsText().c_str());
    ui::note(path_.empty() ? "Carried by the program or a network: saved, it goes to your folder of assets."
                           : ("From " + path_).c_str());
    ImGui::Spacing();
    bool changed = false;
    if (ui::section("Asset")) {
        ui::rowLabel("Label", false, "What its node is called in the Tab menu and on the canvas");
        changed |= ImGui::InputText("##label", &info.label);
        ui::rowLabel("Help", false, "What its node's tooltip and parameters say it does");
        changed |= ImGui::InputTextMultiline("##help", &info.help, ImVec2(-1.0f, ImGui::GetTextLineHeight() * 4.0f));
    }
    if (ui::section("Promoted parameters")) {
        if (info.promoted.empty()) {
            ui::note("None yet. Right click on a parameter's name, Promote to the Asset: it shows on the asset's node, "
                     "each of them with a value of its own.");
        }
        for (size_t i = 0; i < info.promoted.size(); ++i) {
            const sim::Promotion& pr = info.promoted[i];
            ImGui::PushID(static_cast<int>(i));
            const sim::Node* n = net_.named(pr.node);
            if (ImGui::SmallButton("\xc3\x97") && n) {
                net_.promote(n->id, pr.param, false);
                ImGui::PopID();
                return;
            }
            ImGui::SetItemTooltip("Unpromote");
            ImGui::SameLine();
            if (ImGui::Selectable((pr.label.empty() ? pr.name : pr.label).c_str()) && n) {
                canvas_.select(n->id);
                canvas_.reveal(n->id);
            }
            ImGui::SameLine();
            ImGui::TextDisabled("%s \xe2\x86\x90 %s.%s", pr.name.c_str(), pr.node.c_str(), pr.param.c_str());
            ImGui::PopID();
        }
    }
    if (ui::section("Inputs")) {
        int inputs = 0;
        for (const sim::Node& n : net_.nodes()) {
            if (n.type != "asset_input") continue;
            ++inputs;
            ImGui::Text("%d  %s", static_cast<int>(net_.param(n.id, "index")[0]), n.name.c_str());
        }
        if (!inputs) ui::note("None: it makes its geometry of nothing. An Asset Input node brings in what is linked into it.");
    }
    ImGui::Spacing();
    if (ImGui::Button("Save Asset")) commitAsset(path_);
    ImGui::SetItemTooltip("A new version, written to its file: every instance follows (Ctrl+S)");
    if (!levels_.empty()) {
        ImGui::SameLine();
        if (ImGui::Button("Back Up")) leaveRequest_ = true;
        ImGui::SetItemTooltip("To the network it is in, the new version saved (U)");
    }
    if (changed) net_.setAsset(info);
}

}  // namespace pg::editor
