#include "Theme.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <initializer_list>

namespace pg::editor::theme {
namespace {

Fonts gFonts;
float gDpi = 1.0f;
constexpr float kPi = 3.14159265358979f;

/// The first font file that exists, or empty.
std::string findFont(std::initializer_list<const char*> candidates) {
    for (const char* c : candidates) {
        std::error_code ec;
        if (std::filesystem::exists(c, ec)) return c;
    }
    return {};
}

}  // namespace

ImVec4 vec(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }

ImU32 fade(ImU32 c, float a) {
    const float alpha = static_cast<float>((c >> IM_COL32_A_SHIFT) & 0xFF) * std::clamp(a, 0.0f, 1.0f);
    return (c & ~IM_COL32_A_MASK) | (static_cast<ImU32>(alpha + 0.5f) << IM_COL32_A_SHIFT);
}

ImU32 shade(ImU32 c, float amount) {
    ImVec4 v = vec(c);
    for (float* ch : {&v.x, &v.y, &v.z}) {
        *ch = amount >= 0.0f ? *ch + (1.0f - *ch) * amount : *ch * (1.0f + amount);
    }
    return ImGui::ColorConvertFloat4ToU32(v);
}

const Fonts& fonts() { return gFonts; }

float px(float length) { return length * gDpi; }

void apply(float dpi) {
    gDpi = std::max(1.0f, dpi);
    ImGuiStyle& s = ImGui::GetStyle();
    s = ImGuiStyle();
    s.WindowPadding = ImVec2(10, 10);
    s.FramePadding = ImVec2(8, 5);
    s.CellPadding = ImVec2(6, 4);
    s.ItemSpacing = ImVec2(8, 6);
    s.ItemInnerSpacing = ImVec2(6, 4);
    s.IndentSpacing = 14.0f;
    s.ScrollbarSize = 12.0f;
    s.GrabMinSize = 10.0f;
    s.WindowBorderSize = 0.0f;
    s.ChildBorderSize = 0.0f;
    s.PopupBorderSize = 1.0f;
    s.FrameBorderSize = 0.0f;
    s.TabBorderSize = 0.0f;
    s.WindowRounding = 0.0f;
    s.ChildRounding = 0.0f;
    s.FrameRounding = 4.0f;
    s.PopupRounding = 6.0f;
    s.ScrollbarRounding = 6.0f;
    s.GrabRounding = 4.0f;
    s.TabRounding = 4.0f;
    s.WindowTitleAlign = ImVec2(0.5f, 0.5f);
    s.SeparatorTextBorderSize = 1.0f;
    s.ScaleAllSizes(gDpi);

    ImVec4* c = s.Colors;
    const ImVec4 text = vec(kText), dim = vec(kTextDim), accent = vec(kAccent);
    auto a = [](ImU32 col, float alpha) {
        ImVec4 v = vec(col);
        v.w = alpha;
        return v;
    };
    c[ImGuiCol_Text] = text;
    c[ImGuiCol_TextDisabled] = dim;
    c[ImGuiCol_WindowBg] = vec(kPanel);
    c[ImGuiCol_ChildBg] = a(kPanel, 0.0f);
    c[ImGuiCol_PopupBg] = a(IM_COL32(36, 37, 42, 255), 0.98f);
    c[ImGuiCol_Border] = vec(kBorder);
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = vec(kFrame);
    c[ImGuiCol_FrameBgHovered] = vec(kFrameHover);
    c[ImGuiCol_FrameBgActive] = vec(IM_COL32(62, 64, 72, 255));
    c[ImGuiCol_TitleBg] = vec(kHeader);
    c[ImGuiCol_TitleBgActive] = vec(kHeader);
    c[ImGuiCol_TitleBgCollapsed] = vec(kHeader);
    c[ImGuiCol_MenuBarBg] = vec(IM_COL32(27, 28, 32, 255));
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = vec(IM_COL32(62, 64, 72, 255));
    c[ImGuiCol_ScrollbarGrabHovered] = vec(IM_COL32(78, 80, 90, 255));
    c[ImGuiCol_ScrollbarGrabActive] = vec(IM_COL32(92, 95, 106, 255));
    c[ImGuiCol_CheckMark] = accent;
    c[ImGuiCol_SliderGrab] = accent;
    c[ImGuiCol_SliderGrabActive] = vec(kAccentHover);
    c[ImGuiCol_Button] = vec(IM_COL32(48, 50, 56, 255));
    c[ImGuiCol_ButtonHovered] = vec(IM_COL32(60, 62, 70, 255));
    c[ImGuiCol_ButtonActive] = vec(IM_COL32(72, 74, 84, 255));
    c[ImGuiCol_Header] = a(kAccent, 0.22f);
    c[ImGuiCol_HeaderHovered] = a(kAccent, 0.32f);
    c[ImGuiCol_HeaderActive] = a(kAccent, 0.42f);
    c[ImGuiCol_Separator] = vec(kBorder);
    c[ImGuiCol_SeparatorHovered] = a(kAccent, 0.6f);
    c[ImGuiCol_SeparatorActive] = accent;
    c[ImGuiCol_ResizeGrip] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ResizeGripHovered] = a(kAccent, 0.5f);
    c[ImGuiCol_ResizeGripActive] = accent;
    c[ImGuiCol_InputTextCursor] = text;
    c[ImGuiCol_Tab] = vec(IM_COL32(36, 37, 42, 255));
    c[ImGuiCol_TabHovered] = vec(IM_COL32(58, 60, 68, 255));
    c[ImGuiCol_TabSelected] = vec(IM_COL32(52, 54, 61, 255));
    c[ImGuiCol_TabSelectedOverline] = accent;
    c[ImGuiCol_TabDimmed] = vec(IM_COL32(36, 37, 42, 255));
    c[ImGuiCol_TabDimmedSelected] = vec(IM_COL32(46, 48, 54, 255));
    c[ImGuiCol_TabDimmedSelectedOverline] = a(kAccent, 0.5f);
    c[ImGuiCol_PlotLines] = dim;
    c[ImGuiCol_PlotLinesHovered] = accent;
    c[ImGuiCol_PlotHistogram] = accent;
    c[ImGuiCol_PlotHistogramHovered] = vec(kAccentHover);
    c[ImGuiCol_TableHeaderBg] = vec(kHeader);
    c[ImGuiCol_TableBorderStrong] = vec(kBorder);
    c[ImGuiCol_TableBorderLight] = a(kBorder, 0.6f);
    c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1, 1, 1, 0.02f);
    c[ImGuiCol_TextLink] = vec(kBlue);
    c[ImGuiCol_TextSelectedBg] = a(kAccent, 0.35f);
    c[ImGuiCol_DragDropTarget] = accent;
    c[ImGuiCol_NavCursor] = accent;
    c[ImGuiCol_NavWindowingHighlight] = a(IM_COL32_WHITE, 0.7f);
    c[ImGuiCol_NavWindowingDimBg] = ImVec4(0, 0, 0, 0.3f);
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.45f);

    // System fonts when there are any; Dear ImGui's own otherwise.
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    const std::string sans = findFont({"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                                       "/usr/share/fonts/TTF/DejaVuSans.ttf",
                                       "/usr/local/share/fonts/dejavu/DejaVuSans.ttf",  // FreeBSD
                                       "/System/Library/Fonts/Supplemental/Arial.ttf",
                                       "C:/Windows/Fonts/segoeui.ttf"});
    const std::string bold = findFont({"/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
                                       "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
                                       "/usr/local/share/fonts/dejavu/DejaVuSans-Bold.ttf",
                                       "/System/Library/Fonts/Supplemental/Arial Bold.ttf",
                                       "C:/Windows/Fonts/segoeuib.ttf"});
    const std::string mono = findFont({"/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
                                       "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
                                       "/usr/local/share/fonts/dejavu/DejaVuSansMono.ttf",
                                       "/System/Library/Fonts/Menlo.ttc",
                                       "C:/Windows/Fonts/consola.ttf"});
    const float size = 15.0f;
    gFonts.regular = !sans.empty() ? io.Fonts->AddFontFromFileTTF(sans.c_str(), size) : nullptr;
    if (!gFonts.regular) gFonts.regular = io.Fonts->AddFontDefault();
    gFonts.bold = !bold.empty() ? io.Fonts->AddFontFromFileTTF(bold.c_str(), size) : nullptr;
    if (!gFonts.bold) gFonts.bold = gFonts.regular;
    gFonts.mono = !mono.empty() ? io.Fonts->AddFontFromFileTTF(mono.c_str(), size - 1.0f) : nullptr;
    if (!gFonts.mono) gFonts.mono = gFonts.regular;
    io.FontDefault = gFonts.regular;
    s.FontSizeBase = size;
    s.FontScaleDpi = gDpi;
}

// --- icons -----------------------------------------------------------------------------

void drawIcon(ImDrawList* d, Icon icon, ImVec2 c, float size, ImU32 col) {
    const float s = size * 0.5f;  // half the square
    const float t = std::max(1.0f, size / 11.0f);  // stroke
    auto P = [&](float x, float y) { return ImVec2(c.x + x * s, c.y + y * s); };  // -1..1 coordinates
    auto line = [&](float x0, float y0, float x1, float y1, float w = 0.0f) {
        d->AddLine(P(x0, y0), P(x1, y1), col, w > 0.0f ? w : t);
    };
    auto tri = [&](float x0, float y0, float x1, float y1, float x2, float y2) {
        d->AddTriangleFilled(P(x0, y0), P(x1, y1), P(x2, y2), col);
    };
    auto arc = [&](float cx, float cy, float r, float a0, float a1, float w = 0.0f) {
        d->PathArcTo(P(cx, cy), r * s, a0, a1, 24);
        d->PathStroke(col, 0, w > 0.0f ? w : t);
    };
    switch (icon) {
        case Icon::Play: tri(-0.55f, -0.7f, -0.55f, 0.7f, 0.7f, 0.0f); break;
        case Icon::Pause:
            d->AddRectFilled(P(-0.6f, -0.65f), P(-0.15f, 0.65f), col, t);
            d->AddRectFilled(P(0.15f, -0.65f), P(0.6f, 0.65f), col, t);
            break;
        case Icon::Stop: d->AddRectFilled(P(-0.55f, -0.55f), P(0.55f, 0.55f), col, t); break;
        case Icon::First:
            d->AddRectFilled(P(-0.7f, -0.6f), P(-0.45f, 0.6f), col);
            tri(0.6f, -0.6f, 0.6f, 0.6f, -0.35f, 0.0f);
            break;
        case Icon::Last:
            d->AddRectFilled(P(0.45f, -0.6f), P(0.7f, 0.6f), col);
            tri(-0.6f, -0.6f, -0.6f, 0.6f, 0.35f, 0.0f);
            break;
        case Icon::StepBack:
            tri(0.1f, -0.55f, 0.1f, 0.55f, -0.6f, 0.0f);
            d->AddRectFilled(P(0.3f, -0.55f), P(0.52f, 0.55f), col);
            break;
        case Icon::StepForward:
            tri(-0.1f, -0.55f, -0.1f, 0.55f, 0.6f, 0.0f);
            d->AddRectFilled(P(-0.52f, -0.55f), P(-0.3f, 0.55f), col);
            break;
        case Icon::Loop:
            arc(0.0f, 0.0f, 0.6f, -kPi * 0.35f, kPi * 1.35f);
            tri(0.55f, -0.75f, 0.85f, -0.3f, 0.25f, -0.25f);
            break;
        case Icon::Eye:
            d->PathArcTo(P(0.0f, 0.9f), 1.25f * s, -kPi * 0.78f, -kPi * 0.22f, 16);
            d->PathArcTo(P(0.0f, -0.9f), 1.25f * s, kPi * 0.22f, kPi * 0.78f, 16);
            d->PathStroke(col, ImDrawFlags_Closed, t);
            d->AddCircleFilled(c, 0.28f * s, col);
            break;
        case Icon::Grid:
            for (float v : {-0.6f, 0.0f, 0.6f}) {
                line(v, -0.7f, v, 0.7f);
                line(-0.7f, v, 0.7f, v);
            }
            break;
        case Icon::Floor:
            d->AddQuad(P(-0.35f, -0.35f), P(0.85f, -0.35f), P(0.35f, 0.45f), P(-0.85f, 0.45f), col, t);
            line(0.25f, -0.35f, -0.25f, 0.45f);
            break;
        case Icon::Camera:
            d->AddRect(P(-0.8f, -0.4f), P(0.35f, 0.5f), col, t, 0, t);
            tri(0.35f, 0.05f, 0.85f, -0.35f, 0.85f, 0.45f);
            break;
        case Icon::Guides: {
            // A cube in wire.
            const ImVec2 f0 = P(-0.75f, -0.35f), f1 = P(0.35f, -0.35f), f2 = P(0.35f, 0.75f), f3 = P(-0.75f, 0.75f);
            const ImVec2 o(0.4f * s, -0.4f * s);
            d->AddQuad(f0, f1, f2, f3, col, t);
            d->AddLine(f0, ImVec2(f0.x + o.x, f0.y + o.y), col, t);
            d->AddLine(f1, ImVec2(f1.x + o.x, f1.y + o.y), col, t);
            d->AddLine(f2, ImVec2(f2.x + o.x, f2.y + o.y), col, t);
            d->AddLine(ImVec2(f0.x + o.x, f0.y + o.y), ImVec2(f1.x + o.x, f1.y + o.y), col, t);
            d->AddLine(ImVec2(f1.x + o.x, f1.y + o.y), ImVec2(f2.x + o.x, f2.y + o.y), col, t);
            break;
        }
        case Icon::Warning:
            d->AddTriangleFilled(P(0.0f, -0.85f), P(0.9f, 0.75f), P(-0.9f, 0.75f), col);
            d->AddLine(P(0.0f, -0.3f), P(0.0f, 0.25f), IM_COL32(20, 20, 20, 255), t * 1.3f);
            d->AddCircleFilled(P(0.0f, 0.5f), t * 0.8f, IM_COL32(20, 20, 20, 255));
            break;
        case Icon::Error:
            d->AddCircleFilled(c, 0.85f * s, col);
            d->AddLine(P(0.0f, -0.5f), P(0.0f, 0.15f), IM_COL32(255, 255, 255, 255), t * 1.3f);
            d->AddCircleFilled(P(0.0f, 0.45f), t * 0.8f, IM_COL32(255, 255, 255, 255));
            break;
        case Icon::Info:
            d->AddCircle(c, 0.8f * s, col, 0, t);
            line(0.0f, -0.1f, 0.0f, 0.45f);
            d->AddCircleFilled(P(0.0f, -0.38f), t * 0.8f, col);
            break;
        case Icon::Plus:
            line(-0.65f, 0.0f, 0.65f, 0.0f, t * 1.2f);
            line(0.0f, -0.65f, 0.0f, 0.65f, t * 1.2f);
            break;
        case Icon::Folder:
            d->AddRectFilled(P(-0.85f, -0.55f), P(-0.1f, -0.3f), col, t * 0.6f);
            d->AddRectFilled(P(-0.85f, -0.4f), P(0.85f, 0.65f), col, t * 0.6f);
            break;
        case Icon::File:
            d->PathLineTo(P(-0.6f, -0.8f));
            d->PathLineTo(P(0.25f, -0.8f));
            d->PathLineTo(P(0.6f, -0.45f));
            d->PathLineTo(P(0.6f, 0.8f));
            d->PathLineTo(P(-0.6f, 0.8f));
            d->PathStroke(col, ImDrawFlags_Closed, t);
            line(-0.3f, 0.05f, 0.3f, 0.05f);
            line(-0.3f, 0.4f, 0.3f, 0.4f);
            break;
        case Icon::Up:
            line(0.0f, 0.7f, 0.0f, -0.55f, t * 1.2f);
            tri(0.0f, -0.8f, 0.5f, -0.2f, -0.5f, -0.2f);
            break;
        case Icon::Undo:
        case Icon::Redo: {
            const float m = icon == Icon::Undo ? 1.0f : -1.0f;
            d->PathArcTo(P(0.1f * m, 0.15f), 0.55f * s, icon == Icon::Undo ? -kPi * 0.5f : -kPi * 0.5f,
                         icon == Icon::Undo ? kPi * 0.5f : -kPi * 1.5f, 16);
            d->PathLineTo(P(-0.2f * m, 0.7f));
            d->PathStroke(col, 0, t);
            tri(-0.65f * m, -0.4f, -0.1f * m, -0.75f, -0.1f * m, -0.05f);
            break;
        }
        case Icon::Reset:
            arc(0.0f, 0.0f, 0.55f, -kPi * 0.9f, kPi * 0.6f);
            tri(-0.95f, -0.45f, -0.25f, -0.45f, -0.6f, 0.05f);
            break;
        case Icon::Search:
            d->AddCircle(P(-0.15f, -0.15f), 0.5f * s, col, 0, t);
            line(0.22f, 0.22f, 0.75f, 0.75f, t * 1.4f);
            break;
        case Icon::Close:
            line(-0.55f, -0.55f, 0.55f, 0.55f, t * 1.2f);
            line(-0.55f, 0.55f, 0.55f, -0.55f, t * 1.2f);
            break;
        case Icon::Chevron:
            d->PathLineTo(P(-0.25f, -0.55f));
            d->PathLineTo(P(0.3f, 0.0f));
            d->PathLineTo(P(-0.25f, 0.55f));
            d->PathStroke(col, 0, t * 1.2f);
            break;
        case Icon::Link:
            d->AddRect(P(-0.85f, -0.3f), P(0.1f, 0.3f), col, 0.3f * s, 0, t);
            d->AddRect(P(-0.1f, -0.3f), P(0.85f, 0.3f), col, 0.3f * s, 0, t);
            break;
        case Icon::Bypass:
            d->AddCircle(c, 0.7f * s, col, 0, t);
            line(-0.5f, 0.5f, 0.5f, -0.5f);
            break;
        case Icon::Flag:
            line(-0.55f, -0.8f, -0.55f, 0.85f);
            d->AddTriangleFilled(P(-0.5f, -0.8f), P(0.7f, -0.45f), P(-0.5f, -0.1f), col);
            break;
        case Icon::Source:  // a flame
            d->PathArcTo(P(0.0f, 0.25f), 0.55f * s, -kPi * 0.05f, kPi * 1.05f, 16);
            d->PathLineTo(P(-0.25f, -0.35f));
            d->PathLineTo(P(0.05f, -0.9f));
            d->PathLineTo(P(0.35f, -0.4f));
            d->PathFillConvex(col);
            d->AddCircleFilled(P(0.0f, 0.3f), 0.25f * s, shade(col, -0.45f));
            break;
        case Icon::Force:  // a swirl
            arc(0.0f, 0.0f, 0.75f, -kPi * 0.2f, kPi * 1.2f);
            arc(0.0f, 0.0f, 0.4f, kPi * 0.6f, kPi * 2.0f);
            tri(0.75f, -0.55f, 0.95f, 0.05f, 0.35f, -0.1f);
            break;
        case Icon::Collider:  // a solid ball
            d->AddCircleFilled(c, 0.75f * s, col);
            d->AddCircleFilled(P(-0.25f, -0.25f), 0.25f * s, shade(col, 0.45f));
            break;
        case Icon::Solver: {  // a gear
            const int teeth = 8;
            for (int i = 0; i < teeth; ++i) {
                const float a = 2.0f * kPi * static_cast<float>(i) / teeth;
                d->AddLine(P(0.45f * std::cos(a), 0.45f * std::sin(a)), P(0.85f * std::cos(a), 0.85f * std::sin(a)),
                           col, t * 2.2f);
            }
            d->AddCircleFilled(c, 0.58f * s, col);
            d->AddCircleFilled(c, 0.25f * s, IM_COL32(24, 24, 28, 255));
            break;
        }
        case Icon::Look: {  // a sun
            d->AddCircleFilled(c, 0.38f * s, col);
            for (int i = 0; i < 8; ++i) {
                const float a = 2.0f * kPi * static_cast<float>(i) / 8.0f;
                d->AddLine(P(0.58f * std::cos(a), 0.58f * std::sin(a)), P(0.88f * std::cos(a), 0.88f * std::sin(a)),
                           col, t);
            }
            break;
        }
        case Icon::Output:  // a screen
            d->AddRect(P(-0.85f, -0.7f), P(0.85f, 0.4f), col, t, 0, t);
            line(0.0f, 0.4f, 0.0f, 0.7f);
            line(-0.45f, 0.75f, 0.45f, 0.75f);
            break;
        case Icon::Shader:
            d->AddCircleFilled(c, 0.8f * s, shade(col, -0.35f));
            d->AddCircleFilled(P(-0.15f, -0.15f), 0.58f * s, col);
            d->AddCircleFilled(P(-0.3f, -0.3f), 0.22f * s, shade(col, 0.5f));
            break;
        case Icon::Input:
            d->AddRect(P(-0.2f, -0.75f), P(0.8f, 0.75f), col, t, 0, t);
            line(-0.85f, 0.0f, 0.35f, 0.0f);
            tri(0.45f, 0.0f, 0.1f, -0.3f, 0.1f, 0.3f);
            break;
        case Icon::Math:
            line(-0.75f, -0.35f, -0.05f, -0.35f);
            line(-0.4f, -0.7f, -0.4f, 0.0f);
            line(0.15f, 0.45f, 0.8f, 0.45f);
            line(0.2f, -0.7f, 0.75f, -0.15f);
            line(0.2f, -0.15f, 0.75f, -0.7f);
            break;
        case Icon::Texture:
            d->AddRectFilled(P(-0.75f, -0.75f), P(0.0f, 0.0f), col);
            d->AddRectFilled(P(0.0f, 0.0f), P(0.75f, 0.75f), col);
            d->AddRect(P(-0.75f, -0.75f), P(0.75f, 0.75f), col, 0, 0, t);
            break;
        case Icon::Pattern:
            for (float y : {-0.35f, 0.35f}) {
                d->PathClear();
                for (int i = 0; i <= 12; ++i) {
                    const float x = -0.85f + 1.7f * static_cast<float>(i) / 12.0f;
                    d->PathLineTo(P(x, y + 0.25f * std::sin(x * 4.0f)));
                }
                d->PathStroke(col, 0, t);
            }
            break;
        case Icon::Network:
            d->AddRectFilled(P(-0.85f, -0.75f), P(-0.25f, -0.35f), col, t * 0.5f);
            d->AddRectFilled(P(-0.85f, 0.35f), P(-0.25f, 0.75f), col, t * 0.5f);
            d->AddRectFilled(P(0.25f, -0.2f), P(0.85f, 0.2f), col, t * 0.5f);
            d->AddBezierCubic(P(-0.25f, -0.55f), P(0.05f, -0.55f), P(-0.05f, 0.0f), P(0.25f, 0.0f), col, t);
            d->AddBezierCubic(P(-0.25f, 0.55f), P(0.05f, 0.55f), P(-0.05f, 0.0f), P(0.25f, 0.0f), col, t);
            break;
        case Icon::Viewport:
            d->AddRect(P(-0.85f, -0.7f), P(0.85f, 0.7f), col, t, 0, t);
            d->AddCircleFilled(P(0.0f, 0.1f), 0.32f * s, col);
            line(-0.85f, 0.4f, 0.85f, 0.4f);
            break;
        case Icon::Parameters:
            for (int i = 0; i < 3; ++i) {
                const float y = -0.55f + 0.55f * static_cast<float>(i);
                line(-0.8f, y, 0.8f, y);
                d->AddCircleFilled(P(i == 1 ? 0.35f : -0.3f + 0.2f * static_cast<float>(i), y), 0.2f * s, col);
            }
            break;
        case Icon::Timeline:
            line(-0.85f, 0.3f, 0.85f, 0.3f);
            for (int i = 0; i <= 4; ++i) {
                const float x = -0.85f + 0.425f * static_cast<float>(i);
                line(x, 0.1f, x, 0.3f);
            }
            tri(0.1f, -0.75f, 0.45f, -0.75f, 0.275f, -0.3f);
            line(0.275f, -0.35f, 0.275f, 0.55f);
            break;
        case Icon::Code:
            d->PathLineTo(P(-0.3f, -0.55f));
            d->PathLineTo(P(-0.85f, 0.0f));
            d->PathLineTo(P(-0.3f, 0.55f));
            d->PathStroke(col, 0, t);
            d->PathLineTo(P(0.3f, -0.55f));
            d->PathLineTo(P(0.85f, 0.0f));
            d->PathLineTo(P(0.3f, 0.55f));
            d->PathStroke(col, 0, t);
            line(0.15f, -0.75f, -0.15f, 0.75f);
            break;
    }
}

bool iconButton(const char* id, Icon icon, const char* tooltip, bool on, bool enabled, float size) {
    const float side = size > 0.0f ? size : ImGui::GetFrameHeight();
    ImGui::PushID(id);
    if (!enabled) ImGui::BeginDisabled();
    const bool clicked = ImGui::InvisibleButton("##b", ImVec2(side, side));
    const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
    const bool held = ImGui::IsItemActive();
    const ImVec2 lo = ImGui::GetItemRectMin(), hi = ImGui::GetItemRectMax();
    ImDrawList* d = ImGui::GetWindowDrawList();
    if (on) d->AddRectFilled(lo, hi, fade(kAccent, held ? 0.45f : hovered ? 0.35f : 0.25f), px(4.0f));
    else if (hovered && enabled) d->AddRectFilled(lo, hi, held ? kFrameHover : kFrame, px(4.0f));
    const ImU32 col = !enabled ? kTextFaint : on ? kAccentHover : hovered ? kText : kTextDim;
    drawIcon(d, icon, ImVec2((lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f), side * 0.56f, col);
    if (!enabled) ImGui::EndDisabled();
    if (tooltip && *tooltip && hovered) ImGui::SetTooltip("%s", tooltip);
    ImGui::PopID();
    return clicked && enabled;
}

}  // namespace pg::editor::theme
