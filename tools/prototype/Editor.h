#pragma once
//
// The editor -- what prototype opens when it is given no command: one window,
// two networks in the same layout (Workspace.h). Simulation, the default:
// smoke and fire from nodes (SimWorkspace.h). Shaders: shader graphs for
// OpenGL, Vulkan and the rest (ShaderWorkspace.h).
//
#include "Recovery.h"
#include "ShaderWorkspace.h"
#include "SimWorkspace.h"

#include "pg/gl/Gl.h"

#include <memory>
#include <string>
#include <vector>

namespace pg::editor {

class Editor {
public:
    /// `synchronous`: the simulation takes one step per frame of the window,
    /// on its thread -- for screenshots.
    Editor(const gl::Api& gl, std::vector<std::string> libraryFiles, std::string examplesDir, bool synchronous);
    /// Closing as the user wanted -- or as a script or a screenshot ends:
    /// the autosaves go. A crash leaves them, to be recovered.
    ~Editor();
    Editor(const Editor&) = delete;
    Editor& operator=(const Editor&) = delete;

    /// Where what is open and not saved is kept as it is worked on
    /// (Recovery.h); empty: nowhere. What an editor that did not close
    /// left there is offered back -- now, and from File > Recover Unsaved
    /// Work.
    void setRecovery(std::string folder);

    /// Opens a .pgsim network or a .pgsg shader graph, in its workspace.
    bool open(const std::string& path);
    /// A simulation example by name ("campfire"), in the Simulation workspace.
    bool openExample(const std::string& name);
    void showShaders();
    void showSimulation();
    SimWorkspace& simulation() { return *sim_; }
    ShaderWorkspace& shaders() { return *shaders_; }

    /// Draws the whole window for one frame.
    void frame(float dt);

    /// Out of the editor -- once each network with changes not saved has
    /// been asked about (Workspace::unlessUnsaved) and none cancelled.
    void requestQuit() { quitFrom(0); }
    bool quitRequested() const { return quit_; }
    /// "campfire.pgsim * -- Simulation -- prototype"
    std::string title() const;

private:
    void menuBar();
    void statusBar(float height);
    /// Asks about the networks from the `i`-th on, each shown as it is
    /// asked; then quits.
    void quitFrom(size_t i);
    /// The autosaves left behind, to be recovered or let go. Enter recovers
    /// the newest, Escape leaves them for later.
    void recoveryDialog();
    Workspace& current() { return *workspaces_[active_]; }
    const Workspace& current() const { return *workspaces_[active_]; }

    std::unique_ptr<SimWorkspace> sim_;
    std::unique_ptr<ShaderWorkspace> shaders_;
    std::vector<Workspace*> workspaces_;
    size_t active_ = 0;
    size_t shown_ = 0;  ///< the workspace shown last frame

    // The layout, in pixels: kept as the window is resized, changed by the splitters.
    float rightWidth_ = 0.0f;
    float paramsHeight_ = 0.0f;

    bool quit_ = false;
    bool about_ = false;

    std::string recovery_;    ///< the autosaves' folder; empty: none
    std::vector<Kept> kept_;  ///< those left behind, as last looked for
    bool offerRecovery_ = false;
    double now_ = 0.0;        ///< seconds the editor has run
};

}  // namespace pg::editor
