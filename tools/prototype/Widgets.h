#pragma once
//
// The pieces the editor's panels are made of, in the editor's look (Theme.h):
// panel headers, splitters, sections, parameter rows and their widgets, the
// timeline, a file browser.
//
#include "Theme.h"

#include "imgui.h"

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace pg::editor::ui {

// --- panels ----------------------------------------------------------------------------

/// A panel's header strip across the current window: an icon, a title and,
/// dim, some information. Buttons go into it from the right (headerButton).
struct PanelHeader {
    ImVec2 min, max;
    float right = 0.0f;  ///< where the next button from the right ends
};
PanelHeader panelHeader(theme::Icon icon, const char* title, const char* info = nullptr);
/// A panel's header whose title is tabs: the one shown, `current`, bright
/// over an accent line; a click on another shows it. The information follows
/// the tabs.
PanelHeader tabHeader(theme::Icon icon, int& current, const std::vector<const char*>& tabs, const char* info = nullptr);
bool headerButton(PanelHeader& header, const char* id, theme::Icon icon, const char* tooltip, bool on = false,
                  bool enabled = true);

/// Text over the viewport or a picture, on a dark pill so that it reads over
/// a white sky as over a black floor. `at` is the top left of the text.
/// Returns the text's size.
ImVec2 overlayText(ImDrawList* d, ImVec2 at, ImU32 color, const char* text, ImFont* font = nullptr, float size = 0.0f);

/// A menu item with a drawn icon in the menu's icon column: the labels of
/// all its items -- with an icon or without -- start at one place.
bool iconMenuItem(theme::Icon icon, ImU32 color, const char* label, const char* shortcut = nullptr, bool selected = false,
                  bool enabled = true);
/// A submenu with an icon, the same way.
bool beginIconMenu(theme::Icon icon, ImU32 color, const char* label, bool enabled = true);

/// The buttons at the foot of a dialog, at its right, a fixed width each:
/// the first does what the dialog is for, in the accent colour; the rest
/// (Cancel) plain. The index of the one clicked, -1 for none.
int dialogButtons(const std::vector<const char*>& labels, bool firstEnabled = true);

/// Escape pressed: the menu or popup on top closes -- what Dear ImGui does
/// only with its keyboard navigation on, which the editor's own keys keep
/// off. A modal dialog is left to its own Cancel. The key is used up for the
/// frame, so the panels under the popup do not take it too. True when a
/// popup closed. Before the windows of the frame are drawn.
bool closePopupOnEscape();

/// A bar between two panes, dragged to resize the one before it: `size`,
/// kept within [min, max]. `vertical`: the bar is vertical (panes side by side).
void splitter(const char* id, bool vertical, float& size, float min, float max, float length);

// --- sections and rows -------------------------------------------------------------------

/// A section of a parameter pane: a title the user folds and unfolds. True
/// when open.
bool section(const char* title, bool openByDefault = true);

/// A row: the label in the left column -- brighter when the value differs
/// from its default -- then the widget, as wide as the rest (SetNextItemWidth
/// is set). `help` shows when the label is hovered.
void rowLabel(const char* label, bool changed, const char* help);
/// Width of the label column.
float labelWidth();
/// A small button at the right end of a row that puts the value back to its
/// default. Call it before the widget, which then takes the rest of the width.
bool resetButton(const char* id, bool visible);
/// A parameter's key, a diamond at the start of its row -- before rowLabel,
/// which it leaves the cursor for. `state`: 0 not animated, 1 animated with
/// no key at this frame, 2 a key at this frame. Returns 1 for a click, 2 for
/// a right click, 0 for none.
int keyButton(const char* id, int state, const char* tooltip);
/// "fx" after a parameter's key: lit when the parameter is driven by an
/// expression. True when clicked.
bool exprButton(const char* id, bool active, const char* tooltip);

bool sliderFloat(const char* id, float& v, float min, float max, const char* format);
bool sliderInt(const char* id, int& v, int min, int max, const char* format = "%d");
/// Three numbers side by side, x red, y green, z blue.
bool dragVector(const char* id, float v[3], float speed, const char* format);
bool colorEdit(const char* id, float v[3]);
/// An on/off switch.
bool toggle(const char* id, bool& v);
/// A few choices as buttons side by side.
bool segmented(const char* id, int& v, const std::vector<const char*>& labels);

/// A file's size as people read it: "812 B", "8.1 KB", "4.4 MB".
std::string sizeText(uintmax_t bytes);
/// Text that wraps, dim: help under a header.
void note(const char* text);

/// Facts as rows -- a label, dim, and its value -- the values in a column of
/// their own, as wide as the widest label leaves:
///   if (ui::beginRows("facts")) { ui::row("Cells", "%d", n); ...; ui::endRows(); }
bool beginRows(const char* id);
/// A row's label; what is drawn next goes into its value's column.
void rowStart(const char* label);
void row(const char* label, const char* fmt, ...) IM_FMTARGS(2);
void endRows();

/// A key and what it does, for a help menu; with no keys, the title of the
/// group after it.
struct KeyHelp {
    const char* keys;
    const char* what;
};
/// The keys in a column, what they do in the next, the groups under bold
/// titles; then the same from the command line, in code's letters.
void keysHelp(const std::vector<KeyHelp>& keys, const std::vector<const char*>& commands);

/// A list to pick one from by typing -- the menus that add a node: a search
/// field, then the items under headings in a list that scrolls beneath it,
/// no taller than most of the window. Up and Down move the lit item, Enter
/// takes it; the field starts empty and focused each time the menu opens.
class PickList {
public:
    /// The field, and the list's start: inside the open popup, every frame.
    void begin(std::string& search, float width);
    /// A heading over the items after it; `dot` its colour, 0 for none.
    void heading(const char* text, ImU32 dot = 0);
    /// An item: true when it is clicked, or lit when Enter is pressed.
    bool item(const char* id, const char* label, theme::Icon icon, ImU32 iconColor, const char* help);
    /// The list's end; says so when nothing fits.
    void end();

private:
    float width_ = 0.0f;
    int cursor_ = 0, count_ = 0, shown_ = 0;
    bool moved_ = false, enter_ = false;
    std::string last_;
};

// --- the timeline ------------------------------------------------------------------------

struct TimelineState {
    int frames = 150;      ///< the range is 1 .. frames
    int current = 1;       ///< the play head
    int cached = 0;        ///< frames 1 .. cached are ready to show
    /// Of those, the runs in memory, first and last of each -- the rest are
    /// read from disk as they are played. Empty: all of them.
    std::vector<std::pair<int, int>> memory;
    bool simulating = false;
    bool playing = false;
    bool loop = true;
    float fps = 30.0f;
    /// Frames with keys: diamonds on the track -- the selected nodes'
    /// bright, the rest of the network's dim.
    std::vector<float> keys, otherKeys;
};

struct TimelineActions {
    bool togglePlay = false;
    bool toStart = false, toEnd = false, back = false, forward = false;
    bool toggleLoop = false;
    int scrubTo = 0;  ///< a frame the user put the play head on, 0 if none
};

TimelineActions timeline(const char* id, const TimelineState& s);

// --- files ---------------------------------------------------------------------------------

/// A dialog to pick a file to open or a name to save under: the folders and
/// the files with the wanted extensions, a path to type, places to jump to.
class FileBrowser {
public:
    /// `start` is a folder or a file in it. For saving, the file's name is
    /// the suggestion.
    void open(const std::string& title, std::vector<std::string> extensions, bool save, const std::string& start,
              std::vector<std::pair<std::string, std::string>> places = {});
    /// A folder rather than a file: the one named -- clicked or typed -- or,
    /// with no name, the one open. `create`: one that is not there yet may
    /// be named too, for the caller to make. `start` is shown in the folder
    /// above it, with its name typed.
    void openFolder(const std::string& title, bool create, const std::string& start,
                    std::vector<std::pair<std::string, std::string>> places = {});
    /// Draws the dialog while it is open. True, with the path in `chosen`,
    /// once the user picked one. Saving over a file that is there already
    /// is asked about first: Save again (Replace) writes over it.
    bool draw(std::string& chosen);
    bool isOpen() const { return open_ || requested_; }

private:
    void list();
    bool wanted(const std::filesystem::path& p) const;

    std::string title_;
    std::vector<std::string> extensions_;
    bool save_ = false;
    bool folders_ = false;  ///< openFolder: a folder is chosen
    bool open_ = false, requested_ = false;
    std::filesystem::path dir_;
    std::string name_, pathText_, error_;
    std::string replace_;  ///< a file there already, asked about: saving to it again replaces it
    std::vector<std::pair<std::string, std::string>> places_;
    struct Entry {
        std::string name;
        bool folder = false;
        uintmax_t size = 0;
    };
    std::vector<Entry> entries_;
    int selected_ = -1;
};

}  // namespace pg::editor::ui
