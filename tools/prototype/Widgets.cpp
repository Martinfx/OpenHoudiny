#include "Widgets.h"

#include "imgui_internal.h"
#include "misc/cpp/imgui_stdlib.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>

namespace fs = std::filesystem;

namespace pg::editor::ui {
namespace {

/// A path typed by hand: "~" and "~/..." are the home folder.
fs::path typed(const std::string& text) {
    if (text == "~" || text.rfind("~/", 0) == 0) {
        if (const char* home = std::getenv("HOME"); home && *home) return fs::path(home) / text.substr(std::min<size_t>(2, text.size()));
    }
    return fs::path(text);
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

/// A dialog's main button: in the accent colour, white letters.
bool accentButton(const char* label, ImVec2 size) {
    ImGui::PushStyleColor(ImGuiCol_Button, theme::vec(IM_COL32(204, 110, 38, 255)));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::vec(IM_COL32(226, 130, 54, 255)));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme::vec(IM_COL32(182, 96, 30, 255)));
    ImGui::PushStyleColor(ImGuiCol_Text, theme::vec(IM_COL32(255, 255, 255, 255)));
    const bool clicked = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    return clicked;
}

}  // namespace

std::string sizeText(uintmax_t bytes) {
    char buf[32];
    if (bytes < 1024) std::snprintf(buf, sizeof buf, "%ju B", bytes);
    else if (bytes < 1024 * 1024) std::snprintf(buf, sizeof buf, "%.1f KB", static_cast<double>(bytes) / 1024.0);
    else std::snprintf(buf, sizeof buf, "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    return buf;
}

// --- panels ------------------------------------------------------------------------------

namespace {

/// A header's strip across the window and its icon; the cursor goes on
/// under it -- unless `advance` is false, for what is put into the strip
/// first (then the caller ends with the strip's Dummy).
PanelHeader headerStrip(theme::Icon icon, bool advance = true) {
    PanelHeader h;
    const float height = ImGui::GetFrameHeight() + theme::px(6.0f);
    h.min = ImGui::GetCursorScreenPos();
    h.max = ImVec2(h.min.x + ImGui::GetContentRegionAvail().x, h.min.y + height);
    h.right = h.max.x - theme::px(4.0f);
    ImDrawList* d = ImGui::GetWindowDrawList();
    d->AddRectFilled(h.min, h.max, theme::kHeader);
    d->AddLine(ImVec2(h.min.x, h.max.y - 0.5f), ImVec2(h.max.x, h.max.y - 0.5f), IM_COL32(0, 0, 0, 90));
    theme::drawIcon(d, icon, ImVec2(h.min.x + theme::px(16.0f), (h.min.y + h.max.y) * 0.5f), theme::px(14.0f),
                    theme::kTextDim);
    if (advance) ImGui::Dummy(ImVec2(h.max.x - h.min.x, height));
    return h;
}

}  // namespace

PanelHeader panelHeader(theme::Icon icon, const char* title, const char* info) {
    PanelHeader h = headerStrip(icon);
    ImDrawList* d = ImGui::GetWindowDrawList();
    ImFont* bold = theme::fonts().bold;
    const float size = ImGui::GetFontSize();
    const ImVec2 at(h.min.x + theme::px(30.0f), (h.min.y + h.max.y - size) * 0.5f);
    d->AddText(bold, size, at, theme::kText, title);
    if (info && *info) {
        const float w = bold->CalcTextSizeA(size, FLT_MAX, 0.0f, title).x;
        d->AddText(ImVec2(at.x + w + theme::px(10.0f), at.y), theme::kTextDim, info);
    }
    return h;
}

PanelHeader tabHeader(theme::Icon icon, int& current, const std::vector<const char*>& tabs, const char* info) {
    PanelHeader h = headerStrip(icon, false);
    ImDrawList* d = ImGui::GetWindowDrawList();
    ImFont* bold = theme::fonts().bold;
    const float size = ImGui::GetFontSize();
    const float pad = theme::px(10.0f);
    const float ty = (h.min.y + h.max.y - size) * 0.5f;
    float x = h.min.x + theme::px(30.0f) - pad;  // the first tab's text where a title's would be
    for (size_t i = 0; i < tabs.size(); ++i) {
        const float w = bold->CalcTextSizeA(size, FLT_MAX, 0.0f, tabs[i]).x + 2.0f * pad;
        ImGui::SetCursorScreenPos(ImVec2(x, h.min.y));
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::InvisibleButton("##tab", ImVec2(w, h.max.y - h.min.y))) current = static_cast<int>(i);
        const bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();
        const bool on = current == static_cast<int>(i);
        if (hovered && !on) {
            d->AddRectFilled(ImVec2(x, h.min.y + theme::px(4.0f)), ImVec2(x + w, h.max.y - theme::px(4.0f)),
                             theme::fade(IM_COL32_WHITE, 0.05f), theme::px(4.0f));
        }
        d->AddText(bold, size, ImVec2(x + pad, ty), on || hovered ? theme::kText : theme::kTextDim, tabs[i]);
        if (on) {
            d->AddRectFilled(ImVec2(x + pad * 0.6f, h.max.y - theme::px(3.0f)), ImVec2(x + w - pad * 0.6f, h.max.y - theme::px(1.0f)),
                             theme::kAccent, theme::px(1.0f));
        }
        x += w;
    }
    if (info && *info) d->AddText(ImVec2(x + pad, ty), theme::kTextDim, info);
    // The strip's own item last: the cursor goes on under it.
    ImGui::SetCursorScreenPos(h.min);
    ImGui::Dummy(ImVec2(h.max.x - h.min.x, h.max.y - h.min.y));
    return h;
}

ImVec2 overlayText(ImDrawList* d, ImVec2 at, ImU32 color, const char* text, ImFont* font, float size) {
    if (!font) font = ImGui::GetFont();
    if (size <= 0.0f) size = ImGui::GetFontSize();
    const ImVec2 t = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
    const float px = theme::px(6.0f), py = theme::px(2.5f);
    d->AddRectFilled(ImVec2(at.x - px, at.y - py), ImVec2(at.x + t.x + px, at.y + t.y + py), IM_COL32(16, 17, 20, 168),
                     theme::px(5.0f));
    d->AddText(font, size, at, color, text);
    return t;
}

namespace {

// What a menu item declares as its icon: an em space, as wide as the text is
// tall -- Dear ImGui keeps a column that wide for every item of the menu,
// and the drawn icon goes into it.
constexpr const char* kIconRoom = "\xe2\x80\x83";

void menuIcon(ImVec2 at, theme::Icon icon, ImU32 color, bool enabled) {
    const float h = ImGui::GetTextLineHeight();
    theme::drawIcon(ImGui::GetWindowDrawList(), icon, ImVec2(at.x + h * 0.5f, at.y + h * 0.5f), h * 0.95f,
                    enabled ? color : theme::fade(color, 0.4f));
}

}  // namespace

bool iconMenuItem(theme::Icon icon, ImU32 color, const char* label, const char* shortcut, bool selected, bool enabled) {
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::MenuItemEx(label, kIconRoom, shortcut, selected, enabled);
    menuIcon(at, icon, color, enabled);
    return clicked;
}

bool beginIconMenu(theme::Icon icon, ImU32 color, const char* label, bool enabled) {
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const bool open = ImGui::BeginMenuEx(label, kIconRoom, enabled);
    // Drawn into the parent menu: when the submenu is open, its window is
    // the current one.
    ImDrawList* d = open ? ImGui::GetCurrentWindow()->ParentWindow->DrawList : ImGui::GetWindowDrawList();
    const float h = ImGui::GetTextLineHeight();
    theme::drawIcon(d, icon, ImVec2(at.x + h * 0.5f, at.y + h * 0.5f), h * 0.95f, enabled ? color : theme::fade(color, 0.4f));
    return open;
}

int dialogButtons(const std::vector<const char*>& labels, bool firstEnabled) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float gap = theme::px(8.0f);
    std::vector<float> widths;
    float total = 0.0f;
    for (const char* l : labels) {
        widths.push_back(std::max(theme::px(96.0f), ImGui::CalcTextSize(l).x + 2.0f * style.FramePadding.x));
        total += widths.back() + (widths.size() > 1 ? gap : 0.0f);
    }
    ImGui::Dummy(ImVec2(0.0f, theme::px(4.0f)));
    const float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), right - total));
    int clicked = -1;
    for (size_t i = 0; i < labels.size(); ++i) {
        if (i > 0) ImGui::SameLine(0.0f, gap);
        const bool main = i == 0;
        ImGui::BeginDisabled(main && !firstEnabled);
        if (main ? accentButton(labels[i], ImVec2(widths[i], 0.0f)) : ImGui::Button(labels[i], ImVec2(widths[i], 0.0f))) {
            clicked = static_cast<int>(i);
        }
        ImGui::EndDisabled();
    }
    return clicked;
}

bool headerButton(PanelHeader& header, const char* id, theme::Icon icon, const char* tooltip, bool on, bool enabled) {
    const float side = ImGui::GetFrameHeight();
    header.right -= side;
    const ImVec2 back = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos(ImVec2(header.right, (header.min.y + header.max.y - side) * 0.5f));
    const bool clicked = theme::iconButton(id, icon, tooltip, on, enabled, side);
    header.right -= theme::px(2.0f);
    ImGui::SetCursorScreenPos(back);
    return clicked;
}

bool closePopupOnEscape() {
    ImGuiContext& g = *ImGui::GetCurrentContext();
    if (g.OpenPopupStack.empty() || !ImGui::IsKeyPressed(ImGuiKey_Escape, false)) return false;
    const ImGuiWindow* top = g.OpenPopupStack.back().Window;
    if (top && (top->Flags & ImGuiWindowFlags_Modal)) return false;
    ImGui::ClosePopupToLevel(g.OpenPopupStack.Size - 1, true);
    ImGui::SetKeyOwner(ImGuiKey_Escape, ImGui::GetID("##escape"), ImGuiInputFlags_LockThisFrame);
    return true;
}

void splitter(const char* id, bool vertical, float& size, float min, float max, float length) {
    const float thickness = theme::px(5.0f);
    ImGui::PushID(id);
    ImGui::InvisibleButton("##split", vertical ? ImVec2(thickness, length) : ImVec2(length, thickness));
    const bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
    if (hot) ImGui::SetMouseCursor(vertical ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS);
    if (ImGui::IsItemActive()) {
        const ImVec2 delta = ImGui::GetIO().MouseDelta;
        size = std::clamp(size + (vertical ? delta.x : delta.y), min, std::max(min, max));
    }
    const ImVec2 lo = ImGui::GetItemRectMin(), hi = ImGui::GetItemRectMax();
    ImDrawList* d = ImGui::GetWindowDrawList();
    d->AddRectFilled(lo, hi, IM_COL32(18, 19, 21, 255));
    if (hot) {
        const ImVec2 c((lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f);
        const float half = theme::px(16.0f);
        if (vertical) d->AddLine(ImVec2(c.x, c.y - half), ImVec2(c.x, c.y + half), theme::kAccent, theme::px(2.0f));
        else d->AddLine(ImVec2(c.x - half, c.y), ImVec2(c.x + half, c.y), theme::kAccent, theme::px(2.0f));
    }
    ImGui::PopID();
}

// --- sections and rows -------------------------------------------------------------------

bool section(const char* title, bool openByDefault) {
    ImGuiStorage* storage = ImGui::GetStateStorage();
    const ImGuiID id = ImGui::GetID(title);
    bool open = storage->GetBool(id, openByDefault);
    ImGui::Dummy(ImVec2(0.0f, theme::px(2.0f)));
    const ImVec2 lo = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x, height = ImGui::GetFrameHeight();
    if (ImGui::InvisibleButton(title, ImVec2(width, height))) {
        open = !open;
        storage->SetBool(id, open);
    }
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* d = ImGui::GetWindowDrawList();
    const float cy = lo.y + height * 0.5f;
    if (hovered) d->AddRectFilled(lo, ImVec2(lo.x + width, lo.y + height), theme::fade(IM_COL32_WHITE, 0.03f), theme::px(4.0f));
    // The chevron, turned down when open.
    const ImVec2 c(lo.x + theme::px(9.0f), cy);
    const float r = theme::px(4.0f);
    const ImU32 col = hovered ? theme::kText : theme::kTextDim;
    if (open) d->AddTriangleFilled(ImVec2(c.x - r, c.y - r * 0.5f), ImVec2(c.x + r, c.y - r * 0.5f), ImVec2(c.x, c.y + r * 0.7f), col);
    else d->AddTriangleFilled(ImVec2(c.x - r * 0.5f, c.y - r), ImVec2(c.x - r * 0.5f, c.y + r), ImVec2(c.x + r * 0.7f, c.y), col);
    ImFont* bold = theme::fonts().bold;
    const float size = ImGui::GetFontSize() * 0.92f;
    d->AddText(bold, size, ImVec2(lo.x + theme::px(20.0f), cy - size * 0.5f), col, title);
    const float tw = bold->CalcTextSizeA(size, FLT_MAX, 0.0f, title).x;
    d->AddLine(ImVec2(lo.x + theme::px(28.0f) + tw, cy), ImVec2(lo.x + width, cy), IM_COL32(255, 255, 255, 18));
    return open;
}

float labelWidth() {
    const float w = ImGui::GetContentRegionAvail().x;
    return std::clamp(w * 0.36f, theme::px(84.0f), theme::px(150.0f));
}

void rowLabel(const char* label, bool changed, const char* help) {
    const float width = labelWidth();
    const ImVec2 lo = ImGui::GetCursorScreenPos();
    ImGui::AlignTextToFramePadding();
    // Right-aligned against the widget, cut short if it does not fit.
    const ImVec2 t = ImGui::CalcTextSize(label);
    const float gap = theme::px(10.0f);
    ImGui::SetCursorScreenPos(ImVec2(lo.x + std::max(0.0f, width - gap - t.x), ImGui::GetCursorScreenPos().y));
    ImGui::PushStyleColor(ImGuiCol_Text, changed ? theme::vec(theme::kText) : theme::vec(theme::kTextDim));
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    if (help && *help && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(theme::px(340.0f));
        ImGui::TextUnformatted(help);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::SetCursorScreenPos(ImVec2(lo.x + width, lo.y));
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
}

bool resetButton(const char* id, bool visible) {
    const float side = ImGui::GetFrameHeight();
    const float avail = ImGui::GetContentRegionAvail().x;
    const ImVec2 at = ImGui::GetCursorScreenPos();
    bool clicked = false;
    if (visible) {
        ImGui::SetCursorScreenPos(ImVec2(at.x + avail - side, at.y));
        clicked = theme::iconButton(id, theme::Icon::Reset, "Back to the default", false, true, side);
        ImGui::SetCursorScreenPos(at);
    }
    ImGui::SetNextItemWidth(avail - side - theme::px(4.0f));
    return clicked;
}

int keyButton(const char* id, int state, const char* tooltip) {
    const float side = ImGui::GetFrameHeight();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImGui::PushID(id);
    ImGui::InvisibleButton("##key", ImVec2(side * 0.8f, side), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool hovered = ImGui::IsItemHovered();
    int clicked = 0;
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) clicked = 1;
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) clicked = 2;
    if (hovered && tooltip && *tooltip) ImGui::SetTooltip("%s", tooltip);
    ImGui::PopID();
    ImDrawList* d = ImGui::GetWindowDrawList();
    const ImVec2 c(at.x + side * 0.4f, at.y + side * 0.5f);
    const float r = side * (state == 2 ? 0.24f : 0.2f);
    const ImVec2 q[4] = {ImVec2(c.x, c.y - r), ImVec2(c.x + r, c.y), ImVec2(c.x, c.y + r), ImVec2(c.x - r, c.y)};
    const ImU32 key = IM_COL32(255, 196, 70, 255);
    if (state == 2) {
        d->AddConvexPolyFilled(q, 4, key);
    } else if (state == 1) {
        d->AddPolyline(q, 4, key, ImDrawFlags_Closed, theme::px(1.5f));
    } else if (hovered) {
        d->AddPolyline(q, 4, IM_COL32(255, 255, 255, 120), ImDrawFlags_Closed, theme::px(1.2f));
    }
    ImGui::SetCursorScreenPos(at);
    return clicked;
}

bool exprButton(const char* id, bool active, const char* tooltip) {
    const float side = ImGui::GetFrameHeight();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImGui::PushID(id);
    const bool clicked = ImGui::InvisibleButton("##expr", ImVec2(side * 0.9f, side));
    const bool hovered = ImGui::IsItemHovered();
    if (hovered && tooltip && *tooltip) ImGui::SetTooltip("%s", tooltip);
    ImGui::PopID();
    const ImU32 color = active ? IM_COL32(150, 210, 255, 255) : hovered ? IM_COL32(255, 255, 255, 170) : IM_COL32(255, 255, 255, 55);
    const char* text = "fx";
    const ImVec2 size = ImGui::CalcTextSize(text);
    ImGui::GetWindowDrawList()->AddText(ImVec2(at.x + (side * 0.9f - size.x) * 0.5f, at.y + (side - size.y) * 0.5f), color, text);
    ImGui::SetCursorScreenPos(at);
    return clicked;
}

namespace {

/// A slider filled from its left end to the value, with no knob: `widget`
/// draws Dear ImGui's slider with its frame and grab left out, over a frame
/// and a fill painted behind it once `fraction` -- the value's place in the
/// range -- is known.
template <class Widget, class Fraction>
bool filledSlider(const Widget& widget, const Fraction& fraction) {
    ImDrawList* d = ImGui::GetWindowDrawList();
    // The frame's colours as they are now: a keyed parameter tints them.
    const ImU32 frame = ImGui::GetColorU32(ImGuiCol_FrameBg), frameHovered = ImGui::GetColorU32(ImGuiCol_FrameBgHovered),
                frameActive = ImGui::GetColorU32(ImGuiCol_FrameBgActive);
    ImDrawListSplitter layers;  // its own: one inside a table's stacks
    layers.Split(d, 2);
    layers.SetCurrentChannel(d, 1);
    const ImVec4 none(0.0f, 0.0f, 0.0f, 0.0f);
    for (const ImGuiCol c : {ImGuiCol_FrameBg, ImGuiCol_FrameBgHovered, ImGuiCol_FrameBgActive, ImGuiCol_SliderGrab,
                             ImGuiCol_SliderGrabActive}) {
        ImGui::PushStyleColor(c, none);
    }
    const bool changed = widget();
    ImGui::PopStyleColor(5);
    const ImVec2 lo = ImGui::GetItemRectMin(), hi = ImGui::GetItemRectMax();
    const bool active = ImGui::IsItemActive(), hovered = ImGui::IsItemHovered();
    layers.SetCurrentChannel(d, 0);
    const float r = ImGui::GetStyle().FrameRounding;
    d->AddRectFilled(lo, hi, active ? frameActive : hovered ? frameHovered : frame, r);
    const float t = std::clamp(fraction(), 0.0f, 1.0f);
    const float x = lo.x + (hi.x - lo.x) * t;
    if (x > lo.x + 1.0f) {
        d->AddRectFilled(lo, ImVec2(x, hi.y), theme::fade(theme::kAccent, active ? 0.5f : hovered ? 0.42f : 0.34f), r,
                         x >= hi.x - r ? ImDrawFlags_RoundCornersAll : ImDrawFlags_RoundCornersLeft);
    }
    layers.Merge(d);
    return changed;
}

}  // namespace

bool sliderFloat(const char* id, float& v, float min, float max, const char* format) {
    // Past the ends when typed (Ctrl + click): a slider's range is where it
    // is useful, not a limit.
    return filledSlider([&] { return ImGui::SliderFloat(id, &v, min, max, format, ImGuiSliderFlags_None); },
                        [&] { return max > min ? (v - min) / (max - min) : 0.0f; });
}

bool sliderInt(const char* id, int& v, int min, int max, const char* format) {
    return filledSlider([&] { return ImGui::SliderInt(id, &v, min, max, format); },
                        [&] { return max > min ? static_cast<float>(v - min) / static_cast<float>(max - min) : 0.0f; });
}

bool dragVector(const char* id, float v[3], float speed, const char* format) {
    ImGui::PushID(id);
    const float total = ImGui::CalcItemWidth();
    const float gap = theme::px(3.0f);
    const float w = (total - 2.0f * gap) / 3.0f;
    const ImU32 axis[3] = {IM_COL32(222, 80, 80, 255), IM_COL32(110, 200, 90, 255), IM_COL32(80, 130, 235, 255)};
    bool changed = false;
    for (int i = 0; i < 3; ++i) {
        if (i) ImGui::SameLine(0.0f, gap);
        ImGui::PushID(i);
        ImGui::SetNextItemWidth(w);
        changed |= ImGui::DragFloat("##c", &v[i], speed, 0.0f, 0.0f, format);
        const ImVec2 lo = ImGui::GetItemRectMin(), hi = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddRectFilled(lo, ImVec2(lo.x + theme::px(3.0f), hi.y), axis[i],
                                                  ImGui::GetStyle().FrameRounding, ImDrawFlags_RoundCornersLeft);
        ImGui::PopID();
    }
    ImGui::PopID();
    return changed;
}

bool colorEdit(const char* id, float v[3]) {
    return ImGui::ColorEdit3(id, v, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_DisplayRGB);
}

bool toggle(const char* id, bool& v) {
    const float h = ImGui::GetFrameHeight() * 0.8f, w = h * 1.9f;
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float pad = (ImGui::GetFrameHeight() - h) * 0.5f;
    ImGui::InvisibleButton(id, ImVec2(w, ImGui::GetFrameHeight()));
    const bool clicked = ImGui::IsItemClicked();
    if (clicked) v = !v;
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* d = ImGui::GetWindowDrawList();
    const ImVec2 lo(at.x, at.y + pad), hi(at.x + w, at.y + pad + h);
    d->AddRectFilled(lo, hi, v ? (hovered ? theme::kAccentHover : theme::kAccent) : (hovered ? theme::kFrameHover : theme::kFrame),
                     h * 0.5f);
    const float r = h * 0.5f - theme::px(2.5f);
    d->AddCircleFilled(ImVec2(v ? hi.x - h * 0.5f : lo.x + h * 0.5f, lo.y + h * 0.5f), r,
                       v ? IM_COL32(255, 255, 255, 255) : IM_COL32(170, 172, 180, 255));
    return clicked;
}

bool segmented(const char* id, int& v, const std::vector<const char*>& labels) {
    ImGui::PushID(id);
    const float total = ImGui::CalcItemWidth();
    const int n = static_cast<int>(labels.size());
    const float w = total / static_cast<float>(std::max(n, 1));
    const float h = ImGui::GetFrameHeight();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImDrawList* d = ImGui::GetWindowDrawList();
    const float r = ImGui::GetStyle().FrameRounding;
    d->AddRectFilled(at, ImVec2(at.x + total, at.y + h), theme::kFrame, r);
    bool changed = false;
    for (int i = 0; i < n; ++i) {
        ImGui::SetCursorScreenPos(ImVec2(at.x + w * static_cast<float>(i), at.y));
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##s", ImVec2(w, h)) && v != i) {
            v = i;
            changed = true;
        }
        const bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();
        const ImVec2 lo = ImGui::GetItemRectMin(), hi = ImGui::GetItemRectMax();
        if (v == i) d->AddRectFilled(ImVec2(lo.x + 2, lo.y + 2), ImVec2(hi.x - 2, hi.y - 2), IM_COL32(74, 77, 88, 255), r - 1.0f);
        else if (hovered) d->AddRectFilled(ImVec2(lo.x + 2, lo.y + 2), ImVec2(hi.x - 2, hi.y - 2), theme::kFrameHover, r - 1.0f);
        const ImVec2 t = ImGui::CalcTextSize(labels[static_cast<size_t>(i)]);
        d->PushClipRect(lo, hi, true);
        d->AddText(ImVec2(lo.x + (w - t.x) * 0.5f, lo.y + (h - t.y) * 0.5f), v == i ? theme::kText : theme::kTextDim,
                   labels[static_cast<size_t>(i)]);
        d->PopClipRect();
        if (i > 0 && v != i && v != i - 1) {
            d->AddLine(ImVec2(lo.x, lo.y + h * 0.25f), ImVec2(lo.x, lo.y + h * 0.75f), IM_COL32(255, 255, 255, 25));
        }
    }
    ImGui::SetCursorScreenPos(at);
    ImGui::Dummy(ImVec2(total, h));
    ImGui::PopID();
    return changed;
}

void note(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, theme::vec(theme::kTextDim));
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

bool beginRows(const char* id) {
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(theme::px(6.0f), theme::px(2.0f)));
    const bool open = ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoSavedSettings);
    ImGui::PopStyleVar();
    if (open) {
        ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
    }
    return open;
}

void rowStart(const char* label) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::PushStyleColor(ImGuiCol_Text, theme::vec(theme::kTextDim));
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    ImGui::TableSetColumnIndex(1);
}

void row(const char* label, const char* fmt, ...) {
    rowStart(label);
    va_list args;
    va_start(args, fmt);
    ImGui::TextV(fmt, args);
    va_end(args);
}

void endRows() { ImGui::EndTable(); }

void keysHelp(const std::vector<KeyHelp>& keys, const std::vector<const char*>& commands) {
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(theme::px(8.0f), theme::px(2.0f)));
    if (ImGui::BeginTable("##keys", 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoSavedSettings)) {
        for (const KeyHelp& k : keys) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            if (!k.keys || !*k.keys) {
                // A group's title, with room above it after the first.
                if (ImGui::TableGetRowIndex() > 0) ImGui::Dummy(ImVec2(0.0f, theme::px(6.0f)));
                ImGui::PushFont(theme::fonts().bold, 0.0f);
                ImGui::TextUnformatted(k.what);
                ImGui::PopFont();
                continue;
            }
            ImGui::TextUnformatted(k.keys);
            ImGui::TableSetColumnIndex(1);
            ImGui::PushStyleColor(ImGuiCol_Text, theme::vec(theme::kTextDim));
            ImGui::TextUnformatted(k.what);
            ImGui::PopStyleColor();
        }
        ImGui::EndTable();
    }
    ImGui::PopStyleVar();
    if (!commands.empty()) {
        ImGui::Separator();
        ImGui::TextDisabled("The same from the command line:");
        ImGui::PushFont(theme::fonts().mono, 0.0f);
        for (const char* c : commands) ImGui::TextUnformatted(c);
        ImGui::PopFont();
    }
}

// --- a list to pick from by typing -------------------------------------------------------

void PickList::begin(std::string& search, float width) {
    width_ = width;
    if (ImGui::IsWindowAppearing()) {
        search.clear();
        cursor_ = 0;
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::SetNextItemWidth(width);
    ImGui::InputTextWithHint("##search", "Search nodes\xe2\x80\xa6", &search);
    if (search != last_) {
        last_ = search;
        cursor_ = 0;
    }
    // The keys move through what was listed the frame before.
    moved_ = false;
    if (shown_ > 0 && ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
        cursor_ = (cursor_ + 1) % shown_;
        moved_ = true;
    }
    if (shown_ > 0 && ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
        cursor_ = (cursor_ + shown_ - 1) % shown_;
        moved_ = true;
    }
    enter_ = ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter);
    count_ = 0;
    ImGui::Dummy(ImVec2(0.0f, theme::px(2.0f)));
    // A list of a fixed height, which scrolls: the menu's size is known from
    // its first frame, so it opens where it was asked for -- moved only as
    // far as it must to be all in the window.
    const float height = std::min(theme::px(440.0f), std::max(theme::px(160.0f), ImGui::GetMainViewport()->WorkSize.y * 0.55f));
    ImGui::BeginChild("##list", ImVec2(width, height));
}

void PickList::heading(const char* text, ImU32 dot) {
    if (count_ > 0) ImGui::Dummy(ImVec2(0.0f, theme::px(3.0f)));
    const ImVec2 at = ImGui::GetCursorScreenPos();
    if (dot) {
        ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(at.x + theme::px(5.0f), at.y + ImGui::GetTextLineHeight() * 0.5f),
                                                    theme::px(3.5f), dot);
        ImGui::SetCursorScreenPos(ImVec2(at.x + theme::px(14.0f), at.y));
    }
    ImGui::TextDisabled("%s", text);
}

bool PickList::item(const char* id, const char* label, theme::Icon icon, ImU32 iconColor, const char* help) {
    const bool lit = count_++ == cursor_;
    ImGui::PushID(id);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    bool chosen = ImGui::Selectable("##item", lit, 0, ImVec2(width_ - theme::px(14.0f), 0.0f));
    if (lit && moved_) ImGui::SetScrollHereY(0.5f);
    if (help && *help && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(theme::px(320.0f));
        ImGui::TextUnformatted(help);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
    ImDrawList* d = ImGui::GetWindowDrawList();
    const float h = ImGui::GetTextLineHeight();
    theme::drawIcon(d, icon, ImVec2(p.x + theme::px(12.0f), p.y + h * 0.5f), h * 0.85f, iconColor);
    d->AddText(ImVec2(p.x + theme::px(26.0f), p.y), theme::kText, label);
    ImGui::PopID();
    return chosen || (lit && enter_);
}

void PickList::end() {
    if (count_ == 0) ImGui::TextDisabled("Nothing fits");
    ImGui::EndChild();
    shown_ = count_;
    cursor_ = shown_ > 0 ? std::clamp(cursor_, 0, shown_ - 1) : 0;
}

// --- the timeline ------------------------------------------------------------------------

TimelineActions timeline(const char* id, const TimelineState& s) {
    TimelineActions a;
    ImGui::PushID(id);
    const float h = ImGui::GetFrameHeight();
    const float totalHeight = h + theme::px(16.0f);
    const ImVec2 lo = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    ImDrawList* d = ImGui::GetWindowDrawList();
    d->AddRectFilled(lo, ImVec2(lo.x + width, lo.y + totalHeight), theme::kHeader);
    const float y = lo.y + (totalHeight - h) * 0.5f;

    // Transport buttons.
    ImGui::SetCursorScreenPos(ImVec2(lo.x + theme::px(6.0f), y));
    if (theme::iconButton("first", theme::Icon::First, "To the first frame (Home)")) a.toStart = true;
    ImGui::SameLine(0.0f, theme::px(2.0f));
    if (theme::iconButton("back", theme::Icon::StepBack, "A frame back (Left)")) a.back = true;
    ImGui::SameLine(0.0f, theme::px(2.0f));
    if (theme::iconButton("play", s.playing ? theme::Icon::Pause : theme::Icon::Play,
                          s.playing ? "Pause (Space)" : "Play (Space)", s.playing)) {
        a.togglePlay = true;
    }
    ImGui::SameLine(0.0f, theme::px(2.0f));
    if (theme::iconButton("forward", theme::Icon::StepForward, "A frame on (Right)")) a.forward = true;
    ImGui::SameLine(0.0f, theme::px(2.0f));
    if (theme::iconButton("last", theme::Icon::Last, "To the last frame ready (End)")) a.toEnd = true;
    ImGui::SameLine(0.0f, theme::px(2.0f));
    if (theme::iconButton("loop", theme::Icon::Loop, "Play in a loop", s.loop)) a.toggleLoop = true;
    ImGui::SameLine(0.0f, theme::px(10.0f));

    // The frame, and what is ready, at the right.
    char right[96];
    std::snprintf(right, sizeof right, "%d / %d   %.0f fps", s.current, s.frames, static_cast<double>(s.fps));
    const float rightWidth = ImGui::CalcTextSize("0000 / 0000   000 fps").x + theme::px(14.0f);

    // The track.
    const float x0 = ImGui::GetCursorScreenPos().x + theme::px(8.0f);
    const float x1 = lo.x + width - rightWidth - theme::px(8.0f);
    const float trackTop = lo.y + theme::px(5.0f), trackBottom = lo.y + totalHeight - theme::px(5.0f);
    const int frames = std::max(1, s.frames);
    auto xOf = [&](float frame) {
        return x0 + (x1 - x0) * (frames > 1 ? (frame - 1.0f) / static_cast<float>(frames - 1) : 0.5f);
    };
    ImGui::SetCursorScreenPos(ImVec2(x0 - theme::px(6.0f), trackTop));
    ImGui::InvisibleButton("track", ImVec2(std::max(1.0f, x1 - x0 + theme::px(12.0f)), trackBottom - trackTop));
    if (ImGui::IsItemActive() || ImGui::IsItemClicked()) {
        const float t = std::clamp((ImGui::GetIO().MousePos.x - x0) / std::max(1.0f, x1 - x0), 0.0f, 1.0f);
        a.scrubTo = 1 + static_cast<int>(std::lround(t * static_cast<float>(frames - 1)));
    }
    const bool trackHovered = ImGui::IsItemHovered();
    const float mid = (trackTop + trackBottom) * 0.5f + theme::px(4.0f);
    d->AddRectFilled(ImVec2(x0, mid - theme::px(3.0f)), ImVec2(x1, mid + theme::px(3.0f)), IM_COL32(24, 25, 28, 255),
                     theme::px(3.0f));
    // What is simulated: the cache.
    if (s.cached > 0) {
        const float cx = xOf(static_cast<float>(std::min(s.cached, frames)));
        d->AddRectFilled(ImVec2(x0, mid - theme::px(3.0f)), ImVec2(std::max(cx, x0 + 2.0f), mid + theme::px(3.0f)),
                         theme::kAccentDim, theme::px(3.0f));
    }
    if (s.simulating && s.cached < frames) {
        const float cx = xOf(static_cast<float>(s.cached + 1));
        const float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(ImGui::GetTime()) * 6.0f);
        d->AddCircleFilled(ImVec2(cx, mid), theme::px(3.0f), theme::fade(theme::kAccent, 0.4f + 0.6f * pulse));
    }
    // Ticks: as dense as the width allows.
    const float pxPerFrame = (x1 - x0) / static_cast<float>(std::max(1, frames - 1));
    int step = 1;
    for (int c : {1, 2, 5, 10, 20, 25, 50, 100, 200, 250, 500, 1000}) {
        step = c;
        if (static_cast<float>(c) * pxPerFrame >= theme::px(46.0f)) break;
    }
    for (int f = 1; f <= frames; ++f) {
        const bool major = f == 1 || f % step == 0;
        const bool minor = step >= 5 && f % (step / 5) == 0;
        if (!major && !minor) continue;
        const float x = xOf(static_cast<float>(f));
        d->AddLine(ImVec2(x, mid - theme::px(major ? 9.0f : 6.0f)), ImVec2(x, mid - theme::px(4.0f)),
                   major ? IM_COL32(255, 255, 255, 60) : IM_COL32(255, 255, 255, 25));
        if (major) {
            char n[16];
            std::snprintf(n, sizeof n, "%d", f);
            const float size = ImGui::GetFontSize() * 0.78f;
            d->AddText(nullptr, size, ImVec2(x + theme::px(3.0f), mid - theme::px(10.0f) - size * 0.9f), theme::kTextFaint, n);
        }
    }
    // The keys: diamonds on the track.
    auto diamond = [&](float frame, ImU32 col, float r) {
        const float x = xOf(std::clamp(frame, 1.0f, static_cast<float>(frames)));
        const ImVec2 c(x, mid);
        d->AddQuadFilled(ImVec2(c.x, c.y - r), ImVec2(c.x + r, c.y), ImVec2(c.x, c.y + r), ImVec2(c.x - r, c.y), col);
    };
    for (const float f : s.otherKeys) diamond(f, IM_COL32(200, 170, 90, 110), theme::px(3.5f));
    for (const float f : s.keys) {
        diamond(f, IM_COL32(20, 20, 22, 255), theme::px(6.0f));
        diamond(f, IM_COL32(255, 196, 70, 255), theme::px(4.5f));
    }
    // The play head.
    const float px = xOf(static_cast<float>(std::clamp(s.current, 1, frames)));
    d->AddLine(ImVec2(px, trackTop), ImVec2(px, trackBottom), theme::kAccent, theme::px(2.0f));
    d->AddTriangleFilled(ImVec2(px - theme::px(5.0f), trackTop), ImVec2(px + theme::px(5.0f), trackTop),
                         ImVec2(px, trackTop + theme::px(6.0f)), theme::kAccent);
    if (trackHovered) {
        const float t = std::clamp((ImGui::GetIO().MousePos.x - x0) / std::max(1.0f, x1 - x0), 0.0f, 1.0f);
        ImGui::SetTooltip("frame %d", 1 + static_cast<int>(std::lround(t * static_cast<float>(frames - 1))));
    }
    d->AddText(ImVec2(lo.x + width - rightWidth + theme::px(4.0f), lo.y + (totalHeight - ImGui::GetFontSize()) * 0.5f),
               theme::kTextDim, right);

    ImGui::SetCursorScreenPos(lo);
    ImGui::Dummy(ImVec2(width, totalHeight));
    ImGui::PopID();
    return a;
}

// --- files ---------------------------------------------------------------------------------

void FileBrowser::open(const std::string& title, std::vector<std::string> extensions, bool save,
                       const std::string& start, std::vector<std::pair<std::string, std::string>> places) {
    title_ = title;
    extensions_ = std::move(extensions);
    save_ = save;
    folders_ = false;
    places_ = std::move(places);
    error_.clear();
    std::error_code ec;
    fs::path p = start.empty() ? fs::current_path(ec) : typed(start);
    if (fs::is_directory(p, ec)) {
        dir_ = p;
        name_.clear();
    } else {
        dir_ = p.has_parent_path() ? p.parent_path() : fs::current_path(ec);
        name_ = p.filename().string();
    }
    if (!fs::is_directory(dir_, ec)) dir_ = fs::current_path(ec);
    dir_ = fs::weakly_canonical(dir_, ec);
    pathText_ = dir_.string();
    list();
    requested_ = true;
}

void FileBrowser::openFolder(const std::string& title, bool create, const std::string& start,
                             std::vector<std::pair<std::string, std::string>> places) {
    // Shown in the folder above, named: Choose takes it as it is.
    fs::path p = fs::path(start).lexically_normal();
    if (!p.has_filename() && p.has_parent_path()) p = p.parent_path();  // "cache/"
    open(title, {}, create, p.parent_path().string(), std::move(places));
    name_ = p.filename().string();
    folders_ = true;
}

bool FileBrowser::wanted(const fs::path& p) const {
    if (extensions_.empty()) return true;
    const std::string e = lower(p.extension().string());
    return std::find(extensions_.begin(), extensions_.end(), e) != extensions_.end();
}

void FileBrowser::list() {
    entries_.clear();
    selected_ = -1;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir_, fs::directory_options::skip_permission_denied, ec)) {
        const std::string name = e.path().filename().string();
        if (name.empty() || name[0] == '.') continue;
        std::error_code ec2;
        const bool folder = e.is_directory(ec2);
        if (!folder && !wanted(e.path())) continue;
        entries_.push_back({name, folder, folder ? 0 : e.file_size(ec2)});
    }
    std::sort(entries_.begin(), entries_.end(), [](const Entry& a, const Entry& b) {
        if (a.folder != b.folder) return a.folder;
        return lower(a.name) < lower(b.name);
    });
}

bool FileBrowser::draw(std::string& chosen) {
    if (requested_) {
        ImGui::OpenPopup(title_.c_str());
        requested_ = false;
        open_ = true;
    }
    if (!open_) return false;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowSize(ImVec2(std::min(theme::px(760.0f), vp->Size.x * 0.9f), std::min(theme::px(520.0f), vp->Size.y * 0.85f)),
                             ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f), ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));
    bool done = false;
    if (!ImGui::BeginPopupModal(title_.c_str(), &open_)) {
        open_ = false;
        return false;
    }
    auto go = [&](const fs::path& p) {
        std::error_code ec;
        if (!fs::is_directory(p, ec)) {
            error_ = p.string() + ": not a folder";
            return;
        }
        dir_ = fs::weakly_canonical(p, ec);
        pathText_ = dir_.string();
        error_.clear();
        if (folders_) name_.clear();  // the name was of a folder in the one left
        list();
    };
    auto accept = [&](const fs::path& p) {
        fs::path out = p;
        if (save_ && !extensions_.empty() && !wanted(out)) out += extensions_.front();
        chosen = out.string();
        done = true;
    };

    // The path, and up.
    if (theme::iconButton("up", theme::Icon::Up, "The folder above")) go(dir_.parent_path());
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputText("##path", &pathText_, ImGuiInputTextFlags_EnterReturnsTrue)) go(typed(pathText_));

    // Places on the left, the folder on the right.
    const float footer = ImGui::GetFrameHeightWithSpacing() * 2.0f + theme::px(8.0f);
    const float placesWidth = places_.empty() ? 0.0f : theme::px(150.0f);
    if (!places_.empty()) {
        ImGui::BeginChild("places", ImVec2(placesWidth, -footer), ImGuiChildFlags_Borders);
        for (const auto& [label, path] : places_) {
            if (ImGui::Selectable(label.c_str(), fs::path(path) == dir_)) go(fs::path(path));
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", path.c_str());
        }
        ImGui::EndChild();
        ImGui::SameLine();
    }
    ImGui::BeginChild("files", ImVec2(0.0f, -footer), ImGuiChildFlags_Borders);
    const float row = ImGui::GetTextLineHeight();
    for (size_t i = 0; i < entries_.size(); ++i) {
        const Entry& e = entries_[i];
        ImGui::PushID(static_cast<int>(i));
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const bool clicked = ImGui::Selectable("##e", selected_ == static_cast<int>(i),
                                               ImGuiSelectableFlags_AllowDoubleClick | ImGuiSelectableFlags_SpanAllColumns);
        ImDrawList* d = ImGui::GetWindowDrawList();
        theme::drawIcon(d, e.folder ? theme::Icon::Folder : theme::Icon::File,
                        ImVec2(at.x + row * 0.6f, at.y + row * 0.5f), row * 0.9f,
                        e.folder ? IM_COL32(214, 170, 90, 255) : theme::kTextDim);
        d->AddText(ImVec2(at.x + row * 1.6f, at.y), theme::kText, e.name.c_str());
        if (!e.folder) {
            const std::string size = sizeText(e.size);
            const float w = ImGui::CalcTextSize(size.c_str()).x;
            d->AddText(ImVec2(at.x + ImGui::GetContentRegionAvail().x - w, at.y), theme::kTextFaint, size.c_str());
        }
        if (clicked) {
            selected_ = static_cast<int>(i);
            // A file's name, or -- choosing a folder -- a folder's.
            if (e.folder == folders_) name_ = e.name;
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                if (e.folder) {
                    ImGui::PopID();
                    go(dir_ / e.name);
                    break;
                }
                accept(folders_ ? dir_ : dir_ / e.name);  // a file in it: its folder
            }
        }
        ImGui::PopID();
    }
    if (entries_.empty()) ImGui::TextDisabled(extensions_.empty() ? "empty" : "no folders, no files of this kind");
    ImGui::EndChild();

    // The name, and the buttons.
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(folders_ ? "Folder" : save_ ? "Save as" : "File");
    ImGui::SameLine();
    const float buttons = theme::px(200.0f);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - buttons);
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();  // a name can be typed at once
    const bool enter = ImGui::InputText("##name", &name_, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    const bool pressed = accentButton(folders_ ? "Choose" : save_ ? "Save" : "Open", ImVec2(theme::px(92.0f), 0.0f)) || enter;
    if (pressed && folders_) {
        // The folder named, or with no name the one open.
        const fs::path p = name_.empty() ? dir_ : typed(name_).is_absolute() ? typed(name_) : dir_ / name_;
        std::error_code ec;
        if (fs::is_directory(p, ec) || (save_ && !fs::exists(p, ec))) accept(p);
        else error_ = fs::exists(p, ec) ? p.string() + ": not a folder" : "no folder " + p.string();
    } else if (pressed && !name_.empty()) {
        const fs::path p = typed(name_).is_absolute() ? typed(name_) : dir_ / name_;
        std::error_code ec;
        if (fs::is_directory(p, ec)) {
            go(p);
            name_.clear();
        } else if (!save_ && !fs::exists(p, ec)) {
            error_ = "no file " + p.string();
        } else {
            accept(p);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(theme::px(92.0f), 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        open_ = false;
    }
    if (!error_.empty()) ImGui::TextColored(theme::vec(theme::kRed), "%s", error_.c_str());
    else if (folders_) ImGui::TextDisabled("%zu items -- with no name, Choose takes the folder open", entries_.size());
    else ImGui::TextDisabled("%zu items", entries_.size());
    if (done) open_ = false;
    if (!open_) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    return done;
}

}  // namespace pg::editor::ui
