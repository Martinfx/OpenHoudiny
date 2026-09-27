#pragma once
//
// The editor's Simulation network: smoke and fire built from nodes. The
// network on the canvas, the selected node's parameters, the viewport with
// the gas on its floor, and the timeline over the cache of frames.
//
// Geometry nodes cook in a GeometryGraph of the workspace's own (SimGeometry.cpp):
// only what changed cooks again. The node with the display flag shows in
// the viewport; the spreadsheet shows a node's points, vertices, primitives,
// detail and volumes.
//
// The simulation runs on a thread of its own (SimRunner) and keeps its frames:
// play and scrub them without simulating again. A change that alters what is
// simulated -- a source, a force, the solver -- starts it again from frame 1
// while the play head stays; a change of the look only draws the frame again.
// The frames go to a folder on disk and come back from one (Save Cache, Load
// Cache: sim/Cache.h); a geometry node's geometry -- the particles, the gas as
// volumes -- is exported, a frame or every frame (io/Export.h). The shot is
// rendered to a PNG, to numbered PNGs or to a video (io/Video.h), the last
// two a frame at a time behind a modal that shows how far it got (RenderJob).
//
// It starts on an empty scene; File > Examples has finished ones.
//
#include "Gizmo.h"
#include "NodeCanvas.h"
#include "RenderJob.h"
#include "SimRunner.h"
#include "Workspace.h"

#include "pg/gl/Volume.h"
#include "pg/sim/GeometryGraph.h"
#include "pg/sim/Network.h"

#include <map>
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

    // --- geometry (SimGeometry.cpp) ------------------------------------------------------
    /// Syncs the geometry graph with the network and cooks the displayed
    /// node at the frame on screen, for the viewport; notes cook errors.
    void updateGeometry();
    /// Geometry node `id`'s geometry at the frame on screen; null for a
    /// node that is not one.
    GeometryPtr geometryOf(int id);
    /// The spreadsheet: the current node's geometry, or the displayed one's.
    void spreadsheet();

    // --- the viewport -------------------------------------------------------------------
    std::shared_ptr<const sim::Frame> frameToShow() const;
    /// The frame the viewport shows: the simulation's frame on screen, else
    /// the play head's.
    int shownFrame() const { return shown_ ? shown_->number : current_; }
    /// The renderer draws what is there at `frame` -- the play head's: the
    /// look, the objects where they are then -- whatever frame of the
    /// simulation is ready to show with them.
    void pose(int frame);
    void updateGuides();
    void drawGnomon(ImDrawList* d, ImVec2 corner) const;
    /// The size of a render: the camera's picture, or the viewport's.
    void shotSize(int& width, int& height) const;
    /// Draws what a render shows at `frame` -- through the camera, if there
    /// is one -- at twice `width` x `height`, the viewport's view kept.
    void renderShot(int width, int height, int frame);
    /// How far in front of `camera` the middle of the scene is: where an
    /// orbit through it turns round.
    float focusOf(const sim::Camera& camera) const;
    bool renderImage(const std::string& path);
    /// Every frame of the shot -- 1 to the Output's last, as the simulation
    /// gets there -- into `target`: a video when its extension is one's,
    /// else a folder of numbered PNGs.
    void startRender(const std::string& target);
    /// Frame `frame` of the shot for the render job, posed and drawn as the
    /// viewport would show it then. False while the simulation has not got
    /// there; false with why when it will not.
    bool drawShotFrame(int frame, std::vector<uint8_t>& rgb, std::string& error);
    /// Where a render goes by default: where the last one went, the
    /// network's folder, the current one if it can be written, or home.
    std::string renderFolder() const;
    /// A notice over the viewport: what was written, where, with Open and
    /// Show. An error -- or a `sticky` one, the end of a long render --
    /// stays until closed.
    void notify(std::string text, std::string path, bool error, bool sticky = false);
    void drawNotice(ImDrawList* d, ImVec2 lo, ImVec2 hi);
    /// The dialog of Render Video: the kinds of file there are to write.
    void chooseVideo();
    /// A network with no node: what a new scene is.
    bool emptyScene() const { return net_.nodes().empty(); }

    // --- the cache on disk, and export ------------------------------------------------
    /// Where a file made from the network goes by default: its file's
    /// folder, or the current one for an example or a network never saved.
    std::string outputFolder() const;
    /// The network's name for the files made from it: its file's, the
    /// example's, or "untitled".
    std::string stem() const;
    bool saveCache(const std::string& folder);
    /// The frames in `folder` in place of simulated ones, until what is
    /// simulated changes.
    bool loadCache(const std::string& folder);
    /// The dialog that exports geometry node `id`'s geometry: at the frame
    /// on screen, or at every frame cached (`frames`).
    void chooseExport(int id, bool frames);
    bool exportGeometry(int id, const std::string& path);
    /// A file a frame: `pattern` numbered by io::framePath ($F4, or .0007
    /// before the extension).
    bool exportFrames(int id, const std::string& pattern);

    // --- the camera (SimViewport.cpp) ---------------------------------------------------
    /// Looks through the Output's camera, or stops.
    void setThroughCamera(bool on);
    /// The Output's camera moved and turned to see what the viewport sees;
    /// one made if there is none.
    void cameraFromView();
    /// A camera that sees what the viewport sees, linked into the Output.
    int addCamera();

    // --- selecting, the gizmo, adding to the scene (SimViewport.cpp) --------------------
    /// A node the gizmo moves, with its values when a drag began.
    struct Placed {
        int id = 0;
        sim::ParamValue center{}, rotation{}, axis{}, size{};
        float radius = 0.0f, height = 0.0f;
    };
    /// Where a node is and how it is turned; false for one with nothing to move.
    bool placeOf(int id, Vec3& center, sim::Rotation& frame) const;
    /// A box round every domain of what is simulated -- the gas's, the
    /// water's -- or the runner's when nothing compiles.
    sim::Domain sceneBox() const;
    /// "72 × 96 × 72 cells", "water 48 × 24 × 24 · 46 k particles", or both.
    std::string gridsText() const;
    /// The selected nodes the gizmo can move, turn or size.
    std::vector<int> movable() const;
    /// The tool the gizmo is for `nodes`: the chosen one, or the first they allow.
    GizmoMode toolFor(const std::vector<int>& nodes) const;
    void pivotOf(const std::vector<int>& nodes, GizmoMode tool, Vec3& pivot, sim::Rotation& frame) const;
    Placed placedOf(int id) const;
    void applyDrag(const GizmoDrag& drag);
    void restoreDrag();
    /// A key at the play head on where each selected node is -- its place,
    /// turn and size (K).
    void keySelection();
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
    /// An object of the mesh in an OBJ file, sized to fit the scene.
    int addMesh(const std::string& path, const Vec3& at);
    /// Where relative paths of the network (meshes) are read from: its
    /// file's folder, or the examples' for an example.
    std::string folder() const;
    int ensurePyroChain();
    /// The Liquid Solver, made -- with a Water Look into the Output -- when
    /// there is none.
    int ensureLiquidChain();
    /// Rain over the whole scene, a layer of the Output; a storm is heavier
    /// and brings a gusting wind. Returns its id.
    int addRain(bool storm);
    /// Links `node`'s output into that input of every solver that has it.
    void linkIntoSolvers(int node, const char* output, const char* input);
    /// Where a new object, source or force goes in the network.
    ImVec2 freeSlot() const;
    void frameSelection();
    void viewKeys(bool overView);

    sim::Network net_;
    sim::GeometryGraph geometry_;          ///< the geometry nodes, cooked
    std::map<int, std::string> cookErrors_;  ///< what went wrong cooking each
    bool sheet_ = false;                   ///< the parameters panel shows the spreadsheet
    int sheetClass_ = 0;                   ///< points, vertices, primitives, detail, volumes
    std::string editKey_, editText_;       ///< a text parameter being typed: "id.name", and its text
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

    const gl::Api& gl_;
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
    int guidesFrame_ = 0;
    int posedFrame_ = 0;               ///< the frame the renderer draws the objects and the look at
    uint64_t posedRevision_ = ~0ull;
    Vec3 framedSize_;  ///< the domain the camera was framed for, world units
    bool framed_ = false;
    bool throughCamera_ = false;  ///< the viewport looks through the Output's camera
    ImVec2 gateLo_, gateHi_;      ///< the camera's picture in the viewport, while it does

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
    enum class FileAction {
        None, Open, SaveAs, Image, Frames, Video, MeshFile, ImportMesh, SaveCache, LoadCache, ExportGeometry, ExportFrames
    } fileAction_ = FileAction::None;
    int fileNode_ = 0;        ///< MeshFile: the node whose file is chosen; Export...: whose geometry
    std::string fileParam_;
    std::string cacheFolder_;  ///< the folder the cache was last saved to or loaded from

    // Rendering.
    RenderJob job_;
    int jobWidth_ = 0, jobHeight_ = 0;  ///< the size of the job's frames, fixed when it starts
    int jobReturnFrame_ = 1;            ///< the play head, put back when the job ends
    bool jobWasPlaying_ = false;
    std::string renderFolder_;          ///< where the last render went
    struct Notice {
        std::string text, path;
        bool error = false;
        double until = 0.0;  ///< ImGui time it goes away at; 0: when closed
    } notice_;
    ImVec2 noticeLo_, noticeHi_;  ///< the notice, last frame: the viewport's clicks are not its

    std::string message_;
    bool messageError_ = false;
};

}  // namespace pg::editor
