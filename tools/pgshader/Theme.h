#pragma once
//
// The look of the editor: one palette, the fonts, and icons drawn as vectors
// -- no icon font to ship, and they stay sharp at any size and any zoom.
//
// Everything the editor draws takes its colours from here, so the node
// canvas, the panels and the viewport overlays agree.
//
#include "imgui.h"

#include <string>

namespace pg::editor::theme {

// --- the palette ---------------------------------------------------------------------

inline constexpr ImU32 kCanvas = IM_COL32(23, 24, 27, 255);     ///< node canvas, viewport
inline constexpr ImU32 kPanel = IM_COL32(31, 32, 36, 255);      ///< panels
inline constexpr ImU32 kHeader = IM_COL32(38, 39, 44, 255);     ///< panel headers, the timeline
inline constexpr ImU32 kFrame = IM_COL32(44, 45, 51, 255);      ///< inputs
inline constexpr ImU32 kFrameHover = IM_COL32(54, 56, 63, 255);
inline constexpr ImU32 kBorder = IM_COL32(56, 58, 65, 255);
inline constexpr ImU32 kText = IM_COL32(222, 223, 226, 255);
inline constexpr ImU32 kTextDim = IM_COL32(140, 143, 152, 255);
inline constexpr ImU32 kTextFaint = IM_COL32(96, 99, 108, 255);
inline constexpr ImU32 kAccent = IM_COL32(240, 142, 60, 255);   ///< selection, the play head
inline constexpr ImU32 kAccentHover = IM_COL32(255, 166, 92, 255);
inline constexpr ImU32 kAccentDim = IM_COL32(150, 88, 38, 255);
inline constexpr ImU32 kBlue = IM_COL32(86, 156, 255, 255);
inline constexpr ImU32 kGreen = IM_COL32(94, 194, 122, 255);
inline constexpr ImU32 kYellow = IM_COL32(232, 197, 71, 255);
inline constexpr ImU32 kRed = IM_COL32(229, 83, 75, 255);

ImVec4 vec(ImU32 c);
/// `c` with alpha scaled by `a`.
ImU32 fade(ImU32 c, float a);
/// `c` brightened (amount > 0) or darkened (< 0), -1 to 1.
ImU32 shade(ImU32 c, float amount);

// --- fonts ---------------------------------------------------------------------------

struct Fonts {
    ImFont* regular = nullptr;
    ImFont* bold = nullptr;
    ImFont* mono = nullptr;
};
const Fonts& fonts();

/// The style, the colours and the fonts, for a display `dpi` times the
/// usual density. Before the first frame.
void apply(float dpi);
/// Pixels of a length given at the usual density.
float px(float length);

// --- icons ---------------------------------------------------------------------------

enum class Icon {
    Play, Pause, Stop, First, Last, StepBack, StepForward, Loop,
    Eye, Grid, Floor, Camera, Guides,
    Warning, Error, Info,
    Plus, Folder, File, Up, Undo, Redo, Reset, Search, Close, Chevron, Link,
    Bypass, Flag,
    Source, Force, Collider, Solver, Look, Output,  // simulation nodes
    Shader, Input, Math, Texture, Pattern,           // shader nodes
    Network, Viewport, Parameters, Timeline, Code,   // panels
    Select, Move, Rotate, Scale, World, Local, Magnet, Frame,  // viewport tools
    Sphere, Box, Cylinder, Cone, Torus,              // shapes
    Wind, Vortex, Attractor, Drop, Rain, Trash, Copy,
};

/// `icon` in a square `size` wide, centred on `center`.
void drawIcon(ImDrawList* draw, Icon icon, ImVec2 center, float size, ImU32 color);

/// A square button with an icon; `on` draws it pressed. True when clicked.
bool iconButton(const char* id, Icon icon, const char* tooltip, bool on = false, bool enabled = true,
                float size = 0.0f);

}  // namespace pg::editor::theme
