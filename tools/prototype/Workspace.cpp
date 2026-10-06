#include "Workspace.h"

#include "Recovery.h"
#include "Theme.h"

#include <filesystem>

namespace pg::editor {
namespace {

constexpr const char* kUnsaved = "Unsaved Changes";

}  // namespace

void Workspace::unlessUnsaved(std::string doing, std::function<void()> then) {
    if (!unsaved()) {
        then();
        return;
    }
    doing_ = std::move(doing);
    afterAsking_ = std::move(then);
    ask_ = true;
}

void Workspace::unsavedDialog() {
    if (ask_) {
        ImGui::OpenPopup(kUnsaved);
        ask_ = false;
    }
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f), ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal(kUnsaved, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    std::string name = title();
    if (name.size() > 2 && name.compare(name.size() - 2, 2, " *") == 0) name.resize(name.size() - 2);
    ImGui::PushFont(theme::fonts().bold, 0.0f);
    ImGui::Text("%s has changes that are not saved.", name.c_str());
    ImGui::PopFont();
    ImGui::TextDisabled("Save them before %s? Not saved, they are lost.", doing_.c_str());
    const bool typing = ImGui::IsAnyItemActive();
    int answer = ui::dialogButtons({"Save", "Don't Save", "Cancel"});
    if (answer < 0 && !typing) {
        if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) answer = 0;
        else if (ImGui::IsKeyPressed(ImGuiKey_D, false)) answer = 1;
        else if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) answer = 2;
    }
    if (answer >= 0) {
        ImGui::CloseCurrentPopup();
        std::function<void()> then = std::move(afterAsking_);
        afterAsking_ = nullptr;
        if (answer == 0) saveThen(std::move(then));
        else if (answer == 1 && then) then();
    }
    ImGui::EndPopup();
}

void Workspace::autosave(const std::string& folder, double now) {
    if (folder.empty() || now - checkedAt_ < 1.0) return;  // a look a second: unsaved() writes the network
    checkedAt_ = now;
    if (!unsaved()) {
        // Saved -- or what was not is gone: so is its copy.
        dropAutosave();
        return;
    }
    if (now - autosavedAt_ < kAutosaveSeconds) return;
    const std::string text = documentText();
    if (text == autosavedText_) return;
    autosavedAt_ = now;
    std::string error;
    const std::string file = writeAutosave(folder, documentExtension(), documentPath(), documentExample(), text, error);
    if (file.empty()) return;  // tried again in kAutosaveSeconds
    std::error_code ec;
    if (!autosaveFile_.empty() && autosaveFile_ != file) std::filesystem::remove(autosaveFile_, ec);  // saved as another
    autosaveFile_ = file;
    autosavedText_ = text;
}

void Workspace::dropAutosave() {
    if (!autosaveFile_.empty()) {
        std::error_code ec;
        std::filesystem::remove(autosaveFile_, ec);
    }
    autosaveFile_.clear();
    autosavedText_.clear();
    autosavedAt_ = -1e30;
}

}  // namespace pg::editor
