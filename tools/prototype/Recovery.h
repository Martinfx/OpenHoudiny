#pragma once
//
// What is open and not saved, kept in a folder as it is worked on -- an
// autosave -- so that an editor that crashes, or is killed, loses at most
// the last half minute of it: the next one started offers it back.
//
// An autosave is the network's or the graph's own text, two comment lines
// before it saying what it is of:
//
//   # prototype autosave of /home/me/scene.pgsim
//   # example campfire
//
// named for the file -- or the example, or "untitled" -- and the editor's
// process, scene-4182.pgsim: one an editor still running keeps is not left
// behind.
//
#include <string>
#include <vector>

namespace pg::editor {

/// $XDG_STATE_HOME/prototype/recovery, else ~/.local/state/prototype/recovery.
std::string defaultRecoveryFolder();

/// An autosave an editor no longer running left.
struct Kept {
    std::string path;       ///< the autosave
    std::string of;         ///< the file it is of; empty: none yet
    std::string example;    ///< or the example it began as
    std::string extension;  ///< ".pgsim" or ".pgsg": whose
    std::string text;       ///< the network or graph, as its file would hold it
    long long secondsAgo = 0;
};

/// Those in `folder`, newest first.
std::vector<Kept> leftBehind(const std::string& folder);

/// `text` -- what is open, of the file `of` or the example `example` -- as
/// this editor's autosave of its kind (`extension`) in `folder`, written
/// whole. The autosave's path; empty, with `error`, when it cannot be
/// written.
std::string writeAutosave(const std::string& folder, const std::string& extension, const std::string& of,
                          const std::string& example, const std::string& text, std::string& error);

/// What a kept autosave is called in a list: "scene.pgsim", "campfire
/// (example)", "untitled.pgsg".
std::string keptName(const Kept& k);

}  // namespace pg::editor
