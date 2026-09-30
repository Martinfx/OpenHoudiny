#include "Editor.h"

#include <algorithm>
#include <cmath>
#include <filesystem>

namespace fs = std::filesystem;

namespace pg::editor {

Editor::Editor(const gl::Api& gl, std::vector<std::string> libraryFiles, std::string examplesDir, bool synchronous)
    : sim_(std::make_unique<SimWorkspace>(gl, synchronous)),
      shaders_(std::make_unique<ShaderWorkspace>(gl, std::move(libraryFiles), std::move(examplesDir))) {
    workspaces_ = {sim_.get(), shaders_.get()};
}

bool Editor::open(const std::string& path) {
    for (size_t i = 0; i < workspaces_.size(); ++i) {
        if (!workspaces_[i]->canOpen(path)) continue;
        active_ = i;
        return workspaces_[i]->open(path);
    }
    // Not by its extension: whichever reads it.
    for (size_t i = 0; i < workspaces_.size(); ++i) {
        if (workspaces_[i]->open(path)) {
            active_ = i;
            return true;
        }
    }
    return false;
}

bool Editor::openExample(const std::string& name) {
    active_ = 0;
    return sim_->openExample(name);
}

void Editor::showShaders() { active_ = 1; }
void Editor::showSimulation() { active_ = 0; }

std::string Editor::title() const {
    return current().title() + " \xe2\x80\x94 " + current().name() + " \xe2\x80\x94 Prototype";
}

void Editor::menuBar() {
    if (!ImGui::BeginMenuBar()) return;
    // The mark: a flame on an orange tile.
    ImDrawList* d = ImGui::GetWindowDrawList();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float h = ImGui::GetFrameHeight();
    d->AddRectFilled(ImVec2(at.x, at.y + h * 0.12f), ImVec2(at.x + h * 0.76f, at.y + h * 0.88f), theme::kAccent, h * 0.18f);
    theme::drawIcon(d, theme::Icon::Source, ImVec2(at.x + h * 0.38f, at.y + h * 0.5f), h * 0.5f, IM_COL32(40, 22, 10, 255));
    ImGui::Dummy(ImVec2(h * 0.8f, h * 0.5f));
    ImGui::SameLine();
    ImGui::PushFont(theme::fonts().bold, 0.0f);
    ImGui::TextUnformatted("Prototype");
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(theme::px(6.0f), 0.0f));

    Workspace& w = current();
    if (ImGui::BeginMenu("File")) {
        w.fileMenu();
        ImGui::Separator();
        if (ImGui::MenuItem("Quit", "Ctrl+Q")) quit_ = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
        w.editMenu();
        ImGui::EndMenu();
    }
    w.menus();
    if (ImGui::BeginMenu("Help")) {
        w.helpMenu();
        ImGui::Separator();
        if (ImGui::MenuItem("About prototype")) about_ = true;
        ImGui::EndMenu();
    }

    // The networks, as a switch in the middle.
    const char* names[] = {"Simulation", "Shaders"};
    const theme::Icon icons[] = {theme::Icon::Source, theme::Icon::Shader};
    const float button = theme::px(118.0f);
    const float total = button * 2.0f;
    const float mid = ImGui::GetWindowPos().x + ImGui::GetWindowWidth() * 0.5f - total * 0.5f;
    const float x = std::max(ImGui::GetCursorScreenPos().x + theme::px(20.0f), mid);
    const float y = ImGui::GetCursorScreenPos().y;
    d->AddRectFilled(ImVec2(x - 2.0f, y - 1.0f), ImVec2(x + total + 2.0f, y + h + 1.0f), IM_COL32(20, 21, 24, 255), theme::px(6.0f));
    for (size_t i = 0; i < 2; ++i) {
        ImGui::SetCursorScreenPos(ImVec2(x + button * static_cast<float>(i), y));
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::InvisibleButton("ws", ImVec2(button, h))) active_ = i;
        const bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();
        const ImVec2 lo = ImGui::GetItemRectMin(), hi = ImGui::GetItemRectMax();
        const bool on = active_ == i;
        if (on) d->AddRectFilled(ImVec2(lo.x + 2, lo.y + 2), ImVec2(hi.x - 2, hi.y - 2), IM_COL32(58, 60, 68, 255), theme::px(5.0f));
        else if (hovered) d->AddRectFilled(ImVec2(lo.x + 2, lo.y + 2), ImVec2(hi.x - 2, hi.y - 2), IM_COL32(40, 42, 48, 255), theme::px(5.0f));
        const ImVec2 t = ImGui::CalcTextSize(names[i]);
        const float iconSize = h * 0.5f;
        const float start = lo.x + (button - t.x - iconSize - theme::px(6.0f)) * 0.5f;
        theme::drawIcon(d, icons[i], ImVec2(start + iconSize * 0.5f, (lo.y + hi.y) * 0.5f), iconSize,
                        on ? theme::kAccent : theme::kTextDim);
        d->AddText(ImVec2(start + iconSize + theme::px(6.0f), lo.y + (h - t.y) * 0.5f), on ? theme::kText : theme::kTextDim,
                   names[i]);
    }

    // The file, at the right.
    const std::string file = w.title();
    const float fw = ImGui::CalcTextSize(file.c_str()).x;
    const float right = ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - fw - theme::px(14.0f);
    if (right > x + total + theme::px(20.0f)) {
        d->AddText(ImVec2(right, y + (h - ImGui::GetTextLineHeight()) * 0.5f), w.modified() ? theme::kText : theme::kTextDim,
                   file.c_str());
    }
    ImGui::EndMenuBar();
}

void Editor::statusBar(float height) {
    ImDrawList* d = ImGui::GetWindowDrawList();
    const ImVec2 lo = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    d->AddRectFilled(lo, ImVec2(lo.x + width, lo.y + height), IM_COL32(24, 25, 28, 255));
    const float ty = lo.y + (height - ImGui::GetTextLineHeight()) * 0.5f;
    const Workspace& w = current();
    const std::string left = w.status();
    d->AddText(ImVec2(lo.x + theme::px(10.0f), ty), theme::kTextDim, left.c_str());
    if (!w.message().empty()) {
        const float mw = ImGui::CalcTextSize(w.message().c_str()).x;
        const float x = std::max(lo.x + width - mw - theme::px(12.0f), lo.x + ImGui::CalcTextSize(left.c_str()).x + theme::px(40.0f));
        if (w.messageIsError()) {
            theme::drawIcon(d, theme::Icon::Error, ImVec2(x - theme::px(12.0f), ty + ImGui::GetTextLineHeight() * 0.5f),
                            ImGui::GetTextLineHeight() * 0.8f, theme::kRed);
        }
        d->AddText(ImVec2(x, ty), w.messageIsError() ? theme::kRed : theme::kTextDim, w.message().c_str());
    }
    ImGui::Dummy(ImVec2(width, height));
}

void Editor::frame(float dt) {
    if (shown_ != active_) {
        if (shown_ < workspaces_.size()) workspaces_[shown_]->hidden();
        shown_ = active_;
    }
    Workspace& w = current();
    w.update(dt);
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Q)) quit_ = true;
    if (!ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) w.shortcuts();

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(theme::px(8.0f), theme::px(6.0f)));
    ImGui::Begin("prototype", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoBringToFrontOnFocus |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(theme::px(10.0f), theme::px(6.0f)));
    menuBar();
    ImGui::PopStyleVar(2);

    // The panels: viewport and bottom on the left; parameters over the
    // network on the right.
    const float statusHeight = ImGui::GetFrameHeight() + theme::px(2.0f);
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float height = std::max(100.0f, avail.y - statusHeight);
    const float split = theme::px(5.0f);
    if (rightWidth_ <= 0.0f) rightWidth_ = std::round(avail.x * 0.45f);
    if (paramsHeight_ <= 0.0f) paramsHeight_ = std::round(height * 0.46f);
    if (bottomHeight_ <= 0.0f) bottomHeight_ = shaders_->bottomHeight();
    rightWidth_ = std::clamp(rightWidth_, theme::px(300.0f), std::max(theme::px(300.0f), avail.x - theme::px(320.0f)));
    paramsHeight_ = std::clamp(paramsHeight_, theme::px(140.0f), std::max(theme::px(140.0f), height - theme::px(160.0f)));
    const float leftWidth = avail.x - rightWidth_ - split;

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    ImGui::BeginGroup();
    {
        const bool resizable = active_ == 1;  // the timeline has its height; the code panel can grow
        const float bottom = resizable ? std::clamp(bottomHeight_, theme::px(120.0f), std::max(theme::px(120.0f), height - theme::px(160.0f)))
                                       : w.bottomHeight();
        const float viewHeight = height - bottom - (resizable ? split : 0.0f);
        ImGui::BeginChild("viewport", ImVec2(leftWidth, viewHeight), ImGuiChildFlags_None,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(theme::px(8.0f), theme::px(6.0f)));
        w.viewport(ImVec2(leftWidth, viewHeight));
        ImGui::PopStyleVar();
        ImGui::EndChild();
        if (resizable) {
            float top = viewHeight;
            ui::splitter("split_bottom", false, top, theme::px(160.0f), height - theme::px(120.0f) - split, leftWidth);
            bottomHeight_ = height - top - split;
        }
        ImGui::BeginChild("bottom", ImVec2(leftWidth, bottom), ImGuiChildFlags_None,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(theme::px(8.0f), theme::px(6.0f)));
        w.bottom(ImVec2(leftWidth, bottom));
        ImGui::PopStyleVar();
        ImGui::EndChild();
    }
    ImGui::EndGroup();
    ImGui::SameLine(0.0f, 0.0f);
    {
        float left = leftWidth;
        ui::splitter("split_right", true, left, theme::px(320.0f), avail.x - theme::px(300.0f) - split, height);
        rightWidth_ = avail.x - left - split;
    }
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::BeginGroup();
    {
        ImGui::BeginChild("parameters", ImVec2(rightWidth_, paramsHeight_), ImGuiChildFlags_None,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(theme::px(8.0f), theme::px(6.0f)));
        w.parameters(ImVec2(rightWidth_, paramsHeight_));
        ImGui::PopStyleVar();
        ImGui::EndChild();
        ui::splitter("split_params", false, paramsHeight_, theme::px(140.0f), height - theme::px(160.0f), rightWidth_);
        const float networkHeight = height - paramsHeight_ - split;
        ImGui::BeginChild("network", ImVec2(rightWidth_, networkHeight), ImGuiChildFlags_None,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(theme::px(8.0f), theme::px(6.0f)));
        w.network(ImVec2(rightWidth_, networkHeight));
        ImGui::PopStyleVar();
        ImGui::EndChild();
    }
    ImGui::EndGroup();
    statusBar(statusHeight);
    ImGui::PopStyleVar();

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(theme::px(8.0f), theme::px(6.0f)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(theme::px(12.0f), theme::px(12.0f)));
    w.popups();
    if (about_) {
        ImGui::OpenPopup("About Prototype");
        about_ = false;
    }
    if (ImGui::BeginPopupModal("About Prototype", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::PushFont(theme::fonts().bold, 0.0f);
        ImGui::TextUnformatted("Prototype");
        ImGui::PopFont();
        ImGui::TextUnformatted("Procedural geometry, simulations and shaders, built from nodes.");
        ImGui::TextDisabled("Geometry: nodes like Houdini's SOPs, attributes, wrangle.");
        ImGui::TextDisabled("Simulations: smoke and fire, water, rain -- cached, exported, rendered.");
        ImGui::TextDisabled("Shaders: GLSL, Vulkan GLSL, HLSL and more from one graph.");
        ImGui::Spacing();
        if (ImGui::Button("Close", ImVec2(theme::px(120.0f), 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(2);
    ImGui::End();
}

}  // namespace pg::editor
