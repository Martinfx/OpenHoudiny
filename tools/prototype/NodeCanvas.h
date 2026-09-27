#pragma once
//
// A node network on screen: nodes, pins and links, drawn with Dear ImGui's
// draw lists -- zoomed, panned, selected, wired. Both networks of the editor
// use it, the simulation's and the shaders', through an adapter that turns
// the model into CanvasNodes and CanvasLinks each frame and applies what the
// user does back to it (CanvasModel). The canvas keeps no copy of the network:
// only the view -- where it looks, how close, what is selected.
//
//   left drag on a node         move it (and the other selected nodes)
//   left drag on empty canvas   select in a box (Shift adds, Ctrl toggles)
//   left drag from a pin        link; let go on empty canvas: add a node, linked
//   left drag from a linked input   take the link off, or move it elsewhere
//   middle drag, Alt + left drag    pan;   wheel: zoom round the mouse
//   right click / Tab           add a node;  right click on a node: its menu
//   F: frame the selection (or all),  L: lay it out,  Delete / X: remove,
//   Ctrl+D: duplicate,  B: bypass,  R: the display flag,  Ctrl+A: select all
//   left click on the flag at a node's right end: the display flag
//
#include "Theme.h"

#include "imgui.h"

#include <functional>
#include <set>
#include <string>
#include <vector>

namespace pg::editor {

struct CanvasPin {
    std::string label;
    ImU32 color = IM_COL32_WHITE;
    bool many = false;   ///< takes any number of links
    int links = 0;       ///< how many it has
};

struct CanvasNode {
    int id = 0;
    std::string title;             ///< in the header, bold
    std::string subtitle;          ///< after it, dim: the node's type when the title is its name
    ImU32 color = IM_COL32(90, 90, 90, 255);  ///< the header
    theme::Icon icon = theme::Icon::Network;
    float x = 0.0f, y = 0.0f;      ///< world units, top left
    std::vector<CanvasPin> inputs, outputs;
    bool bypassed = false;
    bool displayable = false;      ///< has a display flag
    bool displayed = false;        ///< the flag is on: its geometry shows
    bool dimmed = false;           ///< takes no part in the result
    int problem = 0;               ///< 0 none, 1 a warning, 2 an error
    std::string problemText;
    std::string summary;           ///< a line in the body, dim; empty: none
};

struct CanvasLink {
    int from = 0, fromPin = 0;  ///< a node and one of its outputs
    int to = 0, toPin = 0;      ///< a node and one of its inputs
    ImU32 color = IM_COL32_WHITE;
};

struct PinRef {
    int node = 0;
    int pin = 0;
    bool output = false;
};

/// What the canvas asks of the network it shows.
struct CanvasModel {
    /// Could `from` (an output) feed `to` (an input)? If not, why.
    std::function<bool(const PinRef& from, const PinRef& to, std::string* why)> canConnect;
    std::function<void(const PinRef& from, const PinRef& to)> connect;
    std::function<void(const CanvasLink& link)> disconnect;
    std::function<void(int node, float x, float y)> move;
    std::function<void(const std::vector<int>& nodes)> remove;
    std::function<void(const std::vector<int>& nodes)> duplicate;
    std::function<void(const std::vector<int>& nodes)> toggleBypass;
    /// The display flag of `node` clicked (or R pressed on it).
    std::function<void(int node)> toggleDisplay;
    /// The add menu, drawn inside the canvas's popup: `at` in world units,
    /// `pending` the pin a link was dragged from, if any. True when it added
    /// a node -- the popup closes.
    std::function<bool(ImVec2 at, const PinRef* pending)> addMenu;
    /// The items of a node's context menu, drawn inside its popup.
    std::function<void(int node)> nodeMenu;
};

class NodeCanvas {
public:
    /// Draws the network into the rest of the current window.
    void draw(const char* id, const std::vector<CanvasNode>& nodes, const std::vector<CanvasLink>& links,
              const CanvasModel& model);

    const std::set<int>& selection() const { return selection_; }
    /// The node whose parameters show: the one clicked last, if selected.
    int current() const { return current_; }
    void select(int node, bool add = false);
    /// Adds the node to the selection, or takes it out if it was in.
    void toggle(int node);
    void clearSelection();
    /// Pans and zooms to the selection -- or everything -- on the next draw.
    void frame(bool selectionOnly = false);
    /// Pans to `node` on the next draw, keeping the zoom.
    void reveal(int node);
    /// Lays the nodes out on the next draw -- the selection, or all: in
    /// columns by how far down the flow they are, ordered to cross few links.
    void arrange(bool selectionOnly = false);

    float zoom() const { return zoom_; }

private:
    struct Layout;  // where a node's parts are, in world units
    Layout layoutOf(const CanvasNode& n) const;
    ImVec2 toScreen(ImVec2 world) const;
    ImVec2 toWorld(ImVec2 screen) const;
    ImVec2 pinPosition(const CanvasNode& n, int pin, bool output) const;  // world
    /// The display flag of a node that has one, on screen.
    bool flagRect(const CanvasNode& n, ImVec2& lo, ImVec2& hi) const;
    void drawGrid(ImDrawList* d, ImVec2 lo, ImVec2 hi) const;
    void drawNode(ImDrawList* d, const CanvasNode& n, bool selected, bool hovered, const PinRef* hot,
                  bool hotAccepts) const;
    void drawLink(ImDrawList* d, ImVec2 a, ImVec2 b, ImU32 color, float thickness) const;
    void layOut(const std::vector<CanvasNode>& nodes, const std::vector<CanvasLink>& links, const CanvasModel& model,
                bool selectionOnly);

    ImVec2 origin_{0.0f, 0.0f};  // screen position of the canvas's top left
    ImVec2 size_{0.0f, 0.0f};
    ImVec2 pan_{40.0f, 40.0f};   // screen offset of world (0, 0) from origin_
    float zoom_ = 1.0f;
    std::set<int> selection_;
    int current_ = 0;
    int frameRequest_ = 0;       // 0 none, 1 all, 2 the selection
    int arrangeRequest_ = 0;     // the same
    int revealNode_ = 0;
    bool firstDraw_ = true;

    enum class Drag { None, Nodes, Box, Link, Pan };
    Drag drag_ = Drag::None;
    ImVec2 dragStart_{0.0f, 0.0f};    // screen
    bool dragMoved_ = false;
    int pressedNode_ = 0;
    PinRef linkFrom_;                 // the pin a link is dragged from
    std::vector<CanvasLink> lifted_;  // the link taken off an input to be moved
    std::set<int> boxBase_;           // the selection a box select adds to
    ImVec2 menuAt_{0.0f, 0.0f};       // world: where the add menu puts the node
    bool menuPending_ = false;
    PinRef menuPin_;
    int menuNode_ = 0;
    std::string dropWhy_;             // why the pin under the mouse will not take the link
};

}  // namespace pg::editor
