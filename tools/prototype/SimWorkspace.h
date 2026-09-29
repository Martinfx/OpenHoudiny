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
// Digital assets (pg/sim/Asset.h): geometry nodes chosen become one asset
// (Make Asset), its node in their place. Going into an instance -- double
// click, I -- edits the asset's inside, with the instance's inputs coming
// into its Asset Input nodes; the scene waits, simulated as it was. Back up
// (U), what changed is a new version of the asset: written to its file, and
// every instance follows. A parameter inside is promoted -- right click on
// its name -- to show on the asset's node.
//
#include "Bake.h"
#include "Gizmo.h"
#include "NodeCanvas.h"
#include "RenderJob.h"
#include "SimRunner.h"
#include "Workspace.h"

#include "pg/gl/Volume.h"
#include "pg/sim/Cooker.h"
#include "pg/sim/GeometryGraph.h"
#include "pg/sim/Network.h"

#include <map>
#include <set>
#include <memory>
#include <string>
#include <vector>

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
    /// A parameter as expressions, one field a channel, with what they give.
    void expressionFields(int id, const sim::ParamDef& p, const sim::ParamValue& now);
    void removeNodes(const std::vector<int>& nodes);
    void toggleBypass(const std::vector<int>& nodes);

    // --- digital assets (SimAssets.cpp) --------------------------------------------------
    /// Whether the network edited is an asset's inside -- gone into, or a
    /// .pgasset opened.
    bool editingAsset() const { return !net_.asset().name.empty(); }
    /// Goes into asset node `id`: the asset's network is what is edited.
    bool enterAsset(int id);
    /// Back up a level; what changed inside becomes the asset's new version.
    /// False, with why, when it cannot be one: the workspace stays inside.
    bool leaveAsset();
    /// What changed in the asset edited, as its new version: written to its
    /// file -- `path`, or the one it came from, or one in the user's folder
    /// -- and into the library, every instance following. True when nothing
    /// changed.
    bool commitAsset(const std::string& path = {});
    /// The instance's inputs, at the frame on screen, into the Asset Input
    /// nodes of the inside edited.
    void feedAssetInputs();
    /// The dialog of Make Asset: a name for the selected nodes as one asset.
    void makeAssetDialog();
    /// The asset's own settings -- label, help, what it promotes -- where
    /// the network's overview is.
    void assetOverview();
    /// A parameter's right click menu: promote it, copy a reference to it.
    void paramMenu(int id, const sim::ParamDef& p);
    /// Scene > Building > ...: where in the assets the network edited is.
    std::string levelsText() const;

    // --- parameters --------------------------------------------------------------------
    void nodeParameters(const sim::Node& node, const sim::NodeType& type);
    void networkOverview();

    // --- geometry (SimGeometry.cpp) ------------------------------------------------------
    /// Syncs the geometry graph with the network and cooks the displayed
    /// node at the frame on screen, for the viewport; notes cook errors.
    void updateGeometry();
    /// Geometry node `id`'s geometry at the frame on screen, cooked now on
    /// the window's thread (exports); null for a node that is not one.
    GeometryPtr geometryOf(int id);
    /// The node the spreadsheet shows: the current one, or the displayed.
    int sheetNode() const;
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
    /// The pieces of the rigid bodies of the frame on screen, as the
    /// solver's look draws them, into the renderer -- made again only when
    /// the frame or the look changed.
    void updatePieces();
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
    /// A network of geometry nodes alone, one of them displayed -- a model,
    /// an asset's inside: nothing to simulate is no problem.
    bool geometryOnly() const;

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
    /// Bakes the network into `target` in the background (Bake.h), playing
    /// its frames as they land; `resume`: on from the folder's checkpoint.
    bool startBake(const std::string& target, bool resume);
    /// The bake, looked at: its end said, the frames it wrote found.
    void pollBake();
    /// How far it has got, in the Simulation panel.
    void bakePanel();
    /// The dialog that exports geometry node `id`'s geometry: at the frame
    /// on screen, or at every frame cached (`frames`).
    void chooseExport(int id, bool frames);
    bool exportGeometry(int id, const std::string& path);
    /// A file a frame: `pattern` numbered by io::framePath ($F4, or .0007
    /// before the extension).
    bool exportFrames(int id, const std::string& pattern);
    /// The dialog that exports the shot as a USD stage, and the export: the
    /// frames cached, with the displayed geometry, the pieces, the grit, the
    /// gas (VDB files beside it), the camera and the light
    /// (pg/sim/UsdExport.h).
    void chooseUsd();
    bool exportUsd(const std::string& path);

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
    /// The geometry nodes, cooked: the network's; inside an asset, its inside's.
    std::unique_ptr<sim::GeometryGraph> geometry_ = std::make_unique<sim::GeometryGraph>();

    /// A network gone out of into an asset's inside, as it was left.
    struct Level {
        sim::Network net;
        std::string path, example, savedText;
        History history;
        std::unique_ptr<sim::GeometryGraph> geometry;
        std::shared_ptr<const sim::Network> snapshot;  ///< `net`, for the cooker
        std::string folder;
        NodeCanvas::View view;
        int instance = 0;  ///< the asset node gone into
    };
    std::vector<Level> levels_;
    uint64_t levelsRevision_ = 0;  ///< changes as levels are gone into and out of
    int enterRequest_ = 0;         ///< a node to go into, next frame
    bool leaveRequest_ = false;    ///< back up, next frame
    bool makeAssetOpen_ = false;   ///< the Make Asset dialog is to open
    std::string assetName_, assetLabel_, assetError_;
    std::vector<int> assetNodes_;  ///< what Make Asset makes one
    /// Parameters shown as expressions ("node.param"), though they may have
    /// none yet: fx was clicked.
    std::set<std::string> exprMode_;
    std::map<int, std::string> cookWarnings_;  ///< what each warned about, cooking
    std::map<int, std::string> cookLogs_;      ///< what each printed, cooking
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
    /// The geometry, cooked on a thread of its own -- after runner_, whose
    /// frames it reads: it goes first.
    std::unique_ptr<sim::Cooker> cooker_;
    std::string cookKey_;          ///< what was last asked of it
    double cookAsked_ = 0.0;       ///< when (ImGui time)
    double cookMs_ = 0.0;          ///< how long the last cook took
    GeometryPtr sheetGeometry_;    ///< the spreadsheet's node's, as last cooked
    int sheetGeometryNode_ = 0;
    bool synchronous_ = false;

    // Playback.
    int current_ = 1;
    bool playing_ = true, loop_ = true;
    double clock_ = 0.0;

    // What the viewport shows, to draw again only when it changes.
    std::shared_ptr<const sim::Frame> shown_;
    std::shared_ptr<const sim::Frame> piecesFrame_;  ///< the frame the pieces drawn are of
    std::string piecesKey_;                          ///< ... and the look they are drawn with
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
    std::string shownPlate_;      ///< the plate the renderer has, through the camera
    sim::Camera plateCamera_;     ///< and the camera it was set for
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
        None, Open, SaveAs, Image, Frames, Video, MeshFile, ImportMesh, SaveCache, LoadCache, Bake, ExportGeometry, ExportFrames,
        ExportUsd, OpenAsset, SaveAsset
    } fileAction_ = FileAction::None;
    int fileNode_ = 0;        ///< MeshFile: the node whose file is chosen; Export...: whose geometry
    std::string fileParam_;
    std::string cacheFolder_;  ///< the folder the cache was last saved to or loaded from

    // Preview and bakes.
    bool preview_ = false;     ///< the gas and the water simulated on coarser grids (sim::preview)
    Bake bake_;
    std::string bakeFolder_;   ///< where the last bake went
    double bakePolled_ = 0.0;  ///< when the bake and the frames on disk were last looked at (ImGui time)

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
