#pragma once
//
// What the editor's shell asks of a network it shows -- the simulation's or
// the shaders'. The shell lays the panels out the same for both (Editor.h):
//
//   +-------------------------+--------------+
//   |                         |  Parameters  |
//   |        Viewport         +--------------+
//   |                         |              |
//   +-------------------------+   Network    |
//   |  Timeline / Code        |              |
//   +-------------------------+--------------+
//   status bar
//
// and draws the menu bar; the workspace fills the panels and its menus.
//
#include "Widgets.h"

#include "imgui.h"

#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <string>
#include <vector>

namespace pg::editor {

class Workspace {
public:
    virtual ~Workspace() = default;

    virtual const char* name() const = 0;
    /// "campfire.pgsim *": the file, a star for changes not saved.
    virtual std::string title() const = 0;
    virtual bool modified() const = 0;

    /// Once a frame, before the panels: `dt` seconds since the last.
    virtual void update(float dt) = 0;
    /// Another workspace is shown instead: what works in the background
    /// for this one's panels may stop.
    virtual void hidden() {}
    /// Keys that work anywhere in the workspace.
    virtual void shortcuts() = 0;

    virtual void viewport(ImVec2 size) = 0;
    virtual void bottom(ImVec2 size) = 0;
    /// How tall the bottom panel would like to be.
    virtual float bottomHeight() const = 0;
    virtual void parameters(ImVec2 size) = 0;
    virtual void network(ImVec2 size) = 0;

    /// The File menu's items, and the workspace's own menus.
    virtual void fileMenu() = 0;
    virtual void editMenu() = 0;
    virtual void menus() = 0;
    virtual void helpMenu() = 0;
    /// Dialogs: a file browser, confirmations.
    virtual void popups() = 0;

    /// The left of the status bar; the right shows `message()`.
    virtual std::string status() const = 0;
    virtual const std::string& message() const = 0;
    virtual bool messageIsError() const = 0;

    /// Opens a file of this workspace's kind.
    virtual bool open(const std::string& path) = 0;
    virtual bool canOpen(const std::string& path) const = 0;

    /// What would throw away changes not saved -- New, Open, an example,
    /// quitting -- goes through here: `then` runs at once when everything
    /// is saved; else once the user has said what becomes of the changes.
    /// Save: after they are written (a file picked first where there is
    /// none); Don't Save: at once; Cancel: never. `doing` ends the
    /// question: "Save them before <doing>?".
    void unlessUnsaved(std::string doing, std::function<void()> then);
    /// That question, while it is asked: the shell draws it after popups().
    /// Enter saves, D does not, Escape cancels.
    void unsavedDialog();
    /// Whatever is open and not saved: the network -- and, inside an
    /// asset, those round it.
    virtual bool unsaved() const { return modified(); }

    // --- autosave (Recovery.h) ------------------------------------------------------------

    /// What is open as its file would hold it; the file; the example it
    /// began as; ".pgsim" or ".pgsg".
    virtual std::string documentText() const = 0;
    virtual std::string documentPath() const = 0;
    virtual std::string documentExample() const { return {}; }
    virtual const char* documentExtension() const = 0;
    /// What an editor that did not close kept -- `text`, of the file `of`
    /// or the example `example` -- open in place of what is: not saved,
    /// Ctrl+S saving it where it was.
    virtual bool recover(const std::string& text, const std::string& of, const std::string& example) = 0;
    /// Once a frame, `now` in seconds: what is open and not saved copied
    /// into `folder` -- at once, then at most every kAutosaveSeconds while
    /// it changes; the copy gone once it is saved, or gone. No folder: none.
    void autosave(const std::string& folder, double now);
    /// The copy gone: what is open closes as the user wanted (quitting).
    void dropAutosave();
    static constexpr double kAutosaveSeconds = 30.0;

protected:
    /// Saves all that is open, then runs `then`: at once where it has a
    /// file, once one is picked and written where it has none -- never if
    /// it cannot be written, or no file is picked.
    virtual void saveThen(std::function<void()> then) = 0;

private:
    std::string doing_;
    std::function<void()> afterAsking_;
    bool ask_ = false;
    std::string autosaveFile_, autosavedText_;
    double autosavedAt_ = -1e30, checkedAt_ = -1e30;
};

/// Undo and redo as whole states -- a network saved as text. A change is
/// committed once things settle (no mouse button down, no widget active)
/// and stay so for kQuiet seconds: a drag or a slider is one step, not a
/// hundred, and so is a run of quick changes -- a brush resized notch by
/// notch. The state is written out only to be committed: what tells a
/// change is a key, new whenever the state is (stateKey). Kept: at most
/// kMaxStates states and kMaxBytes of them, the oldest going first.
class History {
public:
    static constexpr size_t kMaxStates = 300;
    static constexpr size_t kMaxBytes = size_t(256) << 20;
    static constexpr double kQuiet = 0.4;

    void reset(std::string state, uint64_t key) {
        undo_.clear();
        redo_.clear();
        bytes_ = 0;
        committed_ = std::move(state);
        key_ = key;
        pending_ = false;
    }
    /// Each frame, `now` in seconds: `text()` writes the state out, called
    /// only when a change is committed.
    template <class Text>
    void track(uint64_t key, bool settled, double now, const Text& text) {
        if (key != key_) {
            key_ = key;
            changedAt_ = now;
            pending_ = true;
        }
        if (pending_ && settled && now - changedAt_ >= kQuiet) commit(text());
    }
    bool canUndo() const { return pending_ || !undo_.empty(); }
    bool canRedo() const { return !pending_ && !redo_.empty(); }
    /// The state to go back to -- a change still waiting committed first;
    /// the current one goes onto redo. adopt() the key of what it restores.
    template <class Text>
    std::string undo(const Text& text) {
        if (pending_) commit(text());
        if (undo_.empty()) return committed_;
        bytes_ += committed_.size();
        redo_.push_back(std::move(committed_));
        committed_ = std::move(undo_.back());
        undo_.pop_back();
        bytes_ -= committed_.size();
        return committed_;
    }
    template <class Text>
    std::string redo(const Text& text) {
        if (pending_) commit(text());
        if (redo_.empty()) return committed_;
        bytes_ += committed_.size();
        undo_.push_back(std::move(committed_));
        committed_ = std::move(redo_.back());
        redo_.pop_back();
        bytes_ -= committed_.size();
        return committed_;
    }
    /// The state now is the one committed -- restored by undo or redo -- of
    /// this key: nothing waits.
    void adopt(uint64_t key) {
        key_ = key;
        pending_ = false;
    }
    size_t states() const { return undo_.size() + redo_.size(); }
    size_t bytes() const { return bytes_; }

private:
    void commit(std::string state) {
        pending_ = false;
        if (state == committed_) return;
        bytes_ += committed_.size();
        undo_.push_back(std::move(committed_));
        committed_ = std::move(state);
        for (const std::string& r : redo_) bytes_ -= r.size();
        redo_.clear();
        while (!undo_.empty() && (undo_.size() > kMaxStates || bytes_ > kMaxBytes)) {
            bytes_ -= undo_.front().size();
            undo_.pop_front();
        }
    }

    std::deque<std::string> undo_, redo_;
    std::string committed_;
    size_t bytes_ = 0;  ///< of undo_ and redo_
    uint64_t key_ = 0;
    double changedAt_ = 0.0;
    bool pending_ = false;  ///< changed since committed_
};

/// A key that changes whenever a graph's saved text would: its revision --
/// every edit -- and where its nodes are, which moving them changes and
/// the revision does not.
template <class Nodes>
uint64_t stateKey(uint64_t revision, const Nodes& nodes) {
    uint64_t h = 1469598103934665603ull ^ revision;
    auto mix = [&h](float v) {
        uint32_t bits = 0;
        std::memcpy(&bits, &v, sizeof bits);
        h = (h ^ bits) * 1099511628211ull;
    };
    for (const auto& n : nodes) {
        mix(n.x);
        mix(n.y);
    }
    return h;
}

/// True when nothing is being dragged or typed: a moment to commit to History.
inline bool settled() {
    return !ImGui::IsAnyItemActive() && !ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
           !ImGui::IsMouseDown(ImGuiMouseButton_Right) && !ImGui::IsMouseDown(ImGuiMouseButton_Middle);
}

}  // namespace pg::editor
