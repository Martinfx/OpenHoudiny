#pragma once
//
// The editor's Simulation network: smoke and fire built from nodes. The
// network on the canvas, the selected node's parameters, the viewport with
// the gas on its floor, and the timeline over the cache of frames.
//
// The simulation runs on a thread of its own (SimRunner) and keeps its frames:
// play and scrub them without simulating again. A change that alters what is
// simulated -- a source, a force, the solver -- starts it again from frame 1
// while the play head stays; a change of the look only draws the frame again.
//
#include "Gizmo.h"
#include "NodeCanvas.h"
#include "SimRunner.h"
#include "Workspace.h"

#include "pg/gl/Volume.h"
#include "pg/sim/Network.h"

#include <memory>
#include <string>

namespace pg::editor {

class SimWorkspace : public Workspace {
public:
    /// `synchronous`: one step of the simulation per frame of the window,
    /// on the window's thread -- for screenshots.
    SimWorkspace(const gl::Api& gl, bool synchronous);

    const char* name() const override { return "Simulation"; }
    std::string title() const override;
    bool modified() const override;

    void update(float dt) override;
    void shortcuts() override;
    void viewport(ImVec2 size) override;
    void bottom(ImVec2 size) override;
    float bottomHeight() const override;
    void parameters(ImVec2 size) override;
    void network(ImVec2 size) override;

    void fileMenu() override;
    void editMenu() override;
    void menus() override;
    void helpMenu() override;
    void popups() override;

    std::string status() const override;
    const std::string& message() const override { return message_; }
    bool messageIsError() const override { return messageError_; }

    bool open(const std::string& path) override;
    bool canOpen(const std::string& path) const override;

    bool openExample(const std::string& name);
    void newNetwork();
    /// Selects a node by name, as if clicked: for screenshots.
    void selectNode(const std::string& name);
    /// What the viewport's gizmo does: select, move, rotate, scale.
    void setTool(GizmoMode tool) { tool_ = tool; }

private:
    void load(const sim::Network& net, const std::string& path, const std::string& example);
    bool save(const std::string& path);
    void recompile();
    void restore(const std::string& state);
    void undo();
    void redo();
    void setMessage(std::string message, bool error = false);

    // --- the network -------------------------------------------------------------------
    std::vector<CanvasNode> canvasNodes() const;
    std::vector<CanvasLink> canvasLinks() const;
    CanvasModel canvasModel();
    bool addMenu(ImVec2 at, const PinRef* pending);
    void nodeMenu(int node);
    int addNode(const std::string& type, ImVec2 at, const PinRef* pending);
    void duplicate(const std::vector<int>& nodes);
    void removeNodes(const std::vector<int>& nodes);
    void toggleBypass(const std::vector<int>& nodes);

    // --- parameters --------------------------------------------------------------------
    void nodeParameters(const sim::Node& node, const sim::NodeType& type);
    void networkOverview();

    // --- the viewport -------------------------------------------------------------------
    std::shared_ptr<const sim::Frame> frameToShow() const;
    void updateGuides();
    void drawGnomon(ImDrawList* d, ImVec2 corner) const;
    bool renderImage(const std::string& path, int width, int height);
    bool renderFrames(const std::string& folder);

    // --- selecting, the gizmo, adding to the scene (SimViewport.cpp) --------------------
    /// A node the gizmo moves, with its values when a drag began.
    struct Placed {
        int id = 0;
        sim::ParamValue center{}, rotation{}, axis{}, size{};
        float radius = 0.0f, height = 0.0f;
    };
    /// Where a node is and how it is turned; false for one with nothing to move.
    bool placeOf(int id, Vec3& center, sim::Rotation& frame) const;
    /// The selected nodes the gizmo can move, turn or size.
    std::vector<int> movable() const;
    /// The tool the gizmo is for `nodes`: the chosen one, or the first they allow.
    GizmoMode toolFor(const std::vector<int>& nodes) const;
    void pivotOf(const std::vector<int>& nodes, GizmoMode tool, Vec3& pivot, sim::Rotation& frame) const;
    Placed placedOf(int id) const;
    void applyDrag(const GizmoDrag& drag);
    void restoreDrag();
    /// The node under a point of the viewport: an object the ray meets, a
    /// source, or a guide line near it. 0 for none.
    int pickAt(const ViewCamera& cam, ImVec2 mouse) const;
    /// Where the ray under a point of the screen meets the floor.
    Vec3 floorPoint(const ViewCamera& cam, ImVec2 screen) const;
    void viewTools(ImVec2 at);
    void viewMenu();
    /// The items that add to the scene, what they add placed at `at`. True
    /// when something was added.
    bool sceneMenu(const Vec3& at);
    int addToScene(const std::string& kind, const Vec3& at);
    int ensurePyroChain();
    /// Links `node`'s output into that input of every solver that has it.
    void linkIntoSolvers(int node, const char* output, const char* input);
    /// Where a new object, source or force goes in the network.
    ImVec2 freeSlot() const;
    void frameSelection();
    void viewKeys(bool overView);

    sim::Network net_;
    std::string path_;       ///< empty: never saved
    std::string example_;    ///< the example it came from, if any
    std::string savedText_;  ///< as on disk, or as the example came
    sim::Compiled compiled_;
    uint64_t compiledRevision_ = ~0ull;
    History history_;

    NodeCanvas canvas_;
    std::string search_;
    std::string nameEdit_;
    int nameEditNode_ = 0;

    gl::VolumeRenderer renderer_;
    std::string rendererLog_;
    std::unique_ptr<SimRunner> runner_;
    bool synchronous_ = false;

    // Playback.
    int current_ = 1;
    bool playing_ = true, loop_ = true;
    double clock_ = 0.0;

    // What the viewport shows, to draw again only when it changes.
    std::shared_ptr<const sim::Frame> shown_;
    bool viewDirty_ = true;
    int viewWidth_ = 0, viewHeight_ = 0;
    bool guides_ = true;
    std::vector<int> guidesSelection_;
    uint64_t guidesRevision_ = ~0ull;
    Vec3 framedSize_;  ///< the domain the camera was framed for, world units
    bool framed_ = false;

    gl::Lines guideLines_;       ///< the guides drawn, for picking
    Gizmo gizmo_;
    GizmoMode tool_ = GizmoMode::Move;
    bool localAxes_ = true, snap_ = false;
    std::vector<Placed> dragStart_;
    Vec3 dragPivot_;
    bool gizmoOwnsMouse_ = false;  ///< the press began on the gizmo
    int hovered_ = 0;              ///< the node under the mouse
    std::vector<int> highlighted_;
    int highlightedHover_ = 0;
    ImVec2 toolsLo_, toolsHi_;     ///< the toolbar, last frame
    ViewCamera camera_;            ///< the viewport's, last frame
    Vec3 addAt_;                   ///< where the add menu puts what it adds
    int newColor_ = 0;

    ui::FileBrowser files_;
    enum class FileAction { None, Open, SaveAs, Image, Frames } fileAction_ = FileAction::None;

    std::string message_;
    bool messageError_ = false;
};

}  // namespace pg::editor
